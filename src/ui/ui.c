/* The FM page's thread (see ui.h).
 *
 * What is edited: the FM sample of the last pad hit (a pad hit, then an FM
 * note start within 80 ms), else the last FM note heard. Its voices play
 * the shared edit patch (voice_hook.c), so each knob move is heard at once.
 * On close, on a change of sample, and 2 s after the last edit, the patch
 * is registered for that Sample object (the next notes keep it) and saved
 * as MPC-FM-Patches/<name>.txt (sampleinfo.c), which wins over the FM
 * file's own patch from then on, also after a reboot. */
#define _GNU_SOURCE
#include "ui.h"

#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "display.h"
#include "fm_page.h"
#include "sampleinfo.h"
#include "surface.h"
#include "voice_hook.h"

void surf_enable(int on);

#define PAD_WINDOW_NS 80000000ull
#define SAVE_AFTER_NS 2000000000ull
#define FRAME_NS 30000000ull
#define STATUS_NS 2000000000ull

static ui_log_fn g_log;
static fm_page g_pg;
static uint32_t g_cur[DISP_W * DISP_H], g_next[DISP_W * DISP_H];

typedef struct {
    int valid;
    uintptr_t sample, provider;
    int root;
    uint64_t t;
    fm_patch patch;
} cand_t;
static cand_t g_last_any, g_last_pad;
static uint64_t g_pad_t; /* last physical pad hit */

static struct {
    uintptr_t sample, provider;
    int root, have_loc;
    name_loc loc;
    uint64_t chosen, last_edit, status_until;
    int unsaved;
} g_tg;

static struct {
    int ready, opens, saves, save_errors, switches;
    char setup_msg[160], last_msg[160];
} g_ui;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char m[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_status_lock);
    copy_str(g_ui.last_msg, sizeof g_ui.last_msg, m);
    pthread_mutex_unlock(&g_status_lock);
    if (g_log)
        g_log(m);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void set_status(const char *s, uint64_t now, int sticky)
{
    copy_str(g_pg.status, sizeof g_pg.status, s);
    g_tg.status_until = sticky ? 0 : now + STATUS_NS;
}

/* Keep the edits: for this Sample object now, and on the card. */
static void commit(const char *why)
{
    if (!g_tg.sample || !g_tg.unsaved)
        return;
    fm_patch p = *vh_edit_patch();
    g_tg.unsaved = 0;
    if (!g_tg.have_loc) {
        set_status("not saved: no name", now_ns(), 1);
        return;
    }
    const name_loc *l = &g_tg.loc;
    vh_disk_add(g_tg.sample, g_tg.provider, l->i, l->obj, l->k, l->ptr, &p, g_tg.root);
    char msg[300];
    if (library_save_edit(l->base, &p, g_tg.root, msg, sizeof msg)) {
        g_ui.saves++;
        set_status("saved", now_ns(), 0);
        logf_("FM page: \"%s\" saved (%s) to %s", l->base, why, msg);
    } else {
        g_ui.save_errors++;
        set_status("NOT saved", now_ns(), 1);
        logf_("FM page: \"%s\" NOT saved: %s", l->base, msg);
    }
}

static void set_target(const cand_t *c)
{
    if (!c->valid || c->sample == g_tg.sample)
        return;
    commit("other pad");
    memset(&g_tg, 0, sizeof g_tg);
    g_tg.chosen = now_ns();
    g_tg.sample = c->sample;
    g_tg.provider = c->provider;
    g_tg.root = c->root;
    if (vh_edit_sample() != c->sample)
        vh_edit_begin(c->sample, &c->patch);
    g_tg.have_loc = sample_name_loc(c->sample, &g_tg.loc);
    char shown[64];
    if (g_tg.have_loc) {
        snprintf(shown, sizeof shown, "%s", g_tg.loc.base);
        size_t n = strlen(shown);
        if (n > 4 && !strcasecmp(shown + n - 4, ".wav"))
            shown[n - 4] = 0;
    } else {
        snprintf(shown, sizeof shown, "(no name)");
    }
    fm_page_target(&g_pg, vh_edit_patch(), shown);
    g_ui.switches++;
    logf_("FM page: editing \"%s\" (Sample %#lx, algorithm %d)", shown, (unsigned long)c->sample,
          c->patch.algorithm + 1);
}

static void read_notes(void)
{
    static uint64_t cursor;
    static vh_note_t notes[16];
    int n;
    while ((n = vh_notes_since(&cursor, notes, 16)) > 0)
        for (int i = 0; i < n; i++) {
            const vh_note_t *t = &notes[i];
            if (!t->fm || !t->sample)
                continue;
            cand_t c = {1, t->sample, t->provider, t->root, t->t_ns, t->patch};
            g_last_any = c;
            if (g_pad_t && t->t_ns >= g_pad_t && t->t_ns - g_pad_t < PAD_WINDOW_NS &&
                !(g_last_pad.valid && g_last_pad.t >= g_pad_t)) /* the first FM layer of the hit */
                g_last_pad = c;
        }
}

/* ---- the screen ---- */

static int g_need_draw;

static void present(int full)
{
    int stride;
    uint32_t *dst = disp_pixels(&stride);
    if (!dst)
        return;
    fm_page_draw(&g_pg, g_next, DISP_W, DISP_H, DISP_W);
    int x0 = DISP_W, y0 = DISP_H, x1 = 0, y1 = 0;
    for (int y = 0; y < DISP_H; y++) {
        const uint32_t *a = g_next + y * DISP_W, *b = g_cur + y * DISP_W;
        if (!full && !memcmp(a, b, DISP_W * 4))
            continue;
        int l = 0, r = DISP_W - 1;
        if (!full) {
            while (a[l] == b[l])
                l++;
            while (a[r] == b[r])
                r--;
        }
        x0 = l < x0 ? l : x0;
        x1 = r + 1 > x1 ? r + 1 : x1;
        y0 = y < y0 ? y : y0;
        y1 = y + 1;
    }
    if (x1 <= x0)
        return;
    for (int y = y0; y < y1; y++) {
        memcpy(dst + (size_t)y * (size_t)stride + x0, g_next + y * DISP_W + x0,
               (size_t)(x1 - x0) * 4);
        memcpy(g_cur + y * DISP_W + x0, g_next + y * DISP_W + x0, (size_t)(x1 - x0) * 4);
    }
    if (!full)
        disp_dirty(x0, y0, x1, y1);
}

static void open_page(int knobs)
{
    for (int k = 0; k < 3; k++) {
        int v = ((knobs >> (8 * k)) & 0xff) - 1;
        g_pg.knob[k] = v;
    }
    fm_page_shift(&g_pg, 0);
    const cand_t *c = g_last_pad.valid ? &g_last_pad : &g_last_any;
    if (g_last_any.valid && g_last_pad.valid && g_last_any.t > g_last_pad.t + 2000000000ull)
        c = &g_last_any; /* the pads were long ago: what plays now */
    set_target(c);
    present(1);
    char msg[160] = "";
    if (!disp_show(msg, sizeof msg)) {
        logf_("FM page: cannot show it: %s", msg);
        surf_request_close();
        return;
    }
    g_ui.opens++;
    logf_("FM page open (%s)", g_pg.patch ? g_pg.name : "no FM pad played yet");
}

static void close_page(int by_button)
{
    commit("page closed");
    char msg[160] = "";
    if (!disp_hide(msg, sizeof msg))
        logf_("FM page: cannot give the screen back: %s", msg);
    logf_("FM page closed%s", by_button ? " (another button)" : "");
}

static void handle(const surf_event *e)
{
    int open = disp_shown();
    switch (e->type) {
    case SEV_OPEN: open_page(e->b); return;
    case SEV_CLOSE:
        if (open)
            close_page(e->a);
        return;
    case SEV_PAD:
        g_pad_t = e->t_ns;
        return;
    default: break;
    }
    if (!open)
        return;
    int dirty = 0;
    switch (e->type) {
    case SEV_BUTTON: dirty = fm_page_button(&g_pg, e->a); break;
    case SEV_KNOB: dirty = fm_page_knob(&g_pg, e->a, e->b); break;
    case SEV_ENCODER: dirty = fm_page_encoder(&g_pg, e->b); break;
    case SEV_SHIFT: dirty = fm_page_shift(&g_pg, e->a); break;
    default: break;
    }
    if (g_pg.changed) {
        g_pg.changed = 0;
        g_tg.unsaved = 1;
        g_tg.last_edit = now_ns();
        if (g_pg.status[0] && g_tg.status_until) /* "saved" is old news now */
            g_pg.status[0] = 0;
    }
    g_need_draw |= dirty;
}

static void *ui_main(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "mpcfm-ui");
    fm_page_init(&g_pg);
    int efd = surf_event_fd();
    uint64_t last_try = 0, last_frame = 0;
    for (;;) {
        struct pollfd pf = {efd, POLLIN, 0};
        if (efd >= 0) {
            if (poll(&pf, 1, 40) > 0) {
                uint64_t v;
                if (read(efd, &v, sizeof v) < 0) {
                    /* nothing to clear */
                }
            }
        } else {
            struct timespec t = {0, 20000000};
            nanosleep(&t, NULL);
        }
        uint64_t now = now_ns();
        if (!g_ui.ready && disp_known() && now - last_try > 1000000000ull) {
            last_try = now;
            char msg[160] = "";
            g_ui.ready = disp_setup(msg, sizeof msg);
            pthread_mutex_lock(&g_status_lock);
            snprintf(g_ui.setup_msg, sizeof g_ui.setup_msg, "%s", msg);
            pthread_mutex_unlock(&g_status_lock);
            if (g_ui.ready) {
                surf_set_ready(1);
                logf_("FM page ready (SHIFT + NOTE REPEAT): %s", msg);
            } else {
                logf_("FM page not ready: %s", msg);
            }
        }
        surf_event e;
        while (surf_next_event(&e)) {
            read_notes(); /* a pad's note may already be there */
            handle(&e);
        }
        read_notes();
        now = now_ns(); /* after the events: an edit may be newer than the top of the loop */
        if (disp_shown()) {
            if (g_last_pad.valid && g_last_pad.sample != g_tg.sample &&
                g_last_pad.t > g_tg.chosen) { /* another FM pad hit since */
                set_target(&g_last_pad);
                g_need_draw = 1;
            } else if (!g_tg.sample && g_last_any.valid) { /* nothing yet: an FM note of the sequence */
                set_target(&g_last_any);
                g_need_draw = 1;
            }
            if (g_tg.unsaved && now - g_tg.last_edit > SAVE_AFTER_NS) {
                commit("2 s after the last edit");
                g_need_draw = 1;
            }
            if (g_tg.status_until && now > g_tg.status_until) {
                g_pg.status[0] = 0;
                g_tg.status_until = 0;
                g_need_draw = 1;
            }
            if (g_need_draw && now - last_frame >= FRAME_NS) {
                g_need_draw = 0;
                last_frame = now;
                present(0);
            }
        }
    }
    return NULL;
}

void ui_start(ui_log_fn log)
{
    g_log = log;
    surf_init();
    surf_enable(1);
    pthread_attr_t a;
    pthread_t th;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&th, &a, ui_main, NULL) != 0)
        surf_enable(0);
    pthread_attr_destroy(&a);
}

void ui_status(char *out, size_t cap)
{
    disp_stats_t d;
    disp_stats(&d);
    uint64_t bytes, taken, lost;
    surf_counts(&bytes, &taken, &lost);
    char setup[160], last[160];
    pthread_mutex_lock(&g_status_lock);
    snprintf(setup, sizeof setup, "%s", g_ui.setup_msg);
    snprintf(last, sizeof last, "%s", g_ui.last_msg);
    pthread_mutex_unlock(&g_status_lock);
    int n = snprintf(out, cap,
                     "FM page: %s%s; opened %d times, saves %d (errors %d), edited pads %d\n"
                     "  screen: %s; app flips %llu, set-crtc %llu, shown as FM page %llu, flip"
                     " errors %llu, dirty %llu (errors %llu, max %.1f ms), DRM fd %d crtc %d\n"
                     "  surface: %llu bytes, %llu messages to the page, %llu events lost; last: %s\n",
                     g_ui.ready ? "ready" : "not ready", disp_shown() ? ", OPEN" : "", g_ui.opens,
                     g_ui.saves, g_ui.save_errors, g_ui.switches, setup[0] ? setup : "-",
                     (unsigned long long)d.app_flips, (unsigned long long)d.app_setcrtc,
                     (unsigned long long)d.swapped, (unsigned long long)d.flip_errors,
                     (unsigned long long)d.dirty_calls, (unsigned long long)d.dirty_errors,
                     (double)d.dirty_max_ns / 1e6, d.fd, d.crtc, (unsigned long long)bytes,
                     (unsigned long long)taken, (unsigned long long)lost, last[0] ? last : "-");
    for (int i = 0; n > 0 && (size_t)n < cap; i++) {
        char line[200];
        if (!surf_port_info(i, line, sizeof line))
            break;
        n += snprintf(out + n, cap - (size_t)n, "  midi in %d: %s\n", i, line);
    }
}
