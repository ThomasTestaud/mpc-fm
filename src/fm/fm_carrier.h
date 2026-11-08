/* FM carrier samples: a WAV that is a normal sample (a recording of the
 * patch) and also carries the patch, so mpc-fm can play it live.
 *
 * The header is hidden in the 4 lowest bits of the first FM_CARRIER_FRAMES
 * 16-bit frames (at most 15 LSB, -66 dB, for 7.6 ms). A stock MPC plays the
 * recording; with mpc-fm the pad plays the FM engine. Because the patch is
 * inside the sample data, it travels with the sample into projects.
 *
 * Layout (little endian), one nibble per frame, low nibble first:
 *   "MPFM", version u8, root MIDI note u8, reserved u16,
 *   patch (FM_PATCH_FLOATS floats), CRC-32 of everything before it.
 */
#ifndef MPCFM_FM_CARRIER_H
#define MPCFM_FM_CARRIER_H

#include <stdint.h>

#include "fm_engine.h"

#define FM_CARRIER_VERSION 1
#define FM_PATCH_FLOATS (FM_OPS * 9 + 3)
#define FM_CARRIER_BYTES (8 + FM_PATCH_FLOATS * 4 + 4)
#define FM_CARRIER_FRAMES (FM_CARRIER_BYTES * 2)

/* Write the header into pcm[0 .. FM_CARRIER_FRAMES-1] (keeps the upper 12
 * bits of each frame). Returns 0 if frames is too short. */
int fm_carrier_encode(const fm_patch *p, int root_note, int16_t *pcm, int frames);

/* Read it back from the float frames the app has in memory (int16 / 32768,
 * or / 32767). 1: valid, *p (sanitized) and *root_note set. 0: no header.
 * -1: the magic is there but the data is damaged. Real-time safe. */
int fm_carrier_decode(const float *frames, int count, fm_patch *p, int *root_note);

/* Quick test of the first frames only (the magic). */
int fm_carrier_has_magic(const float *frames, int count);

#endif
