#ifndef MPCFM_FM_PRESETS_H
#define MPCFM_FM_PRESETS_H

#include "fm_engine.h"

int fm_preset_count(void);
const char *fm_preset_name(int index);
int fm_preset_find(const char *name); /* -1 if not found */
/* Fill p with preset `index` (already sanitized). 0 if there is none. */
int fm_preset_get(int index, fm_patch *p);

#endif
