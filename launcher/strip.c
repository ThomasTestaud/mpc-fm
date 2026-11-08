/* Status strip over the boot splash.
 *
 * SampQOL's qol-run.sh draws its splash on /dev/fb0 just before it starts
 * the app (also at every restart). Our setarch stand-in runs right after
 * that, so it adds one line near the bottom: "mpc-fm 0.0.1  FM on", or why
 * FM is off. Rows 176-239 of the SampQOL splash are empty, so the strip
 * covers nothing of it, and we never change their image file.
 *
 * Best effort: on any error we draw nothing and the app starts as usual.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "font.h"
#include "strip.h"

#define BAND_H 21          /* rows */
#define BAND_FROM_BOTTOM 30 /* rows from the bottom edge to the band top */
#define TEXT_RGB 0x8c8c8cu

static void put_text(unsigned char *band, size_t stride, int width, int x, int y,
                     const char *s, uint32_t rgb)
{
    for (; *s; s++, x += FONT_W) {
        unsigned c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST)
            c = '?';
        const unsigned char *g = font_rows[c - FONT_FIRST];
        for (int r = 0; r < FONT_H; r++)
            for (int b = 0; b < 8; b++) {
                int px = x + b, py = y + r;
                if (!(g[r] & (0x80 >> b)) || px < 0 || px >= width || py < 0 || py >= BAND_H)
                    continue;
                memcpy(band + (size_t)py * stride + (size_t)px * 4, &rgb, 4);
            }
    }
}

int strip_draw(const char *fb_path, const char *text, const char *status, unsigned status_rgb)
{
    int fd = open(fb_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;

    int width, height;
    size_t stride;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0) {
        if (var.bits_per_pixel != 32 || var.xres == 0 || fix.line_length < var.xres * 4) {
            close(fd);
            return -1; /* not the XRGB8888 screen we know */
        }
        width = (int)var.xres;
        height = (int)var.yres;
        stride = fix.line_length;
    } else {
#ifdef MPCFM_TEST
        width = 320; /* tests draw into a plain file */
        height = 240;
        stride = 1280;
#else
        close(fd);
        return -1;
#endif
    }
    int top = height - BAND_FROM_BOTTOM;
    if (top < 0) {
        close(fd);
        return -1;
    }

    size_t len = stride * BAND_H;
    off_t off = (off_t)top * (off_t)stride;
    unsigned char *band = malloc(len);
    if (!band) {
        close(fd);
        return -1;
    }
    if (pread(fd, band, len, off) != (ssize_t)len)
        memset(band, 0, len);

    /* Dim what is under the text, so it reads on any splash. */
    for (int y = 0; y < BAND_H; y++)
        for (int x = 0; x < width; x++) {
            uint32_t p;
            unsigned char *q = band + (size_t)y * stride + (size_t)x * 4;
            memcpy(&p, q, 4);
            p = (p >> 2) & 0x3f3f3fu;
            memcpy(q, &p, 4);
        }

    int n = (int)(strlen(text) + strlen(status));
    int x0 = (width - n * FONT_W) / 2;
    if (x0 < 0)
        x0 = 0;
    int y0 = (BAND_H - FONT_H) / 2;
    put_text(band, stride, width, x0, y0, text, TEXT_RGB);
    put_text(band, stride, width, x0 + (int)strlen(text) * FONT_W, y0, status, status_rgb);

    ssize_t w = pwrite(fd, band, len, off);
    free(band);
    close(fd);
    return w == (ssize_t)len ? 0 : -1;
}
