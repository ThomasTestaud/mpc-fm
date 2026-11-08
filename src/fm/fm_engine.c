/* mpc-fm FM engine. See fm_engine.h.
 *
 * Per call, the frames are cut at FM_CELL boundaries. At the start of a cell
 * each envelope moves one cell ahead. Inside the cell the operator gain
 * ramps linearly, so envelopes and level/gain edits never click.
 *
 * Operators are rendered one after the other over the whole chunk, from
 * operator 4 down to operator 1, into small buffers. These loops have no
 * dependency from one frame to the next, so the compiler can vectorize them
 * (NEON on the device). Only operator 4 with feedback runs frame by frame.
 */
#include "fm_engine.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MOD_DEPTH 2.0f   /* phase deviation in cycles of a modulator at level 1 */
#define FB_MAX 0.3f      /* phase deviation in cycles of feedback 1 */
#define DB_FLOOR -96.0f  /* an envelope below this has ended */
#define MAX_INC 0.45f    /* operators above 0.45 * sample rate are muted */

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

/* ---- algorithms ---- */

typedef struct {
    const char *name;
    uint8_t mods[FM_OPS]; /* mods[i]: bit j = operator j+1 modulates operator i+1 */
    uint8_t carriers;
    float norm;           /* 1/sqrt(number of carriers) */
} algo_def;

#define B(i) (1u << (i))
static const algo_def ALGOS[FM_ALGORITHMS] = {
    {"4>3>2>1", {B(1), B(2), B(3), 0}, B(0), 1.0f},
    {"(4+3)>2>1", {B(1), B(2) | B(3), 0, 0}, B(0), 1.0f},
    {"(4>3)+2>1", {B(1) | B(2), 0, B(3), 0}, B(0), 1.0f},
    {"4>3>2, 1", {0, B(2), B(3), 0}, B(0) | B(1), 0.70710678f},
    {"4>3, 2>1", {B(1), 0, B(3), 0}, B(0) | B(2), 0.70710678f},
    {"4>(3,2,1)", {B(3), B(3), B(3), 0}, B(0) | B(1) | B(2), 0.57735027f},
    {"4>3, 2, 1", {0, 0, B(3), 0}, B(0) | B(1) | B(2), 0.57735027f},
    {"4, 3, 2, 1", {0, 0, 0, 0}, B(0) | B(1) | B(2) | B(3), 0.5f},
};
#undef B

const char *fm_algorithm_name(int a)
{
    return a >= 0 && a < FM_ALGORITHMS ? ALGOS[a].name : "?";
}

int fm_algorithm_carriers(int a)
{
    return a >= 0 && a < FM_ALGORITHMS ? ALGOS[a].carriers : 0;
}

/* ---- sine ---- */

/* sin(2 pi t) for t in [-1/2, 1/2]: folded to u in [-1/4, 1/4], then an
 * odd polynomial (near-minimax fit). Max error 6e-7 (-124 dB). */
static inline float sin2pi_half(float t)
{
    float a = fabsf(t);
    float u = a > 0.25f ? copysignf(0.5f - a, t) : t;
    float u2 = u * u;
    return u * (6.283164046f + u2 * (-41.33714251f + u2 * (81.34077283f + u2 * -70.99346696f)));
}

/* sin(2 pi x) for any x. */
static inline float sin2pi(float x) { return sin2pi_half(x - floorf(x + 0.5f)); }

/* sin(2 pi x) for |x| < 1.5, with a compare instead of floor: a shorter
 * dependency chain for the frame-by-frame feedback loop. Same result as
 * sin2pi() (x - 1 and x + 1 are exact there). */
static inline float sin2pi_near(float x)
{
    return sin2pi_half(x >= 0.5f ? x - 1.0f : x < -0.5f ? x + 1.0f : x);
}

float fm_sin2pi(float x) { return sin2pi(x); }

/* ---- patches ---- */

static float clampf(float x, float lo, float hi, float dflt)
{
    if (!(x == x)) /* NaN */
        return dflt;
    return x < lo ? lo : x > hi ? hi : x;
}

void fm_patch_init(fm_patch *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < FM_OPS; i++) {
        fm_op_params *o = &p->op[i];
        o->ratio = 1.0f;
        o->level = i == 0 ? 1.0f : 0.0f;
        o->attack = 0.001f;
        o->decay = 1.0f;
        o->sustain = 1.0f;
        o->release = 0.3f;
        o->vel_sens = 0.5f;
    }
    p->algorithm = 0;
    p->gain = 1.0f;
}

void fm_patch_sanitize(fm_patch *p)
{
    for (int i = 0; i < FM_OPS; i++) {
        fm_op_params *o = &p->op[i];
        o->ratio = clampf(o->ratio, 0.0625f, 32.0f, 1.0f);
        o->fixed_hz = clampf(o->fixed_hz, 0.0f, 20000.0f, 0.0f);
        o->detune = clampf(o->detune, -100.0f, 100.0f, 0.0f);
        o->level = clampf(o->level, 0.0f, 1.0f, 0.0f);
        o->attack = clampf(o->attack, 0.0005f, 20.0f, 0.001f);
        o->decay = clampf(o->decay, 0.001f, 30.0f, 1.0f);
        o->sustain = clampf(o->sustain, 0.0f, 1.0f, 1.0f);
        o->release = clampf(o->release, 0.001f, 30.0f, 0.3f);
        o->vel_sens = clampf(o->vel_sens, 0.0f, 1.0f, 0.5f);
    }
    if (p->algorithm < 0 || p->algorithm >= FM_ALGORITHMS)
        p->algorithm = 0;
    p->feedback = clampf(p->feedback, 0.0f, 1.0f, 0.0f);
    p->gain = clampf(p->gain, 0.0f, 2.0f, 1.0f);
}

/* ---- envelopes ---- */

static inline float db_to_amp(float db) { return exp2f(db * 0.16609640474f); } /* log2(10)/20 */
static inline float amp_to_db(float a) { return a > 1.6e-5f ? 20.0f * log10f(a) : DB_FLOOR; }

/* Move the envelope `frames` ahead; env_amp is the new amplitude. */
static void env_advance(fm_op_state *o, const fm_op_params *q, float frames, float sr)
{
    switch (o->env_stage) {
    case ENV_ATTACK: {
        float len = q->attack * sr;
        o->env_amp += len > frames ? frames / len : 1.0f;
        if (o->env_amp >= 1.0f) {
            o->env_amp = 1.0f;
            o->env_db = 0.0f;
            o->env_stage = ENV_DECAY;
        }
        return;
    }
    case ENV_DECAY: {
        float sus_db = amp_to_db(q->sustain);
        o->env_db -= frames * 60.0f / (q->decay * sr);
        if (o->env_db <= sus_db) {
            o->env_db = sus_db;
            o->env_stage = ENV_SUSTAIN;
        }
        break;
    }
    case ENV_SUSTAIN:
        o->env_db = amp_to_db(q->sustain); /* follows edits */
        break;
    case ENV_RELEASE:
        o->env_db -= frames * 60.0f / (q->release * sr);
        break;
    default:
        o->env_amp = 0.0f;
        return;
    }
    if (o->env_db <= DB_FLOOR) {
        o->env_db = DB_FLOOR;
        o->env_amp = 0.0f;
        o->env_stage = ENV_IDLE;
        return;
    }
    o->env_amp = db_to_amp(o->env_db);
}

/* ---- voices ---- */

void fm_voice_init(fm_voice *v, float sample_rate)
{
    memset(v, 0, sizeof *v);
    v->sample_rate = sample_rate > 0.0f ? sample_rate : 44100.0f;
    v->inv_sr = 1.0f / v->sample_rate;
    v->cell_pos = FM_CELL;
    v->carrier_mask = (1 << FM_OPS) - 1;
}

void fm_voice_reset(fm_voice *v)
{
    float sr = v->sample_rate;
    fm_voice_init(v, sr);
}

int fm_voice_active(const fm_voice *v)
{
    for (int i = 0; i < FM_OPS; i++)
        if ((v->carrier_mask & (1 << i)) &&
            (v->op[i].env_stage != ENV_IDLE || v->op[i].g1 != 0.0f || v->op[i].g0 != 0.0f))
            return 1;
    return 0;
}

void fm_voice_note_on(fm_voice *v, float velocity)
{
    if (!(velocity >= 0.0f))
        velocity = 0.0f;
    if (velocity > 1.0f)
        velocity = 1.0f;
    if (!fm_voice_active(v)) {
        /* Fresh start: same phases every time, so attacks sound the same. */
        fm_voice_reset(v);
    }
    for (int i = 0; i < FM_OPS; i++)
        v->op[i].env_stage = ENV_ATTACK; /* from the current level */
    v->velocity = velocity;
    v->gate = 1;
}

void fm_voice_note_off(fm_voice *v)
{
    for (int i = 0; i < FM_OPS; i++) {
        fm_op_state *o = &v->op[i];
        if (o->env_stage == ENV_IDLE || o->env_stage == ENV_RELEASE)
            continue;
        if (o->env_stage == ENV_ATTACK)
            o->env_db = amp_to_db(o->env_amp);
        o->env_stage = ENV_RELEASE;
    }
    v->gate = 0;
}

/* Gain of each operator at the end of a new cell. `unit` is MOD_DEPTH for
 * a modulator, or the output scale for a carrier. */
static void start_cell(fm_voice *v, const fm_patch *p, const float unit[FM_OPS],
                       const int muted[FM_OPS])
{
    for (int i = 0; i < FM_OPS; i++) {
        fm_op_state *o = &v->op[i];
        const fm_op_params *q = &p->op[i];
        env_advance(o, q, (float)FM_CELL, v->sample_rate);
        float vel = 1.0f - q->vel_sens + q->vel_sens * v->velocity;
        o->g0 = o->g1;
        o->g1 = muted[i] ? 0.0f : o->env_amp * q->level * vel * unit[i];
    }
    v->cell_pos = 0;
}

/* Phase in cycles, -0.5..0.5, from the fixed-point accumulator. */
static inline float cycles(uint32_t ph) { return (float)(int32_t)ph * 2.3283064365e-10f; }

/* Gain ramps by the frame's place in the cell (pos + k), not by the chunk:
 * the output is the same however the frames are cut into calls. */
static void op_plain(float *restrict y, uint32_t p0, uint32_t inc, float g0, float dg, int pos,
                     int n)
{
    for (int k = 0; k < n; k++)
        y[k] = sin2pi_half(cycles(p0 + (uint32_t)k * inc)) * (g0 + (float)(pos + k) * dg);
}

static void op_mod(float *restrict y, const float *restrict m, uint32_t p0, uint32_t inc, float g0,
                   float dg, int pos, int n)
{
    for (int k = 0; k < n; k++)
        y[k] = sin2pi(cycles(p0 + (uint32_t)k * inc) + m[k]) * (g0 + (float)(pos + k) * dg);
}

/* Operator 4 with feedback: the average of its last two outputs modulates
 * its phase (the average keeps high feedback stable). Frame by frame.
 * |y1|, |y2| <= 1 (normalized) and fbk <= FB_MAX / 2, so the phase stays
 * within +-0.8 cycles: sin2pi_near() is valid. */
static void op_feedback(float *restrict y, float fb[2], uint32_t p0, uint32_t inc, float g0,
                        float dg, int pos, float fbk, float unit, int n)
{
    /* The loop carries the normalized output (gain / unit), so the chain
     * from one frame to the next has one multiply less. */
    float inv = 1.0f / unit, gn0 = g0 * inv, dgn = dg * inv;
    float y1 = fb[0], y2 = fb[1];
    for (int k = 0; k < n; k++) {
        float yn = sin2pi_near(cycles(p0 + (uint32_t)k * inc) + fbk * (y1 + y2)) *
                   (gn0 + (float)(pos + k) * dgn);
        y2 = y1;
        y1 = yn;
        y[k] = yn * unit;
    }
    fb[0] = y1;
    fb[1] = y2;
}

static void render_chunk(fm_voice *v, const fm_patch *p, const algo_def *al,
                         const uint32_t inc[FM_OPS], const float unit[FM_OPS], float *restrict out,
                         int n)
{
    float mod[FM_OPS][FM_CELL];
    float y[FM_CELL];
    float fbk = p->feedback * FB_MAX * 0.5f;

    for (int k = 0; k < n; k++)
        out[k] = 0.0f;
    for (int i = 0; i < FM_OPS; i++)
        if (al->mods[i])
            memset(mod[i], 0, (size_t)n * sizeof(float));

    for (int i = FM_OPS - 1; i >= 0; i--) {
        fm_op_state *o = &v->op[i];
        float dg = (o->g1 - o->g0) * (1.0f / FM_CELL);
        int pos = v->cell_pos;

        if (o->g0 != 0.0f || o->g1 != 0.0f) {
            if (i == FM_OPS - 1 && fbk > 0.0f)
                op_feedback(y, v->fb, o->phase, inc[i], o->g0, dg, pos, fbk, unit[i], n);
            else if (al->mods[i])
                op_mod(y, mod[i], o->phase, inc[i], o->g0, dg, pos, n);
            else
                op_plain(y, o->phase, inc[i], o->g0, dg, pos, n);

            if (al->carriers & (1u << i))
                for (int k = 0; k < n; k++)
                    out[k] += y[k];
            for (int t = 0; t < i; t++)
                if (al->mods[t] & (1u << i))
                    for (int k = 0; k < n; k++)
                        mod[t][k] += y[k];
        } else if (i == FM_OPS - 1) {
            v->fb[0] = v->fb[1] = 0.0f;
        }

        o->phase += inc[i] * (uint32_t)n; /* wraps: exact */
    }
}

void fm_voice_render(fm_voice *v, const fm_patch *p, float note_hz, float *out, int n)
{
    if (n <= 0)
        return;
    if (!(note_hz > 0.0f && note_hz < 24000.0f)) { /* bad pitch: silence */
        memset(out, 0, (size_t)n * sizeof(float));
        return;
    }
    const algo_def *al = &ALGOS[(unsigned)p->algorithm % FM_ALGORITHMS];

    uint32_t inc[FM_OPS];
    float unit[FM_OPS];
    int muted[FM_OPS];
    int audible = al->carriers;
    for (int i = 0; i < FM_OPS; i++) {
        const fm_op_params *q = &p->op[i];
        float hz = q->fixed_hz > 0.0f ? q->fixed_hz : note_hz * q->ratio;
        if (q->detune != 0.0f)
            hz *= exp2f(q->detune * (1.0f / 1200.0f));
        double cyc = (double)hz * (double)v->inv_sr; /* cycles per frame */
        muted[i] = cyc >= MAX_INC; /* would alias: fade out */
        inc[i] = muted[i] ? 0u : (uint32_t)(cyc * 4294967296.0);
        if (q->level <= 0.0f)
            audible &= ~(1 << i);
        unit[i] = (al->carriers & (1u << i)) ? al->norm * p->gain : MOD_DEPTH;
        if (unit[i] <= 0.0f)
            unit[i] = 1e-9f; /* gain 0: keep 1/unit finite */
    }
    v->carrier_mask = audible;

    int done = 0;
    while (done < n) {
        if (v->cell_pos >= FM_CELL)
            start_cell(v, p, unit, muted);
        int len = FM_CELL - v->cell_pos;
        if (len > n - done)
            len = n - done;
        render_chunk(v, p, al, inc, unit, out + done, len);
        v->cell_pos += len;
        done += len;
    }
}
