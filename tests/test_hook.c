#define _GNU_SOURCE
/* Host tests of the generator hook and of discovery, with a fake
 * DefaultSampleGenerator that has the stock field layout, fake stock
 * methods, and a fake Sample -> provider -> holder -> frames chain. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "discover.h"
#include "fm/fm_carrier.h"
#include "fm/fm_engine.h"
#include "fm/fm_presets.h"
#include "voice_hook.h"

static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); if (!(c)) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

#define N 64
#define FRAMES 44100
#define PLAIN 0x2216f70u   /* stands for the plain mono render function */

typedef struct { unsigned char b[0x400]; } gen_t;
static void setf(gen_t *g, int off, float v) { memcpy(g->b + off, &v, 4); }
static float getf(gen_t *g, int off) { float v; memcpy(&v, g->b + off, 4); return v; }
static void setu64(gen_t *g, int off, uint64_t v) { memcpy(g->b + off, &v, 8); }
static uint64_t getu64(gen_t *g, int off) { uint64_t v; memcpy(&v, g->b + off, 8); return v; }
static void seti(gen_t *g, int off, int v) { memcpy(g->b + off, &v, 4); }

/* the fake sample chain: gen+240 -> sample; sample+368 -> provider;
 * provider+40 -> holder; holder+0 -> frames */
static float frames_f[FRAMES];
static int16_t frames_i[FRAMES];
static void *sample, *provider, *holder;
static double g_ratio = 1.0; /* what the fake stock render advances per frame */
static const float *g_src = frames_f;

static void build_chain(void *data)
{
    sample = calloc(1, 0x400);
    provider = calloc(1, 0x100);
    holder = calloc(1, 0x40);
    memcpy((char *)sample + 368, &provider, 8);
    memcpy((char *)provider + 40, &holder, 8);
    memcpy(holder, &data, 8);
}

static void make_frames(int carrier, const char *preset, int root)
{
    static int16_t pcm[FRAMES];
    for (int i = 0; i < FRAMES; i++)
        pcm[i] = (int16_t)lrintf(3000.0f * sinf(6.2831853f * 100.0f * i / 44100.0f));
    if (carrier) {
        fm_patch p;
        fm_preset_get(fm_preset_find(preset), &p);
        fm_carrier_encode(&p, root, pcm, FRAMES);
    }
    for (int i = 0; i < FRAMES; i++) {
        frames_f[i] = pcm[i] / 32768.0f;
        frames_i[i] = pcm[i];
    }
}

/* fake stock render: delay, then interpolated (here: nearest) frames x the gain ramp */
static uintptr_t fake_render(void *self, uintptr_t a_buf, uintptr_t a_n, uintptr_t a3, uintptr_t a4,
                             uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    gen_t *g = self;
    float *buf = (float *)a_buf;
    int n = (int)a_n, delay;
    memcpy(&delay, g->b + 192, 4);
    int d = delay > n ? n : delay;
    memset(buf, 0, (size_t)d * 4);
    seti(g, 192, delay - d);
    float cur = getf(g, 88), tgt = getf(g, 92), step = getf(g, 96);
    uint64_t pos = getu64(g, 368);
    uint64_t inc = (uint64_t)(g_ratio * 4294967296.0);
    for (int k = d; k < n; k++) {
        if (cur != tgt) { cur += step; cur = step >= 0 ? fminf(cur, tgt) : fmaxf(cur, tgt); }
        uint64_t i = pos >> 32;
        if (i >= FRAMES) return 0;
        buf[k] = g_src[i] * cur;
        pos += inc;
    }
    setf(g, 88, cur);
    setu64(g, 368, pos);
    return (pos >> 32) < FRAMES;
}
static uintptr_t fake_start(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                            uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    gen_t *g = self;
    setu64(g, 368, 0);
    setu64(g, 328, PLAIN);
    memcpy(g->b + 240, &sample, 8);
    return 1;
}
static uintptr_t fake_stop(void *self, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                           uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    (void)self; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return 0;
}

static void new_gen(gen_t *g, float gain)
{
    memset(g, 0, sizeof *g);
    setf(g, 88, gain);
    setf(g, 92, gain);
}

static int crossings(const float *x, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) c += (x[i - 1] < 0) != (x[i] < 0);
    return c;
}

static void play(gen_t *g, float *out, int blocks)
{
    for (int b = 0; b < blocks; b++)
        vh_test_render(g, out + b * N, N);
}

int main(void)
{
    static float out[N * 800], ref[N * 800];
    gen_t g;
    char msg[256];

    /* 1. a normal sample: transparent */
    make_frames(0, NULL, 60);
    build_chain(frames_f);
    vh_test_setup(fake_start, fake_stop, fake_render, PLAIN);
    new_gen(&g, 0.5f);
    vh_test_start(&g);
    play(&g, out, 10);
    double d = 0;
    for (int i = 0; i < 10 * N; i++) d = fmax(d, fabs(out[i] - 0.5f * frames_f[i]));
    CHECK(d == 0.0 && vh_stats.fm_starts == 0, "normal sample: stock output unchanged (diff %.1e)", d);
    CHECK(vh_poll(msg, sizeof msg) == 0, "discovery: nothing to learn from a normal sample");

    /* 2. a carrier (another file: another Sample object), path not known yet:
     * plays as a recording, then discovery learns */
    make_frames(1, "E.Piano", 60);
    build_chain(frames_f);
    new_gen(&g, 0.5f);
    vh_test_start(&g);
    CHECK(vh_stats.fm_starts == 0, "carrier, path unknown: plays as the recording first");
    int learned = vh_poll(msg, sizeof msg);
    CHECK(learned, "discovery: %s", msg);
    disc_path_t p[4];
    int np = disc_paths(p, 4);
    CHECK(np == 1 && p[0].hops == 4 && p[0].off[0] == 240 && p[0].off[1] == 368 && p[0].off[2] == 40 &&
              p[0].off[3] == 0 && p[0].data_off == 0 && p[0].fmt == FM_FMT_F32,
          "path = gen +240 +368 +40 +0, float32");

    /* 3. next note: live FM x the stock gain */
    new_gen(&g, 0.7f);
    vh_test_start(&g);
    CHECK(vh_stats.fm_starts == 1 && vh_active_voices() == 1, "next note: FM voice claimed (root %d)", vh_stats.last_root);
    play(&g, out, 600);
    fm_patch ep; fm_preset_get(fm_preset_find("E.Piano"), &ep);
    fm_voice v; fm_voice_init(&v, 44100.0f); fm_voice_note_on(&v, 1.0f);
    for (int b = 0; b < 600; b++) fm_voice_render(&v, &ep, 440.0f * powf(2.0f, -9.0f / 12.0f), ref + b * N, N);
    d = 0;
    for (int i = 0; i < 600 * N; i++) d = fmax(d, fabs(out[i] - 0.7f * ref[i]));
    CHECK(d < 1e-6, "output = FM engine x stock gain (diff %.1e)", d);

    /* 4. pitch follows how far the stock position moves */
    make_frames(1, "Sine", 69); /* root A4 */
    new_gen(&g, 1.0f);
    g_ratio = 2.0;
    vh_test_start(&g);
    play(&g, out, 340); /* the fake sample lasts FRAMES / 2 frames at ratio 2 */
    int c = crossings(out + 441, 20000);
    CHECK(abs(c - (int)(2 * 880 * 20000 / 44100.0)) <= 3, "ratio 2 on root A4: %d crossings in 20000 frames (880 Hz)", c);
    g_ratio = 1.0;

    /* 5. gain ramp and start delay */
    new_gen(&g, 0.0f);
    setf(&g, 92, 1.0f);
    setf(&g, 96, 1.0f / 100.0f); /* ramp over 100 frames */
    seti(&g, 192, 30);           /* 30 frames of silence first */
    vh_test_start(&g);
    play(&g, out, 4);
    float before = 0; for (int k = 0; k < 30; k++) before = fmaxf(before, fabsf(out[k]));
    fm_patch sine; fm_preset_get(fm_preset_find("Sine"), &sine);
    fm_voice_init(&v, 44100.0f); fm_voice_note_on(&v, 1.0f);
    float y[256]; fm_voice_render(&v, &sine, 440.0f, y, 256 - 30);
    d = 0; float gg = 0;
    for (int k = 30; k < 256; k++) { if (gg < 1.0f) gg = fminf(gg + 0.01f, 1.0f); d = fmax(d, fabs(out[k] - y[k - 30] * gg)); }
    CHECK(before == 0.0f && d < 1e-6, "start delay kept silent, then FM x the gain ramp (diff %.1e)", d);

    /* 6. not the plain mono render: stock, released, counted */
    new_gen(&g, 1.0f);
    vh_test_start(&g);
    setu64(&g, 328, PLAIN + 0x100);
    play(&g, out, 2);
    d = 0; for (int i = 0; i < 2 * N; i++) d = fmax(d, fabs(out[i] - frames_f[i]));
    CHECK(d == 0.0 && vh_stats.unsupported == 1 && vh_active_voices() == 0, "stereo/stretch layer: stock recording");

    /* 7. end of sample and stop free the FM voice */
    new_gen(&g, 1.0f);
    vh_test_start(&g);
    play(&g, out, 800); /* past the end */
    CHECK(vh_active_voices() == 0, "FM voice freed when the stock layer ends");
    vh_test_start(&g);
    vh_test_stop(&g);
    CHECK(vh_active_voices() == 0, "FM voice freed at stop");

    /* 8. discovery also finds int16 frames */
    disc_path_t q; int reads = 0;
    void *old_holder = holder;
    build_chain(frames_i);
    gen_t g2; new_gen(&g2, 1.0f); memcpy(g2.b + 240, &sample, 8);
    int ok = disc_search(&g2, 0, 0, &q, &reads);
    char desc[160]; disc_describe(&q, desc, sizeof desc);
    CHECK(ok && q.fmt == FM_FMT_I16, "int16 frames found too: %s (%d reads)", desc, reads);
    (void)old_holder;

    /* 10. a decoy: gen+48 leads (through shared objects) to a carrier; the
     * pad's own sample (+240) is a normal sample. Must not be learned. */
    {
        disc_path_t q2;
        int r2 = 0;
        make_frames(1, "Bell", 60);
        void *dsample = calloc(1, 0x400), *dprov = calloc(1, 0x100), *dhold = calloc(1, 0x40);
        void *dframes = frames_f;
        memcpy((char *)dsample + 368, &dprov, 8);
        memcpy((char *)dprov + 40, &dhold, 8);
        memcpy(dhold, &dframes, 8);
        static float plain[FRAMES];
        void *nsample = calloc(1, 0x400), *nprov = calloc(1, 0x100), *nhold = calloc(1, 0x40);
        void *nframes = plain;
        memcpy((char *)nsample + 368, &nprov, 8);
        memcpy((char *)nprov + 40, &nhold, 8);
        memcpy(nhold, &nframes, 8);
        gen_t g3; new_gen(&g3, 1.0f);
        memcpy(g3.b + 48, &dsample, 8);  /* the decoy branch */
        memcpy(g3.b + 240, &nsample, 8); /* the pad's own sample: not a carrier */
        CHECK(!disc_search(&g3, 0, 0, &q2, &r2), "decoy through gen+48 is not followed (%d reads)", r2);
        memcpy(g3.b + 240, &dsample, 8);
        int ok3 = disc_search(&g3, 0, 0, &q2, &r2);
        CHECK(ok3 && q2.off[0] == 240, "the same carrier through gen+240 is found");
    }

    /* 11. FM voice: the stock position advances 1 frame per frame at any pitch */
    vh_test_setup(fake_start, fake_stop, fake_render, PLAIN);
    make_frames(1, "Sine", 60);
    build_chain(frames_f);
    {
        disc_path_t seed = {4, {240, 368, 40, 0}, 0, FM_FMT_F32};
        disc_seed(&seed);
    }
    new_gen(&g, 1.0f);
    g_ratio = 3.0;
    vh_test_start(&g);
    CHECK(vh_active_voices() == 1, "seeded path: FM from the first note, no search");
    play(&g, out, 10);
    CHECK(getu64(&g, 368) == ((uint64_t)(10 * N) << 32), "ratio 3: position advanced %llu frames for %d (1 per frame)",
          (unsigned long long)(getu64(&g, 368) >> 32), 10 * N);
    CHECK(vh_stats.last_ratio_milli == 3000, "pitch still read as ratio %.3f", vh_stats.last_ratio_milli / 1000.0);
    g_ratio = 1.0;

    /* 12. a non-finite stock gain: stock output kept, FM voice dropped */
    new_gen(&g, 1.0f);
    vh_test_start(&g);
    setf(&g, 88, NAN);
    play(&g, out, 1);
    CHECK(vh_stats.bad_input == 1 && vh_active_voices() == 0, "NaN stock gain: back to stock, counted");

    /* 13. with a working path, a normal sample does not start a search */
    unsigned searches0 = disc_searches();
    make_frames(0, NULL, 60);
    build_chain(frames_f);
    new_gen(&g, 1.0f);
    vh_test_start(&g);
    vh_poll(msg, sizeof msg);
    CHECK(vh_active_voices() == 0 && disc_searches() == searches0, "normal sample on a known path: no search");

    /* 9. vh_install: checks the 16 stock entries, swaps 0, 3 and 7 */
    const uintptr_t VT = 0x6da6430, TI = 0x6e84c30;
    const uintptr_t M[16] = {0x220d720, 0x21fdee0, 0x20c1430, 0x220ef30, 0x220d960, 0x220d660, 0x220d690, 0x220f490,
                             0x220db70, 0x220db30, 0x20bfa80, 0x220d880, 0x220dee0, 0x220d730, 0x220d7a0, 0x220dd30};
    size_t span = 0x7300000;
    char *base = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uintptr_t *vt = (uintptr_t *)(base + VT);
    vt[-1] = (uintptr_t)base + TI;
    for (int i = 0; i < 16; i++) vt[i] = (uintptr_t)base + M[i];
    mprotect((void *)((uintptr_t)vt & ~(uintptr_t)4095), 8192, PROT_READ);
    const char *bid = "1dac7f481a0fe8d128e435ace88d7e6aece0c9c9";
    CHECK(vh_install((uintptr_t)base, "0000", msg, sizeof msg) == 0, "unknown build: no hooks (%s)", msg);
    ok = vh_install((uintptr_t)base, bid, msg, sizeof msg);
    int swapped = 1;
    for (int i = 0; i < 16; i++) {
        int mine = i == 0 || i == 3 || i == 7;
        swapped &= mine ? vt[i] != (uintptr_t)base + M[i] : vt[i] == (uintptr_t)base + M[i];
    }
    CHECK(ok && swapped, "stock vtable: entries 0, 3, 7 swapped, the others kept (%s)", msg);
    CHECK(vh_install((uintptr_t)base, bid, msg, sizeof msg) == 0, "second install refused: %s", msg);
    munmap(base, span);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
