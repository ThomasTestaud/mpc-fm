/* ALSA timing probe (phase 0: measure the real audio budget on the device).
 *
 * fm.so is in LD_PRELOAD, so these definitions come before libasound's.
 * Each one calls the real function (dlsym RTLD_NEXT) and records timing.
 * mpcqol.so (SampQOL) does not define these functions, so there is no
 * clash. If a later SampQOL defines them, its wrapper calls ours through
 * RTLD_NEXT and both still work.
 *
 * Rules for the code on the audio path: no locks, no malloc, no syscalls
 * after the first call of a thread (clock_gettime is a vDSO call).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"

#define EXPORT __attribute__((visibility("default")))
#define TLS __thread __attribute__((tls_model("initial-exec")))

typedef long sframes_t;          /* snd_pcm_sframes_t */
typedef unsigned long uframes_t; /* snd_pcm_uframes_t */

probe_slot g_probe[PROBE_SLOTS];
_Atomic uint64_t g_probe_epoch = 1;
const char *const probe_fn_name[FN_COUNT] = {
    "writei", "writen", "readi", "readn", "mmap_commit", "wait",
};

static int g_active;

static int (*real_open)(void **, const char *, int, int);
static int (*real_close)(void *);
static sframes_t (*real_writei)(void *, const void *, uframes_t);
static sframes_t (*real_writen)(void *, void **, uframes_t);
static sframes_t (*real_readi)(void *, void *, uframes_t);
static sframes_t (*real_readn)(void *, void **, uframes_t);
static sframes_t (*real_mmap_commit)(void *, uframes_t, uframes_t);
static int (*real_wait)(void *, int);
static int (*real_recover)(void *, int, int);

/* Resolved in probe_init(). In processes where fm.so stays passive (sh,
 * setarch, helpers) they resolve on first use instead. */
#define RESOLVE(var, name)                                                  \
    do {                                                                    \
        if (!(var))                                                         \
            *(void **)&(var) = dlsym(RTLD_NEXT, name);                      \
    } while (0)

void probe_init(int active)
{
    RESOLVE(real_open, "snd_pcm_open");
    RESOLVE(real_close, "snd_pcm_close");
    RESOLVE(real_writei, "snd_pcm_writei");
    RESOLVE(real_writen, "snd_pcm_writen");
    RESOLVE(real_readi, "snd_pcm_readi");
    RESOLVE(real_readn, "snd_pcm_readn");
    RESOLVE(real_mmap_commit, "snd_pcm_mmap_commit");
    RESOLVE(real_wait, "snd_pcm_wait");
    RESOLVE(real_recover, "snd_pcm_recover");
    g_active = active;
}

static inline uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- slots ---- */

static probe_slot *slot_find(void *pcm)
{
    for (int i = 0; i < PROBE_SLOTS; i++)
        if (atomic_load_explicit(&g_probe[i].pcm, memory_order_acquire) == pcm)
            return &g_probe[i];
    return NULL;
}

static probe_slot *slot_claim(void *pcm, int stream)
{
    for (int i = 0; i < PROBE_SLOTS; i++) {
        void *expect = NULL;
        if (atomic_compare_exchange_strong(&g_probe[i].pcm, &expect, pcm)) {
            atomic_store(&g_probe[i].stream, stream);
            return &g_probe[i];
        }
    }
    return NULL; /* table full: this handle is not measured */
}

static probe_slot *slot_get(void *pcm, int stream)
{
    probe_slot *s = slot_find(pcm);
    return s ? s : slot_claim(pcm, stream);
}

/* Publish the finished window (seqlock writer side) and start a new one. */
static void publish(probe_slot *s, uint64_t epoch)
{
    uint32_t q = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(s->done, s->cur, sizeof s->done);
    s->done_epoch = s->epoch;
    atomic_store_explicit(&s->seq, q + 2, memory_order_release);
    memset(s->cur, 0, sizeof s->cur);
    s->epoch = epoch;
}

int probe_snapshot(probe_slot *s, fn_window out[FN_COUNT], uint64_t *epoch)
{
    for (int tries = 0; tries < 20; tries++) {
        uint32_t q1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (!(q1 & 1)) {
            memcpy(out, s->done, sizeof s->done);
            uint64_t e = s->done_epoch;
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(&s->seq, memory_order_relaxed) == q1) {
                *epoch = e;
                return 1;
            }
        }
        struct timespec t = {0, 50000};
        nanosleep(&t, NULL);
    }
    return 0;
}

/* ---- per-call accounting (audio path) ---- */

static TLS uint64_t t_last_return;
static TLS void *t_pcm;
static TLS probe_slot *t_slot;
static TLS int t_tid;

static void account(void *pcm, int fn, int stream, uint64_t t0, uint64_t t1, long r)
{
    uint64_t prev = t_last_return;
    t_last_return = t1;

    probe_slot *s = t_slot;
    if (t_pcm != pcm || !s || atomic_load_explicit(&s->pcm, memory_order_relaxed) != pcm) {
        s = slot_get(pcm, stream);
        t_pcm = pcm;
        t_slot = s;
    }
    if (!s)
        return;
    if (!t_tid)
        t_tid = (int)syscall(SYS_gettid); /* once per thread */

    atomic_store_explicit(&s->tid, t_tid, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->total_calls, 1, memory_order_relaxed);
    if (r == -EPIPE || r == -ESTRPIPE)
        atomic_fetch_add_explicit(&s->xrun_results, 1, memory_order_relaxed);

    uint64_t epoch = atomic_load_explicit(&g_probe_epoch, memory_order_relaxed);
    if (s->epoch != epoch)
        publish(s, epoch);

    fn_window *w = &s->cur[fn];
    w->calls++;
    if (r > 0 && fn != FN_WAIT) {
        w->frames += (uint64_t)r;
        w->last_frames = (uint64_t)r;
    }
    uint64_t in = t1 - t0;
    w->in_sum_ns += in;
    if (in > w->in_max_ns)
        w->in_max_ns = in;

    if (prev && t0 >= prev) {
        uint64_t gap = t0 - prev;
        w->gap_n++;
        w->gap_sum_ns += gap;
        if (gap > w->gap_max_ns)
            w->gap_max_ns = gap;
        int b = 0;
        while (b < PROBE_HIST - 1 && gap >= probe_hist_max_ns[b])
            b++;
        w->gap_hist[b]++;
    }

    uint64_t last = s->last_entry_ns[fn];
    if (last && t0 > last && t0 - last > w->cycle_max_ns)
        w->cycle_max_ns = t0 - last;
    s->last_entry_ns[fn] = t0;
}

/* ---- the interposed ALSA functions ---- */

EXPORT int snd_pcm_open(void **pcmp, const char *name, int stream, int mode)
{
    RESOLVE(real_open, "snd_pcm_open");
    if (!real_open)
        return -ENOSYS;
    int r = real_open(pcmp, name, stream, mode);
    if (r == 0 && g_active && pcmp && *pcmp) {
        probe_slot *s = slot_get(*pcmp, stream);
        if (s) {
            /* Not atomic: the logger may print a torn name, nothing worse. */
            strncpy(s->name, name ? name : "?", sizeof s->name - 1);
            s->name[sizeof s->name - 1] = 0;
            atomic_store(&s->stream, stream);
            atomic_store(&s->closed, 0);
        }
    }
    return r;
}

EXPORT int snd_pcm_close(void *pcm)
{
    RESOLVE(real_close, "snd_pcm_close");
    if (!real_close)
        return -ENOSYS;
    if (g_active && pcm) {
        probe_slot *s = slot_find(pcm);
        if (s)
            atomic_store(&s->closed, 1);
    }
    return real_close(pcm);
}

#define TIMED(fn_id, stream, call)                                          \
    do {                                                                    \
        if (!g_active)                                                      \
            return call;                                                    \
        uint64_t t0 = now_ns();                                             \
        long r_ = (long)(call);                                             \
        account(pcm, fn_id, stream, t0, now_ns(), r_);                      \
        return r_;                                                          \
    } while (0)

EXPORT sframes_t snd_pcm_writei(void *pcm, const void *buf, uframes_t size)
{
    RESOLVE(real_writei, "snd_pcm_writei");
    if (!real_writei)
        return -ENOSYS;
    TIMED(FN_WRITEI, 0, real_writei(pcm, buf, size));
}

EXPORT sframes_t snd_pcm_writen(void *pcm, void **bufs, uframes_t size)
{
    RESOLVE(real_writen, "snd_pcm_writen");
    if (!real_writen)
        return -ENOSYS;
    TIMED(FN_WRITEN, 0, real_writen(pcm, bufs, size));
}

EXPORT sframes_t snd_pcm_readi(void *pcm, void *buf, uframes_t size)
{
    RESOLVE(real_readi, "snd_pcm_readi");
    if (!real_readi)
        return -ENOSYS;
    TIMED(FN_READI, 1, real_readi(pcm, buf, size));
}

EXPORT sframes_t snd_pcm_readn(void *pcm, void **bufs, uframes_t size)
{
    RESOLVE(real_readn, "snd_pcm_readn");
    if (!real_readn)
        return -ENOSYS;
    TIMED(FN_READN, 1, real_readn(pcm, bufs, size));
}

EXPORT sframes_t snd_pcm_mmap_commit(void *pcm, uframes_t offset, uframes_t frames)
{
    RESOLVE(real_mmap_commit, "snd_pcm_mmap_commit");
    if (!real_mmap_commit)
        return -ENOSYS;
    TIMED(FN_MMAP_COMMIT, -1, real_mmap_commit(pcm, offset, frames));
}

EXPORT int snd_pcm_wait(void *pcm, int timeout)
{
    RESOLVE(real_wait, "snd_pcm_wait");
    if (!real_wait)
        return -ENOSYS;
    TIMED(FN_WAIT, -1, real_wait(pcm, timeout));
}

EXPORT int snd_pcm_recover(void *pcm, int err, int silent)
{
    RESOLVE(real_recover, "snd_pcm_recover");
    if (!real_recover)
        return -ENOSYS;
    if (g_active && pcm) {
        probe_slot *s = slot_find(pcm);
        if (s)
            atomic_fetch_add_explicit(&s->recovers, 1, memory_order_relaxed);
    }
    return real_recover(pcm, err, silent);
}
