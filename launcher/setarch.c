/* setarch stand-in: puts mpc-fm's fm.so next to SampQOL's mpcqol.so.
 *
 * Our autorun.sh puts the folder with this program first in PATH, then
 * starts SampQOL OS unchanged. SampQOL's qol-run.sh starts the stock app as
 *     LD_PRELOAD=/tmp/mpcqol.so  setarch -R -- "/usr/bin/MPC Sample"
 * so "setarch" is this program. It appends $MPCFM_SO to LD_PRELOAD and
 * then execs the real setarch with the same arguments. Every other use of
 * setarch passes through unchanged.
 *
 * Crash guard. qol-run.sh only starts the app again when it has stopped
 * (crash or kill). State is kept in $MPCFM_STATE (tmpfs, so it resets at
 * every boot). FM stays off until the next boot when:
 *   - the app stopped less than MIN_RUN_S after a start with FM, or
 *   - the app already started MAX_FM_STARTS times with FM.
 * SampQOL itself then starts without FM, so a bad fm.so cannot lock you out.
 * autorun.sh can also turn FM off before (MPCFM_OFF = the short reason).
 *
 * Status strip: over SampQOL's splash it writes "mpc-fm <version>  FM on"
 * or "FM off: <reason>" (strip.c), so you see the result at every start.
 *
 * Built static (musl), so LD_PRELOAD has no effect on this program.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "strip.h"
#include "version.h"

#define MIN_RUN_S 600
#define MAX_FM_STARTS 3

static const char *g_state, *g_logdir;

static long uptime_s(void)
{
#ifdef MPCFM_TEST
    const char *t = getenv("MPCFM_TEST_UPTIME");
    if (t)
        return atol(t);
#endif
    char b[64] = {0};
    int fd = open("/proc/uptime", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    return n > 0 ? atol(b) : -1;
}

static void state_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_state, name);
}

static long read_num(const char *name)
{
    char p[512], b[32] = {0};
    state_path(p, sizeof p, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    return n > 0 ? atol(b) : -1;
}

static void write_num(const char *name, long v)
{
    char p[512], b[32];
    state_path(p, sizeof p, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    int n = snprintf(b, sizeof b, "%ld\n", v);
    if (write(fd, b, (size_t)n) < 0) {
        /* best effort */
    }
    close(fd);
}

static int state_has(const char *name)
{
    char p[512];
    state_path(p, sizeof p, name);
    return access(p, F_OK) == 0;
}

static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...)
{
    char msg[2400], line[2600], when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%F %T", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof line, "%s setarch (up %ld s): %s\n", when, uptime_s(), msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof line - 1;

    const char *dirs[2] = {g_state, g_logdir};
    const char *names[2] = {"launch.log", "mpcfm-launch.log"};
    for (int i = 0; i < 2; i++) {
        if (!dirs[i] || !*dirs[i])
            continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dirs[i], names[i]);
        int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0)
            continue;
        if (write(fd, line, (size_t)n) < 0) {
            /* best effort */
        }
        close(fd);
    }
}

/* After a crash the app restarts through here: keep what explains it.
 * SampQOL's crash log is in RAM (/tmp), the kernel's line in /dev/kmsg. */
static void copy_file_tail(const char *src, const char *dst_name, const char *title)
{
    char buf[16384];
    int fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > (off_t)sizeof buf ? size - (off_t)sizeof buf : 0;
    ssize_t n = pread(fd, buf, sizeof buf, from);
    close(fd);
    if (n <= 0 || !g_logdir || !*g_logdir)
        return;
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_logdir, dst_name);
    int out = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (out < 0)
        return;
    char head[200];
    int hn = snprintf(head, sizeof head, "\n===== %s (up %ld s) =====\n", title, uptime_s());
    if (write(out, head, (size_t)hn) < 0 || write(out, buf, (size_t)n) < 0) {
        /* best effort */
    }
    close(out);
}

static void copy_kernel_crash_lines(void)
{
#ifdef MPCFM_TEST
    const char *kmsg = getenv("MPCFM_TEST_KMSG") ? getenv("MPCFM_TEST_KMSG") : "/nonexistent";
#else
    const char *kmsg = "/dev/kmsg";
#endif
    int fd = open(kmsg, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || !g_logdir || !*g_logdir) {
        if (fd >= 0)
            close(fd);
        return;
    }
    static char keep[40][300];
    int nkeep = 0, total = 0;
    char rec[2048];
    for (;;) { /* one record per read, EAGAIN at the end */
        ssize_t n = read(fd, rec, sizeof rec - 1);
        if (n <= 0)
            break;
        rec[n] = 0;
        char *msg = strchr(rec, ';');
        msg = msg ? msg + 1 : rec;
        if (strstr(msg, "MPC Sample") || strstr(msg, "segfault") || strstr(msg, "unhandled") ||
            strstr(msg, "Unable to handle") || strstr(msg, " pc ") || strstr(msg, "Call trace")) {
            size_t ml = strlen(msg) < sizeof keep[0] - 1 ? strlen(msg) : sizeof keep[0] - 1;
            memcpy(keep[nkeep % 40], msg, ml);
            keep[nkeep % 40][ml] = 0;
            nkeep++;
        }
        total++;
    }
    close(fd);
    char p[512];
    snprintf(p, sizeof p, "%s/mpcfm-kernel.log", g_logdir);
    int out = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (out < 0)
        return;
    char head[160];
    int hn = snprintf(head, sizeof head, "\n===== kernel messages about the app (up %ld s, %d of %d) =====\n",
                      uptime_s(), nkeep < 40 ? nkeep : 40, total);
    if (write(out, head, (size_t)hn) < 0) {
        /* best effort */
    }
    for (int i = nkeep > 40 ? nkeep - 40 : 0; i < nkeep; i++) {
        const char *l = keep[i % 40];
        size_t len = strlen(l);
        if (write(out, l, len) < 0 || (len && l[len - 1] != '\n' && write(out, "\n", 1) < 0))
            break;
    }
    close(out);
}

static int same_file(const char *a, const char *b)
{
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

static int usable(const char *path)
{
    return access(path, X_OK) == 0 && !same_file(path, "/proc/self/exe");
}

/* The first setarch in PATH that is not this program. */
static int find_real(char *out, size_t cap)
{
    const char *skip = getenv("MPCFM_BIN");
    const char *p = getenv("PATH");
    while (p && *p) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && len < 400) {
            char dir[400];
            memcpy(dir, p, len);
            dir[len] = 0;
            if (!(skip && strcmp(dir, skip) == 0)) {
                snprintf(out, cap, "%s/setarch", dir);
                if (usable(out))
                    return 1;
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
    static const char *const fallback[] = {"/usr/bin/setarch", "/bin/setarch",
                                           "/usr/sbin/setarch", "/sbin/setarch"};
    for (size_t i = 0; i < sizeof fallback / sizeof *fallback; i++)
        if (usable(fallback[i])) {
            snprintf(out, cap, "%s", fallback[i]);
            return 1;
        }
    return 0;
}

/* NULL: add FM to this start. Otherwise the reason why not (for the log),
 * and *label, a short one for the screen. */
static const char *decide(const char *so, const char *preload, const char **label)
{
    const char *off = getenv("MPCFM_OFF");
    *label = "not set up";
    if (off && *off) {
        *label = off;
        return "turned off by autorun.sh (see its line above)";
    }
    if (!so || !*so)
        return "MPCFM_SO is not set";
    if (strstr(preload, so)) {
        *label = "on";
        return "fm.so is already in LD_PRELOAD";
    }
    if (access(so, R_OK) != 0) {
        *label = "fm.so missing";
        return "fm.so is missing";
    }
    if (!g_state || !*g_state)
        return "MPCFM_STATE is not set";
    *label = "crash guard";
    if (state_has("off"))
        return "off until the next boot (see the line that turned it off)";

    long now = uptime_s(), starts = read_num("fm-starts"), last = read_num("fm-last");
    if (starts < 0)
        starts = 0;
    const char *why = NULL;
    if (starts >= MAX_FM_STARTS)
        why = "the app already started 3 times with FM in this boot";
    else if (starts > 0 && last >= 0 && now >= 0 && now - last < MIN_RUN_S)
        why = "the app stopped less than 600 s after its start with FM";
    if (why) {
        write_num("off", 1);
        return why;
    }
    write_num("fm-starts", starts + 1);
    write_num("fm-last", now);
    *label = "on";
    return NULL;
}

static const char *fb_path(void)
{
#ifdef MPCFM_TEST
    return getenv("MPCFM_TEST_FB"); /* NULL: tests that do not look at the screen */
#else
    return "/dev/fb0";
#endif
}

/* Draw the status line. Returns strip_draw()'s result. */
static int draw(const char *label, int on)
{
    const char *fb = fb_path();
    if (!fb)
        return STRIP_ERR_OPEN;
    char status[96];
    snprintf(status, sizeof status, on ? "FM %s" : "FM off: %s", label);
    return strip_draw(fb, "mpc-fm " MPCFM_VERSION "  ", status, on ? STRIP_RGB_ON : STRIP_RGB_OFF);
}

static const char *strip_error(int r)
{
    switch (r) {
    case STRIP_ERR_OPEN: return "cannot open the framebuffer";
    case STRIP_ERR_IOCTL: return "no screen info";
    case STRIP_ERR_FORMAT: return "unknown screen format";
    default: return "write failed";
    }
}

static void show(const char *label, int on)
{
    int r = draw(label, on);
    if (r == 0)
        logline("status line drawn: FM %s%s", on ? "" : "off: ", label);
    else if (fb_path())
        logline("status line not drawn: %s", strip_error(r));
}

/* ---- boot status watcher: setarch --mpcfm-splash on|off LABEL ----
 *
 * Started in the background by our autorun.sh, before SampQOL starts. The
 * line drawn at exec time alone can be too short to see: SampQOL shows its
 * splash right after it stops the stock app, then waits about a second and
 * prepares its project safety net before it starts the app. So this
 * watcher waits until the stock app has stopped, then redraws the line
 * every 0.3 s while no app runs (only then is the framebuffer on the
 * screen), and quits when the new app starts or after 60 s. */

static const char *app_exe(void)
{
#ifdef MPCFM_TEST
    const char *t = getenv("MPCFM_TEST_APP_EXE");
    if (t)
        return t;
#endif
    return "/usr/bin/MPC Sample";
}

static int app_running(const char *exe)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        char p[300], t[512];
        snprintf(p, sizeof p, "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(p, t, sizeof t - 1);
        if (n > 0) {
            t[n] = 0;
            found = strcmp(t, exe) == 0;
        }
    }
    closedir(d);
    return found;
}

static void nap_ms(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static int splash_watch(int on, const char *label)
{
    const char *exe = app_exe();
    long limit = 60000, t = 0;
#ifdef MPCFM_TEST
    const char *l = getenv("MPCFM_TEST_SPLASH_MS");
    if (l)
        limit = atol(l);
#endif
    while (app_running(exe)) { /* the stock app still owns the screen */
        if (t >= limit)
            return 0;
        nap_ms(100);
        t += 100;
    }
    nap_ms(300); /* SampQOL draws its splash right after it stops the app */
    t += 300;
    int drawn = 0, last = 0;
    while (t < limit && !app_running(exe)) {
        last = draw(label, on);
        drawn += last == 0;
        nap_ms(300);
        t += 300;
    }
    if (drawn)
        logline("boot status line shown during the splash (%d redraws)", drawn);
    else
        logline("boot status line not shown during the splash: %s",
                t >= limit ? "timeout" : strip_error(last));
    return 0;
}

int main(int argc, char **argv)
{
    g_state = getenv("MPCFM_STATE");
    g_logdir = getenv("MPCFM_LOGDIR");
    if (argc == 4 && strcmp(argv[1], "--mpcfm-splash") == 0)
        return splash_watch(strcmp(argv[2], "on") == 0, argv[3]);
    const char *so = getenv("MPCFM_SO");
    const char *preload = getenv("LD_PRELOAD");

    char real[512];
    if (!find_real(real, sizeof real)) {
        logline("no real setarch found");
        return 127;
    }

    /* Only the start of the stock app by SampQOL gets FM. */
    if (preload && strstr(preload, "mpcqol.so")) {
        long launches = g_state && *g_state ? read_num("launches") : -1;
        if (launches < 0)
            launches = 0;
        if (g_state && *g_state)
            write_num("launches", launches + 1);
        if (launches > 0) { /* a restart: the app stopped, maybe crashed */
#ifdef MPCFM_TEST
            const char *qc = getenv("MPCFM_TEST_QOLCRASH") ? getenv("MPCFM_TEST_QOLCRASH") : "/nonexistent";
#else
            const char *qc = "/tmp/mpcqol-crash.txt";
#endif
            copy_file_tail(qc, "mpcqol-crash-copy.txt", "SampQOL crash log (from RAM)");
            copy_kernel_crash_lines();
            logline("app restart %ld: crash logs copied (if any)", launches);
        }
        const char *label;
        const char *why = decide(so, preload, &label);
        if (!why) {
            char value[2048];
            snprintf(value, sizeof value, "%s %s", preload, so);
            setenv("LD_PRELOAD", value, 1);
            logline("FM on: LD_PRELOAD=%s", value);
        } else {
            logline("FM off: %s", why);
        }
        show(label, !why || strcmp(label, "on") == 0);
    }

    execv(real, argv);
    logline("exec %s failed: %s", real, strerror(errno));
    return 127;
}
