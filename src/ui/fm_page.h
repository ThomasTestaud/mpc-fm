/* The FM page: edit an FM patch with the MPC Sample's own controls.
 *
 *   B1  PATCH         K1 Algorithm  K2 Feedback  K3 Volume
 *   B2  OPERATOR n    K1 Ratio      K2 Level     K3 Detune   SHIFT+K1 Fixed freq
 *   B3  ENVELOPE n    K1 Attack     K2 Decay     K3 Sustain
 *   B2 / B3 again, the ENCODER or -/+: the next operator (on PATCH: the
 *   algorithm).
 *
 * Knobs take over "scaled" (as SampQOL's Parameter Takeover > Scaled): the
 * value moves from where it is, and value and knob reach the end together.
 * No device code here: events in, pixels out, so it is tested on the
 * computer. */
#ifndef MPCFM_FM_PAGE_H
#define MPCFM_FM_PAGE_H

#include <stdint.h>

#include "fm/fm_engine.h"

enum { PAGE_PATCH, PAGE_OP, PAGE_ENV, PAGE_COUNT };

typedef struct {
    fm_patch *patch;  /* the patch being edited (the voices play it live); NULL: none */
    char name[64];    /* its name, as shown */
    char status[48];  /* a short message on the name line ("saved", ...) */
    int page;
    int op;           /* 0..3 */
    int shift;        /* SHIFT held: the second row of knob parameters */
    int knob[3];      /* last knob positions 0..127 (-1: not known yet) */
    float acc[3];     /* knob takeover: the value 0..1 the knob is moving */
    int acc_id[3];    /* which parameter acc belongs to (-1: none) */
    int changed;      /* set at each edit (the caller clears it when saved) */
} fm_page;

void fm_page_init(fm_page *pg);
/* Edit another patch (NULL: none). Keeps the page and the operator. */
void fm_page_target(fm_page *pg, fm_patch *patch, const char *name);

/* Events. Return 1 if something on the screen changed. */
int fm_page_button(fm_page *pg, int which);   /* 0..2 = B1..B3 */
int fm_page_knob(fm_page *pg, int k, int v);  /* k 0..2 = K1..K3, v 0..127 */
int fm_page_encoder(fm_page *pg, int delta);  /* +1 / -1 (also -/+) */
int fm_page_shift(fm_page *pg, int on);

/* Draw the page into an XRGB8888 buffer of w x h (at least 320 x 240). */
void fm_page_draw(const fm_page *pg, uint32_t *px, int w, int h, int stride_px);

/* For the screen and the tests: the knob labels and values of the page.
 * label: NULL when the knob does nothing here. */
const char *fm_page_label(const fm_page *pg, int k);
void fm_page_value_text(const fm_page *pg, int k, char *out, int cap);
float fm_page_value01(const fm_page *pg, int k);

/* The ratio steps of the Ratio knob. */
int fm_ratio_steps(const float **steps);

#endif
