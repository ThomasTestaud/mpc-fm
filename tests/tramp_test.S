/* Trampolines used by tests/test_tramp.c (aarch64 only). */
#include "tramp_aarch64.inc"
TRAMPOLINE tt_call, t_orig, t_pre, t_post, t_gate
TRAMPOLINE tt_sret, t_orig2, t_pre, t_post, t_gate
