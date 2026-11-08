/* Host tests of the FM page model, and pictures of it (PPM files) to look
 * at: build/host/page-*.ppm. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm/fm_presets.h"
#include "ui/fm_page.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "ok   " : "FAIL "); if (!ok_) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t px[240 * 320];

static void save_ppm(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/page-%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n320 240\n255\n");
    for (int i = 0; i < 320 * 240; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8),
                                (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int finite_patch(const fm_patch *p)
{
    fm_patch q = *p;
    fm_patch_sanitize(&q);
    return memcmp(&q, p, sizeof q) == 0; /* sanitize changes nothing: all in range */
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    fm_page pg;
    fm_patch p;
    fm_preset_get(fm_preset_find("Bell"), &p);
    fm_page_init(&pg);

    /* no patch: a message, knobs do nothing */
    fm_page_draw(&pg, px, 320, 240, 320);
    save_ppm(dir, "empty");
    CHECK(fm_page_knob(&pg, 0, 10) == 0 && fm_page_knob(&pg, 0, 50) == 0, "no patch: knobs do nothing");

    fm_page_target(&pg, &p, "FM-Bell-C3");
    CHECK(pg.page == PAGE_PATCH, "starts on PATCH");
    CHECK(!strcmp(fm_page_label(&pg, 0), "Algorithm") && !strcmp(fm_page_label(&pg, 2), "Volume"),
          "PATCH: Algorithm .. Volume");

    /* takeover: the first message only tells where the knob is */
    pg.knob[0] = pg.knob[1] = pg.knob[2] = -1;
    int alg0 = p.algorithm;
    CHECK(fm_page_knob(&pg, 0, 100) == 0 && p.algorithm == alg0, "first knob message: no jump");
    fm_page_knob(&pg, 0, 127);
    CHECK(p.algorithm == FM_ALGORITHMS - 1, "knob at the end: last algorithm (%d)", p.algorithm + 1);
    fm_page_knob(&pg, 0, 0);
    CHECK(p.algorithm == 0, "knob at 0: algorithm 1");
    /* stepped values still move with small turns (accumulated) */
    int moved = 0;
    for (int v = 1; v <= 40; v++)
        moved |= fm_page_knob(&pg, 0, v);
    CHECK(moved && p.algorithm > 0, "small turns step the algorithm (now %d)", p.algorithm + 1);

    /* scaled: from the middle of the knob, the value moves from where it is */
    pg.knob[1] = 64;
    p.feedback = 0.2f;
    fm_page_knob(&pg, 1, 65);
    CHECK(p.feedback > 0.2f && p.feedback < 0.25f, "feedback moves from 0.20 (now %.2f)", p.feedback);
    for (int v = 66; v <= 127; v++)
        fm_page_knob(&pg, 1, v);
    CHECK(p.feedback == 1.0f, "value and knob reach the end together");

    /* operator pages */
    fm_page_button(&pg, 1);
    CHECK(pg.page == PAGE_OP && pg.op == 0, "B2: OPERATOR 1");
    fm_page_button(&pg, 1);
    CHECK(pg.op == 1, "B2 again: OPERATOR 2");
    fm_page_encoder(&pg, 1);
    CHECK(pg.op == 2, "encoder +1: OPERATOR 3");
    fm_page_encoder(&pg, 1);
    fm_page_encoder(&pg, 1);
    CHECK(pg.op == 3, "encoder stops at operator 4");
    fm_page_button(&pg, 2);
    CHECK(pg.page == PAGE_ENV && pg.op == 3, "B3: ENVELOPE of the same operator");
    fm_page_button(&pg, 2);
    CHECK(pg.op == 0, "B3 again wraps to operator 1");
    fm_page_button(&pg, 1);

    /* ratio: musical steps */
    pg.knob[0] = 64;
    float r0 = p.op[0].ratio;
    for (int v = 65; v <= 100; v++)
        fm_page_knob(&pg, 0, v);
    const float *steps;
    int ns = fm_ratio_steps(&steps), on_step = 0;
    for (int i = 0; i < ns; i++)
        on_step |= p.op[0].ratio == steps[i];
    CHECK(p.op[0].ratio > r0 && on_step, "ratio goes up in steps (%.3f -> %.3f)", r0, p.op[0].ratio);
    char t[32];
    fm_page_value_text(&pg, 0, t, sizeof t);
    printf("     ratio shows \"%s\"\n", t);

    /* SHIFT: the fixed frequency */
    CHECK(fm_page_shift(&pg, 1) == 1 && !strcmp(fm_page_label(&pg, 0), "Fixed freq"),
          "SHIFT on OPERATOR: K1 = Fixed freq");
    CHECK(fm_page_label(&pg, 1) == NULL, "SHIFT: K2 does nothing here");
    pg.knob[0] = 0;
    fm_page_knob(&pg, 0, 1);
    CHECK(p.op[0].fixed_hz == 0.0f, "fixed freq stays off at the bottom");
    for (int v = 2; v <= 64; v++)
        fm_page_knob(&pg, 0, v);
    fm_page_value_text(&pg, 0, t, sizeof t);
    CHECK(p.op[0].fixed_hz > 10.0f && p.op[0].fixed_hz < 10000.0f, "fixed freq on: %s", t);
    fm_page_draw(&pg, px, 320, 240, 320);
    save_ppm(dir, "op-shift");
    for (int v = 63; v >= 0; v--)
        fm_page_knob(&pg, 0, v);
    CHECK(p.op[0].fixed_hz == 0.0f, "back to off at 0");
    fm_page_shift(&pg, 0);

    /* envelope times: log scale, readable values */
    fm_page_button(&pg, 2);
    pg.knob[1] = 30;
    for (int v = 31; v <= 90; v++)
        fm_page_knob(&pg, 1, v);
    fm_page_value_text(&pg, 1, t, sizeof t);
    CHECK(p.op[0].decay >= 0.001f && p.op[0].decay <= 30.0f, "decay %s", t);
    CHECK(finite_patch(&p), "every edited value is in range (sanitize changes nothing)");

    /* every page, every knob, every position: values always in range */
    for (int page = 0; page < 3; page++)
        for (int sh = 0; sh < 2; sh++)
            for (int op = 0; op < FM_OPS; op++)
                for (int k = 0; k < 3; k++) {
                    fm_page_button(&pg, page);
                    pg.page = page;
                    pg.op = op;
                    fm_page_shift(&pg, sh);
                    pg.knob[k] = 0;
                    for (int v = 0; v <= 127; v += 7)
                        fm_page_knob(&pg, k, v);
                    fm_page_knob(&pg, k, 127);
                    for (int v = 127; v >= 0; v -= 5)
                        fm_page_knob(&pg, k, v);
                }
    CHECK(finite_patch(&p), "sweeping every knob keeps the patch in range");

    /* pictures */
    fm_preset_get(fm_preset_find("Bell"), &p);
    fm_page_target(&pg, &p, "FM-Bell-C3");
    pg.shift = 0;
    for (int a = 0; a < FM_ALGORITHMS; a++) {
        char n[16];
        p.algorithm = a;
        pg.page = PAGE_PATCH;
        fm_page_draw(&pg, px, 320, 240, 320);
        snprintf(n, sizeof n, "alg%d", a + 1);
        save_ppm(dir, n);
    }
    fm_preset_get(fm_preset_find("Bell"), &p);
    pg.page = PAGE_OP;
    pg.op = 1;
    snprintf(pg.status, sizeof pg.status, "saved");
    fm_page_draw(&pg, px, 320, 240, 320);
    save_ppm(dir, "op");
    pg.page = PAGE_ENV;
    pg.status[0] = 0;
    fm_page_draw(&pg, px, 320, 240, 320);
    save_ppm(dir, "env");
    fm_page_target(&pg, &p, "FM-A-Very-Long-Sample-Name-That-Does-Not-Fit-C3");
    snprintf(pg.status, sizeof pg.status, "not saved: no name");
    fm_page_draw(&pg, px, 320, 240, 320);
    save_ppm(dir, "longname");
    CHECK(1, "pictures in %s/page-*.ppm", dir);

    printf(fails ? "\n%d FAILED\n" : "\nall page tests passed\n", fails);
    return fails != 0;
}
