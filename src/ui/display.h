/* The FM page on the MPC's screen.
 *
 * The app draws its frames into two DRM buffers and shows them with
 * drmModePageFlip (and drmModeSetCrtc for the first one). fm.so defines
 * both (SampQOL calls ours through RTLD_NEXT after drawing its own screens
 * on the app's frame). The page has its own buffer on the app's DRM file:
 * while it is shown, each flip of the app shows our buffer instead (the app
 * still gets its flip event, and its own buffers are never written). The
 * page updates the panel with DIRTYFB on the changed rectangle only: a full
 * SPI frame takes ~50 ms, so never from the audio thread. */
#ifndef MPCFM_DISPLAY_H
#define MPCFM_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#define DISP_W 320
#define DISP_H 240

/* The app has shown a frame: its DRM file and CRTC are known. */
int disp_known(void);
/* Make our buffer (once). 1: ready. 0: msg says why not. UI thread. */
int disp_setup(char *msg, size_t cap);
/* Our buffer, XRGB8888, DISP_W x DISP_H, stride in pixels. */
uint32_t *disp_pixels(int *stride_px);
/* Put our buffer on the screen / give the screen back to the app. */
int disp_show(char *msg, size_t cap);
int disp_hide(char *msg, size_t cap);
int disp_shown(void);
/* Send this rectangle (x0,y0 included, x1,y1 excluded) to the panel. */
int disp_dirty(int x0, int y0, int x1, int y1);

typedef struct {
    uint64_t app_flips, app_setcrtc, swapped, flip_errors, dirty_calls, dirty_errors;
    uint64_t dirty_max_ns;
    int fd, crtc, width, height, app_fb;
} disp_stats_t;
void disp_stats(disp_stats_t *s);

#ifdef MPCFM_TEST
typedef int (*disp_setcrtc_fn)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, int, void *);
typedef int (*disp_flip_fn)(int, uint32_t, uint32_t, uint32_t, void *);
void disp_test_setup(disp_setcrtc_fn setcrtc, disp_flip_fn flip);
uint32_t disp_test_our_fb(void);
#endif

#endif
