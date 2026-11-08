#ifndef MPCFM_STRIP_H
#define MPCFM_STRIP_H

#define STRIP_RGB_ON 0x3ccf6eu  /* green */
#define STRIP_RGB_OFF 0xf5a623u /* amber */

/* Draw "<text><status>" centred near the bottom of the screen. 0 if drawn. */
int strip_draw(const char *fb_path, const char *text, const char *status, unsigned status_rgb);

#endif
