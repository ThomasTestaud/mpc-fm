/* Built-in patches: starting points for sound design (tune them by ear on
 * the device), and "Bench", the most expensive patch for the benchmark. */
#include "fm_presets.h"

#include <string.h>

typedef struct {
    float ratio, fixed_hz, level, attack, decay, sustain, release, vel_sens;
} op_row;

static void set_op(fm_op_params *o, op_row r)
{
    o->ratio = r.ratio;
    o->fixed_hz = r.fixed_hz;
    o->detune = 0.0f;
    o->level = r.level;
    o->attack = r.attack;
    o->decay = r.decay;
    o->sustain = r.sustain;
    o->release = r.release;
    o->vel_sens = r.vel_sens;
}

/* op1..op4 rows: ratio, fixed Hz, level, attack, decay, sustain, release, vel */
static void make(fm_patch *p, int algorithm, float feedback, float gain, op_row o1, op_row o2,
                 op_row o3, op_row o4)
{
    fm_patch_init(p);
    p->algorithm = algorithm;
    p->feedback = feedback;
    p->gain = gain;
    set_op(&p->op[0], o1);
    set_op(&p->op[1], o2);
    set_op(&p->op[2], o3);
    set_op(&p->op[3], o4);
    fm_patch_sanitize(p);
}

static const char *const NAMES[] = {"Sine", "E.Piano", "Bass", "Bell", "Brass", "Metal Hit", "Bench"};

int fm_preset_count(void) { return (int)(sizeof NAMES / sizeof *NAMES); }

const char *fm_preset_name(int i) { return i >= 0 && i < fm_preset_count() ? NAMES[i] : NULL; }

int fm_preset_find(const char *name)
{
    for (int i = 0; i < fm_preset_count(); i++)
        if (strcmp(NAMES[i], name) == 0)
            return i;
    return -1;
}

int fm_preset_get(int i, fm_patch *p)
{
    switch (i) {
    case 0: /* Sine */
        make(p, 7, 0.0f, 1.0f, (op_row){1, 0, 1.0f, 0.002f, 1, 1, 0.2f, 0.5f},
             (op_row){1, 0, 0, 0.002f, 1, 1, 0.2f, 0}, (op_row){1, 0, 0, 0.002f, 1, 1, 0.2f, 0},
             (op_row){1, 0, 0, 0.002f, 1, 1, 0.2f, 0});
        p->gain = 2.0f; /* algorithm 8 scales by 1/2 */
        return 1;
    case 1: /* E.Piano: a soft 1:1 stack and a 14:1 "tine" stack */
        make(p, 4, 0.0f, 1.0f, (op_row){1, 0, 0.9f, 0.002f, 2.5f, 0.15f, 0.4f, 0.4f},
             (op_row){1, 0, 0.22f, 0.002f, 1.5f, 0.1f, 0.4f, 0.8f},
             (op_row){1, 0, 0.3f, 0.002f, 0.6f, 0.0f, 0.3f, 0.6f},
             (op_row){14, 0, 0.08f, 0.001f, 0.12f, 0.0f, 0.1f, 0.9f});
        return 1;
    case 2: /* Bass: serial stack, feedback, fast-closing index */
        make(p, 0, 0.5f, 1.0f, (op_row){1, 0, 1.0f, 0.001f, 1.5f, 0.6f, 0.08f, 0.3f},
             (op_row){1, 0, 0.35f, 0.001f, 0.35f, 0.15f, 0.08f, 0.7f},
             (op_row){2, 0, 0.12f, 0.001f, 0.2f, 0.05f, 0.08f, 0.5f},
             (op_row){1, 0, 0.1f, 0.001f, 0.3f, 0.2f, 0.08f, 0.0f});
        return 1;
    case 3: /* Bell: inharmonic ratios, long decays */
        make(p, 4, 0.0f, 1.0f, (op_row){1, 0, 0.9f, 0.001f, 6.0f, 0.0f, 2.0f, 0.3f},
             (op_row){3.5f, 0, 0.3f, 0.001f, 4.0f, 0.0f, 2.0f, 0.6f},
             (op_row){2, 0, 0.5f, 0.001f, 3.0f, 0.0f, 1.5f, 0.3f},
             (op_row){7.13f, 0, 0.15f, 0.001f, 1.2f, 0.0f, 1.0f, 0.6f});
        return 1;
    case 4: /* Brass: modulators open slower than the carrier */
        make(p, 1, 0.3f, 1.0f, (op_row){1, 0, 1.0f, 0.04f, 2.0f, 0.8f, 0.15f, 0.3f},
             (op_row){1, 0, 0.3f, 0.09f, 1.0f, 0.6f, 0.15f, 0.6f},
             (op_row){1, 0, 0.08f, 0.12f, 1.0f, 0.5f, 0.15f, 0.4f},
             (op_row){1, 0, 0.06f, 0.15f, 1.0f, 0.5f, 0.15f, 0.4f});
        return 1;
    case 5: /* Metal Hit: fixed-frequency, inharmonic, percussive */
        make(p, 2, 0.6f, 1.0f, (op_row){1, 0, 0.9f, 0.0005f, 0.6f, 0.0f, 0.3f, 0.5f},
             (op_row){2.76f, 0, 0.4f, 0.0005f, 0.25f, 0.0f, 0.2f, 0.7f},
             (op_row){1, 1234.0f, 0.3f, 0.0005f, 0.15f, 0.0f, 0.1f, 0.7f},
             (op_row){1, 3517.0f, 0.4f, 0.0005f, 0.1f, 0.0f, 0.1f, 0.5f});
        return 1;
    case 6: /* Bench: every operator always on, feedback on */
        make(p, 0, 1.0f, 1.0f, (op_row){1, 0, 1.0f, 0.001f, 30, 1, 1, 0},
             (op_row){1.5f, 0, 1.0f, 0.001f, 30, 1, 1, 0}, (op_row){2, 0, 1.0f, 0.001f, 30, 1, 1, 0},
             (op_row){3, 0, 1.0f, 0.001f, 30, 1, 1, 0});
        return 1;
    default:
        return 0;
    }
}
