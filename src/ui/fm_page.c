/* The FM page (see fm_page.h). */
#include "fm_page.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fonts.h"

/* ---- parameters ---- */

enum {
    P_NONE,
    P_ALGORITHM,
    P_FEEDBACK,
    P_VOLUME,
    P_RATIO,
    P_LEVEL,
    P_DETUNE,
    P_FIXED,
    P_ATTACK,
    P_DECAY,
    P_SUSTAIN,
};

static const char *const P_LABEL[] = {
    NULL, "Algorithm", "Feedback", "Volume", "Ratio", "Level", "Detune", "Fixed freq",
    "Attack", "Decay", "Sustain",
};

/* [page][shift][knob] */
static const int LAYOUT[PAGE_COUNT][2][3] = {
    [PAGE_PATCH] = {{P_ALGORITHM, P_FEEDBACK, P_VOLUME}, {P_NONE, P_NONE, P_NONE}},
    [PAGE_OP] = {{P_RATIO, P_LEVEL, P_DETUNE}, {P_FIXED, P_NONE, P_NONE}},
    [PAGE_ENV] = {{P_ATTACK, P_DECAY, P_SUSTAIN}, {P_NONE, P_NONE, P_NONE}},
};

static const float RATIOS[] = {0.0625f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f,
                               3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                               10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 18.0f,
                               20.0f, 22.0f, 24.0f, 28.0f, 32.0f};
#define NRATIOS ((int)(sizeof RATIOS / sizeof *RATIOS))

int fm_ratio_steps(const float **steps)
{
    *steps = RATIOS;
    return NRATIOS;
}

#define ATTACK_MIN 0.0005f
#define ATTACK_MAX 20.0f
#define DECAY_MIN 0.001f
#define DECAY_MAX 30.0f
#define FIXED_OFF 0.03f /* knob below this: no fixed frequency */
#define FIXED_MIN 10.0f
#define FIXED_MAX 10000.0f

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static float log_norm(float v, float lo, float hi)
{
    if (!(v > lo))
        return 0.0f;
    return clamp01(logf(v / lo) / logf(hi / lo));
}

static float log_value(float n, float lo, float hi) { return lo * powf(hi / lo, clamp01(n)); }

/* Round to 3 significant digits: the values read well and save exactly. */
static float round3(float v)
{
    if (!(v > 0.0f))
        return v;
    float e = powf(10.0f, floorf(log10f(v)) - 2.0f);
    return roundf(v / e) * e;
}

static int param_id(const fm_page *pg, int k)
{
    int page = pg->page >= 0 && pg->page < PAGE_COUNT ? pg->page : 0;
    return LAYOUT[page][pg->shift ? 1 : 0][k];
}

/* Where the value is, 0..1 (the bar and the knob takeover). */
static float get01(const fm_patch *p, int id, int op)
{
    const fm_op_params *o = &p->op[op];
    switch (id) {
    case P_ALGORITHM: return (float)p->algorithm / (FM_ALGORITHMS - 1);
    case P_FEEDBACK: return clamp01(p->feedback);
    case P_VOLUME: return clamp01(p->gain / 2.0f);
    case P_RATIO: {
        float r = o->ratio;
        if (r <= RATIOS[0])
            return 0.0f;
        for (int i = 0; i + 1 < NRATIOS; i++)
            if (r < RATIOS[i + 1]) /* between two steps: where, on a log scale */
                return ((float)i + logf(r / RATIOS[i]) / logf(RATIOS[i + 1] / RATIOS[i])) /
                       (NRATIOS - 1);
        return 1.0f;
    }
    case P_LEVEL: return clamp01(o->level);
    case P_DETUNE: return clamp01((o->detune + 100.0f) / 200.0f);
    case P_FIXED:
        if (!(o->fixed_hz > 0.0f))
            return 0.0f;
        return FIXED_OFF + (1.0f - FIXED_OFF) * log_norm(o->fixed_hz, FIXED_MIN, FIXED_MAX);
    case P_ATTACK: return log_norm(o->attack, ATTACK_MIN, ATTACK_MAX);
    case P_DECAY: return log_norm(o->decay, DECAY_MIN, DECAY_MAX);
    case P_SUSTAIN: return clamp01(o->sustain);
    default: return 0.0f;
    }
}

/* Set from 0..1. Each field gets one store of a value already in its
 * range: the audio thread may read the patch at any time. */
static void set01(fm_patch *p, int id, int op, float n)
{
    fm_op_params *o = &p->op[op];
    n = clamp01(n);
    switch (id) {
    case P_ALGORITHM: p->algorithm = (int)lrintf(n * (FM_ALGORITHMS - 1)); break;
    case P_FEEDBACK: p->feedback = roundf(n * 100.0f) / 100.0f; break;
    case P_VOLUME: p->gain = roundf(n * 200.0f) / 100.0f; break;
    case P_RATIO: o->ratio = RATIOS[lrintf(n * (NRATIOS - 1))]; break;
    case P_LEVEL: o->level = roundf(n * 100.0f) / 100.0f; break;
    case P_DETUNE: o->detune = roundf(n * 200.0f - 100.0f); break;
    case P_FIXED:
        o->fixed_hz = n < FIXED_OFF ? 0.0f
                                    : round3(log_value((n - FIXED_OFF) / (1.0f - FIXED_OFF),
                                                       FIXED_MIN, FIXED_MAX));
        break;
    case P_ATTACK: o->attack = round3(log_value(n, ATTACK_MIN, ATTACK_MAX)); break;
    case P_DECAY: o->decay = round3(log_value(n, DECAY_MIN, DECAY_MAX)); break;
    case P_SUSTAIN: o->sustain = roundf(n * 100.0f) / 100.0f; break;
    default: break;
    }
}

static void time_text(float s, char *out, int cap)
{
    if (s < 0.01f)
        snprintf(out, (size_t)cap, "%.1f ms", (double)s * 1000.0);
    else if (s < 1.0f)
        snprintf(out, (size_t)cap, "%d ms", (int)lrintf(s * 1000.0f));
    else
        snprintf(out, (size_t)cap, "%.2f s", (double)s);
}

static void ratio_text(float r, char *out, int cap)
{
    if (r < 1.0f && fabsf(1.0f / r - roundf(1.0f / r)) < 0.001f)
        snprintf(out, (size_t)cap, "1/%d", (int)lrintf(1.0f / r));
    else if (fabsf(r - roundf(r)) < 0.001f)
        snprintf(out, (size_t)cap, "x%d", (int)lrintf(r));
    else
        snprintf(out, (size_t)cap, "x%.2f", (double)r);
}

static void value_text(const fm_patch *p, int id, int op, char *out, int cap)
{
    const fm_op_params *o = &p->op[op];
    switch (id) {
    case P_ALGORITHM: snprintf(out, (size_t)cap, "%d", p->algorithm + 1); break;
    case P_FEEDBACK: snprintf(out, (size_t)cap, "%d%%", (int)lrintf(p->feedback * 100.0f)); break;
    case P_VOLUME: snprintf(out, (size_t)cap, "%d%%", (int)lrintf(p->gain * 100.0f)); break;
    case P_RATIO:
        if (o->fixed_hz > 0.0f)
            snprintf(out, (size_t)cap, "fixed");
        else
            ratio_text(o->ratio, out, cap);
        break;
    case P_LEVEL: snprintf(out, (size_t)cap, "%d", (int)lrintf(o->level * 100.0f)); break;
    case P_DETUNE: snprintf(out, (size_t)cap, "%+d ct", (int)lrintf(o->detune)); break;
    case P_FIXED:
        if (!(o->fixed_hz > 0.0f))
            snprintf(out, (size_t)cap, "off");
        else if (o->fixed_hz < 1000.0f)
            snprintf(out, (size_t)cap, "%.4g Hz", (double)o->fixed_hz);
        else
            snprintf(out, (size_t)cap, "%.3g kHz", (double)o->fixed_hz / 1000.0);
        break;
    case P_ATTACK: time_text(o->attack, out, cap); break;
    case P_DECAY: time_text(o->decay, out, cap); break;
    case P_SUSTAIN: snprintf(out, (size_t)cap, "%d%%", (int)lrintf(o->sustain * 100.0f)); break;
    default: out[0] = 0; break;
    }
}

const char *fm_page_label(const fm_page *pg, int k)
{
    if (k < 0 || k > 2)
        return NULL;
    return P_LABEL[param_id(pg, k)];
}

void fm_page_value_text(const fm_page *pg, int k, char *out, int cap)
{
    int id = k >= 0 && k <= 2 ? param_id(pg, k) : P_NONE;
    if (!pg->patch || id == P_NONE) {
        if (cap > 0)
            out[0] = 0;
        return;
    }
    value_text(pg->patch, id, pg->op, out, cap);
}

float fm_page_value01(const fm_page *pg, int k)
{
    int id = k >= 0 && k <= 2 ? param_id(pg, k) : P_NONE;
    return pg->patch && id != P_NONE ? get01(pg->patch, id, pg->op) : 0.0f;
}

/* ---- events ---- */

static void forget_takeover(fm_page *pg)
{
    for (int k = 0; k < 3; k++)
        pg->acc_id[k] = -1;
}

void fm_page_init(fm_page *pg)
{
    memset(pg, 0, sizeof *pg);
    pg->page = PAGE_PATCH;
    for (int k = 0; k < 3; k++)
        pg->knob[k] = -1;
    forget_takeover(pg);
}

void fm_page_target(fm_page *pg, fm_patch *patch, const char *name)
{
    pg->patch = patch;
    snprintf(pg->name, sizeof pg->name, "%s", name ? name : "");
    pg->status[0] = 0;
    forget_takeover(pg);
}

int fm_page_button(fm_page *pg, int which)
{
    static const int PAGE_OF[3] = {PAGE_PATCH, PAGE_OP, PAGE_ENV};
    if (which < 0 || which > 2)
        return 0;
    int to = PAGE_OF[which];
    if (pg->page == to && to != PAGE_PATCH)
        pg->op = (pg->op + 1) % FM_OPS; /* again: the next operator */
    pg->page = to;
    forget_takeover(pg);
    return 1;
}

int fm_page_encoder(fm_page *pg, int delta)
{
    if (delta == 0)
        return 0;
    if (pg->page == PAGE_PATCH) {
        if (!pg->patch)
            return 0;
        int a = pg->patch->algorithm + (delta > 0 ? 1 : -1);
        if (a < 0 || a >= FM_ALGORITHMS)
            return 0;
        pg->patch->algorithm = a;
        pg->changed = 1;
        forget_takeover(pg);
        return 1;
    }
    int op = pg->op + (delta > 0 ? 1 : -1);
    if (op < 0 || op >= FM_OPS)
        return 0;
    pg->op = op;
    forget_takeover(pg);
    return 1;
}

int fm_page_shift(fm_page *pg, int on)
{
    on = on != 0;
    if (pg->shift == on)
        return 0;
    pg->shift = on;
    forget_takeover(pg);
    /* redraw only if the page has a second row */
    for (int k = 0; k < 3; k++)
        if (LAYOUT[pg->page][1][k] != P_NONE)
            return 1;
    return 0;
}

int fm_page_knob(fm_page *pg, int k, int v)
{
    if (k < 0 || k > 2 || v < 0 || v > 127)
        return 0;
    int from = pg->knob[k];
    pg->knob[k] = v;
    int id = param_id(pg, k);
    if (!pg->patch || id == P_NONE || from < 0 || from == v)
        return 0;
    int key = (pg->page * 2 + (pg->shift ? 1 : 0)) * 16 + pg->op * 4 + k;
    if (pg->acc_id[k] != key) { /* first move of this knob on this parameter */
        pg->acc_id[k] = key;
        pg->acc[k] = get01(pg->patch, id, pg->op);
    }
    float x = pg->acc[k];
    if (v > from)
        x += (float)(v - from) * (1.0f - x) / (float)(127 - from);
    else
        x -= (float)(from - v) * x / (float)from;
    if (v == 127)
        x = 1.0f;
    if (v == 0)
        x = 0.0f;
    pg->acc[k] = clamp01(x);
    char before[24], after[24];
    value_text(pg->patch, id, pg->op, before, sizeof before);
    float n0 = get01(pg->patch, id, pg->op);
    set01(pg->patch, id, pg->op, pg->acc[k]);
    value_text(pg->patch, id, pg->op, after, sizeof after);
    int moved = strcmp(before, after) != 0 || get01(pg->patch, id, pg->op) != n0;
    if (moved)
        pg->changed = 1;
    return moved;
}

/* ---- drawing ---- */

#define C_BG 0x101216u
#define C_PANEL 0x1b1e24u
#define C_LINE 0x5a616bu
#define C_TEXT 0xe8e8e8u
#define C_DIM 0x8b929cu
#define C_ACCENT 0xff7a1au
#define C_ACCENT_DK 0x5a2c0au
#define C_CARRIER 0x46b4ffu

typedef struct {
    uint32_t *px;
    int w, h, stride;
} canvas;

static void fill(canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > c->w)
        x1 = c->w;
    if (y1 > c->h)
        y1 = c->h;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++)
            c->px[(size_t)j * (size_t)c->stride + (size_t)i] = rgb;
}

static void plot(canvas *c, int x, int y, uint32_t rgb)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h)
        c->px[(size_t)y * (size_t)c->stride + (size_t)x] = rgb;
}

static void line(canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(c, x0, y0, rgb);
        plot(c, x0 + 1, y0, rgb); /* 2 px wide: reads better on the small screen */
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void frame(canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    fill(c, x, y, w, 1, rgb);
    fill(c, x, y + h - 1, w, 1, rgb);
    fill(c, x, y, 1, h, rgb);
    fill(c, x + w - 1, y, 1, h, rgb);
}

/* Text in font 13 (big = 0) or 18 (big = 1). Returns the width. */
static int text(canvas *c, int x, int y, const char *s, uint32_t rgb, int big)
{
    int fw = big ? FONT18_W : FONT13_W, fh = big ? FONT18_H : FONT13_H;
    int x0 = x;
    for (; *s; s++, x += fw) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126)
            ch = '?';
        const unsigned char *g = big ? font18_rows[ch - 32] : font13_rows[ch - 32];
        for (int r = 0; r < fh; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b))
                    plot(c, x + b, y + r, rgb);
    }
    return x - x0;
}

static int text_w(const char *s, int big) { return (int)strlen(s) * (big ? FONT18_W : FONT13_W); }

static void text_c(canvas *c, int cx, int y, const char *s, uint32_t rgb, int big)
{
    text(c, cx - text_w(s, big) / 2, y, s, rgb, big);
}

/* Where each operator sits in the algorithm picture: column (in halves,
 * 0..6) and row (0..3, row 3: the carriers). */
static const signed char ALG_POS[FM_ALGORITHMS][FM_OPS][2] = {
    {{3, 3}, {3, 2}, {3, 1}, {3, 0}}, /* 4>3>2>1 */
    {{3, 3}, {3, 2}, {4, 1}, {2, 1}}, /* (4+3)>2>1 */
    {{3, 3}, {4, 2}, {2, 2}, {2, 1}}, /* (4>3)+2>1 */
    {{4, 3}, {2, 3}, {2, 2}, {2, 1}}, /* 4>3>2, 1 */
    {{2, 3}, {2, 2}, {4, 3}, {4, 2}}, /* 4>3, 2>1 */
    {{1, 3}, {3, 3}, {5, 3}, {3, 2}}, /* 4>(3,2,1) */
    {{1, 3}, {3, 3}, {5, 3}, {5, 2}}, /* 4>3, 2, 1 */
    {{0, 3}, {2, 3}, {4, 3}, {6, 3}}, /* 4, 3, 2, 1 */
};
/* ALG_MOD[a][i]: bit j = operator j+1 modulates operator i+1 (as the engine). */
static const unsigned char ALG_MOD[FM_ALGORITHMS][FM_OPS] = {
    {2, 4, 8, 0}, {2, 12, 0, 0}, {6, 0, 8, 0}, {0, 4, 8, 0},
    {2, 0, 8, 0}, {8, 8, 8, 0}, {0, 0, 8, 0}, {0, 0, 0, 0},
};

#define BOX_W 34
#define BOX_H 20
#define ALG_TOP 52
#define ALG_ROW 28

static void op_center(int a, int i, int *x, int *y)
{
    *x = 160 + (ALG_POS[a][i][0] - 3) * 27;
    *y = ALG_TOP + ALG_POS[a][i][1] * ALG_ROW + BOX_H / 2;
}

static void draw_algorithm(canvas *c, const fm_page *pg)
{
    const fm_patch *p = pg->patch;
    int a = p->algorithm >= 0 && p->algorithm < FM_ALGORITHMS ? p->algorithm : 0;
    int carriers = fm_algorithm_carriers(a);
    int out_y = ALG_TOP + 3 * ALG_ROW + BOX_H + 8;
    int xmin = 999, xmax = -999;
    for (int i = 0; i < FM_OPS; i++) { /* lines first, boxes over them */
        int x, y;
        op_center(a, i, &x, &y);
        for (int j = 0; j < FM_OPS; j++)
            if (ALG_MOD[a][i] & (1 << j)) {
                int mx, my;
                op_center(a, j, &mx, &my);
                line(c, mx, my + BOX_H / 2, x, y - BOX_H / 2, C_LINE);
            }
        if (carriers & (1 << i)) {
            line(c, x, y + BOX_H / 2, x, out_y, C_LINE);
            xmin = x < xmin ? x : xmin;
            xmax = x > xmax ? x : xmax;
        }
    }
    fill(c, xmin - 1, out_y, xmax - xmin + 3, 2, C_LINE);
    for (int i = 0; i < FM_OPS; i++) {
        int x, y;
        op_center(a, i, &x, &y);
        int bx = x - BOX_W / 2, by = y - BOX_H / 2;
        int sel = pg->page != PAGE_PATCH && pg->op == i;
        int carrier = (carriers >> i) & 1;
        uint32_t edge = sel ? C_ACCENT : carrier ? C_CARRIER : C_LINE;
        fill(c, bx, by, BOX_W, BOX_H, sel ? C_ACCENT_DK : C_PANEL);
        frame(c, bx, by, BOX_W, BOX_H, edge);
        if (sel)
            frame(c, bx + 1, by + 1, BOX_W - 2, BOX_H - 2, edge);
        char n[4];
        snprintf(n, sizeof n, "%d", i + 1);
        text(c, bx + 5, by + 4, n, sel ? C_TEXT : C_DIM, 0);
        /* its level, as a small bar on the right */
        float lv = p->op[i].level;
        int bh = (int)lrintf((BOX_H - 6) * (lv < 0 ? 0 : lv > 1 ? 1 : lv));
        fill(c, bx + BOX_W - 8, by + 3, 4, BOX_H - 6, C_BG);
        fill(c, bx + BOX_W - 8, by + BOX_H - 3 - bh, 4, bh, carrier ? C_CARRIER : C_ACCENT);
        if (i == FM_OPS - 1 && p->feedback > 0.0f) { /* feedback: a loop on its right */
            int fx = bx + BOX_W, fy = by + BOX_H / 2;
            fill(c, fx, fy, 8, 2, C_ACCENT);
            fill(c, fx + 6, by - 4, 2, BOX_H / 2 + 6, C_ACCENT);
            fill(c, x, by - 4, fx + 8 - x, 2, C_ACCENT);
        }
    }
    char t[40];
    snprintf(t, sizeof t, "ALG %d", a + 1);
    text(c, 8, ALG_TOP, t, C_DIM, 0);
    text(c, 8, ALG_TOP + 16, fm_algorithm_name(a), C_DIM, 0);
}

void fm_page_draw(const fm_page *pg, uint32_t *px, int w, int h, int stride_px)
{
    canvas cv = {px, w < 320 ? w : 320, h < 240 ? h : 240, stride_px}, *c = &cv;
    fill(c, 0, 0, 320, 240, C_BG);

    /* tabs, under B1 B2 B3 */
    char tab[3][24];
    int opn = pg->op >= 0 && pg->op < FM_OPS ? pg->op + 1 : 1;
    snprintf(tab[0], sizeof tab[0], "PATCH");
    snprintf(tab[1], sizeof tab[1], "OPERATOR %d", opn);
    snprintf(tab[2], sizeof tab[2], "ENVELOPE %d", opn);
    for (int t = 0; t < 3; t++) {
        int x = t * 107, on = pg->page == t;
        fill(c, x + 2, 0, 103, 22, on ? C_ACCENT : C_PANEL);
        text_c(c, x + 53, 5, tab[t], on ? C_BG : C_DIM, 0);
    }

    /* name line */
    text(c, 8, 28, "FM", C_ACCENT, 1);
    int room = 312 - 30 - (pg->status[0] ? text_w(pg->status, 0) + 8 : 0);
    char nm[sizeof pg->name];
    snprintf(nm, sizeof nm, "%s", pg->patch ? pg->name : "");
    int fit = room / FONT18_W;
    if (fit >= 0 && fit < (int)sizeof nm && (int)strlen(nm) > fit)
        nm[fit] = 0;
    text(c, 30, 28, nm, C_TEXT, 1);
    if (pg->status[0])
        text(c, 312 - text_w(pg->status, 0), 31, pg->status, C_DIM, 0);
    fill(c, 0, 48, 320, 1, C_PANEL);

    if (!pg->patch) {
        text_c(c, 160, 100, "Play an FM pad", C_TEXT, 1);
        text_c(c, 160, 124, "(a sample made by mpc-fm, FM-...)", C_DIM, 0);
        text_c(c, 160, 200, "SHIFT + NOTE REPEAT: close", C_DIM, 0);
        return;
    }
    draw_algorithm(c, pg);

    /* knobs, over K1 K2 K3 */
    fill(c, 0, 170, 320, 1, C_PANEL);
    int any_shift = 0;
    for (int k = 0; k < 3; k++)
        any_shift |= LAYOUT[pg->page][1][k] != P_NONE;
    for (int k = 0; k < 3; k++) {
        int x = k * 107 + 53;
        const char *l = fm_page_label(pg, k);
        if (!l)
            continue;
        char v[24];
        fm_page_value_text(pg, k, v, sizeof v);
        text_c(c, x, 176, l, pg->shift ? C_ACCENT : C_DIM, 0);
        text_c(c, x, 194, v, C_TEXT, 1);
        float n = fm_page_value01(pg, k);
        fill(c, x - 45, 220, 90, 6, C_PANEL);
        fill(c, x - 45, 220, (int)lrintf(90.0f * n), 6, C_ACCENT);
    }
    if (any_shift && !pg->shift)
        text(c, 312 - text_w("SHIFT: more", 0), 228, "SHIFT: more", C_DIM, 0);
}
