/* Transparent call hooks for aarch64 (AAPCS64).
 *
 *   TRAMPOLINE name, orig, pre, post, gate
 *
 * name(...) behaves exactly like *orig(...): every argument register
 * (x0-x8, q0-q7) and the first 64 bytes of stack arguments reach the
 * original unchanged, and its results (x0-x1, q0-q1) come back unchanged.
 * Around the call, C code runs:
 *     void pre(vh_regs *args, vh_ctx *ctx);
 *     void post(vh_regs *args, uint64_t ret, vh_ctx *ctx);
 * args: the saved x0..x8 (then a pad word, then q0..q7); ctx: 64 bytes for
 * pre to pass to post.
 * gate: a 32-bit global; when it is 0, name jumps straight to *orig with
 * nothing touched (no pre/post). Use `vh_gate_always` to always run them.
 *
 * Why: a C wrapper cannot forward stack arguments. The stock note-start
 * method takes 3 of them, and v0.1.2 crashed reading garbage there.
 */
#define TR_FRAME 336
/* frame: [0] x29 x30 | [16] x0..x8, pad | [96] q0..q7 | [224] ctx (64)
 *        [288] result x0 x1 | [304] result q0 q1 */

.macro TRAMPOLINE name, orig, pre, post, gate
    .text
    .p2align 4
    .globl \name
    .hidden \name
    .type \name, %function
\name:
    .cfi_startproc
    hint #34                        /* bti c */
    adrp x9, \gate
    ldr w9, [x9, :lo12:\gate]
    cbnz w9, 1f
    adrp x9, \orig                  /* fast path: a plain tail call */
    ldr x9, [x9, :lo12:\orig]
    br x9
1:
    stp x29, x30, [sp, #-TR_FRAME]!
    .cfi_def_cfa_offset TR_FRAME
    .cfi_offset x29, -TR_FRAME
    .cfi_offset x30, -TR_FRAME + 8
    mov x29, sp
    .cfi_def_cfa x29, TR_FRAME
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    add x0, sp, #16
    add x1, sp, #224
    bl \pre
    ldp x0, x1, [x29, #16]
    ldp x2, x3, [x29, #32]
    ldp x4, x5, [x29, #48]
    ldp x6, x7, [x29, #64]
    ldr x8, [x29, #80]
    ldp q0, q1, [x29, #96]
    ldp q2, q3, [x29, #128]
    ldp q4, q5, [x29, #160]
    ldp q6, q7, [x29, #192]
    sub sp, sp, #64                 /* the caller's stack arguments, copied */
    ldp x9, x10, [x29, #TR_FRAME]
    stp x9, x10, [sp]
    ldp x9, x10, [x29, #TR_FRAME + 16]
    stp x9, x10, [sp, #16]
    ldp x9, x10, [x29, #TR_FRAME + 32]
    stp x9, x10, [sp, #32]
    ldp x9, x10, [x29, #TR_FRAME + 48]
    stp x9, x10, [sp, #48]
    adrp x9, \orig
    ldr x9, [x9, :lo12:\orig]
    blr x9
    add sp, sp, #64
    stp x0, x1, [x29, #288]
    stp q0, q1, [x29, #304]
    mov x1, x0
    add x0, x29, #16
    add x2, x29, #224
    bl \post
    ldp x0, x1, [x29, #288]
    ldp q0, q1, [x29, #304]
    ldp x29, x30, [sp], #TR_FRAME
    .cfi_def_cfa sp, 0
    .cfi_restore x29
    .cfi_restore x30
    ret
    .cfi_endproc
    .size \name, . - \name
.endm
