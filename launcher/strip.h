#ifndef MPCFM_STRIP_H
#define MPCFM_STRIP_H

#define STRIP_RGB_ON 0x3ccf6eu  /* green */
#define STRIP_RGB_OFF 0xf5a623u /* amber */

#define STRIP_ERR_OPEN -1   /* cannot open the framebuffer */
#define STRIP_ERR_IOCTL -2  /* no screen info */
#define STRIP_ERR_FORMAT -3 /* not a 32-bit screen we know */
#define STRIP_ERR_WRITE -4

/* Draw "<text><status>" centred near the bottom of the screen. 0 if drawn,
 * else a STRIP_ERR_ code. */
int strip_draw(const char *fb_path, const char *text, const char *status, unsigned status_rgb);

#endif
