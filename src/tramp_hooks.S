/* The three stock methods we hook, through transparent trampolines.
 * render and stop run our code only while an FM voice exists. */
#include "tramp_aarch64.inc"
TRAMPOLINE vh_tramp_render, vh_orig_render, vh_pre_render, vh_post_render, vh_active
TRAMPOLINE vh_tramp_start, vh_orig_start, vh_pre_start, vh_post_start, vh_gate_always
TRAMPOLINE vh_tramp_stop, vh_orig_stop, vh_pre_stop, vh_post_stop, vh_active
