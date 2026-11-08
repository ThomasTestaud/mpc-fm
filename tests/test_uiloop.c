/* The FM page's thread end to end on the computer: fake surface bytes in,
 * a fake screen (libdrm calls), a fake FM note; the page opens on the
 * pad's sample, a knob edits the live patch, closing saves it. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fm/fm_presets.h"
#include "patchfile.h"
#include "sampleinfo.h"
#include "ui/display.h"
#include "ui/surface.h"
#include "ui/ui.h"
#include "voice_hook.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "ok   " : "FAIL "); if (!ok_) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static char g_logs[16384];
static void log_fn(const char *m)
{
    pthread_mutex_lock(&g_lk);
    size_t n = strlen(g_logs);
    snprintf(g_logs + n, sizeof g_logs - n, "%s\n", m);
    pthread_mutex_unlock(&g_lk);
    printf("     log: %s\n", m);
}
static int logged(const char *s)
{
    pthread_mutex_lock(&g_lk);
    int r = strstr(g_logs, s) != NULL;
    pthread_mutex_unlock(&g_lk);
    return r;
}
static int wait_log(const char *s, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (logged(s))
            return 1;
        usleep(10000);
    }
    return logged(s);
}

static uint32_t g_last_fb;
static int fake_setcrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y, uint32_t *c,
                        int n, void *mode)
{
    (void)fd; (void)crtc; (void)x; (void)y; (void)c; (void)n; (void)mode;
    __atomic_store_n(&g_last_fb, fb, __ATOMIC_RELAXED);
    return 0;
}
static int fake_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *ud)
{
    (void)fd; (void)crtc; (void)flags; (void)ud;
    __atomic_store_n(&g_last_fb, fb, __ATOMIC_RELAXED);
    return 0;
}
int drmModeSetCrtc(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, int, void *);

static surf_stream st;
static void midi(const uint8_t *b, size_t n)
{
    uint8_t t[64];
    memcpy(t, b, n);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    surf_filter(&st, t, n, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}
#define MIDI(...) do { static const uint8_t b_[] = {__VA_ARGS__}; midi(b_, sizeof b_); } while (0)

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    char dir[] = "/tmp/mpcfm-uiloop-XXXXXX";
    if (!mkdtemp(dir))
        return 1;
    char edits[300];
    snprintf(edits, sizeof edits, "%s/MPC-FM-Patches", dir);
    library_set_edit_dir(edits);

    /* the app shows its first frame */
    disp_test_setup(fake_setcrtc, fake_flip);
    struct { uint32_t clock; uint16_t h[5], v[5]; uint32_t r, f, t; char name[32]; } mode =
        {0, {320, 320, 320, 320, 0}, {240, 240, 240, 240, 0}, 60, 0, 0, "320x240"};
    uint32_t conn = 32;
    drmModeSetCrtc(5, 35, 41, 0, 0, &conn, 1, &mode);

    ui_start(log_fn);
    CHECK(wait_log("FM page ready", 3000), "the page gets ready once the app has a screen");

    /* a fake Sample object named FM-Bell-C3.wav, and its FM note after a pad hit */
    static char path[] = "/media/CARD/MPC-FM-Samples/FM-Bell-C3.wav";
    void *sample = calloc(1, 1024);
    char *pp = path;
    memcpy((char *)sample + 56, &pp, 8);
    MIDI(0x99, 37, 100);
    vh_note_t n;
    memset(&n, 0, sizeof n);
    n.sample = (uintptr_t)sample;
    n.provider = 0x1234;
    n.t_ns = now();
    n.fm = 1;
    n.root = 60;
    fm_preset_get(fm_preset_find("Bell"), &n.patch);
    n.algorithm = n.patch.algorithm + 1;
    vh_test_note(&n);
    usleep(100000);

    MIDI(0x90, 49, 0x7f, 0x90, 11, 0x7f, 0x80, 11, 0, 0x80, 49, 0);
    CHECK(wait_log("FM page open (FM-Bell-C3)", 2000), "SHIFT + NOTE REPEAT opens the page on the pad's sample");
    CHECK(vh_edit_sample() == (uintptr_t)sample, "its voices play the edit patch");
    CHECK(__atomic_load_n(&g_last_fb, __ATOMIC_RELAXED) == disp_test_our_fb(), "our buffer is on the screen");
    int stride;
    uint32_t *px = disp_pixels(&stride);
    CHECK(px && px[5 * stride + 10] != 0, "the page is drawn into it");

    /* B2 = OPERATOR 1, K2 = Level: knob from 64 to the end */
    MIDI(0x90, 13, 0x7f, 0x80, 13, 0);
    MIDI(0xb0, 17, 64);
    MIDI(0xb0, 17, 100, 0xb0, 17, 127);
    usleep(200000);
    CHECK(vh_edit_patch()->op[0].level == 1.0f, "K2 on OPERATOR 1: level %.2f, live", vh_edit_patch()->op[0].level);

    MIDI(0x90, 49, 0x7f, 0x90, 11, 0x7f, 0x80, 11, 0, 0x80, 49, 0);
    CHECK(wait_log("FM page closed", 2000), "SHIFT + NOTE REPEAT closes it");
    CHECK(logged("\"FM-Bell-C3.wav\" saved (page closed)"), "the edit is saved on close");
    CHECK(__atomic_load_n(&g_last_fb, __ATOMIC_RELAXED) == 41, "the app's frame is back");
    char f[400], text[4096] = "";
    snprintf(f, sizeof f, "%s/FM-Bell-C3.txt", edits);
    FILE *fp = fopen(f, "r");
    if (fp) {
        size_t r = fread(text, 1, sizeof text - 1, fp);
        text[r] = 0;
        fclose(fp);
    }
    fm_patch got;
    int root;
    CHECK(pf_parse(text, &got, &root) == 1 && got.op[0].level == 1.0f && root == 60,
          "MPC-FM-Patches/FM-Bell-C3.txt has the edit");
    CHECK(vh_disk_count() == 1, "and the Sample object keeps it for its next notes");
    fm_patch lib;
    CHECK(library_find("FM-Bell-C3001.wav", &lib, &root, NULL, 0) == 2 && lib.op[0].level == 1.0f,
          "a copy of the sample (after a project save) finds the edit");

    /* the page closes itself when another button is pressed */
    MIDI(0x90, 49, 0x7f, 0x90, 11, 0x7f, 0x80, 11, 0, 0x80, 49, 0);
    CHECK(wait_log("FM page open (FM-Bell-C3)", 2000), "open again");
    MIDI(0x90, 6, 0x7f, 0x80, 6, 0);
    CHECK(wait_log("FM page closed (another button)", 2000), "SAMPLE closes it");

    char st_txt[4096];
    ui_status(st_txt, sizeof st_txt);
    printf("%s", st_txt);
    unlink(f);
    rmdir(edits);
    rmdir(dir);
    printf(fails ? "\n%d FAILED\n" : "\nall UI loop tests passed\n", fails);
    return fails != 0;
}
