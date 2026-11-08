/* The FM page on the MPC's screen (see display.h). */
#define _GNU_SOURCE
#include "display.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

/* libdrm's drmModeModeInfo (68 bytes) */
typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
} drm_mode_t;

/* uapi drm.h / drm_mode.h */
#define IOCTL_MODE_ADDFB 0xC01C64AEul
#define IOCTL_MODE_RMFB 0xC00464AFul
#define IOCTL_MODE_DIRTYFB 0xC01864B1ul
#define IOCTL_MODE_CREATE_DUMB 0xC02064B2ul
#define IOCTL_MODE_MAP_DUMB 0xC01064B3ul
#define IOCTL_MODE_DESTROY_DUMB 0xC00464B4ul
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct clip_rect { uint16_t x1, y1, x2, y2; };
struct dirty_cmd { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };

typedef int (*setcrtc_fn)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, int, void *);
typedef int (*flip_fn)(int, uint32_t, uint32_t, uint32_t, void *);
static setcrtc_fn real_setcrtc;
static flip_fn real_flip;

static void resolve(void)
{
    if (!real_setcrtc)
        *(void **)&real_setcrtc = dlsym(RTLD_NEXT, "drmModeSetCrtc");
    if (!real_flip)
        *(void **)&real_flip = dlsym(RTLD_NEXT, "drmModePageFlip");
}

/* Taken around every call that changes what is on the screen (the app's
 * flips, our show/hide), so "which buffer is up" is always the last one. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;
static uint32_t g_crtc, g_conn[8], g_app_fb;
static int g_nconn, g_have_mode;
static drm_mode_t g_mode;
static _Atomic int g_known, g_shown;
static uint32_t g_fb, g_pitch;
static uint32_t *g_px;
static disp_stats_t g_st;

int disp_known(void) { return atomic_load(&g_known); }
int disp_shown(void) { return atomic_load(&g_shown); }

static void learn(int fd, uint32_t crtc)
{
    if (g_fd < 0 && fd >= 0) {
        g_fd = fd;
        g_crtc = crtc;
        atomic_store(&g_known, 1);
    }
}

EXPORT int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y,
                          uint32_t *conns, int count, void *mode)
{
    resolve();
    if (!real_setcrtc)
        return -ENOSYS;
    pthread_mutex_lock(&g_lock);
    g_st.app_setcrtc++;
    if (fb && mode && conns && count > 0) { /* the app's screen setup: keep it to show ours */
        learn(fd, crtc);
        if (fd == g_fd && crtc == g_crtc) {
            g_nconn = count < 8 ? count : 8;
            memcpy(g_conn, conns, (size_t)g_nconn * sizeof *g_conn);
            memcpy(&g_mode, mode, sizeof g_mode);
            g_have_mode = 1;
        }
    }
    if (fb && fd == g_fd && crtc == g_crtc) {
        g_app_fb = fb;
        if (atomic_load(&g_shown) && g_fb) {
            fb = g_fb;
            g_st.swapped++;
        }
    }
    int r = real_setcrtc(fd, crtc, fb, x, y, conns, count, mode);
    if (r)
        g_st.flip_errors++;
    pthread_mutex_unlock(&g_lock);
    return r;
}

EXPORT int drmModePageFlip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *user_data)
{
    resolve();
    if (!real_flip)
        return -ENOSYS;
    pthread_mutex_lock(&g_lock);
    g_st.app_flips++;
    learn(fd, crtc);
    if (fd == g_fd && crtc == g_crtc) {
        g_app_fb = fb;
        if (atomic_load(&g_shown) && g_fb) {
            fb = g_fb; /* the app still gets its event, for its own frame */
            g_st.swapped++;
        }
    }
    int r = real_flip(fd, crtc, fb, flags, user_data);
    if (r)
        g_st.flip_errors++;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static __attribute__((unused)) int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

#ifdef MPCFM_TEST
static uint32_t g_test_px[DISP_W * DISP_H];
#endif

int disp_setup(char *msg, size_t cap)
{
    if (g_px)
        return 1;
    if (!atomic_load(&g_known)) {
        snprintf(msg, cap, "the app has not shown a frame yet");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    int ok = 0;
    if (g_have_mode && (g_mode.hdisplay != DISP_W || g_mode.vdisplay != DISP_H)) {
        snprintf(msg, cap, "screen is %dx%d, not %dx%d", g_mode.hdisplay, g_mode.vdisplay, DISP_W,
                 DISP_H);
        goto out;
    }
#ifdef MPCFM_TEST
    g_fb = 0x7e57;
    g_pitch = DISP_W * 4;
    g_px = g_test_px;
    ok = 1;
#else
    struct create_dumb cd = {DISP_H, DISP_W, 32, 0, 0, 0, 0};
    if (xioctl(g_fd, IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
        snprintf(msg, cap, "CREATE_DUMB: %s", strerror(errno));
        goto out;
    }
    struct fb_cmd fc = {0, DISP_W, DISP_H, cd.pitch, 32, 24, cd.handle};
    if (xioctl(g_fd, IOCTL_MODE_ADDFB, &fc) != 0) {
        snprintf(msg, cap, "ADDFB: %s", strerror(errno));
        struct { uint32_t handle; } dd = {cd.handle};
        xioctl(g_fd, IOCTL_MODE_DESTROY_DUMB, &dd);
        goto out;
    }
    struct map_dumb md = {cd.handle, 0, 0};
    void *p = MAP_FAILED;
    if (xioctl(g_fd, IOCTL_MODE_MAP_DUMB, &md) == 0)
        p = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)md.offset);
    if (p == MAP_FAILED || cd.pitch < DISP_W * 4) {
        snprintf(msg, cap, "MAP_DUMB: %s", strerror(errno));
        uint32_t id = fc.fb_id;
        xioctl(g_fd, IOCTL_MODE_RMFB, &id);
        struct { uint32_t handle; } dd = {cd.handle};
        xioctl(g_fd, IOCTL_MODE_DESTROY_DUMB, &dd);
        goto out;
    }
    g_fb = fc.fb_id;
    g_pitch = cd.pitch;
    g_px = p;
    memset(g_px, 0, (size_t)cd.size);
    ok = 1;
#endif
    g_st.width = DISP_W;
    g_st.height = DISP_H;
    snprintf(msg, cap, "own buffer fb %u (pitch %u) on DRM fd %d, crtc %u%s", g_fb, g_pitch, g_fd,
             g_crtc, g_have_mode ? "" : " (app mode not seen)");
out:
    pthread_mutex_unlock(&g_lock);
    return ok;
}

uint32_t *disp_pixels(int *stride_px)
{
    *stride_px = (int)(g_pitch / 4);
    return g_px;
}

/* Put fb on the screen ourselves: SETCRTC if we have the app's mode (a
 * blocking commit: an app flip meanwhile waits for it, and does not fail),
 * else a flip without event, retried while the app's flip is pending. */
static int put_on_screen(uint32_t fb, char *msg, size_t cap)
{
    int r;
    if (g_have_mode) {
        r = real_setcrtc(g_fd, g_crtc, fb, 0, 0, g_conn, g_nconn, &g_mode);
    } else {
        for (int i = 0;; i++) {
            r = real_flip(g_fd, g_crtc, fb, 0, NULL);
            if (r != -EBUSY || i >= 40)
                break;
            struct timespec t = {0, 5000000};
            nanosleep(&t, NULL);
        }
    }
    if (r)
        snprintf(msg, cap, "%s fb %u: error %d", g_have_mode ? "SETCRTC" : "flip", fb, r);
    return r == 0;
}

int disp_show(char *msg, size_t cap)
{
    resolve();
    if (!g_px || !real_setcrtc || !real_flip) {
        snprintf(msg, cap, "no buffer");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    atomic_store(&g_shown, 1);
    int ok = put_on_screen(g_fb, msg, cap);
    if (!ok)
        atomic_store(&g_shown, 0);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int disp_hide(char *msg, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    int ok = 1;
    if (atomic_exchange(&g_shown, 0) && g_app_fb)
        ok = put_on_screen(g_app_fb, msg, cap); /* the app's last frame, untouched */
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int disp_dirty(int x0, int y0, int x1, int y1)
{
    if (!g_px || !atomic_load(&g_shown) || x1 <= x0 || y1 <= y0)
        return 0;
    uint64_t t0 = now_ns();
    int r = 0;
#ifndef MPCFM_TEST
    struct clip_rect c = {(uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1};
    struct dirty_cmd d = {g_fb, 0, 0, 1, (uint64_t)(uintptr_t)&c};
    r = xioctl(g_fd, IOCTL_MODE_DIRTYFB, &d);
#endif
    uint64_t dt = now_ns() - t0;
    pthread_mutex_lock(&g_lock);
    g_st.dirty_calls++;
    if (r)
        g_st.dirty_errors++;
    if (dt > g_st.dirty_max_ns)
        g_st.dirty_max_ns = dt;
    pthread_mutex_unlock(&g_lock);
    return r == 0;
}

void disp_stats(disp_stats_t *s)
{
    pthread_mutex_lock(&g_lock);
    *s = g_st;
    s->fd = g_fd;
    s->crtc = (int)g_crtc;
    s->app_fb = (int)g_app_fb;
    pthread_mutex_unlock(&g_lock);
}

#ifdef MPCFM_TEST
void disp_test_setup(disp_setcrtc_fn setcrtc, disp_flip_fn flip)
{
    real_setcrtc = setcrtc;
    real_flip = flip;
    g_fd = -1;
    g_crtc = g_app_fb = 0;
    g_nconn = g_have_mode = 0;
    atomic_store(&g_known, 0);
    atomic_store(&g_shown, 0);
    g_fb = 0;
    g_px = NULL;
    memset(&g_st, 0, sizeof g_st);
}
uint32_t disp_test_our_fb(void) { return g_fb; }
#endif
