/* FM as the sound source of a pad: hooks on the stock sample voice.
 * See voice_hook.c for how it works and what it relies on. */
#ifndef MPCFM_VOICE_HOOK_H
#define MPCFM_VOICE_HOOK_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Install the hooks on the app loaded at `base`, for the app build
 * `build_id`. 1: installed. 0: not (reason in msg). Call it before the app
 * starts its threads (from the constructor). */
int vh_install(uintptr_t base, const char *build_id, char *msg, size_t cap);

typedef struct {
    _Atomic uint64_t starts;      /* sample voice starts seen */
    _Atomic uint64_t fm_starts;   /* of these, FM carriers */
    _Atomic uint64_t bad_headers; /* magic found, data damaged */
    _Atomic uint64_t no_slot;     /* FM carrier but all FM voices busy */
    _Atomic uint64_t fm_blocks;   /* process calls rendered with FM */
    _Atomic uint64_t fm_ns;       /* time in the FM engine */
    _Atomic uint64_t fm_max_ns;   /* longest FM render of one call (since start) */
    _Atomic int active;           /* FM voices now */
    _Atomic uint32_t last_ratio_milli; /* last pitch ratio x 1000 */
    _Atomic int last_root;
    _Atomic int last_first[8]; /* first frames x 32768 of the last sample started */
} vh_stats_t;

extern vh_stats_t vh_stats;

#ifdef MPCFM_TEST
/* Tests: use these as the stock methods and call the wrappers directly. */
typedef uintptr_t (*vh_fn)(void *, uintptr_t, uintptr_t, uintptr_t);
void vh_test_setup(vh_fn start, vh_fn stop, vh_fn process);
uintptr_t vh_test_start(void *self, uintptr_t a1);
uintptr_t vh_test_stop(void *self);
uintptr_t vh_test_process(void *self, float **out, int start, int n);
#endif

#endif
