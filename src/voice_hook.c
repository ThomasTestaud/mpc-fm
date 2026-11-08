/* FM as the sound source of a pad.
 *
 * The stock engine plays a sample through Qbq::SamplerVoiceMemoryMode (the
 * sample is in RAM). Its virtual methods (app build 1dac7f48, fw 1.3.0.12):
 *   [2] stop     0x44780b0  frees the sample buffers
 *   [3] start    0x4477e20  note start: position, loop, data pointer
 *   [4] process  0x44785f0  process(out, start, n): ADDS n interpolated frames
 *                           into out[0]/out[1] from `start`, times the gains
 *                           at +72/+76; returns 0 when the sample has ended
 * Object fields we use (the object is 0xc0 bytes):
 *   +48 double pitch increment for this block (1.0 = original pitch)
 *   +72 / +76 float left / right gain   +84 u8 fade done
 *   +88 float fade level   +92 float fade step per frame (0: no fade)
 *   +0x68 u8 stereo sample   +0x80 float* frames   +0x88 int end frame
 *
 * We swap 3 vtable entries (no code patching). For a sample whose data
 * starts with an FM carrier header (fm_carrier.h):
 *   start:   claim an FM voice for this object, with the patch from the
 *            header;
 *   process: run the stock reader with gains 0 (so its position, loops,
 *            end of sample and fades stay stock), then add the FM output
 *            with the stock gains and fade; pitch = root note x increment;
 *   stop, or process returning 0: free the FM voice.
 * Every other sample goes straight to the stock methods.
 *
 * The vtable entries are checked against the stock values first: if they
 * differ (other firmware, or another mod hooked them), nothing is changed.
 * Audio path: no locks, no allocation, no syscalls (clock_gettime is vDSO).
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

#include "fm/fm_carrier.h"
#include "fm/fm_engine.h"

#define OFF_INC 48
#define OFF_GAIN_L 72
#define OFF_GAIN_R 76
#define OFF_FADE_DONE 84
#define OFF_FADE 88
#define OFF_FADE_STEP 92
#define OFF_STEREO 0x68
#define OFF_DATA 0x80
#define OFF_END 0x88

#define SLOTS 64
#define SR 44100.0f
#define CHUNK 128

typedef uintptr_t (*vh_fn)(void *, uintptr_t, uintptr_t, uintptr_t);

/* Stock addresses (link-time, add the load base) per app build. */
typedef struct {
    const char *build_id;
    uintptr_t vtable, typeinfo;
    uintptr_t method[5]; /* dtor, deleting dtor, stop, start, process */
} layout_t;

static const layout_t LAYOUTS[] = {
    {"1dac7f481a0fe8d128e435ace88d7e6aece0c9c9", 0x6dd6098, 0x715dab8,
     {0x4477cf0, 0x4477d20, 0x44780b0, 0x4477e20, 0x44785f0}},
};

typedef struct {
    _Atomic(void *) owner; /* the SamplerVoiceMemoryMode object */
    fm_voice voice;
    fm_patch patch;
    float root_hz;
} slot_t;

static slot_t g_slots[SLOTS];
static vh_fn g_start, g_stop, g_process;
vh_stats_t vh_stats;

static inline float f32(void *o, int off) { float v; memcpy(&v, (char *)o + off, 4); return v; }
static inline void set_f32(void *o, int off, float v) { memcpy((char *)o + off, &v, 4); }
static inline double f64(void *o, int off) { double v; memcpy(&v, (char *)o + off, 8); return v; }

static inline uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static slot_t *slot_find(void *self)
{
    if (atomic_load_explicit(&vh_stats.active, memory_order_relaxed) <= 0)
        return NULL;
    for (int i = 0; i < SLOTS; i++)
        if (atomic_load_explicit(&g_slots[i].owner, memory_order_acquire) == self)
            return &g_slots[i];
    return NULL;
}

static void slot_release(void *self)
{
    slot_t *s = slot_find(self);
    if (s) {
        atomic_store_explicit(&s->owner, NULL, memory_order_release);
        atomic_fetch_sub_explicit(&vh_stats.active, 1, memory_order_relaxed);
    }
}

/* After a stock start: is this sample an FM carrier? */
static void claim(void *self)
{
    if (*((unsigned char *)self + OFF_STEREO) & 1)
        return; /* carriers are mono */
    const float *data;
    memcpy(&data, (char *)self + OFF_DATA, sizeof data);
    int end;
    memcpy(&end, (char *)self + OFF_END, 4);
    if (!data || end < FM_CARRIER_FRAMES)
        return;
    for (int i = 0; i < 8; i++) /* for the status file: what the app really holds */
        atomic_store_explicit(&vh_stats.last_first[i], (int)lrintf(data[i] * 32768.0f),
                              memory_order_relaxed);
    if (!fm_carrier_has_magic(data, end))
        return;
    fm_patch patch;
    int root = 60;
    int r = fm_carrier_decode(data, end, &patch, &root);
    if (r != 1) {
        atomic_fetch_add_explicit(&vh_stats.bad_headers, 1, memory_order_relaxed);
        return;
    }
    for (int i = 0; i < SLOTS; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_slots[i].owner, &expect, self)) {
            slot_t *s = &g_slots[i];
            s->patch = patch;
            s->root_hz = 440.0f * powf(2.0f, (float)(root - 69) / 12.0f);
            fm_voice_init(&s->voice, SR);
            fm_voice_note_on(&s->voice, 1.0f); /* velocity: the stock gains carry it */
            atomic_fetch_add_explicit(&vh_stats.active, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&vh_stats.fm_starts, 1, memory_order_relaxed);
            atomic_store_explicit(&vh_stats.last_root, root, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&vh_stats.no_slot, 1, memory_order_relaxed);
}

static uintptr_t w_start(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    slot_release(self); /* a reused voice object: forget its old note */
    uintptr_t r = g_start(self, a1, a2, a3);
    atomic_fetch_add_explicit(&vh_stats.starts, 1, memory_order_relaxed);
    if (r & 0xff)
        claim(self);
    return r;
}

static uintptr_t w_stop(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    slot_release(self);
    return g_stop(self, a1, a2, a3);
}

static uintptr_t w_process(void *self, uintptr_t a_out, uintptr_t a_start, uintptr_t a_n)
{
    slot_t *s = slot_find(self);
    if (!s)
        return g_process(self, a_out, a_start, a_n);

    float **out = (float **)a_out;
    int start = (int)a_start, n = (int)a_n;
    float gl = f32(self, OFF_GAIN_L), gr = f32(self, OFF_GAIN_R);
    float fade = f32(self, OFF_FADE), step = f32(self, OFF_FADE_STEP);
    int fading = step != 0.0f && !(*((unsigned char *)self + OFF_FADE_DONE) & 1);

    /* The stock reader keeps its position, loops and end, but adds nothing. */
    set_f32(self, OFF_GAIN_L, 0.0f);
    set_f32(self, OFF_GAIN_R, 0.0f);
    uintptr_t r = g_process(self, a_out, a_start, a_n);
    set_f32(self, OFF_GAIN_L, gl);
    set_f32(self, OFF_GAIN_R, gr);

    if (n > 0 && out && out[0]) {
        uint64_t t0 = now_ns();
        double ratio = f64(self, OFF_INC);
        float hz = (float)((double)s->root_hz * ratio);
        float y[CHUNK];
        for (int done = 0; done < n;) {
            int len = n - done < CHUNK ? n - done : CHUNK;
            fm_voice_render(&s->voice, &s->patch, hz, y, len);
            float *L = out[0] + start + done, *R = out[1] ? out[1] + start + done : NULL;
            for (int k = 0; k < len; k++) {
                float v = y[k];
                if (fading) { /* same ramp as the stock fade-out */
                    fade -= step;
                    v *= fade > 0.0f ? fade : 0.0f;
                }
                L[k] += gl * v;
                if (R)
                    R[k] += gr * v;
            }
            done += len;
        }
        uint64_t dt = now_ns() - t0;
        atomic_fetch_add_explicit(&vh_stats.fm_ns, dt, memory_order_relaxed);
        atomic_fetch_add_explicit(&vh_stats.fm_blocks, 1, memory_order_relaxed);
        if (dt > atomic_load_explicit(&vh_stats.fm_max_ns, memory_order_relaxed))
            atomic_store_explicit(&vh_stats.fm_max_ns, dt, memory_order_relaxed);
        if (ratio > 0.0 && ratio < 4e6)
            atomic_store_explicit(&vh_stats.last_ratio_milli, (uint32_t)(ratio * 1000.0),
                                  memory_order_relaxed);
    }
    if (!(r & 0xff))
        slot_release(self); /* the stock voice has ended */
    return r;
}

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
    for (int i = 0; i < 5; i++)
        if (vt[i] != base + L->method[i]) {
            snprintf(msg, cap, "vtable entry %d is %#lx, not stock %#lx (hooked by someone else?)",
                     i, (unsigned long)(vt[i] - base), (unsigned long)L->method[i]);
            return 0;
        }
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t)&vt[2] & ~(uintptr_t)(page - 1);
    uintptr_t hi = ((uintptr_t)&vt[5] + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE) != 0) {
        snprintf(msg, cap, "mprotect failed");
        return 0;
    }
    g_stop = (vh_fn)vt[2];
    g_start = (vh_fn)vt[3];
    g_process = (vh_fn)vt[4];
    __atomic_store_n(&vt[2], (uintptr_t)w_stop, __ATOMIC_RELEASE);
    __atomic_store_n(&vt[3], (uintptr_t)w_start, __ATOMIC_RELEASE);
    __atomic_store_n(&vt[4], (uintptr_t)w_process, __ATOMIC_RELEASE);
    mprotect((void *)lo, hi - lo, PROT_READ);
    snprintf(msg, cap, "SamplerVoiceMemoryMode hooks on (vtable %#lx)", (unsigned long)L->vtable);
    return 1;
}

#ifdef MPCFM_TEST
void vh_test_setup(vh_fn start, vh_fn stop, vh_fn process)
{
    g_start = start;
    g_stop = stop;
    g_process = process;
    memset(g_slots, 0, sizeof g_slots);
    memset(&vh_stats, 0, sizeof vh_stats);
}
uintptr_t vh_test_start(void *self, uintptr_t a1) { return w_start(self, a1, 0, 0); }
uintptr_t vh_test_stop(void *self) { return w_stop(self, 0, 0, 0); }
uintptr_t vh_test_process(void *self, float **out, int start, int n)
{
    return w_process(self, (uintptr_t)out, (uintptr_t)start, (uintptr_t)n);
}
#endif
