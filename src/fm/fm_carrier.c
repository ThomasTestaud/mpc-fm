/* FM carrier samples. See fm_carrier.h. */
#include "fm_carrier.h"

#include <math.h>
#include <string.h>

static const unsigned char MAGIC[4] = {'M', 'P', 'F', 'M'};

static uint32_t crc32(const unsigned char *p, int n)
{
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put_f32(unsigned char *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    for (int i = 0; i < 4; i++)
        b[i] = (unsigned char)(u >> (8 * i));
}

static float get_f32(const unsigned char *b)
{
    uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void pack(const fm_patch *p, int root, unsigned char *b)
{
    memcpy(b, MAGIC, 4);
    b[4] = FM_CARRIER_VERSION;
    b[5] = (unsigned char)root;
    b[6] = b[7] = 0;
    unsigned char *q = b + 8;
    for (int i = 0; i < FM_OPS; i++) {
        const fm_op_params *o = &p->op[i];
        const float v[9] = {o->ratio, o->fixed_hz, o->detune, o->level, o->attack,
                            o->decay, o->sustain, o->release, o->vel_sens};
        for (int k = 0; k < 9; k++, q += 4)
            put_f32(q, v[k]);
    }
    put_f32(q, (float)p->algorithm);
    put_f32(q + 4, p->feedback);
    put_f32(q + 8, p->gain);
    q += 12;
    uint32_t c = crc32(b, (int)(q - b));
    for (int i = 0; i < 4; i++)
        q[i] = (unsigned char)(c >> (8 * i));
}

static int unpack(const unsigned char *b, fm_patch *p, int *root)
{
    if (memcmp(b, MAGIC, 4) != 0)
        return 0;
    const int body = FM_CARRIER_BYTES - 4;
    uint32_t c = (uint32_t)b[body] | (uint32_t)b[body + 1] << 8 | (uint32_t)b[body + 2] << 16 |
                 (uint32_t)b[body + 3] << 24;
    if (b[4] != FM_CARRIER_VERSION || crc32(b, body) != c)
        return -1;
    fm_patch_init(p);
    const unsigned char *q = b + 8;
    for (int i = 0; i < FM_OPS; i++) {
        fm_op_params *o = &p->op[i];
        float *v[9] = {&o->ratio, &o->fixed_hz, &o->detune, &o->level, &o->attack,
                       &o->decay, &o->sustain, &o->release, &o->vel_sens};
        for (int k = 0; k < 9; k++, q += 4)
            *v[k] = get_f32(q);
    }
    float a = get_f32(q);
    p->algorithm = a == a && a >= 0.0f && a < (float)FM_ALGORITHMS ? (int)a : 0;
    p->feedback = get_f32(q + 4);
    p->gain = get_f32(q + 8);
    fm_patch_sanitize(p);
    *root = b[5] <= 127 ? b[5] : 60;
    return 1;
}

int fm_carrier_encode(const fm_patch *p, int root_note, int16_t *pcm, int frames)
{
    if (frames < FM_CARRIER_FRAMES)
        return 0;
    unsigned char b[FM_CARRIER_BYTES];
    pack(p, root_note, b);
    for (int i = 0; i < FM_CARRIER_FRAMES; i++) {
        int nib = (b[i / 2] >> (4 * (i & 1))) & 0xf;
        pcm[i] = (int16_t)((pcm[i] & ~0xf) | nib);
    }
    return 1;
}

/* Low nibble of frame i, read back as 16-bit with this scale. */
static inline int nibble(const float *f, int i, float scale)
{
    long v = lrintf(f[i] * scale);
    return (int)(v & 0xf);
}

static int read_bytes(const float *f, int count, float scale, unsigned char *b, int nbytes)
{
    if (count < nbytes * 2)
        return 0;
    for (int i = 0; i < nbytes; i++)
        b[i] = (unsigned char)(nibble(f, 2 * i, scale) | nibble(f, 2 * i + 1, scale) << 4);
    return 1;
}

static const float SCALES[2] = {32768.0f, 32767.0f};

/* Other formats: the 16-bit value of frame i, its low nibble. */
static inline int nibble_fmt(const void *f, int fmt, int i)
{
    if (fmt == FM_FMT_I16) {
        int16_t v;
        memcpy(&v, (const char *)f + 2 * i, 2);
        return v & 0xf;
    }
    int32_t v; /* FM_FMT_I32: 16-bit sample in the high half */
    memcpy(&v, (const char *)f + 4 * i, 4);
    return (v >> 16) & 0xf;
}

static int read_bytes_fmt(const void *f, int fmt, int count, unsigned char *b, int nbytes)
{
    if (count < nbytes * 2)
        return 0;
    for (int i = 0; i < nbytes; i++)
        b[i] = (unsigned char)(nibble_fmt(f, fmt, 2 * i) | nibble_fmt(f, fmt, 2 * i + 1) << 4);
    return 1;
}

int fm_carrier_frame_bytes(int fmt) { return fmt == FM_FMT_I16 ? 2 : 4; }

int fm_carrier_has_magic_fmt(const void *frames, int fmt, int count)
{
    if (fmt == FM_FMT_F32)
        return fm_carrier_has_magic(frames, count);
    unsigned char b[4];
    return read_bytes_fmt(frames, fmt, count, b, 4) && memcmp(b, MAGIC, 4) == 0;
}

int fm_carrier_decode_fmt(const void *frames, int fmt, int count, fm_patch *p, int *root_note)
{
    if (fmt == FM_FMT_F32)
        return fm_carrier_decode(frames, count, p, root_note);
    unsigned char b[FM_CARRIER_BYTES];
    if (!read_bytes_fmt(frames, fmt, count, b, 4) || memcmp(b, MAGIC, 4) != 0)
        return 0;
    if (!read_bytes_fmt(frames, fmt, count, b, FM_CARRIER_BYTES))
        return -1;
    return unpack(b, p, root_note);
}

int fm_carrier_has_magic(const float *frames, int count)
{
    unsigned char b[4];
    for (int s = 0; s < 2; s++)
        if (read_bytes(frames, count, SCALES[s], b, 4) && memcmp(b, MAGIC, 4) == 0)
            return 1;
    return 0;
}

int fm_carrier_decode(const float *frames, int count, fm_patch *p, int *root_note)
{
    int result = 0;
    unsigned char b[FM_CARRIER_BYTES];
    for (int s = 0; s < 2; s++) {
        if (!read_bytes(frames, count, SCALES[s], b, 4) || memcmp(b, MAGIC, 4) != 0)
            continue;
        if (!read_bytes(frames, count, SCALES[s], b, FM_CARRIER_BYTES))
            return -1;
        int r = unpack(b, p, root_note);
        if (r == 1)
            return 1;
        result = r;
    }
    return result;
}
