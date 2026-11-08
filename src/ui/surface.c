/* The rawmidi side of the control surface (see surface.h): fm.so defines
 * the rawmidi input functions the app uses, finds which input is the
 * surface, and runs surface_filter.c on what the app reads from it.
 * Everything else passes unchanged. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>

#include "surface.h"

#define EXPORT __attribute__((visibility("default")))

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_close)(snd_rawmidi_t *);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);
static ssize_t (*real_tread)(snd_rawmidi_t *, struct timespec *, void *, size_t);
static int (*real_info)(snd_rawmidi_t *, snd_rawmidi_info_t *);
static size_t (*real_info_sizeof)(void);
static const char *(*real_info_name)(const snd_rawmidi_info_t *);
static const char *(*real_info_subname)(const snd_rawmidi_info_t *);
static int (*real_info_card)(const snd_rawmidi_info_t *);

#define RESOLVE(var, name)                                                  \
    do {                                                                    \
        if (!(var))                                                         \
            *(void **)&(var) = dlsym(RTLD_NEXT, name);                      \
    } while (0)

#define PORTS 8
typedef struct {
    _Atomic(snd_rawmidi_t *) h;
    int surface;      /* by its name, or by an AC50 SysEx on it */
    surf_stream st;
    char desc[112];
} port_t;
static port_t g_ports[PORTS];
static _Atomic int g_nports;
static int g_enabled;
static _Atomic uint64_t g_bytes;

void surf_enable(int on);
void surf_enable(int on) { g_enabled = on; }

static port_t *port_of(snd_rawmidi_t *h)
{
    int n = atomic_load(&g_nports);
    for (int i = 0; i < n && i < PORTS; i++)
        if (atomic_load_explicit(&g_ports[i].h, memory_order_acquire) == h)
            return &g_ports[i];
    return NULL;
}

static void add_port(snd_rawmidi_t *h, const char *open_name, int mode)
{
    char iname[64] = "?", sub[64] = "";
    int card = -1;
    RESOLVE(real_info, "snd_rawmidi_info");
    RESOLVE(real_info_sizeof, "snd_rawmidi_info_sizeof");
    RESOLVE(real_info_name, "snd_rawmidi_info_get_name");
    RESOLVE(real_info_subname, "snd_rawmidi_info_get_subdevice_name");
    RESOLVE(real_info_card, "snd_rawmidi_info_get_card");
    if (real_info && real_info_sizeof && real_info_name) {
        size_t sz = real_info_sizeof();
        if (sz > 0 && sz <= 4096) {
            unsigned char mem[4096] __attribute__((aligned(16)));
            memset(mem, 0, sz);
            snd_rawmidi_info_t *info = (snd_rawmidi_info_t *)mem;
            if (real_info(h, info) == 0) {
                const char *n = real_info_name(info);
                snprintf(iname, sizeof iname, "%s", n ? n : "?");
                if (real_info_subname) {
                    const char *s = real_info_subname(info);
                    snprintf(sub, sizeof sub, "%s", s ? s : "");
                }
                if (real_info_card)
                    card = real_info_card(info);
            }
        }
    }
    port_t *p = port_of(h);
    if (!p) {
        int i = atomic_load(&g_nports);
        for (int k = 0; k < i && k < PORTS; k++) /* a closed one */
            if (!atomic_load(&g_ports[k].h)) {
                p = &g_ports[k];
                break;
            }
        if (!p) {
            if (i >= PORTS)
                return;
            p = &g_ports[i];
            atomic_store(&g_nports, i + 1);
        }
    }
    memset(&p->st, 0, sizeof p->st);
    p->surface = strcasestr(iname, "surface") != NULL || strcasestr(sub, "surface") != NULL;
    snprintf(p->desc, sizeof p->desc, "\"%s\" (%s%s%s) card %d mode %d", open_name ? open_name : "?",
             iname, sub[0] ? " / " : "", sub, card, mode);
    atomic_store_explicit(&p->h, h, memory_order_release);
}

int surf_port_info(int i, char *out, size_t cap)
{
    if (i < 0 || i >= atomic_load(&g_nports) || i >= PORTS)
        return 0;
    port_t *p = &g_ports[i];
    snprintf(out, cap, "%s: %s%s", p->desc, atomic_load(&p->h) ? "" : "closed, ",
             p->surface ? (p->st.akai_sysex ? "surface (name and SysEx)" : "surface (by name)")
                        : "not the surface");
    return 1;
}

uint64_t surf__taken(void);
uint64_t surf__lost(void);
void surf_counts(uint64_t *bytes, uint64_t *taken, uint64_t *events_lost)
{
    *bytes = atomic_load(&g_bytes);
    *taken = surf__taken();
    *events_lost = surf__lost();
}

EXPORT int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode)
{
    RESOLVE(real_open, "snd_rawmidi_open");
    if (!real_open)
        return -ENOSYS;
    int r = real_open(in, out, name, mode);
    if (r >= 0 && in && *in)
        add_port(*in, name, mode);
    return r;
}

EXPORT int snd_rawmidi_close(snd_rawmidi_t *h)
{
    RESOLVE(real_close, "snd_rawmidi_close");
    port_t *p = port_of(h);
    if (p)
        atomic_store(&p->h, NULL);
    return real_close ? real_close(h) : -ENOSYS;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Not known as the surface yet: does an AC50 SysEx header pass here? */
static void sniff(port_t *p, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == 0xf0 && b[i + 1] == 0x47 && b[i + 2] == 0x7f && b[i + 3] == 0x5c) {
            p->surface = 1;
            p->st.akai_sysex = 1;
            return;
        }
}

/* Bytes kept from a read of r bytes (r > 0). */
static ssize_t filter(port_t *p, void *buf, ssize_t r)
{
    if (!p->surface) {
        sniff(p, buf, (size_t)r);
        return r; /* the bytes that made it the surface pass whole */
    }
    atomic_fetch_add_explicit(&g_bytes, (uint64_t)r, memory_order_relaxed);
    if (!g_enabled)
        return r;
    return (ssize_t)surf_filter(&p->st, buf, (size_t)r, now_ns());
}

/* A read the page took whole: read again. The app's port is non-blocking,
 * so that ends with -EAGAIN as a stock read would; a blocking port waits
 * for the next bytes, as stock. */
EXPORT ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size)
{
    RESOLVE(real_read, "snd_rawmidi_read");
    if (!real_read)
        return -ENOSYS;
    port_t *p = port_of(h);
    for (;;) {
        ssize_t r = real_read(h, buf, size);
        if (r <= 0 || !p || !buf)
            return r;
        ssize_t k = filter(p, buf, r);
        if (k > 0)
            return k;
    }
}

EXPORT ssize_t snd_rawmidi_tread(snd_rawmidi_t *h, struct timespec *ts, void *buf, size_t size)
{
    RESOLVE(real_tread, "snd_rawmidi_tread");
    if (!real_tread)
        return -ENOSYS;
    port_t *p = port_of(h);
    for (;;) {
        ssize_t r = real_tread(h, ts, buf, size);
        if (r <= 0 || !p || !buf)
            return r;
        ssize_t k = filter(p, buf, r);
        if (k > 0)
            return k;
    }
}
