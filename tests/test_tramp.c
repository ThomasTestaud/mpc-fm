/* aarch64 test of the transparent trampolines (runs under qemu-aarch64). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); if (!(c)) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

typedef struct { uint64_t x[9]; uint64_t pad; unsigned char q[8][16]; } vh_regs;
typedef struct { uint64_t w[8]; } vh_ctx;

/* The shape of the stock note-start: x0..x7, s0..s4 and 3 stack args. */
typedef struct { long a[8]; float f[5]; unsigned char s0; void *s1, *s2; } seen_t;
static seen_t seen;
__attribute__((noinline)) static long target(long a0, long a1, long a2, long a3, long a4, long a5,
                                             long a6, long a7, float f0, float f1, float f2, float f3,
                                             float f4, unsigned char s0, void *s1, void *s2)
{
    long a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    float f[5] = {f0, f1, f2, f3, f4};
    memcpy(seen.a, a, sizeof a);
    memcpy(seen.f, f, sizeof f);
    seen.s0 = s0;
    seen.s1 = s1;
    seen.s2 = s2;
    return a0 + 1000;
}

typedef struct { long v[5]; } big_t; /* returned through x8 */
__attribute__((noinline)) static big_t target2(long a, float f)
{
    big_t b = {{a, a * 2, a * 3, (long)f, 77}};
    return b;
}

void *t_orig = (void *)target, *t_orig2 = (void *)target2;
int t_gate = 1;
static int pre_calls, post_calls;
static uint64_t pre_x0, post_ret, ctx_token;

void t_pre(vh_regs *r, vh_ctx *c)
{
    pre_calls++;
    pre_x0 = r->x[0];
    c->w[0] = 0xc0ffee;
    /* clobber argument registers the compiler might use: the trampoline must restore them */
    volatile double junk = 3.25 * (double)r->x[1];
    (void)junk;
}
void t_post(vh_regs *r, uint64_t ret, vh_ctx *c)
{
    (void)r;
    post_calls++;
    post_ret = ret;
    ctx_token = c->w[0];
}

long tt_call(long, long, long, long, long, long, long, long, float, float, float, float, float,
             unsigned char, void *, void *);
big_t tt_sret(long, float);

static int same(void)
{
    long a[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    float f[5] = {1.5f, 2.5f, -3.25f, 4.125f, 1e-3f};
    return memcmp(seen.a, a, sizeof a) == 0 && memcmp(seen.f, f, sizeof f) == 0 && seen.s0 == 0xab &&
           seen.s1 == (void *)0x1111222233334444ull && seen.s2 == (void *)&seen;
}

int main(void)
{
    for (int gate = 1; gate >= 0; gate--) {
        t_gate = gate;
        memset(&seen, 0, sizeof seen);
        pre_calls = post_calls = 0;
        long r = tt_call(11, 22, 33, 44, 55, 66, 77, 88, 1.5f, 2.5f, -3.25f, 4.125f, 1e-3f, 0xab,
                         (void *)0x1111222233334444ull, &seen);
        const char *path = gate ? "full path" : "fast path";
        CHECK(same(), "%s: 8 int + 5 float + 3 stack arguments arrive intact", path);
        CHECK(r == 1011, "%s: return value intact (%ld)", path, r);
        if (gate)
            CHECK(pre_calls == 1 && post_calls == 1 && pre_x0 == 11 && post_ret == 1011 && ctx_token == 0xc0ffee,
                  "full path: pre and post ran, saw the args and the result, shared ctx");
        else
            CHECK(pre_calls == 0 && post_calls == 0, "fast path: no pre/post");
        big_t b = tt_sret(5, 9.0f);
        CHECK(b.v[0] == 5 && b.v[1] == 10 && b.v[2] == 15 && b.v[3] == 9 && b.v[4] == 77,
              "%s: struct returned through x8 intact", path);
    }
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
