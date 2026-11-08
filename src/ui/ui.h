/* The FM page's thread: surface events in, the page on the screen, the
 * edits live on the voices and saved on the card. */
#ifndef MPCFM_UI_H
#define MPCFM_UI_H

#include <stddef.h>

typedef void (*ui_log_fn)(const char *msg);

/* In the app's constructor (before its threads): enable the surface
 * filter and start the UI thread. log: for events (any thread). */
void ui_start(ui_log_fn log);
/* For the status file. */
void ui_status(char *out, size_t cap);

#endif
