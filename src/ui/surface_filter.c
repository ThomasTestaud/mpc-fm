/* The control-surface filter: what goes to the FM page, what to the app.
 * See surface.h. Runs in the app's MIDI thread: no locks, no allocation;
 * one write() to an eventfd per event to wake the UI thread. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "surface.h"

#define N_SHIFT 49
#define N_NOTE_REPEAT 11
#define N_B1 12
#define N_B3 14
#define N_PUSH 111
#define N_MINUS 55
#define N_PLUS 54
#define N_SEQ_RECORD 80
#define N_STOP 81
#define N_PLAY 82
#define CC_K1 16
#define CC_K3 18
#define CC_ENCODER 100

static _Atomic int g_ready, g_open, g_close_req;
static _Atomic int g_knob[3] = {-1, -1, -1};
static int g_shift;
static uint8_t g_held[16]; /* bit per note: its press was taken, take its release too */
static _Atomic uint64_t g_taken, g_lost;

void surf_set_ready(int ready) { atomic_store(&g_ready, ready != 0); }
void surf_request_close(void) { atomic_store(&g_close_req, 1); }
int surf_page_open(void) { return atomic_load(&g_open); }
int surf_knob_pos(int k) { return k >= 0 && k < 3 ? atomic_load(&g_knob[k]) : -1; }

/* ---- event queue: producers in the MIDI thread(s), one consumer ---- */
#define QN 256
static surf_event g_q[QN];
static _Atomic uint32_t g_qseq[QN];
static _Atomic uint32_t g_qhead, g_qtail;
static int g_efd = -1, g_inited;

void surf_init(void)
{
    for (uint32_t i = 0; i < QN; i++)
        atomic_store(&g_qseq[i], i);
    atomic_store(&g_qhead, 0);
    atomic_store(&g_qtail, 0);
#ifdef __linux__
    if (g_efd < 0)
        g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
    g_inited = 1;
}

int surf_event_fd(void) { return g_efd; }

static void push(int type, int a, int b, uint64_t t)
{
    if (!g_inited)
        return;
    uint32_t h = atomic_load_explicit(&g_qhead, memory_order_relaxed);
    for (;;) { /* Vyukov bounded queue */
        uint32_t s = atomic_load_explicit(&g_qseq[h % QN], memory_order_acquire);
        if (s == h) {
            if (atomic_compare_exchange_weak_explicit(&g_qhead, &h, h + 1, memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if ((int32_t)(s - h) < 0) {
            atomic_fetch_add_explicit(&g_lost, 1, memory_order_relaxed); /* full: the UI is stuck */
            return;
        } else {
            h = atomic_load_explicit(&g_qhead, memory_order_relaxed);
        }
    }
    surf_event *e = &g_q[h % QN];
    e->type = (uint8_t)type;
    e->a = (uint8_t)a;
    e->b = b;
    e->t_ns = t;
    atomic_store_explicit(&g_qseq[h % QN], h + 1, memory_order_release);
    if (g_efd >= 0) {
        uint64_t one = 1;
        if (write(g_efd, &one, sizeof one) < 0) {
            /* the counter is full: the UI wakes anyway */
        }
    }
}

int surf_next_event(surf_event *ev)
{
    uint32_t t = atomic_load_explicit(&g_qtail, memory_order_relaxed);
    uint32_t s = atomic_load_explicit(&g_qseq[t % QN], memory_order_acquire);
    if (s != t + 1)
        return 0;
    *ev = g_q[t % QN];
    atomic_store_explicit(&g_qseq[t % QN], t + QN, memory_order_release);
    atomic_store_explicit(&g_qtail, t + 1, memory_order_relaxed);
    return 1;
}

static int held(int note) { return (g_held[note >> 3] >> (note & 7)) & 1; }
static void set_held(int note, int on)
{
    if (on)
        g_held[note >> 3] |= (uint8_t)(1u << (note & 7));
    else
        g_held[note >> 3] &= (uint8_t)~(1u << (note & 7));
}

static void close_page(int by_button, uint64_t t)
{
    if (atomic_exchange(&g_open, 0))
        push(SEV_CLOSE, by_button, 0, t);
}

/* One whole channel message: 1 = the page takes it (the app never sees it). */
static int decide(uint8_t st, uint8_t d1, uint8_t d2, uint64_t t)
{
    int ch = st & 0x0f, type = st & 0xf0;
    if (atomic_exchange(&g_close_req, 0))
        close_page(0, t);
    if (ch == 9 && type == 0x90 && d2 > 0) { /* a pad: always to the app */
        push(SEV_PAD, d1, d2, t);
        return 0;
    }
    if (ch != 0)
        return 0;
    int open = atomic_load(&g_open);
    if (type == 0xb0) {
        if (d1 >= CC_K1 && d1 <= CC_K3) {
            atomic_store(&g_knob[d1 - CC_K1], d2);
            if (open)
                push(SEV_KNOB, d1 - CC_K1, d2, t);
            return open;
        }
        if (d1 == CC_ENCODER && open) {
            int delta = d2 > 64 ? (int)d2 - 128 : (int)d2;
            if (delta)
                push(SEV_ENCODER, 0, delta, t);
            return 1;
        }
        return 0;
    }
    if (type != 0x90 && type != 0x80)
        return 0;
    int press = type == 0x90 && d2 > 0;
    int note = d1 & 0x7f;
    if (!press) { /* a release: taken if its press was */
        if (note == N_SHIFT) {
            g_shift = 0;
            if (open)
                push(SEV_SHIFT, 0, 0, t);
        }
        if (held(note)) {
            set_held(note, 0);
            return 1;
        }
        return 0;
    }
    if (note == N_SHIFT) {
        g_shift = 1;
        if (open)
            push(SEV_SHIFT, 1, 0, t);
        return 0; /* the app sees SHIFT: harmless alone */
    }
    if (note == N_NOTE_REPEAT && g_shift) { /* the page's own button */
        if (open) {
            close_page(0, t);
        } else if (atomic_load(&g_ready)) {
            atomic_store(&g_open, 1);
            int kp = 0;
            for (int k = 0; k < 3; k++)
                kp |= (atomic_load(&g_knob[k]) + 1) << (8 * k);
            push(SEV_OPEN, 0, kp, t);
        } else {
            return 0; /* no screen for the page: stock SHIFT + NOTE REPEAT */
        }
        set_held(note, 1);
        return 1;
    }
    if (!open)
        return 0;
    if (note >= N_B1 && note <= N_B3) {
        push(SEV_BUTTON, note - N_B1, 0, t);
        set_held(note, 1);
        return 1;
    }
    if (note == N_PUSH || note == N_MINUS || note == N_PLUS) {
        if (note == N_PUSH)
            push(SEV_PUSH, 0, 0, t);
        else
            push(SEV_ENCODER, 0, note == N_PLUS ? 1 : -1, t);
        set_held(note, 1);
        return 1;
    }
    if (note == N_PLAY || note == N_STOP || note == N_SEQ_RECORD)
        return 0; /* play the sequence while editing */
    close_page(1, t); /* any other button: back to the MPC */
    return 0;
}

static int msg_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 2;
    default: return 3;
    }
}

size_t surf_filter(surf_stream *st, uint8_t *buf, size_t n, uint64_t t_ns)
{
    size_t out = 0, i = 0;
    while (i < n) {
        uint8_t b = buf[i];
        if (b >= 0xf8) { /* real-time: anywhere, always passes */
            buf[out++] = b;
            i++;
            continue;
        }
        if (st->in_sysex) {
            if (b & 0x80 && b != 0xf7) { /* a status ends a broken SysEx */
                st->in_sysex = 0;
                continue; /* handle this byte again, as a status */
            }
            if (st->sx_n < 4)
                st->sx_hdr[st->sx_n] = b;
            if (++st->sx_n == 4 && st->sx_hdr[0] == 0x47 && st->sx_hdr[1] == 0x7f &&
                st->sx_hdr[2] == 0x5c)
                st->akai_sysex = 1;
            if (b == 0xf7)
                st->in_sysex = 0;
            buf[out++] = b;
            i++;
            continue;
        }
        if (b == 0xf0) {
            st->in_sysex = 1;
            st->sx_n = 0;
            st->skip = 0;
            buf[out++] = b;
            i++;
            continue;
        }
        if (!(b & 0x80)) { /* data without a status here: rest of a split message */
            if (st->skip > 0)
                st->skip--;
            buf[out++] = b;
            i++;
            continue;
        }
        st->skip = 0;
        if (b >= 0xf0) { /* system common: pass */
            buf[out++] = b;
            i++;
            continue;
        }
        int len = msg_len(b);
        size_t j = i + 1;
        uint8_t d[2] = {0, 0};
        int got = 0;
        while (got < len - 1 && j < n) {
            if (buf[j] >= 0xf8) { /* real-time inside: let it out first */
                buf[out++] = buf[j++];
                continue;
            }
            if (buf[j] & 0x80)
                break; /* a new status: this message is cut short */
            d[got++] = buf[j++];
        }
        if (got < len - 1) { /* incomplete */
            buf[out++] = b;
            for (int k = 0; k < got; k++)
                buf[out++] = d[k];
            if (j >= n)
                st->skip = len - 1 - got; /* split across reads: the rest passes */
            i = j;
            continue;
        }
        int take = len == 3 && decide(b, d[0], d[1], t_ns);
        if (take) {
            atomic_fetch_add_explicit(&g_taken, 1, memory_order_relaxed);
        } else {
            buf[out++] = b;
            for (int k = 0; k < len - 1; k++)
                buf[out++] = d[k];
        }
        i = j;
    }
    return out;
}

uint64_t surf__taken(void);
uint64_t surf__taken(void) { return atomic_load(&g_taken); }
uint64_t surf__lost(void);
uint64_t surf__lost(void) { return atomic_load(&g_lost); }

#ifdef MPCFM_TEST
void surf_test_reset(void)
{
    atomic_store(&g_ready, 0);
    atomic_store(&g_open, 0);
    atomic_store(&g_close_req, 0);
    for (int k = 0; k < 3; k++)
        atomic_store(&g_knob[k], -1);
    g_shift = 0;
    memset(g_held, 0, sizeof g_held);
    surf_init();
}
#endif
