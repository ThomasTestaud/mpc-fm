/* Host tests of the FM page plumbing: the control-surface filter, the
 * screen swap (fake libdrm functions), the patch files and the edited
 * patches in the library. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fm/fm_presets.h"
#include "patchfile.h"
#include "sampleinfo.h"
#include "ui/display.h"
#include "ui/surface.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "ok   " : "FAIL "); if (!ok_) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- surface ---- */

static surf_stream st;

/* Feed bytes; returns how many were kept, the kept bytes in out. */
static size_t feed(const uint8_t *in, size_t n, uint8_t *out)
{
    memcpy(out, in, n);
    return surf_filter(&st, out, n, 1000);
}

#define FEED(...)                                                    \
    ({                                                               \
        static const uint8_t in_[] = {__VA_ARGS__};                  \
        feed(in_, sizeof in_, kept);                                 \
    })

static int next_type(surf_event *e)
{
    if (!surf_next_event(e))
        return 0;
    return e->type;
}

static int drain(void)
{
    surf_event e;
    int n = 0;
    while (surf_next_event(&e))
        n++;
    return n;
}

static void test_surface(void)
{
    uint8_t kept[256];
    surf_event e;
    surf_test_reset();
    memset(&st, 0, sizeof st);

    /* closed: everything passes, knob positions are learned */
    CHECK(FEED(0xb0, 16, 40, 0x90, 12, 0x7f, 0x80, 12, 0) == 9, "closed: K1 and B1 pass");
    CHECK(surf_knob_pos(0) == 40, "knob position learned while closed");
    drain();

    /* not ready (no screen): SHIFT + NOTE REPEAT stays stock */
    CHECK(FEED(0x90, 49, 0x7f, 0x90, 11, 0x7f, 0x80, 11, 0, 0x80, 49, 0) == 12 && !surf_page_open(),
          "no screen: SHIFT + NOTE REPEAT goes to the app");
    drain();

    surf_set_ready(1);
    CHECK(FEED(0x90, 49, 0x7f) == 3, "SHIFT passes");
    CHECK(FEED(0x90, 11, 0x7f) == 0 && surf_page_open(), "SHIFT + NOTE REPEAT: taken, page open");
    CHECK(next_type(&e) == SEV_OPEN && ((e.b & 0xff) - 1) == 40 && ((e.b >> 8) & 0xff) == 0,
          "open event carries the knob positions (K1 40, K2 unknown)");
    CHECK(FEED(0x80, 11, 0x00) == 0, "its release is taken too");
    CHECK(FEED(0x80, 49, 0x00) == 3, "SHIFT release passes");
    drain();

    /* open */
    CHECK(FEED(0xb0, 16, 41) == 0 && next_type(&e) == SEV_KNOB && e.a == 0 && e.b == 41,
          "K1 goes to the page");
    CHECK(FEED(0xb0, 18, 3) == 0 && next_type(&e) == SEV_KNOB && e.a == 2, "K3 goes to the page");
    CHECK(FEED(0xb0, 100, 0x01) == 0 && next_type(&e) == SEV_ENCODER && e.b == 1, "encoder +1");
    CHECK(FEED(0xb0, 100, 0x7f) == 0 && next_type(&e) == SEV_ENCODER && e.b == -1, "encoder -1");
    CHECK(FEED(0x90, 13, 0x7f, 0x80, 13, 0) == 0 && next_type(&e) == SEV_BUTTON && e.a == 1,
          "B2 press and release: taken");
    CHECK(FEED(0x90, 54, 0x7f, 0x80, 54, 0) == 0 && next_type(&e) == SEV_ENCODER && e.b == 1,
          "PLUS: next operator");
    CHECK(FEED(0x90, 111, 0x7f, 0x80, 111, 0) == 0 && next_type(&e) == SEV_PUSH, "encoder push taken");
    CHECK(FEED(0xb0, 19, 90) == 3 && FEED(0xb0, 20, 64) == 3, "MAIN VOLUME and fader pass");
    CHECK(FEED(0x99, 37, 100, 0xa9, 37, 20, 0x89, 37, 0) == 9 && next_type(&e) == SEV_PAD &&
              e.a == 37 && e.b == 100,
          "pad hit passes (and is told to the page)");
    CHECK(FEED(0x90, 82, 0x7f, 0x80, 82, 0) == 6 && surf_page_open(), "PLAY passes, page stays");
    static const uint8_t sx[] = {0xf0, 0x47, 0x7f, 0x5c, 0x50, 0x00, 0x10, 1, 2, 3, 4, 5, 6, 7, 8,
                                 9, 10, 11, 12, 13, 14, 15, 16, 0xf7};
    size_t k = feed(sx, sizeof sx, kept);
    CHECK(k == sizeof sx && !memcmp(kept, sx, sizeof sx) && st.akai_sysex,
          "SysEx passes untouched (authentication), seen as AC50");
    CHECK(FEED(0xb0, 0xf8, 16, 50) == 1 && kept[0] == 0xf8, "real-time byte inside a message passes");
    drain();
    /* split across two reads: passes whole */
    CHECK(FEED(0xb0, 16) == 2 && FEED(60) == 1, "message split across reads passes");
    CHECK(!surf_next_event(&e), "and is not taken");
    /* a held button across the close */
    CHECK(FEED(0x90, 12, 0x7f) == 0, "B1 pressed (taken)");
    CHECK(FEED(0x90, 6, 0x7f) == 3 && !surf_page_open(), "SAMPLE: passes and closes the page");
    int sawclose = 0;
    while (surf_next_event(&e))
        sawclose |= e.type == SEV_CLOSE && e.a == 1;
    CHECK(sawclose, "close event (by another button)");
    CHECK(FEED(0x80, 6, 0) == 3, "SAMPLE release passes");
    CHECK(FEED(0x80, 12, 0) == 0, "B1 release taken (its press was)");
    CHECK(FEED(0xb0, 16, 70) == 3, "closed again: K1 passes");

    /* the UI asks to close */
    FEED(0x90, 49, 0x7f, 0x90, 11, 0x7f, 0x80, 11, 0, 0x80, 49, 0);
    CHECK(surf_page_open(), "open again");
    drain();
    surf_request_close();
    CHECK(FEED(0xb0, 17, 5) == 3 && !surf_page_open(), "UI close request: the next message closes");
    CHECK(next_type(&e) == SEV_CLOSE && e.a == 0, "close event (asked)");
    /* running status data and other channels pass */
    CHECK(FEED(0x91, 12, 0x7f, 0xb1, 16, 1) == 6, "other channels pass");
    drain();
}

/* ---- display ---- */

static uint32_t g_last_fb, g_flips, g_crtcs;
static int fake_setcrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y, uint32_t *c,
                        int n, void *mode)
{
    (void)fd; (void)crtc; (void)x; (void)y; (void)c; (void)n; (void)mode;
    g_last_fb = fb;
    g_crtcs++;
    return 0;
}
static int fake_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *ud)
{
    (void)fd; (void)crtc; (void)flags; (void)ud;
    g_last_fb = fb;
    g_flips++;
    return 0;
}

int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y, uint32_t *conns,
                   int count, void *mode);
int drmModePageFlip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *user_data);

static void test_display(void)
{
    disp_test_setup(fake_setcrtc, fake_flip);
    char msg[160];
    CHECK(!disp_known() && !disp_setup(msg, sizeof msg), "nothing shown yet: %s", msg);
    struct {
        uint32_t clock;
        uint16_t h[5], v[5];
        uint32_t r, f, t;
        char name[32];
    } mode = {0, {320, 320, 320, 320, 0}, {240, 240, 240, 240, 0}, 60, 0, 0, "320x240"};
    uint32_t conn = 32;
    drmModeSetCrtc(5, 35, 41, 0, 0, &conn, 1, &mode);
    CHECK(g_last_fb == 41 && disp_known(), "app's first frame passes (fb 41)");
    drmModePageFlip(5, 35, 42, 1, NULL);
    CHECK(g_last_fb == 42, "app flip passes (fb 42)");
    CHECK(disp_setup(msg, sizeof msg), "own buffer: %s", msg);
    uint32_t ours = disp_test_our_fb();
    CHECK(disp_show(msg, sizeof msg) && g_last_fb == ours && disp_shown(), "show: our fb on screen");
    drmModePageFlip(5, 35, 41, 1, NULL);
    CHECK(g_last_fb == ours, "app flip while shown: our fb instead");
    drmModePageFlip(6, 35, 77, 1, NULL);
    CHECK(g_last_fb == 77, "another DRM file: untouched");
    CHECK(disp_hide(msg, sizeof msg) && g_last_fb == 41 && !disp_shown(),
          "hide: the app's last frame (fb 41) back");
    drmModePageFlip(5, 35, 42, 1, NULL);
    CHECK(g_last_fb == 42, "app flips pass again");
    disp_stats_t s;
    disp_stats(&s);
    CHECK(s.swapped == 1 && s.app_flips == 4, "stats: %llu swapped, %llu app flips",
          (unsigned long long)s.swapped, (unsigned long long)s.app_flips);
}

/* ---- patch files ---- */

static int same_patch(const fm_patch *a, const fm_patch *b)
{
    if (a->algorithm != b->algorithm || fabsf(a->feedback - b->feedback) > 1e-6f ||
        fabsf(a->gain - b->gain) > 1e-6f)
        return 0;
    for (int i = 0; i < FM_OPS; i++) {
        const float *x = &a->op[i].ratio, *y = &b->op[i].ratio;
        for (int k = 0; k < 9; k++)
            if (fabsf(x[k] - y[k]) > 1e-5f * (1.0f + fabsf(y[k])))
                return 0;
    }
    return 1;
}

static void test_patchfile(void)
{
    char text[2048];
    int all = 1;
    for (int i = 0; i < fm_preset_count(); i++) {
        fm_patch p, q;
        int root = 0;
        fm_preset_get(i, &p);
        size_t n = pf_format(&p, 48 + i, fm_preset_name(i), text, sizeof text);
        all &= n > 0 && pf_parse(text, &q, &root) == 1 && same_patch(&p, &q) && root == 48 + i;
    }
    CHECK(all, "every preset: text and back, the same");
    fm_patch p;
    fm_preset_get(fm_preset_find("Bell"), &p);
    pf_format(&p, 60, "FM-Bell-C3", text, sizeof text);
    printf("%s", text);
    fm_patch q;
    int root;
    CHECK(pf_parse("# mpc-fm patch\nalgorithm 3\nfeedback 0,5\nop2 ratio 3,5 level 0.25\n", &q,
                   &root) == 1 &&
              q.algorithm == 2 && q.feedback == 0.5f && q.op[1].ratio == 3.5f &&
              q.op[1].level == 0.25f && root == 60,
          "decimal commas read too");
    CHECK(pf_parse("hello\nworld\n", &q, &root) == 0, "not a patch file: refused");
    CHECK(pf_parse("# mpc-fm patch\nalgorithm 99\nvolume -5\nop1 ratio 1000 level 7\n", &q, &root) == 1 &&
              q.algorithm == 0 && q.gain == 0.0f && q.op[0].ratio == 32.0f && q.op[0].level == 1.0f,
          "out-of-range values clamped");
}

static void test_edits(void)
{
    char dir[] = "/tmp/mpcfm-edits-XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(0, "mkdtemp");
        return;
    }
    char edits[300];
    snprintf(edits, sizeof edits, "%s/MPC-FM-Patches", dir);
    library_set_edit_dir(edits);
    const char *dirs[1] = {dir};
    library_scan(dirs, 1);
    fm_patch p, q;
    fm_preset_get(fm_preset_find("Brass"), &p);
    p.op[1].level = 0.42f;
    char msg[300], canon[64];
    CHECK(library_save_edit("FM-Brass-C3.wav", &p, 55, msg, sizeof msg), "saved: %s", msg);
    char path[400];
    snprintf(path, sizeof path, "%s/FM-Brass-C3.txt", edits);
    struct stat sb;
    CHECK(stat(path, &sb) == 0, "file MPC-FM-Patches/FM-Brass-C3.txt");
    library_canonical("FM-Brass-C3002.wav", canon, sizeof canon);
    CHECK(!strcmp(canon, "FM-Brass-C3"), "an MPC copy saves under the same name (%s)", canon);
    int root = 0;
    CHECK(library_find("FM-Brass-C3002.wav", &q, &root, NULL, 0) == 2 && same_patch(&p, &q) && root == 55,
          "the copy finds the edited patch");
    library_scan(dirs, 1);
    CHECK(library_edit_count() == 1 && library_find("FM-Brass-C3", &q, &root, NULL, 0) == 2 &&
              same_patch(&p, &q),
          "after a rescan (a reboot): still there");
    library_canonical("My:Bell?.wav", canon, sizeof canon);
    CHECK(!strcmp(canon, "My_Bell_"), "names made safe for the card (%s)", canon);
    unlink(path);
    rmdir(edits);
    rmdir(dir);
}

int main(void)
{
    test_surface();
    test_display();
    test_patchfile();
    test_edits();
    printf(fails ? "\n%d FAILED\n" : "\nall UI tests passed\n", fails);
    return fails != 0;
}
