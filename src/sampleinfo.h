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

/* ---- the patch library: the FM files on the card ----
 * A pad's copy of an FM file can lose its hidden patch (the MPC rewrites
 * samples when it saves a project). The original files on the card keep
 * it: index them, and find a patch by file name. */

/* Index the carriers under these folders (depth <= 3). Returns how many. */
int library_scan(const char *const *dirs, int ndirs);
int library_count(void);
/* The patch for a sample file name ("FM-Bass-C3002.wav" finds
 * "FM-Bass-C3.wav": the MPC adds 3 digits to copies). An edited patch
 * (FM page) comes before the file. 0: not found; 1: from the file; 2: the
 * edited one. found: the path it came from. */
int library_find(const char *name, fm_patch *p, int *root, char *found, size_t cap);

/* ---- patches edited on the FM page: <dir>/<name>.txt (patchfile.h) ----
 * Scanned with the library. Thread-safe (the logger and the FM page). */
void library_set_edit_dir(const char *dir);
int library_edit_count(void);
/* Changes at each save: samples seen before may need the new patch. */
unsigned library_edit_generation(void);
/* The name a sample's edits are saved under: the library's name for it
 * (copy digits removed) if it has one, else the file name without .wav. */
void library_canonical(const char *name, char *out, size_t cap);
/* Write the file and use it from now on. 1: saved (msg: the path); 0: msg
 * says why not. */
int library_save_edit(const char *name, const fm_patch *p, int root, char *msg, size_t cap);

/* The first frames of a WAV file as floats (first channel), and its format.
 * Returns the number of frames read (0: not a WAV we can read). */
typedef struct {
    int format, channels, bits, rate;
    long frames;
} wav_info;
int wav_read_head(const char *path, float *out, int n, wav_info *info);

#endif
