/* Unit tests of the FM engine (host). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm_engine.h"
#include "fm_presets.h"

#define SR 44100.0f
#define MAXN (44100 * 4)

static int fails;
#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (cond) {                                                                              \
            printf("ok   ");                                                                     \
        } else {                                                                                 \
            printf("FAIL ");                                                                     \
            fails++;                                                                             \
        }                                                                                        \
        printf(__VA_ARGS__);                                                                     \
        printf("\n");                                                                            \
    } while (0)

static float buf[MAXN], buf2[MAXN];

/* Render `frames` of a note (gate on for `hold` frames) in blocks of `block`
 * (block 0: random sizes 1..128). */
static void render(const fm_patch *p, float hz, float vel, int hold, int frames, int block,
                   float *out)
{
    fm_voice v;
    fm_voice_init(&v, SR);
    fm_voice_note_on(&v, vel);
    unsigned seed = 1;
    int done = 0;
    while (done < frames) {
        int n = block;
        if (!n) {
            seed = seed * 1103515245u + 12345u;
            n = 1 + (int)((seed >> 16) % 128);
        }
        if (done < hold && done + n > hold)
            n = hold - done; /* note off exactly at `hold` */
        if (n > frames - done)
            n = frames - done;
        if (done == hold)
            fm_voice_note_off(&v);
        fm_voice_render(&v, p, hz, out + done, n);
        done += n;
    }
}

static int crossings(const float *x, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++)
        c += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
    return c;
}

static float peak(const float *x, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; i++)
        if (fabsf(x[i]) > m)
            m = fabsf(x[i]);
    return m;
}

/* Amplitude of the component at `hz` (Goertzel). */
static float tone_amp(const float *x, int n, float hz)
{
    double w = 2.0 * M_PI * hz / SR, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double s = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return (float)(2.0 * sqrt(re * re + im * im) / n);
}

static fm_patch sine(void)
{
    fm_patch p;
    fm_preset_get(fm_preset_find("Sine"), &p);
    return p;
}

int main(void)
{
    /* sine accuracy */
    double maxerr = 0;
    for (int i = -300000; i <= 300000; i++) {
        float x = (float)i / 100000.0f;
        double e = fabs((double)fm_sin2pi(x) - sin(2.0 * M_PI * (double)x));
        if (e > maxerr)
            maxerr = e;
    }
    CHECK(maxerr < 2e-6, "sin2pi max error %.2e over [-3, 3]", maxerr);

    /* pitch */
    fm_patch p = sine();
    render(&p, 440.0f, 1.0f, MAXN, 44100, 128, buf);
    int c = crossings(buf + 441, 44100 - 441);
    CHECK(abs(c - 2 * 440 * 99 / 100) <= 3, "440 Hz sine: %d zero crossings in 0.99 s", c);
    CHECK(fabsf(peak(buf + 4410, 4410) - 1.0f) < 0.01f, "sine peak %.4f (want 1)", peak(buf + 4410, 4410));

    p.op[0].ratio = 2.0f;
    render(&p, 440.0f, 1.0f, MAXN, 44100, 128, buf);
    c = crossings(buf + 441, 44100 - 441);
    CHECK(abs(c - 2 * 880 * 99 / 100) <= 3, "ratio 2: %d crossings (880 Hz)", c);

    p = sine();
    p.op[0].fixed_hz = 1000.0f;
    render(&p, 123.0f, 1.0f, MAXN, 44100, 128, buf);
    c = crossings(buf + 441, 44100 - 441);
    CHECK(abs(c - 2 * 1000 * 99 / 100) <= 3, "fixed 1000 Hz ignores the note: %d crossings", c);

    p = sine();
    p.op[0].detune = 100.0f; /* +1 semitone */
    render(&p, 440.0f, 1.0f, MAXN, 44100, 128, buf);
    c = crossings(buf + 441, 44100 - 441);
    CHECK(abs(c - (int)(2 * 466.16 * 0.99)) <= 3, "detune +100 cents: %d crossings (466 Hz)", c);

    /* envelopes */
    p = sine();
    p.op[0].attack = 0.1f;
    render(&p, 1000.0f, 1.0f, MAXN, 44100, 128, buf);
    int t90 = -1;
    for (int i = 0; i < 44100 && t90 < 0; i += 32)
        if (peak(buf + i, 64) > 0.9f)
            t90 = i;
    CHECK(t90 > 0.085 * SR && t90 < 0.1 * SR, "attack 0.1 s reaches 90%% at %.1f ms", t90 * 1000.0 / SR);

    p = sine();
    p.op[0].release = 0.2f; /* 60 dB in 0.2 s, ends at -96 dB: 0.32 s */
    int hold = 22050;
    render(&p, 1000.0f, 1.0f, hold, 44100, 128, buf);
    float at = peak(buf + hold + (int)(0.1f * SR), 441);
    CHECK(fabsf(20.0f * log10f(at) + 30.0f) < 2.0f, "release: %.1f dB after 0.1 s (want -30)", 20 * log10f(at));
    CHECK(peak(buf + hold + (int)(0.34f * SR), 44100 - hold - (int)(0.34f * SR)) == 0.0f,
          "silent after the release ends");
    {
        fm_voice v;
        fm_voice_init(&v, SR);
        fm_voice_note_on(&v, 1.0f);
        for (int i = 0; i < 100; i++)
            fm_voice_render(&v, &p, 440.0f, buf, 128);
        fm_voice_note_off(&v);
        int blocks = 0;
        while (fm_voice_active(&v) && blocks < 1000) {
            fm_voice_render(&v, &p, 440.0f, buf, 128);
            blocks++;
        }
        CHECK(blocks > 100 && blocks < 120, "voice inactive %d blocks (%.0f ms) after note off",
              blocks, blocks * 128 * 1000.0 / SR);
    }
    p = sine();
    p.op[0].sustain = 0.0f;
    p.op[0].decay = 0.1f;
    {
        fm_voice v;
        fm_voice_init(&v, SR);
        fm_voice_note_on(&v, 1.0f);
        int blocks = 0;
        while (fm_voice_active(&v) && blocks < 1000) {
            fm_voice_render(&v, &p, 440.0f, buf, 128);
            blocks++;
        }
        CHECK(blocks < 70, "sustain 0: the voice ends by itself (%d blocks)", blocks);
    }

    /* velocity */
    p = sine();
    p.op[0].vel_sens = 1.0f;
    render(&p, 440.0f, 0.5f, MAXN, 8820, 128, buf);
    CHECK(fabsf(peak(buf + 4410, 4410) - 0.5f) < 0.01f, "velocity 0.5, sens 1: peak %.3f", peak(buf + 4410, 4410));

    /* modulation makes sidebands */
    fm_patch_init(&p); /* algorithm 4>3>2>1, operator 1 only */
    p.op[1].level = 0.0f;
    render(&p, 440.0f, 1.0f, MAXN, 44100, 128, buf);
    float h2off = tone_amp(buf + 4410, 32768, 880.0f);
    p.op[1].level = 0.3f; /* operator 2 modulates 1, ratio 1:1 */
    render(&p, 440.0f, 1.0f, MAXN, 44100, 128, buf);
    float h2on = tone_amp(buf + 4410, 32768, 880.0f);
    CHECK(h2off < 1e-3f && h2on > 0.1f, "op2>op1: 880 Hz sideband %.4f (off %.5f)", h2on, h2off);

    /* chunking does not change the output */
    for (int pi = 0; pi < fm_preset_count(); pi++) {
        fm_preset_get(pi, &p);
        render(&p, 220.0f, 0.8f, 30000, 60000, 128, buf);
        render(&p, 220.0f, 0.8f, 30000, 60000, 0, buf2);
        double d = 0;
        for (int i = 0; i < 60000; i++)
            if (fabs(buf[i] - buf2[i]) > d)
                d = fabs(buf[i] - buf2[i]);
        CHECK(d == 0.0, "%-9s: random block sizes give the same output as 128 (diff %.1e)", fm_preset_name(pi), d);
    }

    /* every algorithm, worst settings, wide pitch range: finite and bounded */
    int bad = 0;
    float worst = 0.0f;
    for (int a = 0; a < FM_ALGORITHMS; a++) {
        fm_preset_get(fm_preset_find("Bench"), &p);
        p.algorithm = a;
        p.gain = 1.0f;
        for (float hz = 20.0f; hz < 12000.0f; hz *= 1.7f) {
            render(&p, hz, 1.0f, 4410, 8820, 128, buf);
            for (int i = 0; i < 8820; i++)
                if (!isfinite(buf[i]))
                    bad++;
            float pk = peak(buf, 8820) * sqrtf((float)__builtin_popcount(fm_algorithm_carriers(a))) /
                       (float)__builtin_popcount(fm_algorithm_carriers(a));
            if (pk > worst)
                worst = pk;
        }
    }
    CHECK(bad == 0, "all algorithms at full levels: %d non-finite samples", bad);
    CHECK(worst <= 1.0001f, "carrier sum stays within its bound (%.4f <= 1)", worst);

    /* bad input */
    p = sine();
    fm_voice v;
    fm_voice_init(&v, SR);
    fm_voice_note_on(&v, NAN);
    float pitches[] = {NAN, -5.0f, 0.0f, INFINITY, 1e9f};
    bad = 0;
    for (int i = 0; i < 5; i++) {
        fm_voice_render(&v, &p, pitches[i], buf, 128);
        for (int k = 0; k < 128; k++)
            bad += !isfinite(buf[k]) || buf[k] != 0.0f;
    }
    CHECK(bad == 0, "bad pitch (NaN, <0, 0, inf, huge): silence");

    p = sine();
    render(&p, 30000.0f / 1.0f, 1.0f, MAXN, 4410, 128, buf); /* above 0.45 * sample rate */
    CHECK(peak(buf + 64, 4410 - 64) == 0.0f, "operator above 19.8 kHz is muted");

    fm_patch_init(&p);
    p.op[0].ratio = NAN;
    p.op[1].attack = -1.0f;
    p.op[2].level = 7.0f;
    p.algorithm = 42;
    p.gain = INFINITY;
    fm_patch_sanitize(&p);
    CHECK(p.op[0].ratio == 1.0f && p.op[1].attack == 0.0005f && p.op[2].level == 1.0f &&
              p.algorithm == 0 && p.gain == 2.0f,
          "sanitize: NaN to default, out of range clamped");

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
