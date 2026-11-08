/* Crash report: if the app crashes, write where to the SD card first. */
#ifndef MPCFM_CRASH_H
#define MPCFM_CRASH_H

#include <stdint.h>

/* Install our handler in front of the current ones (SampQOL's), which we
 * still call after writing our report. path: the report file (appended). */
int crash_install(const char *path, uintptr_t app_base, char *msg, int cap);

#endif
