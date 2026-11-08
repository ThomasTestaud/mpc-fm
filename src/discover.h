/* Where does the app keep the frames of a pad's sample? Found at run time.
 *
 * The stock sample data sits behind several objects (Sample -> provider ->
 * buffers), and the app reads it through callbacks. Instead of hard-coding
 * that chain, the logger thread searches memory reachable from a sample
 * generator for an FM carrier header (fm_carrier.h), with process_vm_readv:
 * a bad pointer only fails the read, it cannot crash the app. The chain of
 * offsets that leads to the header is the "path". Later note starts follow
 * the path (a few reads) to know at once if a sample is an FM carrier.
 */
#ifndef MPCFM_DISCOVER_H
#define MPCFM_DISCOVER_H

#include <stddef.h>
#include <stdint.h>

#include "fm/fm_engine.h"

#define DISC_MAX_HOPS 6

typedef struct {
    int hops;
    int off[DISC_MAX_HOPS]; /* p = *(p + off[i]) from the generator */
    int data_off;           /* the frames start at p + data_off */
    int fmt;                /* FM_FMT_* */
} disc_path_t;

/* Audio thread, at a note start. 1: an FM carrier, *p and *root set.
 * 0: not a carrier (or no path known yet: then queued for the logger). */
int disc_lookup(void *gen, fm_patch *p, int *root);

/* Logger thread: search the queued generators. key_off: offset of the
 * generator's sample object pointer (to tell its samples apart). Returns 1
 * and writes a line into msg when something new was learned. */
int disc_poll(uintptr_t image_lo, uintptr_t image_hi, int key_off, char *msg, size_t cap);

/* For the status file. */
int disc_paths(disc_path_t *out, int max);
void disc_describe(const disc_path_t *d, char *out, size_t cap);

/* Safe memory read (process_vm_readv). 0 on success. */
int disc_read(void *dst, uintptr_t addr, size_t n);

/* Tests: search from this object now (no queue). */
int disc_search(void *gen, uintptr_t image_lo, uintptr_t image_hi, disc_path_t *found, int *reads);

#endif
