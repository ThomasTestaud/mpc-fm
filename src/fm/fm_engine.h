/* mpc-fm FM engine: 4 operators, 8 algorithms, one envelope per operator.
 *
 * It is made to be the sound source of a sampler voice: the stock voice
 * calls fm_voice_render() where it would read sample data, then applies its
 * own filter, amp envelope, pan and effects to our output. So the output is
 * mono and dry.
 *
 * Real-time rules: the fm_voice_* functions do not allocate, lock or make
 * syscalls. Any number of frames per call: the output does not depend on
 * how a note is cut into calls (envelopes run on a fixed FM_CELL grid).
 *
 * Patches must go through fm_patch_sanitize() before the audio thread sees
 * them (UI edits, file loads). The audio thread trusts sanitized patches.
 */
#ifndef MPCFM_FM_ENGINE_H
#define MPCFM_FM_ENGINE_H

#define FM_OPS 4
#define FM_ALGORITHMS 8
#define FM_CELL 32 /* envelope grid in frames (0.73 ms at 44.1 kHz) */

/* Operator i+1 is op[i]. Operator 4 (op[3]) is always the top modulator. */
typedef struct {
    float ratio;    /* frequency = note * ratio (if fixed_hz is 0), 1/16..32 */
    float fixed_hz; /* > 0: fixed frequency in Hz, for drums and noise */
    float detune;   /* cents, -100..100 */
    float level;    /* 0..1. Carrier: output level. Modulator: depth */
    float attack;   /* seconds, linear rise */
    float decay;    /* seconds for a 60 dB fall, down to the sustain level */
    float sustain;  /* 0..1 amplitude. 0: the operator ends after its decay */
    float release;  /* seconds for a 60 dB fall after note off */
    float vel_sens; /* 0..1: how much the velocity scales the level */
} fm_op_params;

typedef struct {
    fm_op_params op[FM_OPS];
    int algorithm;  /* 0..7, see fm_algorithm_name() */
    float feedback; /* 0..1: operator 4 modulates itself */
    float gain;     /* 0..2 output gain */
} fm_patch;

/* Voice state. Fields are private to fm_engine.c. */
typedef struct {
    unsigned phase; /* 32-bit fixed point: 2^32 = one cycle (exact, no drift) */
    float g0, g1;   /* gain at the start and at the end of the current cell */
    float env_amp, env_db;
    int env_stage;
} fm_op_state;

typedef struct {
    fm_op_state op[FM_OPS];
    float fb[2]; /* last two outputs of operator 4, normalized */
    float velocity;
    float sample_rate, inv_sr;
    int gate;
    int cell_pos;      /* frames done in the current cell */
    int carrier_mask;  /* audible carriers (level > 0) at the last render */
} fm_voice;

/* A plain sine: operator 1 only. */
void fm_patch_init(fm_patch *p);
/* Clamp every value into its range. NaN becomes the default value. */
void fm_patch_sanitize(fm_patch *p);
const char *fm_algorithm_name(int algorithm);
/* Bit i set: operator i+1 is a carrier. */
int fm_algorithm_carriers(int algorithm);

void fm_voice_init(fm_voice *v, float sample_rate);
/* Velocity 0..1. On a sounding voice, the envelopes restart from their level. */
void fm_voice_note_on(fm_voice *v, float velocity);
void fm_voice_note_off(fm_voice *v);
/* Silence now (voice stealing). */
void fm_voice_reset(fm_voice *v);
/* 0 when every carrier has finished: the voice can be freed. */
int fm_voice_active(const fm_voice *v);
/* Write n frames of mono output. note_hz: pitch of the note now (it can
 * change at every call: tune, Keys mode, pitch modulation). */
void fm_voice_render(fm_voice *v, const fm_patch *p, float note_hz, float *out, int n);

/* sin(2 pi x) as used by the engine (exposed for tests). */
float fm_sin2pi(float x);

#endif
