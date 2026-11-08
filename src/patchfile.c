/* Edited patches as text (see patchfile.h). No printf/strtod for the
 * numbers: the app may change the locale, and "0,5" must not happen. */
#include "patchfile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *p;
    size_t cap, len;
    int full;
} wbuf;

static void put(wbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->full = 1;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void put_int(wbuf *b, long v)
{
    char t[24];
    int neg = v < 0, i = sizeof t - 1;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (neg)
        t[--i] = '-';
    put(b, t + i);
}

/* Up to 6 decimals, trailing zeros dropped: 0.25, 4, 0.0005 */
static void put_num(wbuf *b, float v)
{
    if (!isfinite(v))
        v = 0.0f;
    double d = (double)v;
    if (d < 0) {
        put(b, "-");
        d = -d;
    }
    long long m = llround(d * 1e6);
    put_int(b, (long)(m / 1000000));
    long frac = (long)(m % 1000000);
    if (!frac)
        return;
    char t[8];
    for (int i = 5; i >= 0; i--, frac /= 10)
        t[i] = (char)('0' + frac % 10);
    int n = 6;
    while (n > 0 && t[n - 1] == '0')
        n--;
    t[n] = 0;
    put(b, ".");
    put(b, t);
}

static void kv(wbuf *b, const char *k, float v)
{
    put(b, " ");
    put(b, k);
    put(b, " ");
    put_num(b, v);
}

size_t pf_format(const fm_patch *p, int root, const char *name, char *out, size_t cap)
{
    wbuf b = {out, cap, 0, 0};
    if (!cap)
        return 0;
    out[0] = 0;
    put(&b, "# mpc-fm patch (edited on the MPC; values as on the FM page)\nname ");
    char nm[96];
    snprintf(nm, sizeof nm, "%s", name ? name : "");
    for (char *c = nm; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    put(&b, nm);
    put(&b, "\nroot ");
    put_int(&b, root);
    put(&b, "\nalgorithm ");
    put_int(&b, p->algorithm + 1);
    put(&b, "\nfeedback ");
    put_num(&b, p->feedback);
    put(&b, "\nvolume ");
    put_num(&b, p->gain);
    put(&b, "\n");
    for (int i = 0; i < FM_OPS; i++) {
        const fm_op_params *o = &p->op[i];
        put(&b, "op");
        put_int(&b, i + 1);
        kv(&b, "ratio", o->ratio);
        kv(&b, "fixed", o->fixed_hz);
        kv(&b, "detune", o->detune);
        kv(&b, "level", o->level);
        kv(&b, "attack", o->attack);
        kv(&b, "decay", o->decay);
        kv(&b, "sustain", o->sustain);
        kv(&b, "release", o->release);
        kv(&b, "velocity", o->vel_sens);
        put(&b, "\n");
    }
    return b.full ? 0 : b.len;
}

/* ---- reading ---- */

static const char *skip_sp(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static const char *word(const char *s, char *w, size_t cap)
{
    s = skip_sp(s);
    size_t n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
        if (n + 1 < cap)
            w[n++] = *s;
        s++;
    }
    w[n] = 0;
    return s;
}

/* A number like -12, 0.25, 4. 1: read. */
static int num(const char *w, float *out)
{
    const char *s = w;
    int neg = 0, digits = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    double v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        digits++;
    }
    if (*s == '.' || *s == ',') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * f;
            f *= 0.1;
            digits++;
        }
    }
    if (!digits || *s)
        return 0;
    *out = (float)(neg ? -v : v);
    return 1;
}

static float *op_field(fm_op_params *o, const char *k)
{
    static const struct {
        const char *k;
        size_t off;
    } F[] = {
        {"ratio", offsetof(fm_op_params, ratio)},     {"fixed", offsetof(fm_op_params, fixed_hz)},
        {"detune", offsetof(fm_op_params, detune)},   {"level", offsetof(fm_op_params, level)},
        {"attack", offsetof(fm_op_params, attack)},   {"decay", offsetof(fm_op_params, decay)},
        {"sustain", offsetof(fm_op_params, sustain)}, {"release", offsetof(fm_op_params, release)},
        {"velocity", offsetof(fm_op_params, vel_sens)},
    };
    for (size_t i = 0; i < sizeof F / sizeof *F; i++)
        if (!strcmp(k, F[i].k))
            return (float *)((char *)o + F[i].off);
    return NULL;
}

int pf_parse(const char *text, fm_patch *p, int *root)
{
    fm_patch_init(p);
    int r = 60, magic = 0, fields = 0;
    for (const char *s = text; s && *s;) {
        const char *eol = strchr(s, '\n');
        char line[512];
        size_t n = eol ? (size_t)(eol - s) : strlen(s);
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        s = eol ? eol + 1 : NULL;
        if (strstr(line, "mpc-fm patch"))
            magic = 1;
        char k[32], w[64];
        const char *q = word(line, k, sizeof k);
        if (!k[0] || k[0] == '#')
            continue;
        float v;
        if (!strncmp(k, "op", 2) && k[2] >= '1' && k[2] <= '0' + FM_OPS && !k[3]) {
            fm_op_params *o = &p->op[k[2] - '1'];
            for (;;) {
                q = word(q, w, sizeof w);
                if (!w[0])
                    break;
                float *f = op_field(o, w);
                q = word(q, w, sizeof w);
                if (f && num(w, &v)) {
                    *f = v;
                    fields++;
                }
            }
            continue;
        }
        word(q, w, sizeof w);
        if (!num(w, &v))
            continue;
        if (!strcmp(k, "root"))
            r = (int)lrintf(v), fields++;
        else if (!strcmp(k, "algorithm"))
            p->algorithm = (int)lrintf(v) - 1, fields++;
        else if (!strcmp(k, "feedback"))
            p->feedback = v, fields++;
        else if (!strcmp(k, "volume"))
            p->gain = v, fields++;
    }
    if (!magic || !fields)
        return 0;
    fm_patch_sanitize(p);
    *root = r >= 0 && r <= 127 ? r : 60;
    return 1;
}
