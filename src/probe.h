/* ALSA timing probe: measures how the stock app drives its PCM devices.
 *
 * The audio thread writes the stats (one writer per slot, no locks, no
 * syscalls after its first call). The logger thread reads a published copy
 * once per window through a sequence counter.
 */
#ifndef MPCFM_PROBE_H
#define MPCFM_PROBE_H

#include <stdatomic.h>
#include <stdint.h>

#define PROBE_SLOTS 16
#define PROBE_HIST 8

enum { FN_WRITEI, FN_WRITEN, FN_READI, FN_READN, FN_MMAP_COMMIT, FN_WAIT, FN_COUNT };

/* Stats of one PCM function on one handle, for one window (about 1 s). */
typedef struct {
    uint64_t calls;
    uint64_t frames, last_frames;
    uint64_t in_sum_ns, in_max_ns;   /* time inside the call */
    uint64_t gap_n, gap_sum_ns, gap_max_ns;
    uint64_t cycle_max_ns;           /* entry to next entry */
    uint64_t gap_hist[PROBE_HIST];
} fn_window;

typedef struct {
    _Atomic(void *) pcm;             /* snd_pcm_t *; NULL = free slot */
    char name[64];
    _Atomic int stream;              /* 0 playback, 1 capture, -1 unknown */
    _Atomic int tid;                 /* last thread that used the handle */
    _Atomic int closed;
    _Atomic uint64_t total_calls;
    _Atomic uint64_t xrun_results;   /* a call returned -EPIPE / -ESTRPIPE */
    _Atomic uint64_t recovers;       /* snd_pcm_recover() calls */

    /* Owned by the thread that uses the handle. */
    uint64_t epoch;
    uint64_t last_entry_ns[FN_COUNT];
    fn_window cur[FN_COUNT];

    fn_window sess[FN_COUNT];          /* since the start */
    uint64_t sess_worst_epoch[FN_COUNT]; /* window with the largest gap */

    /* Published copy, guarded by seq (odd while it is written). */
    _Atomic uint32_t seq;
    uint64_t done_epoch;
    fn_window done[FN_COUNT];
    fn_window done_sess[FN_COUNT];
    uint64_t done_worst_epoch[FN_COUNT];
} probe_slot;

/* Gap is the time on a thread from the return of its previous PCM call to
 * this call. When the thread blocks in PCM calls, that is its processing
 * time. Upper bounds of the histogram buckets, in ns (last bucket: above). */
static const uint64_t probe_hist_max_ns[PROBE_HIST - 1] = {
    500000, 1000000, 1500000, 2000000, 2500000, 2900000, 5000000,
};

extern probe_slot g_probe[PROBE_SLOTS];
extern _Atomic uint64_t g_probe_epoch;
extern const char *const probe_fn_name[FN_COUNT];

/* Resolve the real ALSA functions. active = 0: pass calls through only. */
void probe_init(int active);

/* Copy the last published window of a slot, the totals since the start and
 * the window of the worst gap (epochs: about one per second). Returns 0 if
 * the slot stayed busy. */
typedef struct {
    fn_window win[FN_COUNT];
    fn_window sess[FN_COUNT];
    uint64_t worst_epoch[FN_COUNT];
    uint64_t epoch;
} probe_view;
int probe_snapshot(probe_slot *s, probe_view *out);

#endif
