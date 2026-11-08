/* fmrender: render a preset to a WAV file, to listen on the computer.
 *
 *   fmrender PRESET MIDI_NOTE VELOCITY HOLD_S TOTAL_S OUT.wav
 *   fmrender all DIR          (each preset: C2, C3 and C4, into DIR)
 *   fmrender samples DIR      (each preset once, at C3, for MPC pads: FM carriers,
 *                              they play live with mpc-fm, as recordings without)
 *   fmrender list
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm_carrier.h"
#include "fm_engine.h"
#include "fm_presets.h"

#define SR 44100

static void put16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, long v) { put16(f, (int)(v & 0xffff)); put16(f, (int)((v >> 16) & 0xffff)); }

/* 16-bit mono WAV. carrier_root >= 0: embed the patch (FM carrier, see
 * fm_carrier.h) with that root note. Returns the peak level. */
static float render_wav(const fm_patch *p, float hz, float vel, int hold, int total, const char *path,
                        int carrier_root)
{
    float *x = calloc((size_t)total, sizeof *x);
    if (!x)
        return -1;
    fm_voice v;
    fm_voice_init(&v, (float)SR);
    fm_voice_note_on(&v, vel);
    for (int done = 0; done < total;) {
        int n = total - done < 128 ? total - done : 128;
        if (done < hold && done + n > hold)
            n = hold - done;
        if (done == hold)
            fm_voice_note_off(&v);
        fm_voice_render(&v, p, hz, x + done, n);
        done += n;
    }
    int fade = SR / 100; /* 10 ms fade-out: no click if the sound is still on */
    for (int i = 0; i < fade && i < total; i++)
        x[total - 1 - i] *= (float)i / (float)fade;
    float pk = 0;
    for (int i = 0; i < total; i++)
        pk = fabsf(x[i]) > pk ? fabsf(x[i]) : pk;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(x);
        return -1;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + total * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, SR);
    put32(f, SR * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, total * 2);
    int16_t *pcm = malloc((size_t)total * sizeof *pcm);
    if (!pcm) {
        fclose(f);
        free(x);
        return -1;
    }
    for (int i = 0; i < total; i++) {
        float s = x[i] * 0.8f; /* -2 dB headroom */
        s = s > 1 ? 1 : s < -1 ? -1 : s;
        pcm[i] = (int16_t)lrintf(s * 32767.0f);
    }
    if (carrier_root >= 0 && !fm_carrier_encode(p, carrier_root, pcm, total))
        fprintf(stderr, "%s: too short for an FM carrier header\n", path);
    for (int i = 0; i < total; i++)
        put16(f, pcm[i]);
    fclose(f);
    free(pcm);
    free(x);
    return pk;
}

static float midi_hz(int note) { return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f); }

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        for (int i = 0; i < fm_preset_count(); i++) {
            fm_patch p;
            fm_preset_get(i, &p);
            printf("%-10s algorithm %d (%s), feedback %.2f\n", fm_preset_name(i), p.algorithm + 1,
                   fm_algorithm_name(p.algorithm), p.feedback);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "all") == 0) {
        for (int i = 0; i < fm_preset_count(); i++) {
            fm_patch p;
            fm_preset_get(i, &p);
            for (int note = 36; note <= 60; note += 12) {
                char path[512], name[64];
                snprintf(name, sizeof name, "%s", fm_preset_name(i));
                for (char *c = name; *c; c++)
                    if (*c == ' ' || *c == '.')
                        *c = '_';
                snprintf(path, sizeof path, "%s/%s-%d.wav", argv[2], name, note);
                float pk = render_wav(&p, midi_hz(note), 0.8f, SR, 3 * SR, path, -1);
                printf("%s  peak %.2f\n", path, pk);
            }
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "samples") == 0) {
        /* name, hold s, total s: one-shots for pads (Keys mode transposes) */
        static const struct { const char *preset, *file; float hold, total; } S[] = {
            {"E.Piano", "FM-EPiano-C3", 2.0f, 4.0f}, {"Bass", "FM-Bass-C3", 0.6f, 1.2f},
            {"Bell", "FM-Bell-C3", 0.5f, 6.0f},      {"Brass", "FM-Brass-C3", 1.5f, 2.5f},
            {"Metal Hit", "FM-MetalHit-C3", 0.1f, 0.8f}, {"Sine", "FM-Sine-C3", 2.0f, 2.5f},
        };
        for (size_t i = 0; i < sizeof S / sizeof *S; i++) {
            fm_patch p;
            fm_preset_get(fm_preset_find(S[i].preset), &p);
            char path[512];
            snprintf(path, sizeof path, "%s/%s.wav", argv[2], S[i].file);
            float pk = render_wav(&p, midi_hz(60), 0.9f, (int)(S[i].hold * SR), (int)(S[i].total * SR), path, 60);
            printf("%s  peak %.2f\n", path, pk);
        }
        return 0;
    }
    if (argc != 7) {
        fprintf(stderr, "usage: fmrender PRESET NOTE VELOCITY HOLD_S TOTAL_S OUT.wav | all DIR | list\n");
        return 2;
    }
    int idx = fm_preset_find(argv[1]);
    fm_patch p;
    if (idx < 0 || !fm_preset_get(idx, &p)) {
        fprintf(stderr, "no preset \"%s\" (fmrender list)\n", argv[1]);
        return 2;
    }
    float pk = render_wav(&p, midi_hz(atoi(argv[2])), (float)atof(argv[3]), (int)(atof(argv[4]) * SR),
                          (int)(atof(argv[5]) * SR), argv[6], -1);
    printf("%s  peak %.2f\n", argv[6], pk);
    return pk < 0;
}
