/* Edited patches as text files on the card ("MPC-FM-Patches/<name>.txt").
 *
 *   # mpc-fm patch
 *   name FM-Bell-C3
 *   root 60
 *   algorithm 5          (1..8, as on the FM page)
 *   feedback 0.2
 *   volume 1
 *   op1 ratio 1 fixed 0 detune 0 level 1 attack 0.001 decay 4 sustain 0 release 0.3 velocity 0.5
 *   op2 ...
 *
 * Numbers are written and read with '.' whatever the locale. Unknown words
 * are skipped; missing values keep the plain-sine default. */
#ifndef MPCFM_PATCHFILE_H
#define MPCFM_PATCHFILE_H

#include <stddef.h>

#include "fm/fm_engine.h"

/* Returns the length written (0: no room). */
size_t pf_format(const fm_patch *p, int root, const char *name, char *out, size_t cap);
/* 1: a patch file (sanitized *p, *root set). 0: not one. */
int pf_parse(const char *text, fm_patch *p, int *root);

#endif
