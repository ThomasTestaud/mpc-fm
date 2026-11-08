/* fmbench: CPU cost of the FM engine, in the units of the MPC Sample audio
 * callback (128 frames at 44.1 kHz = 2902 us).
 *
 *   fmbench [-c CPU] [-s SECONDS] [-q]
 *     -c CPU   pin to this core (on the MPC: a core the audio thread does
 *              not use, see mpcfm-status.txt)
 *     -s       seconds of audio per test (default 2)
 *     -q       quick: hot loop only
 *
 * "hot": blocks back to back (best case, warm caches).
 * "paced": one block every 2.9 ms, like the real callback (caches go cold
 * between blocks; the worst block includes scheduler noise).
 * Built static for aarch64, it runs on the MPC as is (copy it to /run).
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fm_engine.h"
#include "fm_presets.h"

#define SR 44100.0f
#define BLOCK 128
#define BUDGET_US (BLOCK * 1e6 / SR)
#define MAX_VOICES 32

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static volatile float g_sink;

typedef struct {
    double avg_us, max_us;
} result;

static result run(const fm_patch *p, int voices, double seconds, int paced)
{
    static fm_voice v[MAX_VOICES];
    static float out[BLOCK], mix[BLOCK];
    for (int i = 0; i < voices; i++) {
        fm_voice_init(&v[i], SR);
        fm_voice_note_on(&v[i], 0.8f);
    }
    int blocks = (int)(seconds * SR / BLOCK);
    double total = 0, worst = 0, next = now_us();
    for (int b = 0; b < blocks; b++) {
        if (paced) {
            next += BUDGET_US;
            double wait = next - now_us();
            if (wait > 0) {
                struct timespec ts = {0, (long)(wait * 1000)};
                nanosleep(&ts, NULL);
            }
        }
        double t0 = now_us();
        memset(mix, 0, sizeof mix);
        for (int i = 0; i < voices; i++) {
            /* different pitches, like a chord spread over the keyboard */
            fm_voice_render(&v[i], p, 55.0f * (float)(1 + i % 24), out, BLOCK);
            for (int k = 0; k < BLOCK; k++)
                mix[k] += out[k];
        }
        double dt = now_us() - t0;
        total += dt;
        if (dt > worst && b > 4) /* skip the first blocks (page faults) */
            worst = dt;
    }
    g_sink = mix[0];
    return (result){total / blocks, worst};
}

static void cpu_clock(int cpu, char *out, size_t cap)
{
    char path[128], buf[32] = "";
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu < 0 ? 0 : cpu);
    FILE *f = fopen(path, "r");
    if (f && fgets(buf, sizeof buf, f))
        snprintf(out, cap, "%ld MHz", atol(buf) / 1000);
    else
        snprintf(out, cap, "clock n/a");
    if (f)
        fclose(f);
}

int main(int argc, char **argv)
{
    int cpu = -1, quick = 0;
    double seconds = 2.0;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:q")) != -1) {
        if (opt == 'c')
            cpu = atoi(optarg);
        else if (opt == 's')
            seconds = atof(optarg);
        else if (opt == 'q')
            quick = 1;
        else {
            fprintf(stderr, "usage: fmbench [-c CPU] [-s SECONDS] [-q]\n");
            return 2;
        }
    }
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0)
            perror("sched_setaffinity");
    }

    static const char *const tests[] = {"Bench", "E.Piano", "Bell"};
    static const int counts[] = {1, 8, 16, 32};
    char clk[32];
    cpu_clock(cpu, clk, sizeof clk);
    printf("fmbench: %d frames at %.0f Hz = budget %.0f us per block. cpu %d, %s\n", BLOCK, SR,
           BUDGET_US, cpu, clk);
    printf("%-8s %-7s %6s | %10s %8s %10s | %10s %8s\n", "preset", "mode", "voices", "avg us",
           "avg %", "us/voice", "worst us", "worst %");
    for (int t = 0; t < 3; t++) {
        fm_patch p;
        fm_preset_get(fm_preset_find(tests[t]), &p);
        for (int mode = 0; mode < (quick ? 1 : 2); mode++) {
            if (mode == 1 && t > 0)
                continue; /* paced: the worst-case patch only */
            for (int c = 0; c < 4; c++) {
                result r = run(&p, counts[c], seconds, mode);
                printf("%-8s %-7s %6d | %10.1f %7.2f%% %10.2f | %10.1f %7.2f%%\n", tests[t],
                       mode ? "paced" : "hot", counts[c], r.avg_us, 100.0 * r.avg_us / BUDGET_US,
                       r.avg_us / counts[c], r.max_us, 100.0 * r.max_us / BUDGET_US);
                fflush(stdout);
            }
        }
    }
    cpu_clock(cpu, clk, sizeof clk);
    printf("cpu clock at the end: %s\n", clk);
    return 0;
}
