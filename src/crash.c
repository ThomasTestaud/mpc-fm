/* Crash report for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT.
 *
 * Our handler writes one block to the report file, then calls the handler
 * that was there before (SampQOL's crash log, then the default action), so
 * the crash itself behaves as before. Only async-signal-safe calls in the
 * handler: write(), and process_vm_readv() to walk the frame pointers
 * (a bad stack cannot crash us twice).
 */
#define _GNU_SOURCE
#include "crash.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <stdio.h>
#include <unistd.h>

#include "voice_hook.h"
#include "version.h"

static const int SIGS[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
#define NSIGS (int)(sizeof SIGS / sizeof *SIGS)
static struct sigaction g_old[NSIGS];
static int g_fd = -1;
static uintptr_t g_app_base, g_fm_base;

typedef struct {
    char b[1536];
    int n;
} out_t;

static void put(out_t *o, const char *s)
{
    while (*s && o->n < (int)sizeof o->b - 1)
        o->b[o->n++] = *s++;
}

static void put_hex(out_t *o, uint64_t v)
{
    char t[19] = "0x";
    int i = 2;
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((v >> sh) & 15);
        if (d || started || sh == 0) {
            t[i++] = "0123456789abcdef"[d];
            started = 1;
        }
    }
    t[i] = 0;
    put(o, t);
}

static void put_dec(out_t *o, long v)
{
    char t[24];
    int i = 23;
    t[i] = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    do {
        t[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        t[--i] = '-';
    put(o, t + i);
}

/* An address, and where it is: in the app (offset to compare with the
 * disassembly) or in fm.so. */
__attribute__((unused)) static void put_where(out_t *o, uint64_t a)
{
    put_hex(o, a);
    if (g_app_base && a >= g_app_base && a < g_app_base + 0x7300000) {
        put(o, " (app+");
        put_hex(o, a - g_app_base);
        put(o, ")");
    } else if (g_fm_base && a >= g_fm_base && a < g_fm_base + 0x100000) {
        put(o, " (fm.so+");
        put_hex(o, a - g_fm_base);
        put(o, ")");
    }
}

__attribute__((unused)) static int peek(uint64_t addr, uint64_t *v)
{
    struct iovec l = {v, 8}, r = {(void *)addr, 8};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == 8 ? 0 : -1;
}

static void handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    out_t o = {.n = 0};
    put(&o, "=== crash: signal ");
    put_dec(&o, sig);
    put(&o, " code ");
    put_dec(&o, si ? si->si_code : 0);
    put(&o, ", mpc-fm " MPCFM_VERSION ", tid ");
    put_dec(&o, (long)syscall(SYS_gettid));
    put(&o, ", in our hook: ");
    put(&o, vh_in_hook == 1 ? "YES (our code)" : vh_in_hook == 2 ? "in stock code called by our hook" : "no");
    put(&o, "\nfault address ");
    put_hex(&o, si ? (uint64_t)(uintptr_t)si->si_addr : 0);
#if defined(__aarch64__)
    put(&o, "\npc ");
    put_where(&o, uc->uc_mcontext.pc);
    put(&o, "\nlr ");
    put_where(&o, uc->uc_mcontext.regs[30]);
    put(&o, "\nbacktrace (frame pointers):");
    uint64_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 16 && fp; i++) {
        uint64_t next, ret;
        if (peek(fp, &next) || peek(fp + 8, &ret))
            break;
        put(&o, "\n  ");
        put_where(&o, ret & 0x0000ffffffffffffull); /* strip a PAC signature */
        if (next <= fp)
            break;
        fp = next;
    }
#else
    (void)uc;
#endif
    put(&o, "\n");
    if (g_fd >= 0 && write(g_fd, o.b, (size_t)o.n) < 0) {
        /* nothing more we can do */
    }

    /* Hand over to the previous handler. */
    for (int i = 0; i < NSIGS; i++) {
        if (SIGS[i] != sig)
            continue;
        struct sigaction *old = &g_old[i];
        if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
            old->sa_sigaction(sig, si, ucv);
            return;
        }
        if (old->sa_handler == SIG_IGN)
            return;
        if (old->sa_handler != SIG_DFL && old->sa_handler) {
            old->sa_handler(sig);
            return;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int crash_install(const char *path, uintptr_t app_base, char *msg, int cap)
{
    g_app_base = app_base;
    Dl_info info;
    if (dladdr((void *)crash_install, &info) && info.dli_fbase)
        g_fm_base = (uintptr_t)info.dli_fbase;
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int ok = 0;
    for (int i = 0; i < NSIGS; i++)
        ok += sigaction(SIGS[i], &sa, &g_old[i]) == 0;
    int chained = 0;
    for (int i = 0; i < NSIGS; i++)
        chained += (g_old[i].sa_flags & SA_SIGINFO) || (g_old[i].sa_handler != SIG_DFL);
    if (msg)
        snprintf(msg, (size_t)cap, "crash report on (%d signals, %d previous handlers kept, file %s)", ok,
                 chained, g_fd >= 0 ? "open" : "NOT open");
    return ok == NSIGS;
}
