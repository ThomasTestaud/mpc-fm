/* What a Sample object is: its file name, and if its WAV file is an FM
 * carrier. Logger thread only (safe reads, file I/O). See sampleinfo.h. */
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

/* ---- the library ---- */
#include <dirent.h>
#include <sys/stat.h>

#define LIB_MAX 512
static struct {
    char base[64]; /* file name without ".wav" */
    char path[256];
    fm_patch patch;
    int root;
} g_lib[LIB_MAX];
static int g_nlib;

int library_count(void) { return g_nlib; }

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
        size_t bl = len - 4 < sizeof g_lib[0].base - 1 ? len - 4 : sizeof g_lib[0].base - 1;
        int dup = 0;
        for (int i = 0; i < g_nlib; i++)
            dup |= strlen(g_lib[i].base) == bl && !strncmp(g_lib[i].base, e->d_name, bl);
        if (dup)
            continue; /* the first one found wins */
        memcpy(g_lib[g_nlib].base, e->d_name, bl);
        g_lib[g_nlib].base[bl] = 0;
        size_t pl = strlen(path) < sizeof g_lib[0].path - 1 ? strlen(path) : sizeof g_lib[0].path - 1;
        memcpy(g_lib[g_nlib].path, path, pl);
        g_lib[g_nlib].path[pl] = 0;
        g_lib[g_nlib].patch = p;
        g_lib[g_nlib].root = root;
        g_nlib++;
    }
    closedir(d);
}

int library_scan(const char *const *dirs, int ndirs)
{
    g_nlib = 0;
    for (int i = 0; i < ndirs; i++)
        scan_dir(dirs[i], 3);
    return g_nlib;
}

int library_find(const char *name, fm_patch *p, int *root, char *found, size_t cap)
{
    char b[96];
    const char *slash = strrchr(name, '/');
    snprintf(b, sizeof b, "%s", slash ? slash + 1 : name);
    size_t len = strlen(b);
    if (len > 4 && !strcasecmp(b + len - 4, ".wav"))
        b[len -= 4] = 0;
    for (int strip = 0; strip <= 2; strip++) { /* "X", then X minus 3 digits, twice */
        for (int i = 0; i < g_nlib; i++)
            if (!strcmp(g_lib[i].base, b)) {
                *p = g_lib[i].patch;
                *root = g_lib[i].root;
                if (found)
                    snprintf(found, cap, "%s", g_lib[i].path);
                return 1;
            }
        if (len < 4 || !(b[len - 1] >= '0' && b[len - 1] <= '9') || !(b[len - 2] >= '0' && b[len - 2] <= '9') ||
            !(b[len - 3] >= '0' && b[len - 3] <= '9'))
            break;
        b[len -= 3] = 0;
    }
    return 0;
}

