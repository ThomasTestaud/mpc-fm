/* FM as the sound source of a pad: hooks on the stock sample generator.
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

/* Logger thread, about once per second: learn where sample frames are
 * (discover.c). 1 and a line in msg when something was learned. */
int vh_poll(char *msg, size_t cap);

typedef struct {
    _Atomic uint64_t starts;        /* layer note starts seen */
    _Atomic uint64_t fm_starts;     /* of these, FM carriers */
    _Atomic uint64_t no_slot;       /* FM carrier but all FM voices busy */
    _Atomic uint64_t unsupported;   /* FM carrier on a stereo / time-stretch layer */
    _Atomic uint64_t bad_input;     /* stock gain not finite: dropped to stock */
    _Atomic uint64_t bad_output;    /* FM output not finite / too loud: dropped */
    _Atomic uint64_t fm_blocks;     /* render calls with FM */
    _Atomic uint64_t fm_ns;         /* time in the FM engine */
    _Atomic uint64_t fm_max_ns;     /* longest FM render of one call */
    _Atomic uint64_t lookup_max_ns; /* longest carrier check at a note start */
    _Atomic uint32_t last_ratio_milli; /* last pitch ratio x 1000 */
    _Atomic uint32_t stock_rms_micro;  /* last FM block: loudness of the stock output replaced */
    _Atomic uint32_t fm_rms_micro;     /* ... and of what we wrote (x 1e6) */
    _Atomic int last_root;
} vh_stats_t;

extern vh_stats_t vh_stats;

/* FM voices now. */
int vh_active_voices(void);

/* Test of the output path: FM voices write silence instead of FM. */
void vh_set_silent(int on);

/* Streamed samples (DiskSample): the logger thread reads the carrier
 * header from the sample's WAV file and registers it here. A note start
 * of that Sample object plays FM if its provider and its file-name field
 * are still the ones seen (so a reused object cannot match).
 * name_i: field of the Sample object holding the name pointer, or the
 * field holding an object whose field name_k holds it (name_k >= 0). */
int vh_disk_add(uintptr_t sample, uintptr_t provider, int name_i, uintptr_t name_obj, int name_k,
                uintptr_t name_ptr, const void *patch, int root);
int vh_disk_count(void);

/* One note start, for the logs (written by the audio thread). */
enum { VH_KIND_UNKNOWN, VH_KIND_MEMORY, VH_KIND_DISK, VH_KIND_RECORDED };
typedef struct {
    uintptr_t sample;      /* the Sample object */
    int kind;              /* VH_KIND_*: where the sample's audio comes from */
    uint32_t vt_off;       /* its provider's vtable (app offset) */
    int fm;                /* 1: played as FM */
    int algorithm;         /* if FM */
    unsigned char nib[8];  /* low nibbles of the first frames read (0xff: none) */
} vh_note_t;
/* Note starts since *cursor (at most max); moves the cursor. */
int vh_notes_since(uint64_t *cursor, vh_note_t *out, int max);
const char *vh_kind_name(int kind);

/* 1 while the thread runs our hook code, 2 while it runs the stock method
 * called from our hook, else 0 (for crash reports). */
extern __thread int vh_in_hook __attribute__((tls_model("initial-exec")));

typedef uintptr_t (*vh_fn)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                           uintptr_t, uintptr_t);

#ifdef MPCFM_TEST
/* Tests: fake provider vtables (the first word of a fake provider). */
#define VH_TEST_VT_MEMORY 0x5a5a0010u
#define VH_TEST_VT_DISK 0x5a5a0020u
/* Tests: use these as the stock methods and call the wrappers directly. */
void vh_test_setup(vh_fn start, vh_fn stop, vh_fn render, uintptr_t plain_mono);
uintptr_t vh_test_start(void *gen);
uintptr_t vh_test_stop(void *gen);
uintptr_t vh_test_render(void *gen, float *buf, int n);
#endif

#endif
