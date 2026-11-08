/* The MPC Sample's control surface, seen before the app sees it.
 *
 * The app reads the surface (an ALSA rawmidi port) with snd_rawmidi_tread
 * / snd_rawmidi_read; fm.so defines both (SampQOL does not), so every byte
 * passes here first. While the FM page is closed, nothing is changed. When
 * SHIFT + NOTE REPEAT opens it, K1-K3, B1-B3, the encoder (turn and push)
 * and -/+ go to the page instead of the app; pads, SHIFT, the fader, MAIN
 * VOLUME and the transport still go to the app (edits are heard live). Any
 * other button closes the page and does its stock job. SysEx is never
 * touched (the surface's authentication answer, pad colors).
 *
 * Messages (channel 1): K1-K3 CC 16-18, encoder CC 100 (1 = +1, 127 = -1),
 * buttons are notes (press 90 nn 7F, release 80 nn 00): B1-B3 12-14,
 * encoder push 111, SHIFT 49, NOTE REPEAT 11, MINUS 55, PLUS 54,
 * PLAY 82, STOP 81, SEQ RECORD 80. Pads: channel 10 notes. */
#ifndef MPCFM_SURFACE_H
#define MPCFM_SURFACE_H

#include <stddef.h>
#include <stdint.h>

enum {
    SEV_OPEN = 1, /* the page opens; b: knob positions, 8 bits each (+1; 0 = not known) */
    SEV_CLOSE,    /* a: 1 = another button closed it */
    SEV_BUTTON,   /* a: 0..2 = B1..B3 */
    SEV_KNOB,     /* a: 0..2 = K1..K3, b: 0..127 */
    SEV_ENCODER,  /* b: +1 / -1 (turn, or -/+) */
    SEV_PUSH,     /* encoder push */
    SEV_SHIFT,    /* a: 1 down, 0 up */
    SEV_PAD,      /* a: note, b: velocity (pad hit, page open or not) */
};

typedef struct {
    uint8_t type, a;
    int32_t b;
    uint64_t t_ns;
} surf_event;

/* ---- the filter (no I/O: tested on the computer) ---- */

/* Once, in the app, before its threads start. */
void surf_init(void);

/* Byte-stream state of one rawmidi input. */
typedef struct {
    int in_sysex;
    int sx_n;          /* bytes of the current SysEx seen (for the header) */
    uint8_t sx_hdr[4];
    int skip;          /* data bytes of a message split across reads: pass */
    int akai_sysex;    /* an AC50 SysEx (F0 47 7F 5C) was seen: the surface */
} surf_stream;

/* Filter n bytes in place; returns the bytes kept. Messages split across
 * two reads always pass (it does not happen with the surface: it writes
 * each message at once). */
size_t surf_filter(surf_stream *st, uint8_t *buf, size_t n, uint64_t t_ns);

/* The page may open (the UI has a screen). Set by the UI thread. */
void surf_set_ready(int ready);
/* Ask the page to close (UI side, for example no FM target). */
void surf_request_close(void);
int surf_page_open(void);
/* Last known knob positions (-1: not known). */
int surf_knob_pos(int k);

/* Events for the UI thread, in order. 1 if one was taken. */
int surf_next_event(surf_event *ev);
/* A file descriptor that becomes readable when events wait (eventfd), or
 * -1. Read it (8 bytes) to clear. */
int surf_event_fd(void);

/* ---- the rawmidi side (surface.c) ---- */

/* Inputs the app opened, for the logs: "name (card): surface yes/no". 1 if
 * there is a line i. */
int surf_port_info(int i, char *out, size_t cap);
/* Counters: bytes read on the surface, messages kept by the page. */
void surf_counts(uint64_t *bytes, uint64_t *taken, uint64_t *events_lost);

#ifdef MPCFM_TEST
void surf_test_reset(void);
#endif

#endif
