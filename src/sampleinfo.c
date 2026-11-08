/* What a Sample object is: its file name, and if its WAV file is an FM
 * carrier, and the patch library. See sampleinfo.h. */
#define _GNU_SOURCE
#include "sampleinfo.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "discover.h"
#include "fm/fm_carrier.h"

/* The sample's file name: a text that ends in ".wav", pointed to by a
 * field of the Sample object or of an object it points to (safe reads,
 * logger thread only). Also where it was found, so the audio thread can
 * check a Sample object still holds the same file. */

static int text_at(uintptr_t p, name_loc *out)
{
    char t[256];
    if (disc_read(t, p, 96) != 0)
        return 0;
    size_t n = 0;
    while (n < 96 && t[n] >= 0x20 && t[n] < 0x7f)
        n++;
    if (n == 96 && disc_read(t, p, sizeof t) == 0) /* a long path */
        while (n < sizeof t && t[n] >= 0x20 && t[n] < 0x7f)
            n++;
    if (n < 4 || n >= sizeof t || t[n] != 0)
        return 0;
    int wav = 0;
    for (size_t i = 0; i + 4 <= n; i++)
        if (t[i] == '.' && (t[i + 1] | 32) == 'w' && (t[i + 2] | 32) == 'a' && (t[i + 3] | 32) == 'v')
            wav = 1;
    if (!wav)
        return 0;
    memcpy(out->path, t, n + 1);
    const char *b = strrchr(out->path, '/');
    b = b ? b + 1 : out->path;
    size_t bl = strlen(b) < sizeof out->base - 1 ? strlen(b) : sizeof out->base - 1;
    memcpy(out->base, b, bl);
    out->base[bl] = 0;
    out->ptr = p;
    return 1;
}

static int plausible_ptr(uintptr_t v) { return v >= 0x10000 && v < (1ull << 48) && !(v & 7); }

/* Prefer a full path (with '/'): needed to open the file. */
int sample_name_loc(uintptr_t sample, name_loc *out)
{
    uintptr_t f[128];
    name_loc cand;
    int have = 0;
    memset(out, 0, sizeof *out);
    snprintf(out->base, sizeof out->base, "?");
    if (disc_read(f, sample, sizeof f) != 0)
        return 0;
    for (int i = 0; i < 128; i++)
        if (plausible_ptr(f[i]) && text_at(f[i], &cand)) {
            cand.i = i;
            cand.k = -1;
            cand.obj = 0;
            if (!have || (strchr(cand.path, '/') && !strchr(out->path, '/')))
                *out = cand;
            have = 1;
            if (strchr(out->path, '/'))
                return 1;
        }
    for (int i = 0; i < 128; i++) {
        uintptr_t g[64];
        if (!plausible_ptr(f[i]) || disc_read(g, f[i], sizeof g) != 0)
            continue;
        for (int k = 0; k < 64; k++)
            if (plausible_ptr(g[k]) && text_at(g[k], &cand)) {
                cand.i = i;
                cand.k = k;
                cand.obj = f[i];
                if (!have || (strchr(cand.path, '/') && !strchr(out->path, '/')))
                    *out = cand;
                have = 1;
                if (strchr(out->path, '/'))
                    return 1;
            }
    }
    return have;
}

void sample_name(uintptr_t sample, char *out, size_t cap)
{
    name_loc l;
    sample_name_loc(sample, &l);
    size_t n = strlen(l.base) < cap - 1 ? strlen(l.base) : cap - 1;
    memcpy(out, l.base, n);
    out[n] = 0;
}

int wav_read_head(const char *path, float *out, int nwant, wav_info *info)
{
    static unsigned char buf[65536];
    memset(info, 0, sizeof *info);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
        return 0;
    size_t data = 0, data_len = 0;
    for (size_t o = 12; o + 8 <= (size_t)n;) {
        uint32_t len = (uint32_t)buf[o + 4] | (uint32_t)buf[o + 5] << 8 | (uint32_t)buf[o + 6] << 16 |
                       (uint32_t)buf[o + 7] << 24;
        if (!memcmp(buf + o, "fmt ", 4) && o + 24 <= (size_t)n) {
            info->format = buf[o + 8] | buf[o + 9] << 8;
            info->channels = buf[o + 10] | buf[o + 11] << 8;
            info->rate = (int)((uint32_t)buf[o + 12] | (uint32_t)buf[o + 13] << 8 |
                               (uint32_t)buf[o + 14] << 16 | (uint32_t)buf[o + 15] << 24);
            info->bits = buf[o + 22] | buf[o + 23] << 8;
            if (info->format == 0xfffe && o + 34 <= (size_t)n) /* extensible: the subformat */
                info->format = buf[o + 32] | buf[o + 33] << 8;
        } else if (!memcmp(buf + o, "data", 4)) {
            data = o + 8;
            data_len = len;
            break;
        }
        o += 8 + len + (len & 1);
    }
    int bits = info->bits;
    if (!data || info->channels < 1 || (bits != 16 && bits != 24 && bits != 32))
        return 0;
    size_t frame = (size_t)info->channels * (size_t)(bits / 8);
    info->frames = (long)(data_len / frame);
    int got = 0;
    for (int i = 0; i < nwant; i++, got++) {
        if ((size_t)(i + 1) * frame > data_len || data + (size_t)(i + 1) * frame > (size_t)n)
            break;
        const unsigned char *q = buf + data + (size_t)i * frame;
        if (bits == 16) {
            int16_t v;
            memcpy(&v, q, 2);
            out[i] = (float)v / 32768.0f;
        } else if (bits == 24) {
            int32_t v = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8;
            out[i] = (float)v / 8388608.0f;
        } else if (info->format == 3) {
            memcpy(&out[i], q, 4);
        } else {
            int32_t v;
            memcpy(&v, q, 4);
            out[i] = (float)((double)v / 2147483648.0);
        }
    }
    return got;
}

int carrier_from_file(const char *path, fm_patch *p, int *root)
{
    static float f[FM_CARRIER_FRAMES];
    wav_info info;
    int n = wav_read_head(path, f, FM_CARRIER_FRAMES, &info);
    if (!n && !info.bits)
        return -1;
    if (n < FM_CARRIER_FRAMES)
        return 0;
    return fm_carrier_decode(f, FM_CARRIER_FRAMES, p, root) == 1;
}

/* ---- the library ----
 * The FM files on the card (by file name), and the patches edited on the
 * FM page (text files, see patchfile.h), which come first. The logger
 * thread scans; the logger and the FM page look up and save: one lock. */
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include "patchfile.h"

#define LIB_MAX 512
#define EDIT_MAX 256
typedef struct {
    char base[64]; /* file name without ".wav" / ".txt" */
    char path[256];
    fm_patch patch;
    int root;
} lib_entry;
static lib_entry g_lib[LIB_MAX], g_edit[EDIT_MAX];
static int g_nlib, g_nedit;
static char g_edit_dir[256];
static pthread_mutex_t g_lib_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned g_edit_gen;

int library_count(void) { return __atomic_load_n(&g_nlib, __ATOMIC_RELAXED); }
int library_edit_count(void) { return __atomic_load_n(&g_nedit, __ATOMIC_RELAXED); }
unsigned library_edit_generation(void) { return __atomic_load_n(&g_edit_gen, __ATOMIC_ACQUIRE); }

void library_set_edit_dir(const char *dir)
{
    pthread_mutex_lock(&g_lib_lock);
    snprintf(g_edit_dir, sizeof g_edit_dir, "%s", dir ? dir : "");
    pthread_mutex_unlock(&g_lib_lock);
}

static void copy_str(char *dst, size_t cap, const char *src, size_t n)
{
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int find_in(const lib_entry *t, int n, const char *base)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].base, base))
            return i;
    return -1;
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nlib < LIB_MAX) {
        if (e->d_name[0] == '.')
            continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len = strlen(e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0)
                scan_dir(path, depth - 1);
            continue;
        }
        if (len < 8 || strncmp(e->d_name, "FM-", 3) || strcasecmp(e->d_name + len - 4, ".wav"))
            continue;
        fm_patch p;
        int root = 60;
        if (carrier_from_file(path, &p, &root) != 1)
            continue;
        char base[64];
        copy_str(base, sizeof base, e->d_name, len - 4);
        if (find_in(g_lib, g_nlib, base) >= 0)
            continue; /* the first one found wins */
        lib_entry *l = &g_lib[g_nlib++];
        snprintf(l->base, sizeof l->base, "%s", base);
        copy_str(l->path, sizeof l->path, path, strlen(path));
        l->patch = p;
        l->root = root;
    }
    closedir(d);
}

static int read_file(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    return (int)n;
}

static void scan_edits(void)
{
    g_nedit = 0;
    if (!g_edit_dir[0])
        return;
    DIR *d = opendir(g_edit_dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nedit < EDIT_MAX) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".txt"))
            continue;
        char path[512], text[4096];
        snprintf(path, sizeof path, "%s/%s", g_edit_dir, e->d_name);
        fm_patch p;
        int root = 60;
        if (read_file(path, text, sizeof text) <= 0 || !pf_parse(text, &p, &root))
            continue;
        lib_entry *l = &g_edit[g_nedit++];
        copy_str(l->base, sizeof l->base, e->d_name, len - 4);
        copy_str(l->path, sizeof l->path, path, strlen(path));
        l->patch = p;
        l->root = root;
    }
    closedir(d);
}

int library_scan(const char *const *dirs, int ndirs)
{
    pthread_mutex_lock(&g_lib_lock);
    g_nlib = 0;
    for (int i = 0; i < ndirs; i++)
        scan_dir(dirs[i], 3);
    scan_edits();
    int n = g_nlib;
    pthread_mutex_unlock(&g_lib_lock);
    return n;
}

/* "FM-Bass-C3002.wav" -> "FM-Bass-C3002", then "FM-Bass-C3" (the MPC adds 3
 * digits to copies), up to twice. Calls try() with each; stops at 1. */
static int each_name(const char *name, int (*try)(const char *base, void *ctx), void *ctx)
{
    char b[96];
    const char *slash = strrchr(name, '/');
    snprintf(b, sizeof b, "%s", slash ? slash + 1 : name);
    size_t len = strlen(b);
    if (len > 4 && !strcasecmp(b + len - 4, ".wav"))
        b[len -= 4] = 0;
    for (int strip = 0; strip <= 2; strip++) {
        if (try(b, ctx))
            return 1;
        if (len < 4 || !(b[len - 1] >= '0' && b[len - 1] <= '9') ||
            !(b[len - 2] >= '0' && b[len - 2] <= '9') || !(b[len - 3] >= '0' && b[len - 3] <= '9'))
            break;
        b[len -= 3] = 0;
    }
    return 0;
}

typedef struct {
    fm_patch *p;
    int *root;
    char *found;
    size_t cap;
    int edited;
} find_ctx;

static int try_find(const char *base, void *v)
{
    find_ctx *c = v;
    int i = find_in(g_edit, g_nedit, base);
    const lib_entry *l = i >= 0 ? &g_edit[i] : NULL;
    if (!l) {
        i = find_in(g_lib, g_nlib, base);
        l = i >= 0 ? &g_lib[i] : NULL;
    }
    if (!l)
        return 0;
    c->edited = l >= g_edit && l < g_edit + EDIT_MAX;
    *c->p = l->patch;
    *c->root = l->root;
    if (c->found)
        snprintf(c->found, c->cap, "%s", l->path);
    return 1;
}

int library_find(const char *name, fm_patch *p, int *root, char *found, size_t cap)
{
    find_ctx c = {p, root, found, cap, 0};
    pthread_mutex_lock(&g_lib_lock);
    int r = each_name(name, try_find, &c);
    pthread_mutex_unlock(&g_lib_lock);
    return r ? 1 + c.edited : 0;
}

typedef struct {
    char *out;
    size_t cap;
} canon_ctx;

static int try_canon(const char *base, void *v)
{
    canon_ctx *c = v;
    if (find_in(g_edit, g_nedit, base) < 0 && find_in(g_lib, g_nlib, base) < 0)
        return 0;
    snprintf(c->out, c->cap, "%s", base);
    return 1;
}

static int keep_first(const char *base, void *v)
{
    canon_ctx *c = v;
    snprintf(c->out, c->cap, "%s", base);
    return 1;
}

void library_canonical(const char *name, char *out, size_t cap)
{
    canon_ctx c = {out, cap};
    pthread_mutex_lock(&g_lib_lock);
    int r = each_name(name, try_canon, &c);
    pthread_mutex_unlock(&g_lib_lock);
    if (!r)
        each_name(name, keep_first, &c);
    for (char *s = out; *s; s++) /* a FAT file name */
        if (strchr("/\\:*?\"<>|", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}

int library_save_edit(const char *name, const fm_patch *p, int root, char *msg, size_t cap)
{
    char base[64], text[2048], path[512], tmp[520];
    library_canonical(name, base, sizeof base);
    if (!base[0]) {
        snprintf(msg, cap, "no name");
        return 0;
    }
    size_t n = pf_format(p, root, base, text, sizeof text);
    pthread_mutex_lock(&g_lib_lock);
    int ok = 0;
    if (!g_edit_dir[0]) {
        snprintf(msg, cap, "no folder for edits");
        goto out;
    }
    mkdir(g_edit_dir, 0755);
    snprintf(path, sizeof path, "%s/%s.txt", g_edit_dir, base);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || !n) {
        snprintf(msg, cap, "cannot write %s", path);
        if (fd >= 0)
            close(fd);
        goto out;
    }
    ssize_t w = write(fd, text, n);
    int sync_ok = fsync(fd) == 0;
    close(fd);
    if (w != (ssize_t)n || !sync_ok || rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(msg, cap, "cannot write %s", path);
        goto out;
    }
    int i = find_in(g_edit, g_nedit, base);
    if (i < 0 && g_nedit < EDIT_MAX)
        i = g_nedit++;
    if (i >= 0) {
        lib_entry *l = &g_edit[i];
        snprintf(l->base, sizeof l->base, "%s", base);
        copy_str(l->path, sizeof l->path, path, strlen(path));
        l->patch = *p;
        l->root = root;
    }
    snprintf(msg, cap, "%s", path);
    __atomic_add_fetch(&g_edit_gen, 1, __ATOMIC_RELEASE);
    ok = 1;
out:
    pthread_mutex_unlock(&g_lib_lock);
    return ok;
}
