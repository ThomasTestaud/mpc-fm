/* FM as the sound source of a pad.
 *
 * The stock drum voice (class Voice) renders each of its layers through a
 * sample generator (class DefaultSampleGenerator), then filters, applies
 * the amp envelope and pans. App build 1dac7f48 (fw 1.3.0.12):
 *   vtable 0x6da6430 (typeinfo 0x6e84c30), 16 entries; we swap
 *     [0] render(gen, float *buf, int n, ...)  writes n mono frames;
 *         returns 1 while playing, 0 at the end. It jumps to the
 *         function pointer at gen+328; 0x2216f70 is the plain mono one
 *         (no time-stretch), the only one we replace;
 *     [3] stop       [7] start (note on)
 *   gen+88/+92/+96  gain ramp: current, target, step per frame
 *   gen+192         frames of silence before the sample starts
 *   gen+240         Sample object (its frames: found by discover.c)
 *   gen+368         read position, 32.32 fixed point
 * In 0x2216f70, out = interpolated sample x gain ramp; the voice does the
 * rest. So for an FM carrier we run the stock render (its loops, end and
 * ramp state stay stock), then overwrite buf with the FM engine x the same
 * gain ramp. The pitch is how far the stock position moved in this block
 * (tune, Keys mode, pitch envelope: all included). Then we set the
 * position to advance one frame per frame: a synth note does not get
 * shorter when it plays higher (v0.1.1 let the stock speed run).
 * Guards: a non-finite stock gain, or an FM output that is not finite or
 * absurdly loud, gives the block back to stock and drops the FM voice, so
 * a bad value never reaches the mix.
 *
 * The vtable is checked first; if anything differs, nothing is changed.
 * Audio path: no locks, no allocation; at note start only, a few
 * process_vm_readv calls (discover.c) to read the sample's first frames.
 * (The first attempt, v0.1.0, hooked Qbq::SamplerVoiceMemoryMode: the
 * drum pads never use it.)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "voice_hook.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "discover.h"
#include "fm/fm_carrier.h"
#include "fm/fm_engine.h"

#define OFF_RENDER_FN 328
#define OFF_GAIN 88
#define OFF_DELAY 192
#define OFF_SAMPLE 240
#define OFF_POS 368

#define VT_ENTRIES 16
#define SLOT_RENDER 0
#define SLOT_STOP 3
#define SLOT_START 7

#define SLOTS 64
#define SR 44100.0f
#define CHUNK 128
#define MAX_ABS 8.0f /* FM output above this is a bug: give back to stock */

/* Reader fields of the generator itself (from start: +0x168 .. +0x3b0). */
#define READER_LO 0x168
#define READER_HI 0x3b0

typedef struct {
    const char *build_id;
    uintptr_t vtable, typeinfo, method[VT_ENTRIES], plain_mono;
    uintptr_t image_size;
    disc_path_t frames_path; /* where a sample's first frames are, from the generator */
} layout_t;

static const layout_t LAYOUTS[] = {
    {"1dac7f481a0fe8d128e435ace88d7e6aece0c9c9", 0x6da6430, 0x6e84c30,
     {0x220d720, 0x21fdee0, 0x20c1430, 0x220ef30, 0x220d960, 0x220d660, 0x220d690, 0x220f490,
      0x220db70, 0x220db30, 0x20bfa80, 0x220d880, 0x220dee0, 0x220d730, 0x220d7a0, 0x220dd30},
     0x2216f70, 0x7300000,
     /* learned the same way in two device runs (v0.1.1, v0.1.2): the
      * generator's reader points at frame 0 at note start */
     {2, {528, 0}, 0, FM_FMT_F32}},
};

typedef struct {
    _Atomic(void *) owner; /* the DefaultSampleGenerator */
    fm_voice voice;
    fm_patch patch;
    float root_hz;
    double ratio;
} slot_t;

static slot_t g_slots[SLOTS];
static uintptr_t g_plain_mono, g_image_lo, g_image_hi;
vh_stats_t vh_stats;

/* Read by the trampolines (tramp_hooks.S): the stock methods, and the gates
 * (render/stop skip our code entirely while no FM voice exists). */
void *vh_orig_start, *vh_orig_stop, *vh_orig_render;
int vh_active;
int vh_gate_always = 1;

int vh_active_voices(void) { return __atomic_load_n(&vh_active, __ATOMIC_RELAXED); }

static inline uint64_t u64(void *o, int off) { uint64_t v; memcpy(&v, (char *)o + off, 8); return v; }
static inline void set_u64(void *o, int off, uint64_t v) { memcpy((char *)o + off, &v, 8); }

/* For the crash report: 1 while in our hook code, 2 while in the stock
 * method called from our hook. */
__thread int vh_in_hook __attribute__((tls_model("initial-exec")));

/* A discovered path must stay tied to the pad played: through its sample
 * object (+240, up to 5 hops) or its own reader fields (up to 2 hops). */
static int first_hop(int off)
{
    if (off == OFF_SAMPLE)
        return 5;
    if (off >= READER_LO && off < READER_HI)
        return 2;
    return 0;
}

static inline uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static slot_t *slot_find(void *gen)
{
    if (__atomic_load_n(&vh_active, __ATOMIC_RELAXED) <= 0)
        return NULL;
    for (int i = 0; i < SLOTS; i++)
        if (atomic_load_explicit(&g_slots[i].owner, memory_order_acquire) == gen)
            return &g_slots[i];
    return NULL;
}

static void slot_release(void *gen)
{
    slot_t *s = slot_find(gen);
    if (s) {
        atomic_store_explicit(&s->owner, NULL, memory_order_release);
        __atomic_fetch_sub(&vh_active, 1, __ATOMIC_RELAXED);
    }
}

static void claim(void *gen)
{
    uint64_t t0 = now_ns();
    fm_patch patch;
    int root = 60;
    int found = disc_lookup(gen, &patch, &root);
    uint64_t dt = now_ns() - t0;
    if (dt > atomic_load_explicit(&vh_stats.lookup_max_ns, memory_order_relaxed))
        atomic_store_explicit(&vh_stats.lookup_max_ns, dt, memory_order_relaxed);
    if (!found)
        return;
    for (int i = 0; i < SLOTS; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_slots[i].owner, &expect, gen)) {
            slot_t *s = &g_slots[i];
            s->patch = patch;
            s->root_hz = 440.0f * powf(2.0f, (float)(root - 69) / 12.0f);
            s->ratio = 1.0;
            fm_voice_init(&s->voice, SR);
            fm_voice_note_on(&s->voice, 1.0f); /* velocity: the stock gain carries it */
            __atomic_fetch_add(&vh_active, 1, __ATOMIC_RELAXED);
            atomic_fetch_add_explicit(&vh_stats.fm_starts, 1, memory_order_relaxed);
            atomic_store_explicit(&vh_stats.last_root, root, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&vh_stats.no_slot, 1, memory_order_relaxed);
}

/* ---- hooks: pre/post pairs, run by the trampolines around the stock
 * method (tramp_aarch64.inc). The stock method always gets exactly its own
 * arguments, stack arguments included. ---- */

typedef struct {
    uint64_t x[9]; /* x0..x8 as the caller passed them */
    uint64_t pad;
    unsigned char q[8][16];
} vh_regs;
typedef struct {
    uint64_t w[8];
} vh_ctx;

void vh_pre_start(vh_regs *r, vh_ctx *c)
{
    (void)r;
    (void)c;
    vh_in_hook = 2;
}

void vh_post_start(vh_regs *r, uint64_t ret, vh_ctx *c)
{
    (void)ret;
    (void)c;
    void *gen = (void *)r->x[0];
    vh_in_hook = 1;
    atomic_fetch_add_explicit(&vh_stats.starts, 1, memory_order_relaxed);
    slot_release(gen); /* a reused generator: forget its old note */
    claim(gen);
    vh_in_hook = 0;
}

void vh_pre_stop(vh_regs *r, vh_ctx *c)
{
    (void)r;
    (void)c;
    vh_in_hook = 2;
}

void vh_post_stop(vh_regs *r, uint64_t ret, vh_ctx *c)
{
    (void)ret;
    (void)c;
    vh_in_hook = 1;
    slot_release((void *)r->x[0]);
    vh_in_hook = 0;
}

static void drop(void *gen, _Atomic uint64_t *counter)
{
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
    slot_release(gen);
}

/* ctx: w[0] slot (0: not ours), w[1] delay, w[2] gain, w[3] target,
 * w[4] step, w[5] position before */
void vh_pre_render(vh_regs *r, vh_ctx *c)
{
    void *gen = (void *)r->x[0];
    c->w[0] = 0;
    vh_in_hook = 1;
    slot_t *s = slot_find(gen);
    if (s && u64(gen, OFF_RENDER_FN) != g_plain_mono) { /* stereo or time-stretch: stock */
        drop(gen, &vh_stats.unsupported);
        s = NULL;
    }
    if (s) {
        int delay;
        memcpy(&delay, (char *)gen + OFF_DELAY, 4);
        c->w[0] = (uint64_t)(uintptr_t)s;
        c->w[1] = (uint64_t)(int64_t)delay;
        memcpy(&c->w[2], (char *)gen + OFF_GAIN, 4);
        memcpy(&c->w[3], (char *)gen + OFF_GAIN + 4, 4);
        memcpy(&c->w[4], (char *)gen + OFF_GAIN + 8, 4);
        c->w[5] = u64(gen, OFF_POS);
    }
    vh_in_hook = 2;
}

void vh_post_render(vh_regs *r, uint64_t ret, vh_ctx *c)
{
    slot_t *s = (slot_t *)(uintptr_t)c->w[0];
    if (!s) {
        vh_in_hook = 0;
        return;
    }
    vh_in_hook = 1;
    void *gen = (void *)r->x[0];
    float *buf = (float *)r->x[1];
    int n = (int)r->x[2];
    int delay = (int)(int64_t)c->w[1];
    float g, target, step;
    memcpy(&g, &c->w[2], 4);
    memcpy(&target, &c->w[3], 4);
    memcpy(&step, &c->w[4], 4);
    uint64_t pos0 = c->w[5];

    int d = delay < 0 ? 0 : delay > n ? n : delay; /* the stock wrote zeros there */
    int m = n - d;
    int sane = isfinite(g) && isfinite(target) && isfinite(step) && fabsf(g) < 16.0f &&
               fabsf(target) < 16.0f && n > 0 && n <= 65536;
    if (!sane) {
        drop(gen, &vh_stats.bad_input); /* keep the stock output */
        vh_in_hook = 0;
        return;
    }
    if (m > 0 && buf) {
        uint64_t t0 = now_ns();
        uint64_t pos1 = u64(gen, OFF_POS);
        if (pos1 > pos0) {
            double ratio = (double)(pos1 - pos0) / ((double)m * 4294967296.0);
            if (ratio > 0.0 && ratio < 64.0)
                s->ratio = ratio;
        }
        float hz = (float)((double)s->root_hz * s->ratio);
        float y[CHUNK];
        int bad = 0;
        uint32_t gb, tb;
        memcpy(&tb, &target, 4);
        for (int done = 0; done < m && !bad;) {
            int len = m - done < CHUNK ? m - done : CHUNK;
            fm_voice_render(&s->voice, &s->patch, hz, y, len);
            for (int k = 0; k < len; k++)
                if (!(fabsf(y[k]) < MAX_ABS)) /* NaN fails this too */
                    bad = 1;
            if (bad)
                break;
            float *out = buf + d + done;
            for (int k = 0; k < len; k++) {
                memcpy(&gb, &g, 4);
                if (gb != tb) { /* the same ramp as the stock gain stage */
                    g += step;
                    g = step >= 0.0f ? (g < target ? g : target) : (g > target ? g : target);
                }
                out[k] = y[k] * g;
            }
            done += len;
        }
        if (bad) {
            /* Rare and a bug: this block may be half FM; make it silent
             * rather than wrong, and give the layer back to stock. */
            memset(buf + d, 0, (size_t)m * sizeof(float));
            drop(gen, &vh_stats.bad_output);
            vh_in_hook = 0;
            return;
        }
        /* One frame per frame from now on, whatever the pitch. */
        if (ret & 0xff)
            set_u64(gen, OFF_POS, pos0 + ((uint64_t)m << 32));
        uint64_t dt = now_ns() - t0;
        atomic_fetch_add_explicit(&vh_stats.fm_ns, dt, memory_order_relaxed);
        atomic_fetch_add_explicit(&vh_stats.fm_blocks, 1, memory_order_relaxed);
        if (dt > atomic_load_explicit(&vh_stats.fm_max_ns, memory_order_relaxed))
            atomic_store_explicit(&vh_stats.fm_max_ns, dt, memory_order_relaxed);
        atomic_store_explicit(&vh_stats.last_ratio_milli, (uint32_t)(s->ratio * 1000.0),
                              memory_order_relaxed);
    }
    if (!(ret & 0xff))
        slot_release(gen); /* the stock layer has ended */
    vh_in_hook = 0;
}

/* The functions that go into the vtable. On the device: the assembly
 * trampolines. On the computer (tests): the same pre/post around a plain
 * call, enough for fakes that take only register arguments. */
#if defined(__aarch64__) && !defined(MPCFM_TEST)
extern char vh_tramp_render[], vh_tramp_start[], vh_tramp_stop[];
#define TRAMP_RENDER ((uintptr_t)vh_tramp_render)
#define TRAMP_START ((uintptr_t)vh_tramp_start)
#define TRAMP_STOP ((uintptr_t)vh_tramp_stop)
#else
static uintptr_t emulate(void *orig, int gated, void (*pre)(vh_regs *, vh_ctx *),
                         void (*post)(vh_regs *, uint64_t, vh_ctx *), uintptr_t a0, uintptr_t a1,
                         uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                         uintptr_t a7)
{
    if (gated && !__atomic_load_n(&vh_active, __ATOMIC_RELAXED))
        return ((vh_fn)orig)((void *)a0, a1, a2, a3, a4, a5, a6, a7);
    vh_regs r = {{a0, a1, a2, a3, a4, a5, a6, a7, 0}, 0, {{0}}};
    vh_ctx c = {{0}};
    pre(&r, &c);
    uintptr_t ret = ((vh_fn)orig)((void *)a0, a1, a2, a3, a4, a5, a6, a7);
    post(&r, ret, &c);
    return ret;
}
static uintptr_t e_render(void *g, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                          uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    return emulate(vh_orig_render, 1, vh_pre_render, vh_post_render, (uintptr_t)g, a1, a2, a3,
                   a4, a5, a6, a7);
}
static uintptr_t e_start(void *g, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                         uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    return emulate(vh_orig_start, 0, vh_pre_start, vh_post_start, (uintptr_t)g, a1, a2, a3, a4,
                   a5, a6, a7);
}
static uintptr_t e_stop(void *g, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                        uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    return emulate(vh_orig_stop, 1, vh_pre_stop, vh_post_stop, (uintptr_t)g, a1, a2, a3, a4, a5,
                   a6, a7);
}
#define TRAMP_RENDER ((uintptr_t)e_render)
#define TRAMP_START ((uintptr_t)e_start)
#define TRAMP_STOP ((uintptr_t)e_stop)
#endif

int vh_install(uintptr_t base, const char *build_id, char *msg, size_t cap)
{
    const layout_t *L = NULL;
    for (size_t i = 0; i < sizeof LAYOUTS / sizeof *LAYOUTS; i++)
        if (build_id && strcmp(build_id, LAYOUTS[i].build_id) == 0)
            L = &LAYOUTS[i];
    if (!L) {
        snprintf(msg, cap, "unknown app build %s", build_id ? build_id : "?");
        return 0;
    }
    uintptr_t *vt = (uintptr_t *)(base + L->vtable);
    if (vt[-1] != base + L->typeinfo) {
        snprintf(msg, cap, "vtable: typeinfo is %#lx, not %#lx", (unsigned long)(vt[-1] - base),
                 (unsigned long)L->typeinfo);
        return 0;
    }
    for (int i = 0; i < VT_ENTRIES; i++)
        if (vt[i] != base + L->method[i]) {
            snprintf(msg, cap, "vtable entry %d is %#lx, not stock %#lx (hooked by someone else?)",
                     i, (unsigned long)(vt[i] - base), (unsigned long)L->method[i]);
            return 0;
        }
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t)&vt[0] & ~(uintptr_t)(page - 1);
    uintptr_t hi = ((uintptr_t)&vt[VT_ENTRIES] + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE) != 0) {
        snprintf(msg, cap, "mprotect failed");
        return 0;
    }
    vh_orig_render = (void *)vt[SLOT_RENDER];
    vh_orig_stop = (void *)vt[SLOT_STOP];
    vh_orig_start = (void *)vt[SLOT_START];
    g_plain_mono = base + L->plain_mono;
    g_image_lo = base;
    g_image_hi = base + L->image_size;
    disc_set_first_hop(first_hop);
    disc_seed(&L->frames_path);
    __atomic_store_n(&vt[SLOT_RENDER], TRAMP_RENDER, __ATOMIC_RELEASE);
    __atomic_store_n(&vt[SLOT_STOP], TRAMP_STOP, __ATOMIC_RELEASE);
    __atomic_store_n(&vt[SLOT_START], TRAMP_START, __ATOMIC_RELEASE);
    mprotect((void *)lo, hi - lo, PROT_READ);
    snprintf(msg, cap, "DefaultSampleGenerator hooks on (vtable %#lx, trampolines)",
             (unsigned long)L->vtable);
    return 1;
}

int vh_poll(char *msg, size_t cap)
{
    return disc_poll(g_image_lo, g_image_hi, OFF_SAMPLE, msg, cap);
}

#ifdef MPCFM_TEST
void vh_test_setup(vh_fn start, vh_fn stop, vh_fn render, uintptr_t plain_mono)
{
    vh_orig_start = (void *)start;
    vh_orig_stop = (void *)stop;
    vh_orig_render = (void *)render;
    g_plain_mono = plain_mono;
    g_image_lo = g_image_hi = 0;
    disc_set_first_hop(first_hop);
    disc_set_throttle(0);
    memset(g_slots, 0, sizeof g_slots);
    memset(&vh_stats, 0, sizeof vh_stats);
    vh_active = 0;
}
uintptr_t vh_test_start(void *gen) { return e_start(gen, 0, 0, 0, 0, 0, 0, 0); }
uintptr_t vh_test_stop(void *gen) { return e_stop(gen, 0, 0, 0, 0, 0, 0, 0); }
uintptr_t vh_test_render(void *gen, float *buf, int n)
{
    return e_render(gen, (uintptr_t)buf, (uintptr_t)n, 0, 0, 0, 0, 0);
}
#endif
