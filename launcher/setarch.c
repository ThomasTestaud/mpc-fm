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
 *
 * Built static (musl), so LD_PRELOAD has no effect on this program.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* NULL: add FM to this start. Otherwise the reason why not. */
static const char *decide(const char *so, const char *preload)
{
    if (!so || !*so)
        return "MPCFM_SO is not set";
    if (strstr(preload, so))
        return "fm.so is already in LD_PRELOAD";
    if (access(so, R_OK) != 0)
        return "fm.so is missing";
    if (!g_state || !*g_state)
        return "MPCFM_STATE is not set";
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
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    g_state = getenv("MPCFM_STATE");
    g_logdir = getenv("MPCFM_LOGDIR");
    const char *so = getenv("MPCFM_SO");
    const char *preload = getenv("LD_PRELOAD");

    char real[512];
    if (!find_real(real, sizeof real)) {
        logline("no real setarch found");
        return 127;
    }

    /* Only the start of the stock app by SampQOL gets FM. */
    if (preload && strstr(preload, "mpcqol.so")) {
        const char *why = decide(so, preload);
        if (!why) {
            char value[2048];
            snprintf(value, sizeof value, "%s %s", preload, so);
            setenv("LD_PRELOAD", value, 1);
            logline("FM on: LD_PRELOAD=%s", value);
        } else {
            logline("FM off: %s", why);
        }
    }

    execv(real, argv);
    logline("exec %s failed: %s", real, strerror(errno));
    return 127;
}
