#define _GNU_SOURCE
/* Host tests of the voice hook wrappers, with a fake sample voice object
 * that has the stock layout and fake stock methods. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fm/fm_carrier.h"
#include "fm/fm_engine.h"
#include "fm/fm_presets.h"
#include "voice_hook.h"

static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); if (!(c)) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

#define N 128
#define FRAMES 44100
static float sample[FRAMES];

/* fake object with the stock field offsets */
typedef struct { unsigned char b[0xc0]; long pos; } obj_t;
static void setf(obj_t *o, int off, float v) { memcpy(o->b + off, &v, 4); }
static float getf(obj_t *o, int off) { float v; memcpy(&v, o->b + off, 4); return v; }
static void setd(obj_t *o, int off, double v) { memcpy(o->b + off, &v, 8); }

static int stock_calls;
static float stock_gains_seen[2];
/* fake stock process: out += gain * sample, 1 frame per frame, 0 at the end */
static uintptr_t fake_process(void *self, uintptr_t a_out, uintptr_t a_start, uintptr_t a_n)
{
    obj_t *o = self;
    float **out = (float **)a_out;
    float gl = getf(o, 72), gr = getf(o, 76);
    stock_gains_seen[0] = gl;
    stock_gains_seen[1] = gr;
    stock_calls++;
    int end; memcpy(&end, o->b + 0x88, 4);
    for (int k = 0; k < (int)a_n; k++) {
        if (o->pos >= end) return 0;
        float v = sample[o->pos++];
        out[0][a_start + k] += gl * v;
        out[1][a_start + k] += gr * v;
    }
    return o->pos < end;
}
static uintptr_t fake_start(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    (void)a1; (void)a2; (void)a3;
    obj_t *o = self;
    o->pos = 0;
    const float *d = sample;
    memcpy(o->b + 0x80, &d, sizeof d);
    int end = FRAMES; memcpy(o->b + 0x88, &end, 4);
    o->b[0x68] = 0;
    return 1;
}
static uintptr_t fake_stop(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    (void)self; (void)a1; (void)a2; (void)a3;
    return 0;
}

static void new_obj(obj_t *o, float gl, float gr, double ratio)
{
    memset(o, 0, sizeof *o);
    setf(o, 72, gl);
    setf(o, 76, gr);
    setd(o, 48, ratio);
}

static int crossings(const float *x, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) c += (x[i - 1] < 0) != (x[i] < 0);
    return c;
}

/* fill `sample` with a quiet tone, as int16 / 32768; with or without a carrier header */
static void make_sample(int carrier, int damage)
{
    static int16_t pcm[FRAMES];
    for (int i = 0; i < FRAMES; i++)
        pcm[i] = (int16_t)lrintf(3000.0f * sinf(6.2831853f * 100.0f * i / 44100.0f));
    if (carrier) {
        fm_patch p;
        fm_preset_get(fm_preset_find("E.Piano"), &p);
        fm_carrier_encode(&p, 60, pcm, FRAMES);
        if (damage)
            pcm[100] ^= 1;
    }
    for (int i = 0; i < FRAMES; i++)
        sample[i] = pcm[i] / 32768.0f;
}

static void run(obj_t *o, float *L, float *R, int blocks)
{
    for (int b = 0; b < blocks; b++) {
        float *out[2] = {L + b * N, R + b * N};
        vh_test_process(o, out, 0, N);
    }
}

int main(void)
{
    static float L[N * 400], R[N * 400];
    obj_t o;

    /* a normal sample: the hook is transparent */
    vh_test_setup(fake_start, fake_stop, fake_process);
    make_sample(0, 0);
    new_obj(&o, 0.5f, 0.25f, 1.0);
    vh_test_start(&o, 0);
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    run(&o, L, R, 10);
    double d = 0;
    for (int i = 0; i < 10 * N; i++) {
        d = fmax(d, fabs(L[i] - 0.5f * sample[i]));
        d = fmax(d, fabs(R[i] - 0.25f * sample[i]));
    }
    CHECK(d == 0.0 && vh_stats.fm_starts == 0, "normal sample: stock output unchanged (diff %.1e)", d);

    /* an FM carrier: FM replaces the sample, with the stock gains */
    vh_test_setup(fake_start, fake_stop, fake_process);
    make_sample(1, 0);
    new_obj(&o, 0.5f, 0.25f, 1.0);
    vh_test_start(&o, 0);
    CHECK(vh_stats.fm_starts == 1 && vh_stats.active == 1, "carrier detected at start (root %d)", vh_stats.last_root);
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    run(&o, L, R, 300); /* the fake sample lasts 344.5 blocks: still sounding */
    CHECK(stock_gains_seen[0] == 0.0f && stock_gains_seen[1] == 0.0f, "stock reader ran with gains 0");
    CHECK(getf(&o, 72) == 0.5f && getf(&o, 76) == 0.25f, "gains restored after each call");
    fm_patch p; fm_preset_get(fm_preset_find("E.Piano"), &p);
    fm_voice v; fm_voice_init(&v, 44100.0f); fm_voice_note_on(&v, 1.0f);
    static float ref[N * 300];
    for (int b = 0; b < 300; b++) fm_voice_render(&v, &p, 440.0f * powf(2.0f, -9.0f / 12.0f), ref + b * N, N);
    d = 0;
    for (int i = 0; i < 300 * N; i++) {
        d = fmax(d, fabs(L[i] - 0.5f * ref[i]));
        d = fmax(d, fabs(R[i] - 0.25f * ref[i]));
    }
    CHECK(d < 1e-6, "output = FM engine x stock gains (diff %.1e)", d);

    /* pitch follows the stock increment (tune, Keys mode) */
    vh_test_setup(fake_start, fake_stop, fake_process);
    fm_patch sine; fm_preset_get(fm_preset_find("Sine"), &sine);
    static int16_t pcm[FRAMES];
    memset(pcm, 0, sizeof pcm);
    fm_carrier_encode(&sine, 69, pcm, FRAMES); /* root A4 = 440 Hz */
    for (int i = 0; i < FRAMES; i++) sample[i] = pcm[i] / 32768.0f;
    new_obj(&o, 1.0f, 1.0f, 2.0); /* one octave up */
    vh_test_start(&o, 0);
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    run(&o, L, R, 345);
    int c = crossings(L + 441, 44100 - 441);
    CHECK(abs(c - 2 * 880 * 99 / 100) <= 4, "ratio 2 on root A4: %d crossings in 0.99 s (880 Hz)", c);

    /* stock fade-out ramp is applied to FM too */
    vh_test_setup(fake_start, fake_stop, fake_process);
    new_obj(&o, 1.0f, 1.0f, 1.0);
    vh_test_start(&o, 0);
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    run(&o, L, R, 20);
    setf(&o, 88, 1.0f);
    setf(&o, 92, 1.0f / 64.0f); /* fade over 64 frames */
    float *out2[2] = {L + 20 * N, R + 20 * N};
    vh_test_process(&o, out2, 0, N);
    float tail = 0; for (int k = 64; k < N; k++) tail = fmaxf(tail, fabsf(L[20 * N + k]));
    CHECK(tail == 0.0f && fabsf(L[20 * N + 1]) > 0.0f, "fade-out ramp: silent after 64 frames");

    /* the voice ends when stock process returns 0, or at stop */
    vh_test_setup(fake_start, fake_stop, fake_process);
    new_obj(&o, 1.0f, 1.0f, 1.0);
    vh_test_start(&o, 0);
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    run(&o, L, R, 400); /* the fake sample ends after 344.5 blocks */
    CHECK(vh_stats.active == 0, "FM voice freed when the sample ends");
    vh_test_start(&o, 0);
    vh_test_stop(&o);
    CHECK(vh_stats.active == 0, "FM voice freed at stop");

    /* a damaged header: no FM, counted */
    vh_test_setup(fake_start, fake_stop, fake_process);
    make_sample(1, 1);
    new_obj(&o, 1.0f, 1.0f, 1.0);
    vh_test_start(&o, 0);
    CHECK(vh_stats.fm_starts == 0 && vh_stats.bad_headers == 1, "damaged header: plays as a sample, counted");

    /* vh_install: checks the stock vtable, swaps 3 entries, keeps it read-only */
    const uintptr_t VT = 0x6dd6098, TI = 0x715dab8;
    const uintptr_t M[5] = {0x4477cf0, 0x4477d20, 0x44780b0, 0x4477e20, 0x44785f0};
    size_t span = 0x7200000;
    char *base = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uintptr_t *vt = (uintptr_t *)(base + VT);
    vt[-1] = (uintptr_t)base + TI;
    for (int i = 0; i < 5; i++) vt[i] = (uintptr_t)base + M[i];
    char page_note[8];
    (void)page_note;
    mprotect((void *)((uintptr_t)vt & ~(uintptr_t)4095), 8192, PROT_READ);
    char msg[160];
    const char *bid = "1dac7f481a0fe8d128e435ace88d7e6aece0c9c9";
    CHECK(vh_install((uintptr_t)base, "0000", msg, sizeof msg) == 0, "unknown build: no hooks (%s)", msg);
    int ok = vh_install((uintptr_t)base, bid, msg, sizeof msg);
    int swapped = vt[2] != (uintptr_t)base + M[2] && vt[3] != (uintptr_t)base + M[3] &&
                  vt[4] != (uintptr_t)base + M[4] && vt[0] == (uintptr_t)base + M[0];
    CHECK(ok && swapped, "stock vtable: entries 2-4 swapped, 0-1 kept (%s)", msg);
    CHECK(vh_install((uintptr_t)base, bid, msg, sizeof msg) == 0, "second install refused: %s", msg);
    munmap(base, span);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
