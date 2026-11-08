/* mpc-fm: FM synth add-on for the Akai MPC Sample, loaded next to SampQOL OS.
 *
 * Phase 0 build: no sound yet. It measures the real audio budget on the
 * device and writes it to a status file once per second:
 *   - timing of the stock app's ALSA calls (alsa_probe.c): block period,
 *     processing time per block, xruns;
 *   - CPU use, core, scheduling policy and priority of every app thread;
 *   - CPU clocks and governor, memory.
 *
 * Files: /tmp/mpcfm/status.txt (every second), and in $MPCFM_LOGDIR (the SD
 * card): mpcfm-status.txt (every 10 s) and mpcfm-events.log.
 *
 * fm.so is preloaded into every program that the launch chain starts
 * (sh, setarch, the app). It does nothing unless it is in the stock app.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"
#include "version.h"
#include "crash.h"
#include "discover.h"
#include "voice_hook.h"

#ifndef MPCFM_TARGET_EXE
#define MPCFM_TARGET_EXE "/usr/bin/MPC Sample"
#endif

#define STATUS_CAP (24 * 1024)
#define EVENTS_MAX_BYTES (256 * 1024)
#define MAX_THREADS 256
#define SD_EVERY_S 10
#define LATE_GAP_NS 2500000ull /* a block that used > 2.5 of its 2.9 ms */

static char g_tmpdir[256];
static char g_logdir[256];
static char g_preload[512];
static char g_buildid[2 * 32 + 1] = "none";
static int g_qol_loaded;
static uintptr_t g_app_base;
static char g_hook_msg[160] = "not tried";
static int g_hooks_on;

/* Stock app builds we know (code hooks only run on these). */
static const struct {
    const char *build_id, *firmware;
} KNOWN_APPS[] = {
    {"1dac7f481a0fe8d128e435ace88d7e6aece0c9c9", "1.3.0.12"},
};

static const char *known_firmware(void)
{
    for (size_t i = 0; i < sizeof KNOWN_APPS / sizeof *KNOWN_APPS; i++)
        if (strcmp(g_buildid, KNOWN_APPS[i].build_id) == 0)
            return KNOWN_APPS[i].firmware;
    return NULL;
}

/* ---- helpers (logger thread and constructor only) ---- */

typedef struct {
    char *buf;
    size_t cap, len;
} sbuf;

static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf *b, const char *fmt, ...)
{
    if (b->len + 1 >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    size_t room = b->cap - b->len - 1;
    b->len += (size_t)n < room ? (size_t)n : room;
}

static int read_text(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
        out[--n] = 0;
    return (int)n;
}

static int write_atomic(const char *dir, const char *name, const char *data, size_t len)
{
    char tmp[512], dst[512];
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, data, len);
    close(fd);
    if (w != (ssize_t)len) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, dst);
}

static void append_capped(const char *dir, const char *name, const char *line)
{
    char path[512], old[512];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (stat(path, &st) == 0 && st.st_size > EVENTS_MAX_BYTES) {
        snprintf(old, sizeof old, "%s/%s.old", dir, name);
        rename(path, old);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, line, strlen(line)) < 0) {
        /* best effort */
    }
    close(fd);
}

static void event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void event(const char *fmt, ...)
{
    char msg[512], line[600], when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%F %T", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%s %s\n", when, msg);
    append_capped(g_tmpdir, "events.log", line);
    if (g_logdir[0])
        append_capped(g_logdir, "mpcfm-events.log", line);
}

/* ---- loaded objects: build-id of the stock app, is SampQOL loaded ---- */

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    int *index = data;
    if ((*index)++ == 0) { /* the first object is the main program */
        g_app_base = (uintptr_t)info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
            if (ph->p_type != PT_NOTE)
                continue;
            const unsigned char *p = (const unsigned char *)(info->dlpi_addr + ph->p_vaddr);
            const unsigned char *end = p + ph->p_memsz;
            while (end - p >= 12) {
                uint32_t nsz, dsz, type;
                memcpy(&nsz, p, 4);
                memcpy(&dsz, p + 4, 4);
                memcpy(&type, p + 8, 4);
                size_t name_room = (nsz + 3u) & ~3u, desc_room = (dsz + 3u) & ~3u;
                if ((size_t)(end - p) < 12 + name_room + dsz)
                    break;
                const unsigned char *name = p + 12, *desc = name + name_room;
                if (type == NT_GNU_BUILD_ID && nsz == 4 && memcmp(name, "GNU", 4) == 0) {
                    size_t n = dsz < 32 ? dsz : 32;
                    for (size_t k = 0; k < n; k++)
                        snprintf(g_buildid + 2 * k, 3, "%02x", desc[k]);
                }
                if ((size_t)(end - desc) < desc_room)
                    break;
                p = desc + desc_room;
            }
        }
    }
    if (info->dlpi_name && strstr(info->dlpi_name, "mpcqol.so"))
        g_qol_loaded = 1;
    return 0;
}

/* ---- threads of this process ---- */

typedef struct {
    int tid, cpu, prio, policy;
    char comm[20];
    uint64_t run_ns, delta_ns;
} tinfo;

static tinfo g_threads[MAX_THREADS], g_prev[MAX_THREADS];
static int g_nthreads, g_nprev;

static const char *policy_name(int p)
{
    switch (p) {
    case 0: return "OTHER";
    case 1: return "FIFO";
    case 2: return "RR";
    case 3: return "BATCH";
    case 5: return "IDLE";
    case 6: return "DEADLINE";
    default: return "?";
    }
}

static int by_delta_desc(const void *a, const void *b)
{
    const tinfo *x = a, *y = b;
    return (y->delta_ns > x->delta_ns) - (y->delta_ns < x->delta_ns);
}

static void scan_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return;
    long tck = sysconf(_SC_CLK_TCK);
    if (tck <= 0)
        tck = 100;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_THREADS) {
        int tid = atoi(e->d_name);
        if (tid <= 0)
            continue;
        char path[64], buf[1024];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        if (read_text(path, buf, sizeof buf) <= 0)
            continue;
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp || !rp[1])
            continue;
        tinfo *t = &g_threads[n];
        memset(t, 0, sizeof *t);
        t->tid = tid;
        size_t cl = (size_t)(rp - lp - 1);
        if (cl >= sizeof t->comm)
            cl = sizeof t->comm - 1;
        memcpy(t->comm, lp + 1, cl);
        /* Fields after ')': 3 state, 14 utime, 15 stime, 39 processor,
         * 40 rt_priority, 41 policy. */
        unsigned long long f[42] = {0};
        char *save = NULL, *tok = strtok_r(rp + 2, " ", &save);
        for (int idx = 3; tok && idx < 42; idx++, tok = strtok_r(NULL, " ", &save))
            if (idx > 3)
                f[idx] = strtoull(tok, NULL, 10);
        t->cpu = (int)f[39];
        t->prio = (int)f[40];
        t->policy = (int)f[41];
        t->run_ns = (f[14] + f[15]) * (1000000000ull / (unsigned long long)tck);
        char ss[128]; /* schedstat: ns on CPU, more precise than ticks */
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", tid);
        if (read_text(path, ss, sizeof ss) > 0)
            t->run_ns = strtoull(ss, NULL, 10);
        for (int k = 0; k < g_nprev; k++)
            if (g_prev[k].tid == tid && g_prev[k].run_ns <= t->run_ns) {
                t->delta_ns = t->run_ns - g_prev[k].run_ns;
                break;
            }
        n++;
    }
    closedir(d);
    memcpy(g_prev, g_threads, (size_t)n * sizeof *g_threads);
    g_nprev = n;
    g_nthreads = n;
    qsort(g_threads, (size_t)n, sizeof *g_threads, by_delta_desc);
}

static const char *thread_comm(int tid)
{
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i].tid == tid)
            return g_threads[i].comm;
    return "?";
}

/* ---- status file ---- */

static double ms(uint64_t ns) { return (double)ns / 1e6; }

static void status_clocks(sbuf *b)
{
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    sb_printf(b, "clock:");
    for (int c = 0; c < ncpu && c < 8; c++) {
        char p[128], cur[32] = "", max[32] = "", gov[32] = "";
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        if (read_text(p, cur, sizeof cur) <= 0) {
            sb_printf(b, " cpu%d n/a", c);
            continue;
        }
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
        read_text(p, max, sizeof max);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", c);
        read_text(p, gov, sizeof gov);
        sb_printf(b, " cpu%d %ld/%ld MHz %s", c, atol(cur) / 1000, atol(max) / 1000, gov);
    }
    sb_printf(b, "\n");
}

static void status_memory(sbuf *b)
{
    char t[256], mi[4096];
    long rss_mb = -1, avail_mb = -1;
    if (read_text("/proc/self/statm", t, sizeof t) > 0) {
        long size = 0, pages = 0;
        if (sscanf(t, "%ld %ld", &size, &pages) == 2)
            rss_mb = pages * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
    }
    if (read_text("/proc/meminfo", mi, sizeof mi) > 0) {
        char *a = strstr(mi, "MemAvailable:");
        if (a)
            avail_mb = atol(a + 13) / 1024;
    }
    sb_printf(b, "memory: app rss %ld MB, available %ld MB\n", rss_mb, avail_mb);
}

typedef struct {
    uint64_t xruns, recovers;
    int seen;
} slot_memo;

static slot_memo g_memo[PROBE_SLOTS];

static void status_audio(sbuf *b)
{
    uint64_t epoch = atomic_load(&g_probe_epoch);
    sb_printf(b, "--- audio: ALSA calls in the last 1 s window ---\n");
    int any = 0;
    for (int i = 0; i < PROBE_SLOTS; i++) {
        probe_slot *s = &g_probe[i];
        if (!atomic_load(&s->pcm))
            continue;
        any = 1;
        static probe_view v; /* logger thread only */
        int ok = probe_snapshot(s, &v);
        const fn_window *w = v.win;
        uint64_t win = v.epoch;
        int stream = atomic_load(&s->stream), tid = atomic_load(&s->tid);
        uint64_t xr = atomic_load(&s->xrun_results), rc = atomic_load(&s->recovers);
        const char *dir = stream == 0 ? "playback" : stream == 1 ? "capture" : "?";
        sb_printf(b, "pcm%d \"%s\" %s tid %d (%s)%s calls %llu xrun %llu recover %llu%s\n",
                  i, s->name[0] ? s->name : "?", dir, tid, thread_comm(tid),
                  atomic_load(&s->closed) ? " CLOSED" : "",
                  (unsigned long long)atomic_load(&s->total_calls),
                  (unsigned long long)xr, (unsigned long long)rc,
                  !ok ? " (busy)" : win + 2 < epoch ? " (no calls lately)" : "");

        if (!g_memo[i].seen && tid != 0) { /* tid is known after the first I/O call */
            g_memo[i].seen = 1;
            event("pcm%d \"%s\" %s first used by tid %d (%s)", i, s->name, dir, tid,
                  thread_comm(tid));
        }
        if (xr != g_memo[i].xruns || rc != g_memo[i].recovers) {
            event("xrun pcm%d: results %llu, recover calls %llu", i, (unsigned long long)xr,
                  (unsigned long long)rc);
            g_memo[i].xruns = xr;
            g_memo[i].recovers = rc;
        }
        if (!ok)
            continue;
        for (int f = 0; f < FN_COUNT; f++) {
            const fn_window *x = &w[f];
            if (!x->calls)
                continue;
            sb_printf(b,
                      "  %-11s %5llu/s frames %4llu | gap avg %.3f max %.3f ms"
                      " | in call avg %.3f max %.3f ms | cycle max %.3f ms | gap hist",
                      probe_fn_name[f], (unsigned long long)x->calls,
                      (unsigned long long)x->last_frames,
                      x->gap_n ? ms(x->gap_sum_ns / x->gap_n) : 0.0, ms(x->gap_max_ns),
                      ms(x->in_sum_ns / x->calls), ms(x->in_max_ns), ms(x->cycle_max_ns));
            for (int h = 0; h < PROBE_HIST; h++)
                sb_printf(b, " %llu", (unsigned long long)x->gap_hist[h]);
            sb_printf(b, "\n");
            const fn_window *t = &v.sess[f];
            if (t->calls) {
                sb_printf(b, "    since start: %llu calls | gap avg %.3f max %.3f ms (at ~%llu s)"
                             " | cycle max %.3f ms | gap hist",
                          (unsigned long long)t->calls,
                          t->gap_n ? ms(t->gap_sum_ns / t->gap_n) : 0.0, ms(t->gap_max_ns),
                          (unsigned long long)v.worst_epoch[f], ms(t->cycle_max_ns));
                for (int h = 0; h < PROBE_HIST; h++)
                    sb_printf(b, " %llu", (unsigned long long)t->gap_hist[h]);
                sb_printf(b, "\n");
            }
            if (stream == 0 && (f == FN_WRITEI || f == FN_WRITEN || f == FN_MMAP_COMMIT) &&
                x->gap_max_ns > LATE_GAP_NS)
                event("late block on pcm%d: gap max %.2f ms before %s", i, ms(x->gap_max_ns),
                      probe_fn_name[f]);
        }
    }
    if (!any)
        sb_printf(b, "(no PCM calls seen yet)\n");
    sb_printf(b, "gap hist ms: <0.5 <1 <1.5 <2 <2.5 <2.9 <5 >=5. gap = time since the thread's"
                 " previous PCM call returned (its processing time when it blocks in PCM calls)\n");
}

static void status_threads(sbuf *b)
{
    sb_printf(b, "--- threads: CPU in the last 1 s (top 12 and all real-time) ---\n");
    sb_printf(b, "  tid   %-16s  cpu%%  core policy prio\n", "name");
    for (int i = 0; i < g_nthreads; i++) {
        const tinfo *t = &g_threads[i];
        if (i >= 12 && t->policy != 1 && t->policy != 2)
            continue;
        sb_printf(b, "  %-5d %-16s %5.1f  %4d %-6s %d\n", t->tid, t->comm,
                  (double)t->delta_ns / 1e7, t->cpu, policy_name(t->policy), t->prio);
    }
}

static uint64_t g_prev_blocks, g_prev_ns;

static void status_fm(sbuf *b)
{
    sb_printf(b, "--- FM voices ---\nhooks: %s\n", g_hook_msg);
    if (!g_hooks_on)
        return;
    uint64_t blocks = atomic_load(&vh_stats.fm_blocks), ns = atomic_load(&vh_stats.fm_ns);
    uint64_t db = blocks - g_prev_blocks, dn = ns - g_prev_ns;
    g_prev_blocks = blocks;
    g_prev_ns = ns;
    sb_printf(b,
              "layer starts %llu, FM notes %llu, FM voices now %d, no free FM voice %llu,"
              " FM file on a stereo/stretch layer %llu, dropped: bad stock gain %llu,"
              " bad FM output %llu\n",
              (unsigned long long)atomic_load(&vh_stats.starts),
              (unsigned long long)atomic_load(&vh_stats.fm_starts), atomic_load(&vh_stats.active),
              (unsigned long long)atomic_load(&vh_stats.no_slot),
              (unsigned long long)atomic_load(&vh_stats.unsupported),
              (unsigned long long)atomic_load(&vh_stats.bad_input),
              (unsigned long long)atomic_load(&vh_stats.bad_output));
    sb_printf(b,
              "FM render: %llu calls in the last 1 s, avg %.1f us per call, max %.1f us since start;"
              " last pitch ratio %.3f (root note %d); carrier check max %.1f us\n",
              (unsigned long long)db, db ? (double)dn / (double)db / 1e3 : 0.0,
              (double)atomic_load(&vh_stats.fm_max_ns) / 1e3,
              atomic_load(&vh_stats.last_ratio_milli) / 1000.0, atomic_load(&vh_stats.last_root),
              (double)atomic_load(&vh_stats.lookup_max_ns) / 1e3);
    disc_path_t paths[4];
    int n = disc_paths(paths, 4);
    if (!n)
        sb_printf(b, "carrier frames: not found yet (play a pad with an FM-*.wav file once)\n");
    for (int i = 0; i < n; i++) {
        char d[160];
        disc_describe(&paths[i], d, sizeof d);
        sb_printf(b, "carrier frames path %d: %s\n", i, d);
    }
}

/* SampQOL's own log, copied to the card: to see if both mods get along. */
static void copy_qol_log(void)
{
    static char buf[64 * 1024];
    int fd = open("/tmp/mpcqol.log", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > (off_t)sizeof buf ? size - (off_t)sizeof buf : 0;
    ssize_t n = pread(fd, buf, sizeof buf, from);
    close(fd);
    if (n > 0)
        write_atomic(g_logdir, "mpcqol-log-copy.txt", buf, (size_t)n);
}

static void build_status(sbuf *b, long up_s)
{
    sb_printf(b, "mpc-fm %s (FM pads: prototype)  pid %d  up %ld s\n",
              MPCFM_VERSION, getpid(), up_s);
    const char *fw = known_firmware();
    sb_printf(b, "app build-id %s (%s%s)  SampQOL loaded: %s\n", g_buildid,
              fw ? "firmware " : "UNKNOWN build: hooks stay off", fw ? fw : "",
              g_qol_loaded ? "yes" : "no");
    sb_printf(b, "LD_PRELOAD=%s\n", g_preload);
    status_clocks(b);
    status_memory(b);
    scan_threads();
    status_audio(b);
    status_fm(b);
    status_threads(b);
}

static void *logger_main(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "mpcfm-log");
    /* Low priority: this thread must never compete with the audio. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);

    mkdir(g_tmpdir, 0755);
    if (g_logdir[0])
        mkdir(g_logdir, 0755);
    event("start mpc-fm %s pid %d, app build-id %s, SampQOL %s", MPCFM_VERSION, getpid(),
          g_buildid, g_qol_loaded ? "loaded" : "NOT loaded");
    event("FM hooks: %s", g_hook_msg);

    char *buf = malloc(STATUS_CAP);
    if (!buf)
        return NULL;
    for (long tick = 1;; tick++) {
        struct timespec one = {1, 0};
        nanosleep(&one, NULL);
        sbuf b = {buf, STATUS_CAP, 0};
        build_status(&b, tick);
        /* Start a new window: each audio thread publishes the finished one
         * at its next call; we read it one second later. */
        atomic_fetch_add(&g_probe_epoch, 1);
        write_atomic(g_tmpdir, "status.txt", buf, b.len);
        if (g_logdir[0] && (tick == 1 || tick % SD_EVERY_S == 0))
            write_atomic(g_logdir, "mpcfm-status.txt", buf, b.len);
        if (g_logdir[0] && (tick == 20 || tick % 60 == 0))
            copy_qol_log();
        if (tick == 5) { /* after SampQOL has put its own crash handler */
            char path[300], m[200];
            snprintf(path, sizeof path, "%s/%s", g_logdir[0] ? g_logdir : g_tmpdir, "mpcfm-crash.log");
            crash_install(path, g_app_base, m, sizeof m);
            event("%s", m);
        }
        if (g_hooks_on) {
            char m[256];
            if (vh_poll(m, sizeof m)) {
                event("%s", m);
                if (g_logdir[0]) /* show it on the card at once */
                    write_atomic(g_logdir, "mpcfm-status.txt", buf, b.len);
            }
        }
    }
    return NULL;
}

__attribute__((constructor)) static void mpcfm_init(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    if (strcmp(exe, MPCFM_TARGET_EXE) != 0)
        return; /* sh, setarch or a helper program: stay passive */

    const char *v = getenv("MPCFM_TMPDIR");
    snprintf(g_tmpdir, sizeof g_tmpdir, "%s", v && *v ? v : "/tmp/mpcfm");
    v = getenv("MPCFM_LOGDIR");
    if (v && *v)
        snprintf(g_logdir, sizeof g_logdir, "%s", v);
    v = getenv("LD_PRELOAD");
    snprintf(g_preload, sizeof g_preload, "%s", v ? v : "");

    int index = 0;
    dl_iterate_phdr(phdr_cb, &index);

    v = getenv("MPCFM_PROBE");
    probe_init(!(v && strcmp(v, "0") == 0));

    /* Before the app starts its threads: swap the sample voice methods. */
    v = getenv("MPCFM_HOOKS");
    if (v && strcmp(v, "0") == 0)
        snprintf(g_hook_msg, sizeof g_hook_msg, "off (MPCFM_HOOKS=0)");
    else if (!known_firmware())
        snprintf(g_hook_msg, sizeof g_hook_msg, "off: unknown app build");
    else
        g_hooks_on = vh_install(g_app_base, g_buildid, g_hook_msg, sizeof g_hook_msg);

    pthread_attr_t a;
    pthread_t th;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_create(&th, &a, logger_main, NULL); /* no logger: the probe still runs */
    pthread_attr_destroy(&a);
}
