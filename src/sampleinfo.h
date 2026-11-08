#ifndef MPCFM_SAMPLEINFO_H
#define MPCFM_SAMPLEINFO_H

#include <stddef.h>
#include <stdint.h>

#include "fm/fm_engine.h"

/* Where a Sample object's file name is, and the name itself. */
typedef struct {
    int i, k;        /* Sample field i holds the text pointer; or, if k >= 0,
                        field i holds an object whose field k holds it */
    uintptr_t obj;   /* that object (k >= 0) */
    uintptr_t ptr;   /* the text */
    char path[256];  /* full text (a path when it has a '/') */
    char base[64];   /* file name only */
} name_loc;

/* Find the ".wav" text of a Sample object (safe reads). 1 if found. */
int sample_name_loc(uintptr_t sample, name_loc *out);
/* Just the file name, or "?". */
void sample_name(uintptr_t sample, char *out, size_t cap);
/* Read the carrier header from a WAV file (16/24/32-bit PCM or float,
 * first channel). 1: a carrier, *p and *root set; 0: not; -1: unreadable. */
int carrier_from_file(const char *path, fm_patch *p, int *root);

#endif
