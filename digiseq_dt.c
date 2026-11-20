/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DIGISEQ for Digitakt (digiseq-dt): sequencer types a track can play in
 * place of its own trigs, for the Digitakt mk1 (OS 1.53 and 1.54). Every
 * track picks its own TYPE: the 8 audio tracks (a note is the sample's pitch,
 * as a note lock) and the 8 MIDI tracks.
 *
 *   SNAKE  a Cartesian sequencer after MDD Snake (and Make Noise's Rene):
 *          DIGISNAKE for Digitakt 0.2, unchanged.
 *   185    a stage sequencer after ML-185 (Nico Starke's free Max for Live
 *          device, itself after RYK's M-185): eight stages, each with a pitch,
 *          a velocity, a length in clock pulses (1-8) and a mode: OFF, ONE,
 *          ALL (every DIV pulses) or HOLD (one note for the whole stage).
 *   ARP    an arpeggiator (0.3), played by the track's own note trigs: a
 *          trig's note (a MIDI track's chord) is held for the trig's LEN
 *          (HOLD: until the next trig) and arpeggiated over RNG octaves by
 *          MODE, at SPD, through a 16-step offset sequence, with RPT repeats
 *          of each note and a WALK that moves it all each cycle. In slice
 *          mode a note is a slice, so it arpeggiates slices.
 *          0.4: it also plays the keys held in keyboard mode (FUNC + TRK)
 *          on an audio track, HOLD latching them (kbsites.s).
 *
 * The page opens with TRIG pressed on the TRIG page (press TRIG twice). The
 * Digitakt's core cannot add a page to a view, so DIGISEQ draws its page over
 * the screen and takes the keys and knobs while it is open; TRIG, another
 * page key, or any screen opening over the pages (a menu) closes it. The keys
 * are the Digitone DIGISEQ's (its README); short messages show on the page's
 * bottom line instead of the Digitone's drop-down. Nothing is saved with the
 * project yet (the Digitakt's core has no project data): PRINT to keep it.
 *
 * The sequencer side uses seq-events 0.2 for the Digitakt: ev_gen_trig,
 * ev_gen_notes, ev_fire_vel and ev_fire_len. The pattern pointer and the
 * transport come from the same per-OS addresses seq-events uses (mod.json's
 * cflags); the UI reads the key dispatcher's screens to know when the TRIG
 * page is up.
 */
#include "digitakt-mk1/core3.h"
#include "digitakt-mk1/seq.h"

#ifndef SQ_LEN_MAX
#error "DIGISEQ needs seq-events 0.2 (ev_fire_len)"
#endif
#ifndef DSQ_PATTERN_PP
#error "build with -DDSQ_PATTERN_PP and -DDSQ_TRANSPORT (mod.json cflags)"
#endif
#define TRANSPORT   (*(volatile uint32_t *)DSQ_TRANSPORT)        /* 0 stopped, 1 playing */
#define PATTERN     (*(uint8_t *const volatile *)DSQ_PATTERN_PP) /* the active pattern */
#define TRACK_STRIDE 911
#define TRACK_OF(pat, t) ((pat) + TRACK_STRIDE * (t))

#define TRACKS    SQ_TRACKS     /* 0-7 the audio tracks, 8-15 the MIDI tracks */
#define CELLS     8
#define NOTE_MAX  24

/* Short messages: the stock drop-down (0.6), as the Digitone's DIGISEQ shows
 * them. A small printf of %s, %d and %c into msgbuf, then the firmware's
 * drop-down routine (DSQ_POPUP: f(obj, fmt, ...), the one the pages call with
 * "%s=%s"), whose object is the shown page view's +108 (measured in digiemu on
 * OS 1.53: the TRIG page's and the master view's alike). With no view to ask,
 * the message shows on the page's bottom line instead. UI task only. */
static char msgbuf[40];
static unsigned long frames;               /* UI frames: the random numbers' seed */
static unsigned char msg_ticks, redraw, msg_stock;    /* msg_stock: the drop-down has it */
static void *popup_obj(void);
static void sayf(const char *fmt, ...)
{
    __builtin_va_list ap;
    char *o = msgbuf, *end = msgbuf + sizeof msgbuf - 1;
    __builtin_va_start(ap, fmt);
    for (; *fmt && o < end; fmt++) {
        if (*fmt != '%') {
            *o++ = *fmt;
            continue;
        }
        fmt++;
        if (*fmt == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s && o < end)
                *o++ = *s++;
        } else if (*fmt == 'c') {
            *o++ = (char)__builtin_va_arg(ap, int);
        } else if (*fmt == 'd') {
            int v = __builtin_va_arg(ap, int);
            char tmp[12];
            int n = 0;
            unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
            if (v < 0)
                *o++ = '-';
            do {
                tmp[n++] = (char)('0' + u % 10);
                u /= 10;
            } while (u && n < 11);
            while (n && o < end)
                *o++ = tmp[--n];
        } else if (*fmt) {
            *o++ = *fmt;
        } else {
            break;
        }
    }
    *o = 0;
    __builtin_va_end(ap);
#ifdef DSQ_POPUP
    {
        void *obj = popup_obj();
        if (obj) {
            ((void (*)(void *, const char *, ...))DSQ_POPUP)(obj, "%s", msgbuf);
            msg_stock = 1;                  /* the page leaves the title bar to it */
            msg_ticks = 60;
            redraw = 1;
            return;
        }
    }
#endif
    msg_stock = 0;
    msg_ticks = 60;
    redraw = 1;
}
#define POPUP(...) sayf(__VA_ARGS__)

enum { E_SNAKE = 1, E_185 = 2, E_ARP = 3, E_EUC = 4, E_TUR = 5, E_GRD = 6 };
#define NTYPES 7
static const char *const type_names[NTYPES] = { "OFF", "SNAKE", "185", "ARP", "EUCLID", "TURING", "GRIDS" };

/* ==== the project's data ================================================ */

/* SNAKE (DIGISNAKE's) */
enum { P_ON, P_SHAPE, P_DIR, P_LEN, P_DIV, P_SCALE, P_RANGE, P_SHIFT, NSET };
struct snake {
    signed char note[CELLS];    /* semitones from NOTE */
    unsigned char vel[CELLS];   /* 1-127 */
    unsigned char set[NSET];    /* the PATH page; set[P_ON] mirrors TYPE SNAKE */
    unsigned char gate;         /* bit c: cell c fires */
    unsigned char pad[7];       /* pad[0]: 2X's speed to put back + 1 */
};                              /* 32 bytes */
struct snake digiseq_snake[TRACKS];

/* 185 and the track's TYPE */
enum { M_OFF, M_ONE, M_ALL, M_HOLD, MODES };
static const char *const mode_names[MODES] = { "OFF", "ONE", "ALL", "HOLD" };
enum { S_TYPE, S_NLEN, S_DIR, S_STAGES, S_CLOCK, S_SCALE, S_RANGE, S_SHIFT, NSETS };   /* 0.12: SNAKE's knobs */
struct stages {
    signed char pitch[CELLS];   /* semitones from NOTE */
    unsigned char vel[CELLS];   /* 1-127 */
    unsigned char pulses[CELLS];/* 1-8 clock pulses */
    unsigned char mode[CELLS];  /* M_ */
    unsigned char div[CELLS];   /* ALL: a note every div pulses, 1-4 */
    unsigned char set[NSETS];   /* SETUP; set[S_TYPE] unused (struct track's) */
};                              /* 48 bytes */
struct track {
    unsigned char on;           /* TYPE is not OFF */
    unsigned char eng;          /* E_SNAKE or E_185: the type (kept while OFF) */
    unsigned char spd;          /* 185 2X: the speed to put back + 1 */
    unsigned char pad0;
    struct stages st;
    unsigned char pad[12];
};                              /* 64 bytes */
struct track digiseq_trk[TRACKS];

/* ARP: the ARP page's settings, on SNAKE's knobs (0.12) */
enum { A_TYPE, A_HOLD, A_MODE, A_RPT, A_SPD, A_SCALE, A_RNG, A_INT, NASETS };
struct arpset {
    unsigned char set[NASETS];  /* set[A_TYPE] unused (struct track's) */
};                              /* 8 bytes */
struct arpset digiseq_arp[TRACKS];

/* EUCLID (0.9): HITS spread as evenly as they go over STEPS (1-16), turned
 * by ROT; ACC of the hits accented (spread evenly among them, turned by
 * AROT), at AVL and ANOT. A step a clock pulse (CLOCK). */
#define EUC_MAX  16
enum { U_STEPS, U_HITS, U_ROT, U_ACC, U_NOTE, U_VEL, U_AVL, U_AROT, NUSETS };   /* the EUCLID page */
enum { V_TYPE, V_LEN, V_CHNC, V_GHST, V_CLOCK, V_ANOT, V_VSPR, V_SPIN, NVSETS };   /* SETUP, on SNAKE's knobs (0.12) */
struct euclid {
    unsigned char set[NUSETS];  /* U_NOTE: NOTE_MAX + semitones */
    unsigned char setup[NVSETS];/* setup[V_TYPE] unused (struct track's); V_ANOT: NOTE_MAX + semitones */
    unsigned char spd;          /* 2X: the speed to put back + 1 */
};                              /* 17 bytes: the Digitakt keeps no project data, so no room kept */
struct euclid digiseq_euc[TRACKS];

/* TURING (0.10), after the Music Thing Turing Machine: a 16-bit register
 * loops over its last LEN bits (2-16). Each clock pulse the bit coming round
 * flips by CHG (0: a locked loop; 100: it doubles the loop, inverted) and goes
 * back in at the start. Its bits 0-7 are the note (over RANGE semitones from
 * NOTE, to SCALE), bits 4-11 whether the step plays (DENS) and bits 8-15 how
 * far below VEL it plays (VSPR). It runs on through pattern starts. */
enum { R_LEN, R_CHG, R_RANGE, R_SCALE, R_DENS, R_NOTE, R_VEL, R_VSPR, NRSETS };   /* the TURING page */
struct turing {
    uint16_t reg;               /* the register: bit 0 the newest */
    unsigned char set[NRSETS];  /* R_NOTE: NOTE_MAX + semitones */
    unsigned char setup[NVSETS];/* setup[V_TYPE] unused; V_CLOCK, V_LEN as EUCLID's */
    unsigned char spd;          /* 2X: the speed to put back + 1 */
};                              /* 19 bytes */
struct turing digiseq_tur[TRACKS];
#define TUR_RANGE_MAX 48

/* GRIDS (0.11), after Mutable Instruments Grids: the track plays one PART
 * (BD, SD or HH) of Grids' map of drum patterns, 32 steps long, at the place
 * X, Y on the map; a step plays when its level there is above what FILL
 * lets through, accented (AVL) when it is high; CHAOS adds a random amount
 * to the levels, new each time round. Each track has its own X and Y. */
#include "grids_map.h"
#define GRD_STEPS 32
#define GRD_PARTS 3
enum { G_PART, G_X, G_Y, G_FILL, G_CHAOS, G_NOTE, G_VEL, G_AVL, NGSETS };     /* the GRIDS page */
struct grids {
    unsigned char set[NGSETS];  /* X, Y, FILL, CHAOS 0-127; G_NOTE: NOTE_MAX + semitones */
    unsigned char setup[NVSETS];/* setup[V_TYPE] unused; V_CLOCK, V_LEN as EUCLID's */
    unsigned char spd;          /* 2X: the speed to put back + 1 */
};                              /* 17 bytes */
struct grids digiseq_grd[TRACKS];
static const char *const part_names[GRD_PARTS] = { "BD", "SD", "HH" };
static const char *const part_long[GRD_PARTS] = { "Kick", "Snare", "Hat" };

/* ==== shared tables ===================================================== */

#define SHAPES 7
static const unsigned char shapes[SHAPES][CELLS] = {
    {0, 1, 2, 3, 4, 5, 6, 7},   /* ROWS   left to right, top then bottom */
    {0, 1, 2, 3, 7, 6, 5, 4},   /* SNAKE  along the top, back along the bottom */
    {0, 4, 1, 5, 2, 6, 3, 7},   /* COLS   down each column */
    {0, 4, 5, 1, 2, 6, 7, 3},   /* ZIGZAG down, across, up, across */
    {0, 5, 2, 7, 4, 1, 6, 3},   /* DIAG   diagonal hops */
    {0, 3, 7, 4, 1, 2, 6, 5},   /* CORNER the corners, then the middle */
    {0, 6, 1, 7, 2, 4, 3, 5},   /* HOP    two across and down */
};
static const char *const shape_names[SHAPES] = {
    "ROWS", "SNAKE", "COLS", "ZIGZAG", "DIAG", "CORNER", "HOP"
};

enum { D_FWD, D_BWD, D_FB, D_BF, D_RND, DIRS };
static const char *const dir_names[DIRS] = { "FWD", "BWD", "F/B", "B/F", "RND" };

#define SCALES 12
static const unsigned short scale_mask[SCALES] = {
    0xfff, 0xab5, 0x5ad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x9ad, 0x295, 0x4a9, 0x4e9, 0x555,
};
static const char *const scale_names[SCALES] = {
    "CHR", "MAJ", "MIN", "DOR", "PHR", "LYD", "MIX", "HMIN", "PENT", "mPNT", "BLUE", "WHOL"
};

/* SNAKE's PATH page */
static const unsigned char set_min[NSET] = { 0, 0, 0, 1, 0, 0, 1, 0 };
static const unsigned char set_max[NSET] = { 1, SHAPES - 1, DIRS - 1, CELLS, 8, SCALES - 1, NOTE_MAX, CELLS - 1 };
static const char *const set_labels[NSET] = { "TYPE", "SHAPE", "DIR", "STEPS", "CLOCK", "SCALE", "RANGE", "SHIFT" };

/* 185's SETUP page */
#define NLENS 5
static const unsigned char sset_min[NSETS] = { 0, 0, 0, 1, 0, 0, 1, 0 };
static const unsigned char sset_max[NSETS] = { 0, NLENS - 1, DIRS - 1, CELLS, 8, SCALES - 1, NOTE_MAX, CELLS - 1 };
static const char *const sset_labels[NSETS] = { "TYPE", "LEN", "DIR", "STAGES", "CLOCK", "SCALE", "RANGE", "SHIFT" };
static const char *const nlen_names[NLENS] = { "TRK", "1/32", "1/16", "1/8", "1/4" };

/* ARP's page. MODE: UP, DOWN, UP-DN (the ends once), UP+DN (the ends twice),
 * CONV (outside in), DIV (inside out), PINKY (each note, then the top),
 * THUMB (the bottom, then each note), ORDER (as played), RAND, SHUF (each
 * note once a cycle, in a new order). SPD: steps a note (no faster than the
 * sequencer's step: 1/16). */
enum { AM_UP, AM_DOWN, AM_UPDN, AM_UPDN2, AM_CONV, AM_DIV, AM_PINKY, AM_THUMB, AM_ORDER,
       AM_RAND, AM_SHUF, AMODES };
static const char *const amode_names[AMODES] = {
    "UP", "DOWN", "UP-DN", "UP+DN", "CONV", "DIV", "PINKY", "THUMB", "ORDER", "RAND", "SHUF"
};
#define SPDS 6
static const unsigned char spd_steps[SPDS] = { 1, 2, 3, 4, 8, 16 };
static const char *const spd_names[SPDS] = { "1X", "1/2", "1/3", "1/4", "1/8", "1/16" };   /* 0.12: as CLOCK, steps a note */
/* RNG: how many copies of the notes, each INT semitones (slices) above the
 * last: INT 12 (OCT) octaves, INT 1 the next slices of a chain */
#define RNG_MAX 8
#define INT_OCT 12
static const unsigned char aset_min[NASETS] = { 0, 0, 0, 1, 0, 0, 1, 1 };
static const unsigned char aset_max[NASETS] = { 0, 1, AMODES - 1, 4, SPDS - 1, SCALES - 1, RNG_MAX, INT_OCT };
static const char *const aset_labels[NASETS] = { "TYPE", "HOLD", "MODE", "REPEAT", "CLOCK", "SCALE", "RANGE", "INTRVL" };

/* EUCLID's pages */
static const unsigned char uset_min[NUSETS] = { 1, 0, 0, 0, 0, 1, 1, 0 };
static const unsigned char uset_max[NUSETS] = { EUC_MAX, EUC_MAX, EUC_MAX - 1, EUC_MAX, 2 * NOTE_MAX, 127, 127, EUC_MAX - 1 };
static const char *const uset_labels[NUSETS] = { "STEPS", "HITS", "ROTATE", "ACCENT", "NOTE", "VEL", "A.VEL", "A.ROT" };
static const char *const uset_long[NUSETS] = { "Steps", "Hits", "Rotate", "Accents", "Note", "Velocity",
                                               "Accent Velocity", "Accent Rotate" };
static const char *const vset_labels[NVSETS] = { "TYPE", "LEN", "CHANCE", "GHOST", "CLOCK", "A.NOTE", "SPREAD", "SPIN" };
static const unsigned char vset_max[NVSETS] = { 0, NLENS - 1, 100, 127, 8, 2 * NOTE_MAX, 126, EUC_MAX - 1 };
/* GRIDS' */
static const unsigned char gset_min[NGSETS] = { 0, 0, 0, 0, 0, 0, 1, 1 };
static const unsigned char gset_max[NGSETS] = { 2, 127, 127, 127, 127, 2 * NOTE_MAX, 127, 127 };
static const char *const gset_labels[NGSETS] = { "PART", "MAP X", "MAP Y", "FILL", "CHAOS", "NOTE", "VEL", "A.VEL" };
static const char *const gset_long[NGSETS] = { "Part", "Map X", "Map Y", "Fill", "Chaos", "Note", "Velocity",
                                               "Accent Velocity" };
/* TURING's */
static const unsigned char rset_min[NRSETS] = { 2, 0, 1, 0, 0, 0, 1, 0 };
static const unsigned char rset_max[NRSETS] = { 16, 100, TUR_RANGE_MAX, 0 /* SCALES - 1 */, 100, 2 * NOTE_MAX, 127, 126 };
static const char *const rset_labels[NRSETS] = { "STEPS", "CHANGE", "RANGE", "SCALE", "FILL", "NOTE", "VEL", "SPREAD" };
static const char *const rset_long[NRSETS] = { "Steps", "Change", "Range", "Scale", "Fill", "Note",
                                               "Velocity", "Velocity Spread" };

/* DIV (SNAKE) and CLOCK (185): sequencer steps a move (1 = 1X, 2 = 1/2, 4 =
 * 1/4, 8 = 1/8), or 0: 2X, the track itself at twice its speed (per-track
 * SCALE mode) and a move every step. */
#define DIVS 5
static const unsigned char div_list[DIVS] = { 8, 4, 2, 1, 0 };
static const char *div_name(int v)
{
    switch (v) {
    case 0: return "2X";
    case 1: return "1X";
    case 2: return "1/2";
    case 4: return "1/4";
    case 8: return "1/8";
    default: return "1/?";
    }
}
static int div_turn(int v, long d)
{
    int i, best = 3;
    for (i = 0; i < DIVS; i++)
        if (div_list[i] == v)
            best = i;
    if (best == 3 && v != 1) {
        for (i = 0; i < DIVS - 1; i++)
            if (div_list[i] >= v)
                best = i;
    }
    best += (int)d;
    if (best < 0) best = 0;
    if (best > DIVS - 1) best = DIVS - 1;
    return div_list[best];
}
static int steps_of(int div)                /* sequencer steps a move */
{
    return div < 1 ? 1 : div;
}

/* ---- LEN: the note lengths, as the TRIG page's LEN index ------
 * Measured on the Digitone's TRIG page (emulated OS 1.44; the Digitakt's
 * LEN takes the same values, checked in digiemu on 1.53), in steps: index 14 is 1/16
 * (one step), 30 1/8, 46 1/4, 62 1/2, 78 16 steps, 94 32, 110 64; below each
 * of those the index moves in 1/16, 1/8, 1/4, 1/2, 1 and 2 steps. h: half
 * steps (1-128), the index for that length (or the nearest below). */
static int len_for_halves(int h)
{
    if (h < 1) h = 1;
    if (h > 128) h = 128;
    if (h <= 4)  return 8 * h - 2;          /* to 2 steps: 1/16 of a step an index */
    if (h <= 8)  return 30 + (h - 4) * 4;   /* to 4: 1/8 */
    if (h <= 16) return 46 + (h - 8) * 2;   /* to 8: 1/4 */
    if (h <= 32) return 62 + (h - 16);      /* to 16: 1/2 */
    if (h <= 64) return 78 + (h - 32) / 2;  /* to 32: 1 */
    return 94 + (h - 64) / 4;               /* to 64: 2 */
}

/* ==== defaults and checks ================================================ */

static void snake_defaults(struct snake *s)
{
    static const signed char demo[CELLS] = { 0, 3, 7, 10, 12, 10, 7, 3 };
    int c;
    for (c = 0; c < CELLS; c++) {
        s->note[c] = demo[c];
        s->vel[c] = 100;
    }
    s->gate = 0xff;
    s->set[P_ON] = 0;
    s->set[P_SHAPE] = 1;
    s->set[P_DIR] = D_FWD;
    s->set[P_LEN] = CELLS;
    s->set[P_DIV] = 1;
    s->set[P_SCALE] = 0;
    s->set[P_RANGE] = 12;
    s->set[P_SHIFT] = 0;
    for (c = 0; c < 7; c++)
        s->pad[c] = 0;
}

static void snake_check(struct snake *s)
{
    int c;
    for (c = 0; c < CELLS; c++) {
        if (s->note[c] > NOTE_MAX) s->note[c] = NOTE_MAX;
        else if (s->note[c] < -NOTE_MAX) s->note[c] = -NOTE_MAX;
        if (s->vel[c] < 1) s->vel[c] = 1;
        else if (s->vel[c] > 127) s->vel[c] = 127;
    }
    for (c = 0; c < NSET; c++)
        if (s->set[c] < set_min[c] || s->set[c] > set_max[c])
            s->set[c] = c == P_DIV ? 1 : c == P_RANGE ? 12 : set_min[c];
    if (s->set[P_LEN] < 1)
        s->set[P_LEN] = CELLS;
}

/* A demo line: a rising figure with a held note and a ratchet */
static void stages_defaults(struct stages *g)
{
    static const signed char demo_p[CELLS] = { 0, 7, 12, 10, 3, 5, 7, 0 };
    static const unsigned char demo_l[CELLS] = { 1, 1, 2, 1, 4, 1, 2, 1 };
    static const unsigned char demo_m[CELLS] = { M_ONE, M_ONE, M_ALL, M_ONE, M_HOLD, M_OFF, M_ONE, M_ONE };
    int c;
    for (c = 0; c < CELLS; c++) {
        g->pitch[c] = demo_p[c];
        g->vel[c] = 100;
        g->pulses[c] = demo_l[c];
        g->mode[c] = demo_m[c];
        g->div[c] = 1;
    }
    g->set[S_TYPE] = 0;
    g->set[S_DIR] = D_FWD;
    g->set[S_STAGES] = CELLS;
    g->set[S_CLOCK] = 1;
    g->set[S_SCALE] = 0;
    g->set[S_RANGE] = 12;
    g->set[S_NLEN] = 0;
    g->set[S_SHIFT] = 0;
}

static void stages_check(struct stages *g)
{
    int c;
    for (c = 0; c < CELLS; c++) {
        if (g->pitch[c] > NOTE_MAX) g->pitch[c] = NOTE_MAX;
        else if (g->pitch[c] < -NOTE_MAX) g->pitch[c] = -NOTE_MAX;
        if (g->vel[c] < 1) g->vel[c] = 1;
        else if (g->vel[c] > 127) g->vel[c] = 127;
        if (g->pulses[c] < 1 || g->pulses[c] > 8) g->pulses[c] = 1;
        if (g->mode[c] >= MODES) g->mode[c] = M_ONE;
        if (g->div[c] < 1 || g->div[c] > 4) g->div[c] = 1;
    }
    for (c = 0; c < NSETS; c++)
        if (g->set[c] < sset_min[c] || g->set[c] > sset_max[c])
            g->set[c] = c == S_CLOCK ? 1 : c == S_RANGE ? 12 : c == S_STAGES ? CELLS : sset_min[c];
}

static void arp_defaults(struct arpset *a)
{
    a->set[A_TYPE] = 0;
    a->set[A_HOLD] = 0;
    a->set[A_MODE] = AM_UP;
    a->set[A_RPT] = 1;
    a->set[A_SPD] = 0;                      /* 1/16 */
    a->set[A_SCALE] = 0;                    /* CHR: no scale */
    a->set[A_RNG] = 2;
    a->set[A_INT] = INT_OCT;
}

static void euc_defaults(struct euclid *u)
{
    int c;
    u->set[U_STEPS] = 16;
    u->set[U_HITS] = 5;
    u->set[U_ROT] = 0;
    u->set[U_ACC] = 2;
    u->set[U_NOTE] = NOTE_MAX;              /* +0 */
    u->set[U_VEL] = 90;
    u->set[U_AVL] = 127;
    u->set[U_AROT] = 0;
    for (c = 0; c < NVSETS; c++)
        u->setup[c] = 0;
    u->setup[V_CLOCK] = 1;
    u->setup[V_ANOT] = NOTE_MAX;            /* +0 */
    u->setup[V_CHNC] = 100;
    u->spd = 0;
}

/* the knobs' ranges that depend on others: HITS and ROT on STEPS, ACC and AROT on HITS */
static int euc_max(const struct euclid *u, int c)
{
    int n = u->set[U_STEPS], k = u->set[U_HITS];
    switch (c) {
    case U_HITS: return n;
    case U_ROT:  return n > 0 ? n - 1 : 0;
    case U_ACC:  return k;
    case U_AROT: return k > 0 ? k - 1 : 0;
    default:     return uset_max[c];
    }
}

static void euc_check(struct euclid *u)
{
    int c;
    for (c = 0; c < NUSETS; c++) {
        int hi = euc_max(u, c);
        if (u->set[c] < uset_min[c])
            u->set[c] = uset_min[c];
        if (u->set[c] > hi)
            u->set[c] = (unsigned char)hi;
    }
    for (c = V_LEN; c < NVSETS; c++)
        if (c != V_CLOCK && u->setup[c] > vset_max[c])
            u->setup[c] = c == V_ANOT ? NOTE_MAX : 0;
}

static void tur_defaults(struct turing *r)
{
    int c;
    r->reg = 0x9a35;
    r->set[R_LEN] = 8;
    r->set[R_CHG] = 10;
    r->set[R_RANGE] = 12;
    r->set[R_SCALE] = 2;                    /* MIN */
    r->set[R_DENS] = 75;
    r->set[R_NOTE] = NOTE_MAX;              /* +0 */
    r->set[R_VEL] = 110;
    r->set[R_VSPR] = 40;
    for (c = 0; c < NVSETS; c++)
        r->setup[c] = 0;
    r->setup[V_CLOCK] = 1;
    r->spd = 0;
}

static int rset_hi(int c)
{
    return c == R_SCALE ? SCALES - 1 : rset_max[c];
}

static void tur_check(struct turing *r)
{
    int c;
    for (c = 0; c < NRSETS; c++) {
        if (r->set[c] < rset_min[c])
            r->set[c] = rset_min[c];
        if (r->set[c] > rset_hi(c))
            r->set[c] = (unsigned char)rset_hi(c);
    }
    if (r->setup[V_LEN] >= NLENS)
        r->setup[V_LEN] = 0;
}

static void grd_defaults(struct grids *g)
{
    int c;
    g->set[G_PART] = 0;
    g->set[G_X] = 64;
    g->set[G_Y] = 64;
    g->set[G_FILL] = 64;
    g->set[G_CHAOS] = 0;
    g->set[G_NOTE] = NOTE_MAX;              /* +0 */
    g->set[G_VEL] = 95;
    g->set[G_AVL] = 127;
    for (c = 0; c < NVSETS; c++)
        g->setup[c] = 0;
    g->setup[V_CLOCK] = 1;
    g->spd = 0;
}

static void grd_check(struct grids *g)
{
    int c;
    for (c = 0; c < NGSETS; c++) {
        if (g->set[c] < gset_min[c])
            g->set[c] = gset_min[c];
        if (g->set[c] > gset_max[c])
            g->set[c] = gset_max[c];
    }
    if (g->setup[V_LEN] >= NLENS)
        g->setup[V_LEN] = 0;
}

static void track_defaults(struct track *k)
{
    int c;
    k->on = 0;
    k->eng = E_SNAKE;
    k->spd = 0;
    k->pad0 = 0;
    stages_defaults(&k->st);
    for (c = 0; c < 12; c++)
        k->pad[c] = 0;
}

/* ==== state shared by the UI and the fires ================================ */

static unsigned char data_ready;
static volatile unsigned char reset_req[TRACKS];
/* Starting from STOP (DIGISNAKE 1.2): the sequencer prepares step 1 while
 * stopped (a fire with the transport at 0): that fire starts the sequence
 * (prep), and the first fire after PLAY is then step 2. Without a prepare
 * (rewind), the first fire after PLAY starts it. */
static volatile unsigned char prep[TRACKS], rewind[TRACKS];

/* DIRECT JUMP for the Digitakt (1.0 or later), when the build has it: its
 * count of pattern starts (START ON BAR). When it moves, every sequence starts
 * again with the pattern. A weak import: without DIRECT JUMP it reads 0. */
extern volatile uint32_t dj_starts;
static uint32_t dj_seen[TRACKS];

static void changed(void)
{
}

/* A track's whole state, for TRIG + NO, FUNC + NO and PRINT's undo */
struct whole {
    struct snake s;
    struct track k;
    struct arpset a;
    struct euclid u;
    struct turing r;
    struct grids g;
};
/* One of each (0.11): the page edits one track at a time, so TRIG + NO's copy
 * and FUNC + NO's are the last track's that took one (hold_t, run_t; -1 none) */
static struct whole hold_copy, run_copy;
static signed char hold_t = -1, run_t = -1;
static unsigned char run_active[TRACKS];

/* a byte copy: struct assignments would call memcpy, which a mod has not */
static void copy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
}

static void get_whole(int t, struct whole *w)
{
    copy(&w->s, &digiseq_snake[t], sizeof w->s);
    copy(&w->k, &digiseq_trk[t], sizeof w->k);
    copy(&w->a, &digiseq_arp[t], sizeof w->a);
    copy(&w->u, &digiseq_euc[t], sizeof w->u);
    copy(&w->r, &digiseq_tur[t], sizeof w->r);
    copy(&w->g, &digiseq_grd[t], sizeof w->g);
}
static void put_whole(int t, const struct whole *w)
{
    copy(&digiseq_snake[t], &w->s, sizeof w->s);
    copy(&digiseq_trk[t], &w->k, sizeof w->k);
    copy(&digiseq_arp[t], &w->a, sizeof w->a);
    copy(&digiseq_euc[t], &w->u, sizeof w->u);
    copy(&digiseq_tur[t], &w->r, sizeof w->r);
    copy(&digiseq_grd[t], &w->g, sizeof w->g);
    digiseq_snake[t].set[P_ON] = (unsigned char)(w->k.on && w->k.eng == E_SNAKE);
}

static void put_together(void)              /* the first UI frame: every track's defaults */
{
    int t;
    for (t = 0; t < TRACKS; t++) {
        snake_defaults(&digiseq_snake[t]);
        track_defaults(&digiseq_trk[t]);
        arp_defaults(&digiseq_arp[t]);
        euc_defaults(&digiseq_euc[t]);
        tur_defaults(&digiseq_tur[t]);
        grd_defaults(&digiseq_grd[t]);
        reset_req[t] = 1;
    }
    data_ready = 1;
}

/* ==== playback (interrupt level, in the sequencer's step fire) ============ */

static unsigned long irq_rand = 0x2545f491UL;
static unsigned long xorshift(unsigned long *s)
{
    unsigned long x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x2545f491UL;
    return *s;
}

static int eng_on(int32_t track, int eng)
{
    return data_ready && (uint32_t)track < TRACKS && digiseq_trk[track].on && digiseq_trk[track].eng == eng;
}

/* A walk over len places, by a direction: SNAKE's path, 185's stages */
struct walk {
    unsigned char k;            /* the place, 0..len-1 */
    signed char pp;             /* ping-pong's way */
};

static void walk_on(struct walk *w, int dir, int len, unsigned long *rnd)
{
    int k = w->k;
    switch (dir) {
    case D_BWD:
        k = k > 0 ? k - 1 : len - 1;
        break;
    case D_FB:
    case D_BF:
        if (len < 2) {
            k = 0;
            break;
        }
        if (w->pp >= 0) {
            if (k + 1 >= len) { w->pp = -1; k = len - 2; } else k++;
        } else {
            if (k <= 0) { w->pp = 1; k = 1; } else k--;
        }
        break;
    case D_RND:
        k = (int)(xorshift(rnd) % (unsigned long)len);
        break;
    default:
        k = k + 1 < len ? k + 1 : 0;
        break;
    }
    if (k >= len)
        k = 0;
    w->k = (unsigned char)k;
}

static void walk_start(struct walk *w, int dir, int len)
{
    if (dir == D_BWD || dir == D_BF) {
        w->k = (unsigned char)(len - 1);
        w->pp = -1;
    } else {
        w->k = 0;
        w->pp = 1;
    }
}

/* ---- SNAKE ---- */
struct play {
    struct walk w;
    unsigned char count;        /* sequencer steps since the last move */
    unsigned char cell;         /* the cell the step plays */
    unsigned char moved;        /* this step is a snake step (DIV: the others rest) */
};
static struct play play[TRACKS];
volatile unsigned char digiseq_cell[TRACKS];        /* for the screen */

static int snake_len(const struct snake *s)
{
    int len = s->set[P_LEN];
    return len < 1 || len > CELLS ? CELLS : len;
}

static int cell_of(const struct snake *s, int k, int len)
{
    int i = (k + s->set[P_SHIFT]) % len;
    return shapes[s->set[P_SHAPE] % SHAPES][i];
}

static void snake_start(struct play *p, const struct snake *s, int len)
{
    p->count = 0;
    p->moved = 1;
    walk_start(&p->w, s->set[P_DIR], len);
}

static void snake_advance(struct play *p, const struct snake *s, int len, unsigned long *rnd)
{
    if (++p->count >= s->set[P_DIV]) {
        p->count = 0;
        p->moved = 1;
        walk_on(&p->w, s->set[P_DIR], len, rnd);
    } else {
        p->moved = 0;
    }
}

/* ---- 185 ---- */
struct play185 {
    struct walk w;              /* the stage, as the k-th of STAGES */
    unsigned char pulse;        /* the pulse within the stage */
    unsigned char count;        /* sequencer steps since the last pulse */
    unsigned char moved;        /* this step is a clock pulse */
    unsigned char stage;        /* the stage the step plays, 0-7 */
    unsigned char fire;         /* the step fires */
    unsigned char hold;         /* a HOLD's first pulse: its length in steps, else 0 */
};
static struct play185 p185[TRACKS];
volatile unsigned char digiseq_stage[TRACKS], digiseq_pulse[TRACKS];

static int stages_n(const struct stages *g)
{
    int n = g->set[S_STAGES];
    return n < 1 || n > CELLS ? CELLS : n;
}

static int stage_of(const struct stages *g, int k)
{
    return (k + g->set[S_SHIFT]) % CELLS;
}

static void st_decide(struct play185 *p, const struct stages *g)
{
    int s = stage_of(g, p->w.k), m = g->mode[s], d = g->div[s];
    p->stage = (unsigned char)s;
    p->hold = 0;
    if (!p->moved || m == M_OFF)
        p->fire = 0;
    else if (m == M_ALL)
        p->fire = (unsigned char)(p->pulse % (d < 1 ? 1 : d) == 0);
    else
        p->fire = (unsigned char)(p->pulse == 0);
    if (p->fire && m == M_HOLD)
        p->hold = (unsigned char)(g->pulses[s] * steps_of(g->set[S_CLOCK]));
}

static void st_start(struct play185 *p, const struct stages *g)
{
    p->count = 0;
    p->pulse = 0;
    p->moved = 1;
    walk_start(&p->w, g->set[S_DIR], stages_n(g));
    st_decide(p, g);
}

static void st_advance(struct play185 *p, const struct stages *g, unsigned long *rnd)
{
    int n = stages_n(g);
    if (++p->count >= steps_of(g->set[S_CLOCK])) {
        p->count = 0;
        p->moved = 1;
        if (p->w.k >= n)
            p->w.k = 0;
        if (++p->pulse >= g->pulses[stage_of(g, p->w.k)]) {
            p->pulse = 0;
            walk_on(&p->w, g->set[S_DIR], n, rnd);
        }
    } else {
        p->moved = 0;
    }
    if (p->w.k >= n)
        p->w.k = 0;
    st_decide(p, g);
}

/* ---- EUCLID ---- */
struct playeuc {
    unsigned char pos;          /* the step, 0..STEPS-1 */
    unsigned char count;        /* sequencer steps since the last pulse */
    unsigned char moved;        /* this step is a clock pulse */
    unsigned char fire;         /* the step fires */
    unsigned char acc;          /* ... accented */
    unsigned char ghost;        /* ... a ghost note (GHST, on a rest) */
    unsigned char spin;         /* SPIN so far: the steps the pattern has turned */
    unsigned char vdrop;        /* VSPR: how far below VEL it plays */
};
static struct playeuc peuc[TRACKS];
volatile unsigned char digiseq_epos[TRACKS], digiseq_espin[TRACKS];   /* for the screen */

static int euc_steps(const struct euclid *u)
{
    int n = u->set[U_STEPS];
    return n < 1 || n > EUC_MAX ? EUC_MAX : n;
}

/* step i of the pattern, turned spin steps more than ROT: -1 a rest, else
 * the hit's number (0 the first) */
static int euc_hit(const struct euclid *u, int i, int spin)
{
    int n = euc_steps(u), k = u->set[U_HITS], j;
    if (k < 1)
        return -1;
    if (k > n)
        k = n;
    j = (i - u->set[U_ROT] - spin) % n;
    if (j < 0)
        j += n;
    return (j * k) % n < k ? (j * k) / n : -1;
}

static int euc_accent(const struct euclid *u, int hit)
{
    int k = u->set[U_HITS], a = u->set[U_ACC], r;
    if (hit < 0 || a < 1 || k < 1)
        return 0;
    if (a > k)
        a = k;
    r = (hit - u->set[U_AROT]) % k;
    if (r < 0)
        r += k;
    return (r * a) % k < a;
}

/* SETUP's extras (0.12): CHNC, the chance a hit plays (one that doesn't is a
 * rest); GHST, a ghost note at that velocity on each rest (0: OFF); VSPR, a
 * random amount below the velocity; SPIN, the steps the pattern turns each
 * time round. */
static void euc_decide(struct playeuc *p, const struct euclid *u, unsigned long *rnd)
{
    int h = p->moved ? euc_hit(u, p->pos, p->spin) : -1, sp = u->setup[V_VSPR];
    if (h >= 0 && u->setup[V_CHNC] < 100 && (int)(xorshift(rnd) % 100) >= u->setup[V_CHNC])
        h = -1;
    p->fire = (unsigned char)(h >= 0);
    p->acc = (unsigned char)euc_accent(u, h);
    p->ghost = (unsigned char)(p->moved && h < 0 && u->setup[V_GHST] > 0);
    if (p->ghost)
        p->fire = 1;
    p->vdrop = (unsigned char)(sp > 0 ? xorshift(rnd) % (unsigned long)(sp + 1) : 0);
}

static void euc_start(struct playeuc *p, const struct euclid *u, unsigned long *rnd)
{
    p->pos = 0;
    p->count = 0;
    p->moved = 1;
    p->spin = 0;
    euc_decide(p, u, rnd);
}

static void euc_advance(struct playeuc *p, const struct euclid *u, unsigned long *rnd)
{
    int n = euc_steps(u);
    if (++p->count >= steps_of(u->setup[V_CLOCK])) {
        p->count = 0;
        p->moved = 1;
        if (p->pos + 1 < n) {
            p->pos++;
        } else {                            /* round again: SPIN turns it */
            p->pos = 0;
            p->spin = (unsigned char)((p->spin + u->setup[V_SPIN]) % n);
        }
    } else {
        p->moved = 0;
    }
    if (p->pos >= n)
        p->pos = 0;
    if (p->spin >= n)
        p->spin = 0;
    euc_decide(p, u, rnd);
}

static int euc_note_of(const struct euclid *u, const struct playeuc *p, int base)
{
    int n = base + u->set[U_NOTE] - NOTE_MAX;
    if (p->acc)
        n += u->setup[V_ANOT] - NOTE_MAX;
    return n;
}

static int euc_vel_of(const struct euclid *u, const struct playeuc *p)
{
    int v = p->ghost ? u->setup[V_GHST] : p->acc ? u->set[U_AVL] : u->set[U_VEL];
    v -= p->vdrop;
    return v < 1 ? 1 : v;
}

static int quantise(int note, int root, int scale);
/* ---- TURING ---- */
struct playtur {
    unsigned char count;        /* sequencer steps since the last pulse */
    unsigned char moved;        /* this step is a clock pulse */
    unsigned char fire;         /* the step fires */
    unsigned char pad;
    uint16_t reg;               /* the register as the step plays it */
};
static struct playtur ptur[TRACKS];

static int tur_len(const struct turing *r)
{
    int n = r->set[R_LEN];
    return n < 2 || n > 16 ? 16 : n;
}

/* the register one pulse on: the bit leaving the loop, perhaps flipped, back in at bit 0 */
static uint16_t tur_shift(const struct turing *r, uint16_t reg, unsigned long *rnd)
{
    int out = (reg >> (tur_len(r) - 1)) & 1;
    if (r->set[R_CHG] && (int)(xorshift(rnd) % 100UL) < r->set[R_CHG])
        out ^= 1;
    return (uint16_t)((reg << 1) | out);
}

static int tur_fires(const struct turing *r, uint16_t reg)
{
    return ((((reg >> 4) & 0xff) * 100) >> 8) < r->set[R_DENS];
}

static void tur_decide(struct playtur *p, const struct turing *r)
{
    p->fire = (unsigned char)(p->moved && tur_fires(r, p->reg));
}

static void tur_start(struct playtur *p, struct turing *r)
{
    p->count = 0;
    p->moved = 1;
    p->reg = r->reg;                        /* it runs on: where it is */
    tur_decide(p, r);
}

static void tur_advance(struct playtur *p, struct turing *r, unsigned long *rnd, int keep)
{
    if (++p->count >= steps_of(r->setup[V_CLOCK])) {
        p->count = 0;
        p->moved = 1;
        p->reg = tur_shift(r, p->reg, rnd);
        if (keep)
            r->reg = p->reg;                /* the register is the track's: it runs on */
    } else {
        p->moved = 0;
    }
    tur_decide(p, r);
}

static int tur_note(const struct turing *r, uint16_t reg, int base, int root)
{
    int n = base + r->set[R_NOTE] - NOTE_MAX + ((reg & 0xff) * r->set[R_RANGE]) / 255;
    return quantise(n, root, r->set[R_SCALE]);
}

static int tur_velocity(const struct turing *r, uint16_t reg)
{
    int v = r->set[R_VEL] - (r->set[R_VSPR] * ((reg >> 8) & 0xff)) / 255;
    return v < 1 ? 1 : v;
}

/* ---- GRIDS ---- */
struct playgrd {
    unsigned char pos;          /* the step, 0-31 */
    unsigned char count;        /* sequencer steps since the last pulse */
    unsigned char moved;        /* this step is a clock pulse */
    unsigned char fire;         /* the step fires */
    unsigned char acc;          /* ... accented */
    unsigned char pert;         /* CHAOS: what this time round adds to the levels */
};
static struct playgrd pgrd[TRACKS];
volatile unsigned char digiseq_gpos[TRACKS], digiseq_gpert[TRACKS];     /* for the screen */

/* the map's level for a step of the track's part at its X, Y (Grids' ReadDrumMap) */
static int grd_level(const struct grids *g, int step)
{
    int x = g->set[G_X] << 1, y = g->set[G_Y] << 1;         /* 0-254 */
    int i = x >> 6, j = y >> 6, xx = (x << 2) & 0xff, yy = (y << 2) & 0xff;
    int off = (g->set[G_PART] % GRD_PARTS) * GRD_STEPS + (step & (GRD_STEPS - 1));
    int a = grids_map[5 * i + j][off], b = grids_map[5 * (i + 1) + j][off];
    int c = grids_map[5 * i + j + 1][off], d = grids_map[5 * (i + 1) + j + 1][off];
    int ab = a + (((b - a) * xx) >> 8), cd = c + (((d - c) * xx) >> 8);
    return ab + (((cd - ab) * yy) >> 8);
}

/* -> 0 a rest, 1 a hit, 2 an accented hit */
static int grd_hit(const struct grids *g, int step, int pert)
{
    int level = grd_level(g, step);
    level = level < 255 - pert ? level + pert : 255;
    if (level <= 255 - (g->set[G_FILL] << 1))
        return 0;
    return level > 192 ? 2 : 1;
}

static void grd_decide(struct playgrd *p, const struct grids *g, unsigned long *rnd)
{
    int h;
    if (p->moved && p->pos == 0)            /* a new time round: CHAOS */
        p->pert = (unsigned char)(((xorshift(rnd) >> 8 & 0xff) * (g->set[G_CHAOS] >> 1)) >> 8);
    h = p->moved ? grd_hit(g, p->pos, p->pert) : 0;
    p->fire = (unsigned char)(h > 0);
    p->acc = (unsigned char)(h > 1);
}

static void grd_start(struct playgrd *p, const struct grids *g, unsigned long *rnd)
{
    p->pos = 0;
    p->count = 0;
    p->moved = 1;
    grd_decide(p, g, rnd);
}

static void grd_advance(struct playgrd *p, const struct grids *g, unsigned long *rnd)
{
    if (++p->count >= steps_of(g->setup[V_CLOCK])) {
        p->count = 0;
        p->moved = 1;
        p->pos = (unsigned char)((p->pos + 1) & (GRD_STEPS - 1));
    } else {
        p->moved = 0;
    }
    grd_decide(p, g, rnd);
}

/* ---- the step clock: start, catch up, advance (every type but ARP) ---- */
static void eng_start(int t)
{
    if (digiseq_trk[t].eng == E_GRD)
        grd_start(&pgrd[t], &digiseq_grd[t], &irq_rand);
    else if (digiseq_trk[t].eng == E_TUR)
        tur_start(&ptur[t], &digiseq_tur[t]);
    else if (digiseq_trk[t].eng == E_EUC)
        euc_start(&peuc[t], &digiseq_euc[t], &irq_rand);
    else if (digiseq_trk[t].eng == E_185)
        st_start(&p185[t], &digiseq_trk[t].st);
    else
        snake_start(&play[t], &digiseq_snake[t], snake_len(&digiseq_snake[t]));
}

static void eng_advance(int t, unsigned long *rnd)
{
    if (digiseq_trk[t].eng == E_GRD)
        grd_advance(&pgrd[t], &digiseq_grd[t], rnd);
    else if (digiseq_trk[t].eng == E_TUR)
        tur_advance(&ptur[t], &digiseq_tur[t], rnd, 1);
    else if (digiseq_trk[t].eng == E_EUC)
        euc_advance(&peuc[t], &digiseq_euc[t], rnd);
    else if (digiseq_trk[t].eng == E_185)
        st_advance(&p185[t], &digiseq_trk[t].st, rnd);
    else
        snake_advance(&play[t], &digiseq_snake[t], snake_len(&digiseq_snake[t]), rnd);
}

/* ---- ARP ----
 * The track's own note trigs play it. A note trig is a key going down: its
 * note (a MIDI track's chord: up to 4 notes) is held for the trig's LEN
 * (the step's LEN lock, else the track's; INF, or HOLD on: until the next
 * note trig) and the arp plays a note every SPD steps from the trig's step.
 * The trig itself plays the first. Whether a trig starts the arp is the
 * trig's own business: its condition or probability decides (the arp starts
 * in ev_gen_notes, which runs only for a step that plays); the arp's other
 * notes play whatever is stored on their steps. The step's LEN only sets how
 * long the arp runs: every arp note, the trig's included, is the track's LEN long. */
#define A_STEP_LEN  0x0c0                   /* s8[64]: a step's LEN, -1 the track's */
#define A_TRK_LEN   0x381                   /* s8: the track's LEN */
#define A_INF       0xffffu
#define MAXHELD     8                       /* notes held at once (keys) */
#define MAXARP      (MAXHELD * RNG_MAX)     /* times RNG */

/* The keyboard (0.4): the keys held on an audio track in keyboard mode (FUNC +
 * TRK), as kbsites.s hands them over (UI task). A key's note is held while it
 * is (HOLD: until a key goes down with none held, which starts a new set).
 * While they hold notes they play the arp, in place of the track's trigs. */
struct live {
    signed char note[MAXHELD];  /* as played */
    unsigned char key[MAXHELD]; /* the trig key (0-15) each came from */
    volatile unsigned char n;   /* notes held */
    unsigned char vel;
    unsigned short down;        /* the keys down now, bit k: key k */
};
static struct live live[SQ_AUDIO];

struct parp {
    unsigned char active;       /* a note is held */
    unsigned char live;         /* the keyboard plays it (struct live) */
    unsigned char thru;         /* live recording, no key held: the step plays as stored */
    unsigned char srcp;         /* this step has a note trig (it may start the arp) */
    unsigned char capvel;       /* the trig's velocity is the arp's: take it */
    unsigned char fire;         /* this step fires */
    unsigned char nheld;        /* the notes held, 1-SQ_NOTES */
    unsigned char vel;
    unsigned char p;            /* where the arp is in MODE's cycle */
    unsigned char rep;          /* RPT: the plays of this note so far */
    unsigned short elapsed;     /* steps since the trig */
    unsigned short units;       /* how long it is held, in 1/16 steps (A_INF: on) */
    signed char held[MAXHELD];  /* the notes, low to high */
    signed char order[MAXHELD]; /* as played */
    unsigned char perm[MAXARP]; /* SHUF's order this cycle */
};
static struct parp parp[TRACKS];

static int aclamp(int n)
{
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

/* LEN, as the TRIG page's index, in 1/16 steps (see len_for_halves); 127 is INF */
static unsigned units_for_len(int l)
{
    if (l < 0 || l >= 127) return A_INF;
    if (l <= 30)  return (unsigned)(l + 2);
    if (l <= 46)  return 32u + (unsigned)(l - 30) * 2u;
    if (l <= 62)  return 64u + (unsigned)(l - 46) * 4u;
    if (l <= 78)  return 128u + (unsigned)(l - 62) * 8u;
    if (l <= 94)  return 256u + (unsigned)(l - 78) * 16u;
    if (l <= 110) return 512u + (unsigned)(l - 94) * 32u;
    return 1024u + (unsigned)(l - 110) * 64u;
}

static unsigned arp_units(int t, int step)
{
    uint8_t *pat = PATTERN, *trk;
    int l;
    if (!pat || step < 0 || step >= SQ_STEPS)
        return 16;
    trk = TRACK_OF(pat, t);
    l = (signed char)trk[A_STEP_LEN + step];
    if (l < 0)
        l = (signed char)trk[A_TRK_LEN];
    return units_for_len(l);
}

static void arp_reset(struct parp *a)
{
    a->active = a->live = a->thru = a->srcp = a->capvel = a->fire = 0;
}

/* ---- live recording (0.5) ----
 * The keyboard's controller (kbsites.s hands it over with each key) keeps the
 * sequencer's state at +152; that state's +353 is set while REC is lit and
 * +361 while it records live (REC + PLAY; measured in digiemu on OS 1.53, the
 * same offsets read by the same getters in 1.54). While an ARP track records
 * live, each note the keys' arp plays is written on its step as a note trig
 *  (note lock, velocity, the track's LEN, no micro timing or condition), over what was
 * there; the steps it does not play are left as they were. With no key held,
 * the track's trigs play as stored (no arp), so what was recorded sounds as
 * it will. When the recording stops, TYPE goes to OFF on the tracks it
 * recorded (digiseq_tick). */
#define KB_SEQ        152
#define SEQ_REC       353
#define SEQ_LIVE      361
#define A_W_TRIG      0x0001
#define A_W_SYN       0x0080
#define A_W_SYN_SET   0x0800
#define A_VEL         0x080
#define A_MICRO       0x100
#define A_COND        0x200
#define A_NOTE_LOCK   0x280
#define A_DEF_TRIGS   0x382
static uint8_t *volatile kb_obj;
static volatile uint16_t arp_recorded;      /* tracks the recording wrote to */

/* The sequencer's state: the keyboard's controller's +152, or (0.8, so SNAKE
 * and 185 record without a key pressed) a page view's +108, the same object
 * (measured in digiemu on OS 1.53: both 0x42a3b6b0). The UI keeps it in
 * seq_obj, taken only while a page view (TRIG .. LFO) is shown. */
static void *volatile seq_obj;

static int rec_live(void)
{
    uint32_t o = (uint32_t)kb_obj, s;
    if (TRANSPORT != 1)
        return 0;
    if (o >= 0x40000000u && o < 0x48000000u)
        s = *(volatile uint32_t *)(o + KB_SEQ);
    else
        s = (uint32_t)seq_obj;
    if (s < 0x40000000u || s >= 0x48000000u)
        return 0;
    return ((volatile uint8_t *)s)[SEQ_REC] && ((volatile uint8_t *)s)[SEQ_LIVE];
}

static void arp_record(int t, int step, int note, int vel, int len)
{
    uint8_t *pat = PATTERN, *trk;
    unsigned w;
    if (!pat || step < 0 || step >= SQ_STEPS)
        return;
    trk = TRACK_OF(pat, t);
    w = (unsigned)(trk[2 * step] << 8 | trk[2 * step + 1]);
    w |= A_W_TRIG;
    if (!((w & A_W_SYN_SET) ? (w & A_W_SYN) : (trk[A_DEF_TRIGS + 1] & A_W_SYN)))
        w |= A_W_SYN_SET | A_W_SYN;         /* a note trig, whatever the default */
    trk[A_NOTE_LOCK + step] = (uint8_t)note;
    trk[A_VEL + step] = (uint8_t)vel;
    trk[A_STEP_LEN + step] = (uint8_t)len;  /* 255: the track's */
    trk[A_MICRO + step] = 0;
    trk[A_COND + step] = 0xff;
    trk[2 * step] = (uint8_t)(w >> 8);
    trk[2 * step + 1] = (uint8_t)w;
    arp_recorded |= (uint16_t)(1u << t);
}

/* The notes held from a list, as played: held low to high */
static void arp_hold(struct parp *a, const signed char *notes, int n)
{
    int i, j;
    signed char x;
    if (n > MAXHELD)
        n = MAXHELD;
    for (i = 0; i < n; i++)
        a->order[i] = a->held[i] = notes[i];
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && a->held[j - 1] > a->held[j]; j--) {
            x = a->held[j];
            a->held[j] = a->held[j - 1];
            a->held[j - 1] = x;
        }
    a->nheld = (unsigned char)n;
}

static int arp_period(int mode, int m)
{
    switch (mode) {
    case AM_UPDN:  return m < 2 ? 1 : 2 * m - 2;
    case AM_UPDN2: return 2 * m;
    case AM_PINKY:
    case AM_THUMB: return m < 2 ? 1 : 2 * (m - 1);
    default:       return m;
    }
}

static int conv_index(int p, int m)
{
    int k = p / 2;
    return (p & 1) ? m - 1 - k : k;
}

static int arp_index(struct parp *a, int mode, int p, int m)
{
    switch (mode) {
    case AM_DOWN:  return m - 1 - p;
    case AM_UPDN:  return p < m ? p : 2 * m - 2 - p;
    case AM_UPDN2: return p < m ? p : 2 * m - 1 - p;
    case AM_CONV:  return conv_index(p, m);
    case AM_DIV:   return conv_index(m - 1 - p, m);
    case AM_PINKY: return m < 2 ? 0 : (p & 1) ? m - 1 : p / 2;
    case AM_THUMB: return m < 2 ? 0 : (p & 1) ? p / 2 + 1 : 0;
    case AM_RAND:  return (int)(xorshift(&irq_rand) % (unsigned long)m);
    case AM_SHUF:  return a->perm[p % MAXARP];
    default:       return p;                /* UP, ORDER */
    }
}

static void arp_shuffle(struct parp *a, int m)
{
    int i, j;
    unsigned char x;
    for (i = 0; i < m; i++)
        a->perm[i] = (unsigned char)i;
    for (i = m - 1; i > 0; i--) {
        j = (int)(xorshift(&irq_rand) % (unsigned long)(i + 1));
        x = a->perm[i];
        a->perm[i] = a->perm[j];
        a->perm[j] = x;
    }
}

/* A note trig that plays: the arp starts with its notes */
static void arp_start(int t, int step, const int8_t notes[SQ_NOTES])
{
    struct parp *a = &parp[t];
    signed char got[SQ_NOTES];
    int n = 0, i, v;
    if (notes[0] < 0)
        return;
    got[n++] = notes[0];
    if (t >= SQ_AUDIO)                      /* a MIDI track's chord */
        for (i = 1; i < SQ_NOTES; i++)
            if (notes[i] && (v = notes[0] + notes[i]) >= 0 && v <= 127)
                got[n++] = (signed char)v;
    arp_hold(a, got, n);
    a->active = 1;
    a->capvel = 1;
    a->elapsed = 0;
    a->units = (unsigned short)arp_units(t, step);
    a->p = a->rep = 0;
}

/* The arp's next note */
static int arp_next(int t)
{
    struct parp *a = &parp[t];
    const struct arpset *s = &digiseq_arp[t];
    int n = a->nheld, rng = s->set[A_RNG], mode = s->set[A_MODE], rpt = s->set[A_RPT];
    int m, period, idx, note, iv = s->set[A_INT];
    const signed char *list;
    if (n < 1 || n > MAXHELD)
        return -1;
    if (rng < 1 || rng > RNG_MAX) rng = 1;
    if (mode >= AMODES) mode = AM_UP;
    if (rpt < 1 || rpt > 4) rpt = 1;
    if (iv < 1 || iv > INT_OCT) iv = INT_OCT;
    m = n * rng;
    period = arp_period(mode, m);
    if (a->p >= period)
        a->p = 0;
    if (mode == AM_SHUF && a->p == 0 && a->rep == 0)
        arp_shuffle(a, m);
    idx = arp_index(a, mode, a->p, m);
    if (idx < 0 || idx >= m)
        idx = 0;
    list = mode == AM_ORDER ? a->order : a->held;
    note = list[idx % n] + iv * (idx / n);
    if (++a->rep >= rpt) {
        a->rep = 0;
        if (++a->p >= period)
            a->p = 0;
    }
    return aclamp(note);
}

static void arp_trig(int32_t track, int32_t first, uint32_t *trig)
{
    struct parp *a = &parp[track];
    if (!first && a->thru)
        return;
    if (first) {
        const struct arpset *s = &digiseq_arp[track];
        int spd = spd_steps[s->set[A_SPD] < SPDS ? s->set[A_SPD] : 0];
        if (TRANSPORT == 0) {               /* stopped: the first step, from nothing */
            arp_reset(a);
            prep[track] = 1;
            rewind[track] = 0;
        } else if (prep[track]) {
            prep[track] = rewind[track] = 0;
        } else if (rewind[track]) {
            rewind[track] = 0;
            arp_reset(a);
        }
        reset_req[track] = 0;
        a->thru = 0;
        if (track < SQ_AUDIO && live[track].n) {    /* the keyboard plays it */
            if (!a->live) {                 /* from its first note, on this step */
                a->live = a->active = 1;
                a->capvel = 0;
                a->elapsed = 0;
                a->p = a->rep = 0;
            } else if (a->elapsed < 0xfffe) {
                a->elapsed++;
            }
            a->srcp = 0;                    /* the track's trigs wait */
            a->fire = (unsigned char)(a->elapsed % spd == 0);
            goto out;
        }
        if (a->live)                        /* the keys let go */
            a->live = a->active = 0;
        a->thru = (unsigned char)rec_live();
        if (a->thru) {                      /* recording, no key: the trigs as stored */
            a->active = a->srcp = a->fire = 0;
            return;
        }
        if (a->active) {
            if (a->elapsed < 0xfffe)
                a->elapsed++;
            if (!s->set[A_HOLD] && a->units != A_INF && (unsigned long)a->elapsed * 16 >= a->units)
                a->active = 0;              /* the note is let go */
        }
        a->srcp = (unsigned char)((*trig & SQ_TRIG) != 0);
        a->fire = (unsigned char)(a->srcp || (a->active && a->elapsed % spd == 0));
    }
out:
    if (a->fire)
        *trig |= SQ_TRIG;
    else
        *trig &= ~SQ_TRIG;
}

/* Both reads of a step (first 1, then 0) must answer the same: the first
 * moves the sequence, the second reuses where it is. */
void digiseq_trig(int32_t track, int32_t step, int32_t first, uint32_t *trig)
{
    int fire;
    if (!data_ready || (uint32_t)track >= TRACKS || !digiseq_trk[track].on)
        return;
    if (digiseq_trk[track].eng == E_ARP) {
        arp_trig(track, first, trig);
        return;
    }
    if (first) {
        if (TRANSPORT == 0) {            /* stopped: prepare the first step */
            prep[track] = 1;
            rewind[track] = reset_req[track] = 0;
            dj_seen[track] = dj_starts;
            eng_start(track);
        } else if (prep[track] || rewind[track]) {
            int n;
            prep[track] = rewind[track] = reset_req[track] = 0;
            dj_seen[track] = dj_starts;
            eng_start(track);
            for (n = 0; n < step && n < SQ_STEPS; n++)
                eng_advance(track, &irq_rand);
        } else if (reset_req[track] || dj_starts != dj_seen[track]) {
            reset_req[track] = 0;
            dj_seen[track] = dj_starts;     /* a pattern start: from the beginning */
            eng_start(track);
        } else {
            eng_advance(track, &irq_rand);
        }
        if (digiseq_trk[track].eng == E_GRD) {
            digiseq_gpos[track] = pgrd[track].pos;
            digiseq_gpert[track] = pgrd[track].pert;
        } else if (digiseq_trk[track].eng == E_EUC) {
            digiseq_epos[track] = peuc[track].pos;
            digiseq_espin[track] = peuc[track].spin;
        } else if (digiseq_trk[track].eng == E_185) {
            digiseq_stage[track] = p185[track].stage;
            digiseq_pulse[track] = p185[track].pulse;
        } else {
            struct play *p = &play[track];
            const struct snake *s = &digiseq_snake[track];
            int len = snake_len(s);
            if (p->w.k >= len)
                p->w.k = 0;
            p->cell = (unsigned char)cell_of(s, p->w.k, len);
            digiseq_cell[track] = p->cell;
        }
    }
    if (digiseq_trk[track].eng == E_GRD) {
        fire = pgrd[track].fire;
    } else if (digiseq_trk[track].eng == E_TUR) {
        fire = ptur[track].fire;
    } else if (digiseq_trk[track].eng == E_EUC) {
        fire = peuc[track].fire;
    } else if (digiseq_trk[track].eng == E_185) {
        fire = p185[track].fire;
    } else {
        const struct play *p = &play[track];
        fire = p->moved && ((digiseq_snake[track].gate >> p->cell) & 1);
    }
    if (fire)
        *trig |= SQ_TRIG;
    else
        *trig &= ~SQ_TRIG;
}

static int quantise(int note, int root, int scale)
{
    unsigned short m;
    int i, deg;
    if (scale <= 0 || scale >= SCALES)
        return note;
    m = scale_mask[scale];
    for (i = 0; i < 12; i++) {              /* down to the nearest scale tone */
        deg = (note - root - i) % 12;
        if (deg < 0)
            deg += 12;
        if ((m >> deg) & 1)
            return note - i;
    }
    return note;
}

static int clamp_note(int n)
{
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

static int len185(int track);
static int lenEuc(int track);
static int lenTur(int track);
static int lenGrd(int track);
static int grd_vel(int track)
{
    const struct grids *g = &digiseq_grd[track];
    return pgrd[track].acc ? g->set[G_AVL] : g->set[G_VEL];
}
static int euc_note(int track, int base)
{
    return euc_note_of(&digiseq_euc[track], &peuc[track], base);
}
static int euc_vel(int track)
{
    return euc_vel_of(&digiseq_euc[track], &peuc[track]);
}
void digiseq_notes(int32_t track, int32_t step, int32_t generated, int8_t notes[SQ_NOTES])
{
    struct sq_track info;
    int base, root = 0, n, i;
    (void)generated;
    if (!data_ready || (uint32_t)track >= TRACKS || !digiseq_trk[track].on)
        return;
    if (digiseq_trk[track].eng == E_ARP) {
        struct parp *a = &parp[track];
        if (a->thru)                        /* recording, no key: as stored */
            return;
        if (a->live) {                      /* the keys held now, as they are */
            const struct live *L = &live[track];
            signed char got[MAXHELD];
            int k = L->n;
            if (k > MAXHELD)
                k = MAXHELD;
            for (i = 0; i < k; i++)
                got[i] = L->note[i];
            if (k)
                arp_hold(a, got, k);
        } else if (a->srcp) {               /* the note trig plays: the arp starts */
            a->srcp = 0;
            arp_start(track, step, notes);
        }
        if (!a->active)
            return;
        n = arp_next(track);
        if (n < 0)
            return;
        {                                   /* SCALE, rooted on the track's NOTE */
            int sc = digiseq_arp[track].set[A_SCALE];
            if (sc > 0 && sc < SCALES) {
                int r = sq_track_info(track, &info, sizeof info) == 0 ? info.note % 12 : 0;
                n = clamp_note(quantise(n, r, sc));
            }
        }
        notes[0] = (int8_t)n;
        if (a->live && rec_live())          /* recording live: onto its step (LEN: the track's) */
            arp_record(track, step, n, live[track].vel ? live[track].vel : 100, 255);
        for (i = 1; i < SQ_NOTES; i++)
            notes[i] = 0;
        return;
    }
    if (sq_track_info(track, &info, sizeof info) == 0)
        base = info.note;
    else
        base = notes[0] >= 0 ? notes[0] : 60;
    root = base % 12;                       /* the Digitakt keeps no key: the scale's root is NOTE */
    if (digiseq_trk[track].eng == E_GRD) {
        n = base + digiseq_grd[track].set[G_NOTE] - NOTE_MAX;
    } else if (digiseq_trk[track].eng == E_TUR) {
        n = tur_note(&digiseq_tur[track], ptur[track].reg, base, root);
    } else if (digiseq_trk[track].eng == E_EUC) {
        n = euc_note(track, base);
    } else if (digiseq_trk[track].eng == E_185) {
        const struct stages *g = &digiseq_trk[track].st;
        n = quantise(base + g->pitch[p185[track].stage], root, g->set[S_SCALE]);
    } else {
        const struct snake *s = &digiseq_snake[track];
        n = quantise(base + s->note[play[track].cell], root, s->set[P_SCALE]);
    }
    notes[0] = (int8_t)clamp_note(n);
    for (i = 1; i < SQ_NOTES; i++)          /* one note, not the step's chord */
        notes[i] = 0;
    if (rec_live()) {                       /* 0.8: recording live, onto its step */
        if (digiseq_trk[track].eng == E_GRD)
            arp_record(track, step, notes[0], grd_vel(track), lenGrd(track));
        else if (digiseq_trk[track].eng == E_TUR)
            arp_record(track, step, notes[0], tur_velocity(&digiseq_tur[track], ptur[track].reg), lenTur(track));
        else if (digiseq_trk[track].eng == E_EUC)
            arp_record(track, step, notes[0], euc_vel(track), lenEuc(track));
        else if (digiseq_trk[track].eng == E_185)
            arp_record(track, step, notes[0], digiseq_trk[track].st.vel[p185[track].stage], len185(track));
        else
            arp_record(track, step, notes[0], digiseq_snake[track].vel[play[track].cell], 255);
    }
}

void digiseq_vel(int32_t track, int32_t step, int32_t *vel)
{
    (void)step;
    if (eng_on(track, E_ARP)) {
        struct parp *a = &parp[track];
        if (a->thru)
            return;
        if (a->live) {                      /* the keyboard's velocity */
            if (track < SQ_AUDIO && live[track].vel)
                *vel = live[track].vel;
        } else if (a->capvel) {             /* the trig's: the arp's notes play it */
            a->capvel = 0;
            a->vel = (unsigned char)(*vel < 1 ? 1 : *vel > 127 ? 127 : *vel);
        } else if (a->active) {
            *vel = a->vel;
        }
    } else if (eng_on(track, E_GRD))
        *vel = grd_vel(track);
    else if (eng_on(track, E_TUR))
        *vel = tur_velocity(&digiseq_tur[track], ptur[track].reg);
    else if (eng_on(track, E_EUC))
        *vel = euc_vel(track);
    else if (eng_on(track, E_185))
        *vel = digiseq_trk[track].st.vel[p185[track].stage];
    else if (eng_on(track, E_SNAKE))
        *vel = digiseq_snake[track].vel[play[track].cell];
}

/* A step the type fires plays, whatever trig condition or probability a
 * stored trig on that step had: those belong to the track's own trigs. */
void digiseq_cond(int32_t track, int32_t step, int32_t *pass)
{
    (void)step;
    if (data_ready && (uint32_t)track < TRACKS && digiseq_trk[track].on) {
        if (digiseq_trk[track].eng == E_ARP && (parp[track].srcp || parp[track].thru))
            return;                         /* ARP: a note trig's own condition decides */
        *pass = 1;
    }
}

/* 185: a HOLD's note lasts its stage; LEN (SETUP) sets the others' (TRK:
 * the track's LEN, so the steps' own LEN locks from before do not show) */
#define TR_LEN_OFF_TRACK 0x381
static const unsigned char nlen_halves[NLENS] = { 0, 1, 2, 4, 8 };
/* 185's note length as LEN's index (255: the track's), as digiseq_len sets it */
static int len185(int track)
{
    const struct play185 *p = &p185[track];
    const struct stages *g = &digiseq_trk[track].st;
    if (p->hold)
        return len_for_halves(2 * p->hold);
    if (g->set[S_NLEN] > 0 && g->set[S_NLEN] < NLENS)
        return len_for_halves(nlen_halves[g->set[S_NLEN]]);
    return 255;
}

/* EUCLID's note length as LEN's index (255: the track's) */
static int lenEuc(int track)
{
    int v = digiseq_euc[track].setup[V_LEN];
    return v > 0 && v < NLENS ? len_for_halves(nlen_halves[v]) : 255;
}

static int lenGrd(int track)
{
    int v = digiseq_grd[track].setup[V_LEN];
    return v > 0 && v < NLENS ? len_for_halves(nlen_halves[v]) : 255;
}

static int lenTur(int track)
{
    int v = digiseq_tur[track].setup[V_LEN];
    return v > 0 && v < NLENS ? len_for_halves(nlen_halves[v]) : 255;
}

void digiseq_len(int32_t track, int32_t step, int32_t *len)
{
    const struct play185 *p;
    const struct stages *g;
    (void)step;
    if (eng_on(track, E_EUC) || eng_on(track, E_TUR) || eng_on(track, E_GRD)) {   /* LEN (SETUP), else the track's */
        int l = eng_on(track, E_GRD) ? lenGrd(track) : eng_on(track, E_TUR) ? lenTur(track) : lenEuc(track);
        if (l != 255)
            *len = l;
        else if (PATTERN)
            *len = (signed char)TRACK_OF(PATTERN, track)[TR_LEN_OFF_TRACK];
        return;
    }
    if (eng_on(track, E_ARP)) {             /* the track's LEN, the trig's note included */
        if (parp[track].thru)
            return;
        if (PATTERN)
            *len = (signed char)TRACK_OF(PATTERN, track)[A_TRK_LEN];
        return;
    }
    if (!eng_on(track, E_185))
        return;
    p = &p185[track];
    g = &digiseq_trk[track].st;
    if (p->hold)
        *len = len_for_halves(2 * p->hold);
    else if (g->set[S_NLEN] > 0 && g->set[S_NLEN] < NLENS)
        *len = len_for_halves(nlen_halves[g->set[S_NLEN]]);
    else if (PATTERN)                    /* TRK: the track's LEN, not a step's own */
        *len = (signed char)TRACK_OF(PATTERN, track)[TR_LEN_OFF_TRACK];
}

/* ==== the keyboard (kbsites.s) ==========================================
 * UI task. A key down on an audio track whose TYPE is ARP: its note joins the
 * notes held (with HOLD on and no key down, a new set starts). Playing, the arp
 * plays it and the stock note does not (-> 1); stopped, the stock note plays as
 * ever and the key is still held, so a HOLD set waits for PLAY. */
static int arp_kb_track(int t)
{
    return t >= 0 && t < SQ_AUDIO && digiseq_trk[t].on && digiseq_trk[t].eng == E_ARP;
}

static void live_drop(struct live *L, int i)
{
    int j, n = L->n;
    for (j = i; j + 1 < n; j++) {
        L->note[j] = L->note[j + 1];
        L->key[j] = L->key[j + 1];
    }
    L->n = (unsigned char)(n - 1);          /* last: the fire reads n notes */
}

int digiseq_kb_on(void *obj, int32_t key, int32_t track, int32_t note, int32_t vel)
{
    struct live *L;
    int i;
    kb_obj = (uint8_t *)obj;                /* the sequencer's state: live recording */
    if (!data_ready || key < 0 || key > 15 || note < 0 || note > 127 || !arp_kb_track(track))
        return 0;
    L = &live[track];
    if (digiseq_arp[track].set[A_HOLD] && !L->down)
        L->n = 0;                           /* HOLD: a new set */
    L->down |= (unsigned short)(1u << key);
    for (i = 0; i < L->n && L->key[i] != key; i++)
        ;
    if (i < L->n) {
        L->note[i] = (signed char)note;     /* the key again (HOLD): its new note */
    } else {
        if (L->n >= MAXHELD)
            live_drop(L, 0);                /* full: the oldest goes */
        L->note[L->n] = (signed char)note;
        L->key[L->n] = (unsigned char)key;
        L->n = (unsigned char)(L->n + 1);
    }
    L->vel = (unsigned char)(vel < 1 ? 1 : vel > 127 ? 127 : vel);
    redraw = 1;
    return TRANSPORT == 1;
}

void digiseq_kb_off(int32_t key)
{
    int t, i;
    if (key < 0 || key > 15)
        return;
    for (t = 0; t < SQ_AUDIO; t++) {
        struct live *L = &live[t];
        if (!(L->down & (1u << key)))
            continue;
        L->down &= (unsigned short)~(1u << key);
        if (!digiseq_arp[t].set[A_HOLD])
            for (i = 0; i < L->n; i++)
                if (L->key[i] == key) {
                    live_drop(L, i);
                    break;
                }
        redraw = 1;
    }
}

/* each UI frame: a track no longer ARP holds nothing; HOLD off lets go of
 * the keys no longer down */
static void live_tidy(void)
{
    int t, i;
    for (t = 0; t < SQ_AUDIO; t++) {
        struct live *L = &live[t];
        if (!L->n && !L->down)
            continue;
        if (!arp_kb_track(t)) {
            L->n = 0;
            L->down = 0;
            continue;
        }
        if (!digiseq_arp[t].set[A_HOLD])
            for (i = L->n - 1; i >= 0; i--)
                if (!(L->down & (1u << L->key[i])))
                    live_drop(L, i);
    }
}

/* ==== 2X: the track's own speed =========================================
 * The pattern (911 bytes a track) keeps a speed per track at +0x38b, used
 * when its SCALE mode is per track (the settings' +0x19 == 1, at +0x1ec40): an index into the
 * stock speeds (2X, 3/2X, 1X, 3/4X, 1/2X, 1/4X, 1/8X). 2X halves the track's
 * step; switching it or the type off, or the pattern changing, puts it back.
 * The speed it had + 1 is kept with the type's data (SNAKE: pad[0]; 185:
 * spd), so a power cycle with 2X on still knows what to put back. */
#define PAT_MODE   (0x1ec40 + 0x19)
#define TR_SPEED   0x38b
static uint8_t *mod_pat[TRACKS];
static signed char mod_orig[TRACKS];
static unsigned char mod_eng[TRACKS];

static int per_track_mode(void)
{
    uint8_t *pat = PATTERN;
    return pat && pat[PAT_MODE] == 1;
}

static int speed_half(int i)
{
    static const signed char h[7] = { 0, 1, 0, 1, 2, 4, 5 };
    return i >= 0 && i < 7 ? h[i] : i;
}

static unsigned char *spd_keep(int t, int eng)
{
    return eng == E_185 ? &digiseq_trk[t].spd : eng == E_EUC ? &digiseq_euc[t].spd :
           eng == E_TUR ? &digiseq_tur[t].spd : eng == E_GRD ? &digiseq_grd[t].spd : &digiseq_snake[t].pad[0];
}

static void speed_sync(void)
{
    uint8_t *pat = PATTERN;
    int t, pt = pat && pat[PAT_MODE] == 1;
    for (t = 0; t < TRACKS; t++) {
        const struct track *k = &digiseq_trk[t];
        int eng = k->eng;
        int want = pt && k->on &&                   /* ARP: never 2X */
                   (eng == E_185 ? k->st.set[S_CLOCK] == 0 :
                    eng == E_EUC ? digiseq_euc[t].setup[V_CLOCK] == 0 :
                    eng == E_TUR ? digiseq_tur[t].setup[V_CLOCK] == 0 :
                    eng == E_GRD ? digiseq_grd[t].setup[V_CLOCK] == 0 :
                    eng == E_SNAKE ? digiseq_snake[t].set[P_DIV] == 0 : 0);
        if (mod_pat[t] && (mod_pat[t] != pat || !want || mod_eng[t] != eng)) {     /* put it back */
            uint8_t *tr = TRACK_OF(mod_pat[t], t);
            if ((signed char)tr[TR_SPEED] == speed_half(mod_orig[t]))
                tr[TR_SPEED] = (uint8_t)mod_orig[t];
            mod_pat[t] = 0;
            *spd_keep(t, mod_eng[t]) = 0;
            changed();
        }
        if (want && !mod_pat[t]) {
            uint8_t *tr = TRACK_OF(pat, t);
            unsigned char *keep = spd_keep(t, eng);
            int o = (signed char)tr[TR_SPEED], h;
            if (*keep && o == speed_half(*keep - 1)) {          /* ours, from before */
                mod_pat[t] = pat;
                mod_orig[t] = (signed char)(*keep - 1);
                mod_eng[t] = (unsigned char)eng;
            } else if ((h = speed_half(o)) != o) {
                mod_orig[t] = (signed char)o;
                tr[TR_SPEED] = (uint8_t)h;
                mod_pat[t] = pat;
                mod_eng[t] = (unsigned char)eng;
                *keep = (unsigned char)(o + 1);
                changed();
            }
        }
    }
}

/* ==== PRINT (TRIG + FUNC + REC) ==========================================
 * Plays the track's type through the track's length, as it would from the
 * start (or, playing, from where it is), and writes each step as a stock
 * trig, then sets TYPE to OFF. The pattern (seq-events-dt1's dev.h): 911
 * bytes a track, its step words big-endian u16 at +0 (bit 0 the trig, 0x0080
 * the SYN trig and 0x0800 its override of the track's default at +0x382),
 * note lock 1 a step at +0x280, each step's VEL at +0x80 and LEN at +0xc0
 * (255: the track's), its micro timing at +0x100 and its trig condition at
 * +0x200 (255: none; measured in digiemu). A printed note is put on its step
 * with no condition, so a trig left there from before cannot shift or mute it. A step that fires becomes a note trig with the note as
 * its note lock, its velocity and (185's held and LEN-set notes) its LEN; a
 * rest clears a note trig and keeps a lock trig. A 2X speed stays. */
#define TR_VEL_OFF    0x080
#define TR_LEN_OFF    0x0c0
#define TR_MICRO_OFF  0x100                /* a step's micro timing, 0: on the step */
#define TR_COND_OFF   0x200                /* its trig condition or probability, 255: none */
#define TR_NOTE_LOCK  0x280
#define TR_DEF_TRIGS  0x382
#define W_TRIG        0x0001
#define W_SYN         0x0080
#define W_SYN_SET     0x0800
static uint8_t print_bak[TRACK_STRIDE];
static signed char print_t = -1;
static uint8_t *print_pat;

static int word_syn(const uint8_t *trk, unsigned w)
{
    return (w & W_SYN_SET) ? (w & W_SYN) != 0 : (trk[TR_DEF_TRIGS + 1] & W_SYN) != 0;
}

static int print_track(int t)
{
    uint8_t *pat = PATTERN, *trk;
    struct track *k = &digiseq_trk[t];
    struct sq_track info;
    unsigned long rnd = 0x9e3779b9UL ^ frames;
    int len, base, root, step, i, first, playing, pending;
    uint32_t last;
    struct play qs;
    struct play185 q8;
    struct playeuc qe;
    struct playtur qt;
    struct playgrd qg;
    const struct grids *gr = &digiseq_grd[t];
    struct turing *tr = &digiseq_tur[t];
    const struct euclid *u = &digiseq_euc[t];
    const struct snake *s = &digiseq_snake[t];
    const struct stages *g = &k->st;
    int slen = snake_len(s);
    if (!pat || sq_track_info(t, &info, sizeof info) != 0)
        return -1;
    len = info.len;
    if (len < 1 || len > SQ_STEPS)
        return -1;
    base = info.note;
    root = base % 12;
    trk = TRACK_OF(pat, t);
    copy(print_bak, trk, TRACK_STRIDE);
    copy(&qs, &play[t], sizeof qs);
    copy(&q8, &p185[t], sizeof q8);
    copy(&qe, &peuc[t], sizeof qe);
    copy(&qt, &ptur[t], sizeof qt);
    copy(&qg, &pgrd[t], sizeof qg);
    print_t = (signed char)t;
    print_pat = pat;
    last = sq_clock[t].last;
    playing = TRANSPORT == 1 && k->on && last < (uint32_t)len;
    pending = !playing || reset_req[t] || prep[t] || rewind[t] || dj_starts != dj_seen[t];
    first = playing ? ((int)last + 1) % len : 0;
    for (i = 0; i < len; i++) {
        int fire, n, vel, l = -1;
        unsigned w;
        step = (first + i) % len;
        if (k->eng == E_GRD) {
            if (i == 0 && pending)
                grd_start(&qg, gr, &rnd);
            else
                grd_advance(&qg, gr, &rnd);
            fire = qg.fire;
            n = base + gr->set[G_NOTE] - NOTE_MAX;
            vel = qg.acc ? gr->set[G_AVL] : gr->set[G_VEL];
            if (gr->setup[V_LEN] > 0 && gr->setup[V_LEN] < NLENS)
                l = len_for_halves(nlen_halves[gr->setup[V_LEN]]);
        } else if (k->eng == E_TUR) {              /* what it would play next, the register left as it is */
            if (i == 0 && pending)
                tur_start(&qt, tr);
            else
                tur_advance(&qt, tr, &rnd, 0);
            fire = qt.fire;
            n = tur_note(tr, qt.reg, base, root);
            vel = tur_velocity(tr, qt.reg);
            if (tr->setup[V_LEN] > 0 && tr->setup[V_LEN] < NLENS)
                l = len_for_halves(nlen_halves[tr->setup[V_LEN]]);
        } else if (k->eng == E_EUC) {
            if (i == 0 && pending)
                euc_start(&qe, u, &rnd);
            else
                euc_advance(&qe, u, &rnd);
            fire = qe.fire;
            n = euc_note_of(u, &qe, base);
            vel = euc_vel_of(u, &qe);
            if (u->setup[V_LEN] > 0 && u->setup[V_LEN] < NLENS)
                l = len_for_halves(nlen_halves[u->setup[V_LEN]]);
        } else if (k->eng == E_185) {
            if (i == 0 && pending)
                st_start(&q8, g);
            else
                st_advance(&q8, g, &rnd);
            fire = q8.fire;
            n = quantise(base + g->pitch[q8.stage], root, g->set[S_SCALE]);
            vel = g->vel[q8.stage];
            if (q8.hold)
                l = len_for_halves(2 * q8.hold);
            else if (g->set[S_NLEN] > 0 && g->set[S_NLEN] < NLENS)
                l = len_for_halves(nlen_halves[g->set[S_NLEN]]);
        } else {
            int cell;
            if (i == 0 && pending)
                snake_start(&qs, s, slen);
            else
                snake_advance(&qs, s, slen, &rnd);
            if (qs.w.k >= slen)
                qs.w.k = 0;
            cell = cell_of(s, qs.w.k, slen);
            fire = qs.moved && ((s->gate >> cell) & 1);
            n = quantise(base + s->note[cell], root, s->set[P_SCALE]);
            vel = s->vel[cell];
        }
        w = (unsigned)(trk[2 * step] << 8 | trk[2 * step + 1]);
        if (fire) {
            w |= W_TRIG;
            if (!word_syn(trk, w))
                w |= W_SYN_SET | W_SYN;     /* a note trig, whatever the default */
            trk[TR_NOTE_LOCK + step] = (uint8_t)clamp_note(n);
            trk[TR_VEL_OFF + step] = (uint8_t)vel;
            trk[TR_MICRO_OFF + step] = 0;           /* on the step, as it played */
            trk[TR_COND_OFF + step] = 0xff;         /* no condition: it plays every time */
            if (k->eng == E_185 || k->eng == E_EUC || k->eng == E_TUR || k->eng == E_GRD)
                trk[TR_LEN_OFF + step] = (uint8_t)l;    /* -1 (255): the track's LEN */
        } else if ((w & W_TRIG) && word_syn(trk, w)) {
            w &= ~(unsigned)W_TRIG;         /* a rest: no note trig (lock trigs stay) */
        }
        trk[2 * step] = (uint8_t)(w >> 8);
        trk[2 * step + 1] = (uint8_t)w;
    }
    if (mod_pat[t]) {                       /* keep the 2X the print was made at */
        mod_pat[t] = 0;
        *spd_keep(t, mod_eng[t]) = 0;
    }
    k->on = 0;
    digiseq_snake[t].set[P_ON] = 0;
    sq_rearm(1u << t);
    return len;
}

static void unprint(int t)
{
    uint8_t *trk;
    if (print_t != t || !print_pat || print_pat != PATTERN)
        return;
    trk = TRACK_OF(print_pat, t);
    copy(trk, print_bak, TRACK_STRIDE);
    print_t = -1;
    sq_rearm(1u << t);
}

/* ==== the page ===========================================================
 * The key dispatcher's brain (each key event's first argument) keeps the
 * screens shown: +0x54 the bottom one (the page views), +0x5c the top one
 * (another while a menu or a browser is open over the pages). The bottom
 * one's +8 is the page manager, whose +0xb4 is the view shown; a view's +136
 * is its page key (measured in digiemu on OS 1.53: TRIG 19). */
#define BR_BOTTOM 0x54
#define BR_TOP    0x5c
#define SCR_PM    8
#define PM_VIEW   0xb4
#define VIEW_KEY  136

#define KEY_FUNC  1
#define KEY_TRK   2
#define KEY_REC   9
#define KEY_PLAY  10
#define KEY_STOP  11
#define KEY_YES   12
#define KEY_NO    13
#define KEY_UP    14
#define KEY_DOWN  15
#define KEY_LEFT  16
#define KEY_RIGHT 17
#define KEY_PAGE  18
#define KEY_TRIG  19
#define KEY_LFO   23
#define KEY_STEP1 24
#define EV_KEY(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(unsigned long *)((char *)(ev) + 16))
#define EV_ENC(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_DELTA(ev) (*(long *)((char *)(ev) + 16))
#define F_DOWN    1
#define F_FUNC    2
#define F_REPEAT  8
#define F_UP      0x10

static uint32_t rd(uint32_t a)
{
    return a >= 0x40000000u && a < 0x48000000u ? *(volatile uint32_t *)a : 0;
}

/* the page key whose view is on screen with nothing over it, or 0 */
static uint32_t page_view_key(void *brain)
{
    uint32_t b = (uint32_t)brain, bottom, pm, view;
    if (!b || !(bottom = rd(b + BR_BOTTOM)) || bottom != rd(b + BR_TOP))
        return 0;
    if (!(pm = rd(bottom + SCR_PM)) || !(view = rd(pm + PM_VIEW)))
        return 0;
    return rd(view + VIEW_KEY);
}

/* the drop-down's object: the shown page view's +108 */
#define VIEW_POPUP 108
void *digiseq_brain;
static void *popup_obj(void)
{
    uint32_t b = (uint32_t)digiseq_brain, bottom, pm, view, obj;
    if (!b || !(bottom = rd(b + BR_BOTTOM)) || !(pm = rd(bottom + SCR_PM)) || !(view = rd(pm + PM_VIEW)))
        return 0;
    obj = rd(view + VIEW_POPUP);
    return obj >= 0x40000000u && obj < 0x48000000u ? (void *)obj : 0;
}

/* pages: SNAKE's and 185's, numbered within the type */
enum { PG_NOTES, PG_GATES, PG_VEL, PG_PATH, SNAKE_PAGES };
enum { PG8_PITCH, PG8_VEL, PG8_PULSES, PG8_MODE, PG8_DIV, PG8_SETUP, PAGES_185 };
static const char *const snake_pages[SNAKE_PAGES] = { "NOTES", "GATES", "VELOCITY", "SETUP" };
static const char *const pages185[PAGES_185] = { "NOTES", "VELOCITY", "PULSES", "MODE", "DIV", "SETUP" };
static const char *const snake_what[SNAKE_PAGES] = { "Notes", "Gates", "Velocity", "Setup" };
static const char *const what185[PAGES_185] = { "Notes", "Velocity", "Pulses", "Modes", "Divisions", "Setup" };
enum { PGU_EUC, PGU_SETUP, EUC_PAGES };
static const char *const euc_pages[EUC_PAGES] = { "EUCLID", "SETUP" };
static const char *const euc_what[EUC_PAGES] = { "Euclid", "Setup" };
enum { PGG_GRD, PGG_SETUP, GRD_PAGES };
static const char *const grd_pages[GRD_PAGES] = { "GRIDS", "SETUP" };
static const char *const grd_what[GRD_PAGES] = { "Grids", "Setup" };
enum { PGR_TUR, PGR_SETUP, TUR_PAGES };
static const char *const tur_pages[TUR_PAGES] = { "TURING", "SETUP" };
static const char *const tur_what[TUR_PAGES] = { "Turing", "Setup" };
enum { PGA_ARP, ARP_PAGES };
static const char *const arp_pages[ARP_PAGES] = { "ARP" };
static const char *const arp_what[ARP_PAGES] = { "Arp" };

unsigned char digiseq_open;                         /* the page is up */
unsigned char digiseq_pg_snake, digiseq_pg_185;     /* the page shown, per type */
unsigned char digiseq_pg_arp, digiseq_pg_euc, digiseq_pg_tur, digiseq_pg_grd;
void *digiseq_brain;
static unsigned char trig_held, trig_used, stock_held, stock_used;
static unsigned char func_held, trk_held;
static long enc_acc[8];
#define COUNTS    4
static unsigned long ui_rand = 0x9e3779b9UL;

static int ui_track(void)
{
    uint32_t t = fw_active_track;
    return t < TRACKS ? (int)t : -1;
}

static int eng_of(int t)
{
    int e = digiseq_trk[t].eng;
    return e == E_185 || e == E_ARP || e == E_EUC || e == E_TUR || e == E_GRD ? e : E_SNAKE;
}

static unsigned char *pg_of(int eng)
{
    return eng == E_185 ? &digiseq_pg_185 : eng == E_ARP ? &digiseq_pg_arp :
           eng == E_EUC ? &digiseq_pg_euc : eng == E_TUR ? &digiseq_pg_tur :
           eng == E_GRD ? &digiseq_pg_grd : &digiseq_pg_snake;
}

static int pages_of(int eng)
{
    return eng == E_185 ? PAGES_185 : eng == E_ARP ? ARP_PAGES : eng == E_EUC ? EUC_PAGES : eng == E_TUR ? TUR_PAGES :
           eng == E_GRD ? GRD_PAGES : SNAKE_PAGES;
}

static int last_page(int eng)
{
    return pages_of(eng) - 1;
}

static int clampi(long v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

static int type_of(int t)
{
    return digiseq_trk[t].on ? eng_of(t) : 0;
}

static void set_type(int t, int ty)
{
    struct track *k = &digiseq_trk[t];
    if (ty == 0) {
        k->on = 0;
    } else {
        if (k->eng != ty || !k->on)         /* stay on TYPE: the new type's last page */
            *pg_of(ty) = (unsigned char)last_page(ty);
        k->eng = (unsigned char)ty;
        k->on = 1;
    }
    digiseq_snake[t].set[P_ON] = (unsigned char)(k->on && k->eng == E_SNAKE);
    if (k->on && k->eng == E_ARP)
        arp_reset(&parp[t]);                /* nothing held until the next note trig */
    reset_req[t] = 1;
}

/* ---- SNAKE's page edits ---- */
static void snake_randomise(struct snake *s, int pg)
{
    int c, r = s->set[P_RANGE];
    if (r < 1 || r > NOTE_MAX)
        r = 12;
    for (c = 0; c < CELLS; c++) {
        unsigned long x = xorshift(&ui_rand);
        if (pg == PG_NOTES)
            s->note[c] = (signed char)((long)(x % (unsigned long)(2 * r + 1)) - r);
        else if (pg == PG_VEL)
            s->vel[c] = (unsigned char)(40 + x % 88);
        else if (pg == PG_GATES) {
            if ((x >> 16) % 100 < 65)
                s->gate |= (unsigned char)(1u << c);
            else
                s->gate &= (unsigned char)~(1u << c);
        }
    }
    if (pg == PG_GATES && !s->gate)
        s->gate = (unsigned char)(1u << (xorshift(&ui_rand) % CELLS));
    if (pg == PG_PATH) {                    /* everything but TYPE and SCALE */
        static const unsigned char divs[8] = { 1, 1, 1, 1, 1, 2, 2, 4 };
        s->set[P_SHAPE] = (unsigned char)(xorshift(&ui_rand) % SHAPES);
        s->set[P_DIR] = (unsigned char)(xorshift(&ui_rand) % DIRS);
        s->set[P_LEN] = (unsigned char)(3 + xorshift(&ui_rand) % (CELLS - 2));
        s->set[P_DIV] = divs[xorshift(&ui_rand) & 7];
        s->set[P_RANGE] = (unsigned char)(5 + xorshift(&ui_rand) % (NOTE_MAX - 4));
        s->set[P_SHIFT] = (unsigned char)(xorshift(&ui_rand) % CELLS);
    }
}

static void snake_clear(struct snake *s, int pg)
{
    struct snake d;
    int c;
    snake_defaults(&d);
    for (c = 0; c < CELLS; c++) {
        if (pg == PG_NOTES)
            s->note[c] = 0;
        else if (pg == PG_VEL)
            s->vel[c] = d.vel[c];
    }
    if (pg == PG_GATES)
        s->gate = 0xff;
    if (pg == PG_PATH) {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++)
            s->set[c] = d.set[c];
        s->set[P_ON] = on;
    }
}

static void snake_nudge(struct snake *s, int pg, int d, int func)
{
    int c;
    if (pg == PG_NOTES) {
        for (c = 0; c < CELLS; c++)
            s->note[c] = (signed char)clampi(s->note[c] + d * (func ? 12 : 1), -NOTE_MAX, NOTE_MAX);
    } else if (pg == PG_VEL) {
        for (c = 0; c < CELLS; c++)
            s->vel[c] = (unsigned char)clampi(s->vel[c] + d * 8, 1, 127);
    } else if (pg == PG_GATES) {
        unsigned g = s->gate;
        g = d > 0 ? ((g << 1) | (g >> 7)) : ((g >> 1) | (g << 7));
        s->gate = (unsigned char)g;
    }
}

static void snake_paste(struct snake *s, const struct snake *from, int pg)
{
    int c;
    if (pg == PG_NOTES)
        for (c = 0; c < CELLS; c++) s->note[c] = from->note[c];
    else if (pg == PG_VEL)
        for (c = 0; c < CELLS; c++) s->vel[c] = from->vel[c];
    else if (pg == PG_GATES)
        s->gate = from->gate;
    else {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++) s->set[c] = from->set[c];
        s->set[P_ON] = on;
    }
}

/* ---- 185's page edits ---- */
static void st_randomise(struct stages *g, int pg)
{
    int c, r = g->set[S_RANGE];
    if (r < 1 || r > NOTE_MAX)
        r = 12;
    for (c = 0; c < CELLS; c++) {
        unsigned long x = xorshift(&ui_rand);
        switch (pg) {
        case PG8_PITCH:
            g->pitch[c] = (signed char)((long)(x % (unsigned long)(2 * r + 1)) - r);
            break;
        case PG8_VEL:
            g->vel[c] = (unsigned char)(40 + x % 88);
            break;
        case PG8_PULSES: {                  /* mostly short stages */
            static const unsigned char l[16] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 4, 4, 6, 8, 2 };
            g->pulses[c] = l[(x >> 8) & 15];
            break;
        }
        case PG8_MODE: {                    /* ONE most, then HOLD, ALL, OFF */
            unsigned long v = (x >> 12) % 100;
            g->mode[c] = (unsigned char)(v < 45 ? M_ONE : v < 70 ? M_HOLD : v < 85 ? M_ALL : M_OFF);
            break;
        }
        case PG8_DIV: {
            static const unsigned char dv[8] = { 1, 1, 1, 1, 2, 2, 3, 4 };
            g->div[c] = dv[(x >> 4) & 7];
            break;
        }
        default:
            break;
        }
    }
    if (pg == PG8_MODE) {                   /* never all rests */
        for (c = 0; c < CELLS && g->mode[c] == M_OFF; c++)
            ;
        if (c == CELLS)
            g->mode[xorshift(&ui_rand) % CELLS] = M_ONE;
    }
    if (pg == PG8_SETUP) {                  /* DIR, STAGES, RANGE, SHIFT */
        g->set[S_DIR] = (unsigned char)(xorshift(&ui_rand) % DIRS);
        g->set[S_STAGES] = (unsigned char)(3 + xorshift(&ui_rand) % (CELLS - 2));
        g->set[S_RANGE] = (unsigned char)(5 + xorshift(&ui_rand) % (NOTE_MAX - 4));
        g->set[S_SHIFT] = (unsigned char)(xorshift(&ui_rand) % CELLS);
    }
}

static void st_clear(struct stages *g, int pg)
{
    struct stages d;
    int c;
    stages_defaults(&d);
    for (c = 0; c < CELLS; c++) {
        switch (pg) {
        case PG8_PITCH:  g->pitch[c] = 0; break;
        case PG8_VEL:    g->vel[c] = 100; break;
        case PG8_PULSES: g->pulses[c] = 1; break;
        case PG8_MODE:   g->mode[c] = M_ONE; break;
        case PG8_DIV:    g->div[c] = 1; break;
        default: break;
        }
    }
    if (pg == PG8_SETUP)
        for (c = 0; c < NSETS; c++)
            g->set[c] = d.set[c];
}

static void st_nudge(struct stages *g, int pg, int d, int func)
{
    int c;
    unsigned char tmp;
    switch (pg) {
    case PG8_PITCH:
        for (c = 0; c < CELLS; c++)
            g->pitch[c] = (signed char)clampi(g->pitch[c] + d * (func ? 12 : 1), -NOTE_MAX, NOTE_MAX);
        break;
    case PG8_VEL:
        for (c = 0; c < CELLS; c++)
            g->vel[c] = (unsigned char)clampi(g->vel[c] + d * 8, 1, 127);
        break;
    case PG8_PULSES:
        for (c = 0; c < CELLS; c++)
            g->pulses[c] = (unsigned char)clampi(g->pulses[c] + d, 1, 8);
        break;
    case PG8_DIV:
        for (c = 0; c < CELLS; c++)
            g->div[c] = (unsigned char)clampi(g->div[c] + d, 1, 4);
        break;
    case PG8_MODE:                          /* rotate the stages' modes */
        if (d > 0) {
            tmp = g->mode[CELLS - 1];
            for (c = CELLS - 1; c > 0; c--) g->mode[c] = g->mode[c - 1];
            g->mode[0] = tmp;
        } else {
            tmp = g->mode[0];
            for (c = 0; c < CELLS - 1; c++) g->mode[c] = g->mode[c + 1];
            g->mode[CELLS - 1] = tmp;
        }
        break;
    default:
        break;
    }
}

static void st_paste(struct stages *g, const struct stages *from, int pg)
{
    int c;
    for (c = 0; c < CELLS; c++) {
        switch (pg) {
        case PG8_PITCH:  g->pitch[c] = from->pitch[c]; break;
        case PG8_VEL:    g->vel[c] = from->vel[c]; break;
        case PG8_PULSES: g->pulses[c] = from->pulses[c]; break;
        case PG8_MODE:   g->mode[c] = from->mode[c]; break;
        case PG8_DIV:    g->div[c] = from->div[c]; break;
        default: break;
        }
    }
    if (pg == PG8_SETUP)
        for (c = 0; c < NSETS; c++)
            g->set[c] = from->set[c];
}

/* ---- ARP's page edits ---- */
static void arp_randomise(struct arpset *a)  /* MODE, SPD, RNG, RPT (INT stays) */
{
    static const unsigned char spd[8] = { 0, 0, 0, 0, 1, 1, 2, 3 };
    static const unsigned char rpt[8] = { 1, 1, 1, 1, 1, 2, 2, 3 };
    a->set[A_MODE] = (unsigned char)(xorshift(&ui_rand) % AMODES);
    a->set[A_SPD] = spd[xorshift(&ui_rand) & 7];
    a->set[A_RNG] = (unsigned char)(a->set[A_INT] == INT_OCT ? 1 + xorshift(&ui_rand) % 3
                                                             : 2 + xorshift(&ui_rand) % (RNG_MAX - 1));
    a->set[A_RPT] = rpt[xorshift(&ui_rand) & 7];
}

static void arp_clear(struct arpset *a)
{
    struct arpset d;
    int c;
    arp_defaults(&d);
    for (c = 1; c < NASETS; c++)
        a->set[c] = d.set[c];
}

static void arp_paste(struct arpset *a, const struct arpset *from)
{
    int c;
    for (c = 1; c < NASETS; c++)
        a->set[c] = from->set[c];
}

/* ---- EUCLID's page edits ---- */
static void euc_randomise(struct euclid *u, int pg)
{
    if (pg == PGU_EUC) {                    /* a pattern: STEPS 5-16, HITS, ROT, accents */
        static const unsigned char steps[8] = { 16, 16, 16, 12, 8, 7, 5, 13 };
        int n = steps[xorshift(&ui_rand) & 7];
        u->set[U_STEPS] = (unsigned char)n;
        u->set[U_HITS] = (unsigned char)(1 + xorshift(&ui_rand) % (unsigned long)(n - 1 > 1 ? n - 1 : 1));
        u->set[U_ROT] = (unsigned char)(xorshift(&ui_rand) % (unsigned long)n);
        u->set[U_ACC] = (unsigned char)(xorshift(&ui_rand) % (unsigned long)(u->set[U_HITS] + 1));
        u->set[U_AROT] = (unsigned char)(xorshift(&ui_rand) % (unsigned long)u->set[U_HITS]);
    } else {                                /* CLOCK and ANOT */
        static const unsigned char clk[8] = { 1, 1, 1, 1, 1, 2, 2, 0 };
        static const signed char anot[8] = { 0, 0, 0, 12, 12, 7, -12, 5 };
        u->setup[V_CLOCK] = clk[xorshift(&ui_rand) & 7];
        u->setup[V_ANOT] = (unsigned char)(NOTE_MAX + anot[xorshift(&ui_rand) & 7]);
    }
    euc_check(u);
}

static void euc_clear(struct euclid *u, int pg)
{
    struct euclid d;
    int c;
    euc_defaults(&d);
    if (pg == PGU_EUC) {
        for (c = 0; c < NUSETS; c++)
            u->set[c] = d.set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            u->setup[c] = d.setup[c];
    }
}

static void euc_paste(struct euclid *u, const struct euclid *from, int pg)
{
    int c;
    if (pg == PGU_EUC) {
        for (c = 0; c < NUSETS; c++)
            u->set[c] = from->set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            u->setup[c] = from->setup[c];
    }
    euc_check(u);
}

static void euc_nudge(struct euclid *u, int d)     /* UP / DOWN: ROT */
{
    int n = euc_steps(u);
    u->set[U_ROT] = (unsigned char)(((int)u->set[U_ROT] + d + n) % n);
}

/* ---- TURING's page edits ---- */
static void tur_randomise(struct turing *r, int pg)
{
    if (pg == PGR_TUR) {                    /* a new register */
        r->reg = (uint16_t)(xorshift(&ui_rand) >> 5);
    } else {                                /* CLOCK */
        static const unsigned char clk[8] = { 1, 1, 1, 1, 1, 2, 2, 0 };
        r->setup[V_CLOCK] = clk[xorshift(&ui_rand) & 7];
    }
}

static void tur_clear(struct turing *r, int pg)
{
    struct turing d;
    int c;
    tur_defaults(&d);
    if (pg == PGR_TUR) {
        r->reg = d.reg;
        for (c = 0; c < NRSETS; c++)
            r->set[c] = d.set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            r->setup[c] = d.setup[c];
    }
}

static void tur_paste(struct turing *r, const struct turing *from, int pg)
{
    int c;
    if (pg == PGR_TUR) {
        r->reg = from->reg;
        for (c = 0; c < NRSETS; c++)
            r->set[c] = from->set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            r->setup[c] = from->setup[c];
    }
    tur_check(r);
}

static void tur_nudge(struct turing *r, int d)     /* UP / DOWN: turn the loop a step */
{
    int n = tur_len(r), b;
    uint16_t m = (uint16_t)((1u << n) - 1), loop = r->reg & m;
    if (d > 0)
        b = (loop >> (n - 1)) & 1, loop = (uint16_t)(((loop << 1) | b) & m);
    else
        b = loop & 1, loop = (uint16_t)((loop >> 1) | (b << (n - 1)));
    r->reg = (uint16_t)((r->reg & ~m) | loop);
}

/* ---- GRIDS' page edits ---- */
static void grd_randomise(struct grids *g, int pg)
{
    if (pg == PGG_GRD) {                    /* a new place on the map; the part stays */
        g->set[G_X] = (unsigned char)(xorshift(&ui_rand) >> 9 & 127);
        g->set[G_Y] = (unsigned char)(xorshift(&ui_rand) >> 9 & 127);
    } else {
        static const unsigned char clk[8] = { 1, 1, 1, 1, 1, 1, 2, 0 };
        g->setup[V_CLOCK] = clk[xorshift(&ui_rand) & 7];
    }
}

static void grd_clear(struct grids *g, int pg)
{
    struct grids d;
    int c;
    grd_defaults(&d);
    if (pg == PGG_GRD) {
        for (c = 1; c < NGSETS; c++)        /* the part stays */
            g->set[c] = d.set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            g->setup[c] = d.setup[c];
    }
}

static void grd_paste(struct grids *g, const struct grids *from, int pg)
{
    int c;
    if (pg == PGG_GRD) {
        for (c = 0; c < NGSETS; c++)
            g->set[c] = from->set[c];
    } else {
        for (c = 1; c < NVSETS; c++)
            g->setup[c] = from->setup[c];
    }
    grd_check(g);
}

/* ---- shared page actions ---- */
static void page_randomise(int t, int pg)
{
    ui_rand ^= frames;
    if (eng_of(t) == E_GRD)
        grd_randomise(&digiseq_grd[t], pg);
    else if (eng_of(t) == E_TUR)
        tur_randomise(&digiseq_tur[t], pg);
    else if (eng_of(t) == E_EUC)
        euc_randomise(&digiseq_euc[t], pg);
    else if (eng_of(t) == E_ARP)
        arp_randomise(&digiseq_arp[t]);
    else if (eng_of(t) == E_185)
        st_randomise(&digiseq_trk[t].st, pg);
    else
        snake_randomise(&digiseq_snake[t], pg);
}

static void randomise_all(int t)
{
    ui_rand ^= frames;
    if (eng_of(t) == E_GRD) {
        grd_randomise(&digiseq_grd[t], PGG_GRD);
    } else if (eng_of(t) == E_TUR) {
        tur_randomise(&digiseq_tur[t], PGR_TUR);
    } else if (eng_of(t) == E_EUC) {
        euc_randomise(&digiseq_euc[t], PGU_EUC);
    } else if (eng_of(t) == E_ARP) {
        arp_randomise(&digiseq_arp[t]);
    } else if (eng_of(t) == E_185) {
        st_randomise(&digiseq_trk[t].st, PG8_PITCH);
        st_randomise(&digiseq_trk[t].st, PG8_VEL);
        st_randomise(&digiseq_trk[t].st, PG8_PULSES);
        st_randomise(&digiseq_trk[t].st, PG8_MODE);
    } else {
        snake_randomise(&digiseq_snake[t], PG_NOTES);
        snake_randomise(&digiseq_snake[t], PG_GATES);
        snake_randomise(&digiseq_snake[t], PG_VEL);
    }
}

static const char *what_of(int eng, int pg)
{
    return eng == E_185 ? what185[pg % PAGES_185] : eng == E_ARP ? arp_what[pg % ARP_PAGES]
         : eng == E_EUC ? euc_what[pg % EUC_PAGES] : eng == E_TUR ? tur_what[pg % TUR_PAGES]
         : eng == E_GRD ? grd_what[pg % GRD_PAGES] : snake_what[pg % SNAKE_PAGES];
}

/* COPY / PASTE: one page, from any track to any other of the same type */
static struct whole clip;
static signed char clip_pg = -1;
static unsigned char clip_eng;

static void edited(int t)
{
    run_active[t] = 0;
    changed();
}

static void reload(int t, const struct whole *from, int valid)
{
    if (!valid)
        return;
    put_whole(t, from);
    reset_req[t] = 1;
    changed();
    POPUP("Sequence reloaded");
}

static int next_stop(int v, long d, const signed char *stops, int n)
{
    int i;
    if (d > 0) {
        for (i = 0; i < n; i++)
            if (stops[i] > v)
                return stops[i];
        return stops[n - 1];
    }
    for (i = n - 1; i >= 0; i--)
        if (stops[i] < v)
            return stops[i];
    return stops[0];
}
static const signed char note_stops[5] = { -24, -12, 0, 12, 24 };
static const signed char vel_stops[5] = { 1, 32, 64, 96, 127 };
static const signed char range_stops[3] = { 1, 12, 24 };
static const signed char ghost_stops[5] = { 0, 16, 32, 48, 64 };

static void open_page(void)
{
    digiseq_open = 1;
    trig_held = trig_used = 0;
    redraw = 1;
}

static void close_page(void)
{
    digiseq_open = 0;
    trig_held = 0;
    redraw = 1;
}

/* A key while the page is open: -> 1 to take it. */
static int page_key(int t, unsigned long key, unsigned long flags)
{
    int down = (flags & F_DOWN) && !(flags & F_REPEAT);
    int func = (flags & F_FUNC) || func_held;
    int eng = eng_of(t), pg;
    unsigned char *pgp = pg_of(eng);
    if (*pgp >= pages_of(eng))
        *pgp = 0;
    pg = *pgp;
    redraw = 1;
    if (key == KEY_TRIG && down && func && !trig_held) {   /* FUNC + TRIG: QUANTIZE */
        close_page();
        return 0;
    }
    if (key == KEY_TRIG) {
        if ((flags & F_UP) || !(flags & F_DOWN)) {      /* its release */
            if (trig_held && !trig_used)
                close_page();                           /* back to the TRIG page */
            trig_held = 0;
            return 1;
        }
        if (down) {
            trig_held = 1;
            trig_used = 0;
            print_t = -1;
            get_whole(t, &hold_copy);
            hold_t = (signed char)t;
        }
        return 1;
    }
    if (key > KEY_TRIG && key <= KEY_LFO) {             /* another page key: to its page */
        close_page();
        return 0;
    }
    if ((key == KEY_PLAY || key == KEY_STOP || key == KEY_REC) && !trig_held)
        return 0;                                       /* the transport, as ever */
    if (!down)
        return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN
            || key == KEY_YES || key == KEY_NO
            || (key >= KEY_STEP1 && key < KEY_STEP1 + CELLS && !trk_held &&
                ((eng == E_SNAKE && pg == PG_GATES) || (eng == E_185 && pg == PG8_MODE)))
            || (key >= KEY_STEP1 && key < KEY_STEP1 + 16 && !trk_held &&
                ((eng == E_EUC && pg == PGU_EUC) ||
                 (eng == E_TUR && pg == PGR_TUR) || (eng == E_GRD && pg == PGG_GRD && key < KEY_STEP1 + 3)));

    if (key == KEY_YES && func && digiseq_trk[t].on) {  /* randomise the note pages */
        trig_used = 1;
        if (!run_active[t] || run_t != t) {
            get_whole(t, &run_copy);
            run_t = (signed char)t;
            run_active[t] = 1;
        }
        randomise_all(t);
        POPUP("%s randomised", eng == E_185 ? "Stages" : eng == E_ARP ? "Arp" : eng == E_EUC ? "Euclid" : eng == E_TUR ? "Register" : eng == E_GRD ? "Map place" : "Snake");
        return 1;
    }
    if (key == KEY_NO && trig_held) {
        trig_used = 1;
        unprint(t);
        reload(t, &hold_copy, hold_t == t);
        run_active[t] = 0;
        return 1;
    }
    if (key == KEY_NO && func) {
        trig_used = 1;
        reload(t, &run_copy, run_t == t);
        run_active[t] = 0;
        return 1;
    }
    if ((key == KEY_UP || key == KEY_DOWN) && trig_held) {     /* TYPE */
        int ty = (type_of(t) + (key == KEY_UP ? 1 : NTYPES - 1)) % NTYPES;
        trig_used = 1;
        set_type(t, ty);
        edited(t);
        POPUP("Type=%s", type_names[ty]);
        return 1;
    }
    if (!digiseq_trk[t].on)                 /* OFF: nothing to edit */
        return key == KEY_YES || key == KEY_NO || key == KEY_LEFT || key == KEY_RIGHT
            || key == KEY_UP || key == KEY_DOWN || (key >= KEY_STEP1 && key < KEY_STEP1 + 16 && !trk_held)
            || (trig_held && (key == KEY_REC || key == KEY_STOP || key == KEY_PLAY));
    if (key == KEY_YES && trig_held) {
        trig_used = 1;
        page_randomise(t, pg);
        if (pg == last_page(eng))
            reset_req[t] = 1;
        edited(t);
        POPUP("%s randomised", what_of(eng, pg));
        return 1;
    }
    if (key == KEY_REC && trig_held && func) {                               /* PRINT */
        int n;
        trig_used = 1;
        if (eng == E_ARP) {                 /* record it live instead */
            POPUP("Print N/A for ARP");
            return 1;
        }
        n = print_track(t);
        if (n < 0) {
            POPUP("No pattern to print to");
        } else {
            reset_req[t] = 1;
            edited(t);
            POPUP("Printed %d steps", n);
        }
        return 1;
    }
    if (key == KEY_REC && trig_held) {                                       /* COPY */
        trig_used = 1;
        get_whole(t, &clip);
        clip_pg = (signed char)pg;
        clip_eng = (unsigned char)eng;
        POPUP("%s copied", what_of(eng, pg));
        return 1;
    }
    if (key == KEY_STOP && trig_held) {                                      /* PASTE */
        trig_used = 1;
        if (clip_pg < 0) {
            POPUP("Nothing to paste");
            return 1;
        }
        if (clip_eng != eng) {
            POPUP("The copy is %s's", type_names[clip_eng]);
            return 1;
        }
        *pgp = (unsigned char)clip_pg;
        if (eng == E_GRD)
            grd_paste(&digiseq_grd[t], &clip.g, clip_pg);
        else if (eng == E_TUR)
            tur_paste(&digiseq_tur[t], &clip.r, clip_pg);
        else if (eng == E_EUC)
            euc_paste(&digiseq_euc[t], &clip.u, clip_pg);
        else if (eng == E_ARP)
            arp_paste(&digiseq_arp[t], &clip.a);
        else if (eng == E_185)
            st_paste(&digiseq_trk[t].st, &clip.k.st, clip_pg);
        else
            snake_paste(&digiseq_snake[t], &clip.s, clip_pg);
        reset_req[t] = 1;
        edited(t);
        POPUP("%s pasted", what_of(eng, clip_pg));
        return 1;
    }
    if (key == KEY_PLAY && trig_held) {                                      /* CLEAR */
        trig_used = 1;
        if (eng == E_GRD)
            grd_clear(&digiseq_grd[t], pg);
        else if (eng == E_TUR)
            tur_clear(&digiseq_tur[t], pg);
        else if (eng == E_EUC)
            euc_clear(&digiseq_euc[t], pg);
        else if (eng == E_ARP)
            arp_clear(&digiseq_arp[t]);
        else if (eng == E_185)
            st_clear(&digiseq_trk[t].st, pg);
        else
            snake_clear(&digiseq_snake[t], pg);
        reset_req[t] = 1;
        edited(t);
        POPUP("%s cleared", what_of(eng, pg));
        return 1;
    }
    if (key == KEY_LEFT) {
        *pgp = (unsigned char)(pg ? pg - 1 : pages_of(eng) - 1);
        return 1;
    }
    if (key == KEY_RIGHT) {
        *pgp = (unsigned char)((pg + 1) % pages_of(eng));
        return 1;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        if (eng == E_GRD) {                 /* UP / DOWN: FILL */
            struct grids *g = &digiseq_grd[t];
            g->set[G_FILL] = (unsigned char)clampi(g->set[G_FILL] + (key == KEY_UP ? 4 : -4), 0, 127);
            POPUP("Fill=%d", g->set[G_FILL]);
        } else if (eng == E_TUR) {
            tur_nudge(&digiseq_tur[t], key == KEY_UP ? 1 : -1);
            POPUP("Register turned %s", key == KEY_UP ? "on" : "back");
        } else if (eng == E_EUC) {
            struct euclid *u = &digiseq_euc[t];
            euc_nudge(u, key == KEY_UP ? 1 : -1);
            POPUP("Rotate=%d", u->set[U_ROT]);
        } else if (eng == E_ARP)
            ;
        else if (eng == E_185)
            st_nudge(&digiseq_trk[t].st, pg, key == KEY_UP ? 1 : -1, func);
        else
            snake_nudge(&digiseq_snake[t], pg, key == KEY_UP ? 1 : -1, func);
        edited(t);
        return 1;
    }
    if (!trk_held && eng == E_GRD && pg == PGG_GRD && key >= KEY_STEP1 && key < KEY_STEP1 + GRD_PARTS) {
        int c = (int)(key - KEY_STEP1);     /* trig keys 1-3: the part */
        digiseq_grd[t].set[G_PART] = (unsigned char)c;
        POPUP("Part=%s", part_long[c]);
        edited(t);
        return 1;
    }
    if (!trk_held && eng == E_TUR && pg == PGR_TUR && key >= KEY_STEP1 && key < KEY_STEP1 + 16) {
        int c = (int)(key - KEY_STEP1);
        struct turing *r = &digiseq_tur[t];
        if (func) {                         /* FUNC + trig key: flip that bit */
            r->reg ^= (uint16_t)(1u << c);
            POPUP("Bit %d=%d", c + 1, (r->reg >> c) & 1);
        } else {                            /* trig key: LEN */
            r->set[R_LEN] = (unsigned char)(c < 1 ? 2 : c + 1);
            POPUP("Steps=%d", r->set[R_LEN]);
        }
        edited(t);
        return 1;
    }
    if (!trk_held && eng == E_EUC && pg == PGU_EUC && key >= KEY_STEP1 && key < KEY_STEP1 + EUC_MAX) {
        int c = (int)(key - KEY_STEP1);
        struct euclid *u = &digiseq_euc[t];
        if (func) {                         /* FUNC + trig key: HITS */
            u->set[U_HITS] = (unsigned char)(c + 1);
            euc_check(u);
            POPUP("Hits=%d", u->set[U_HITS]);
        } else {                            /* trig key: STEPS */
            u->set[U_STEPS] = (unsigned char)(c + 1);
            euc_check(u);
            reset_req[t] = 1;
            POPUP("Steps=%d", c + 1);
        }
        edited(t);
        return 1;
    }
    if (!trk_held && key >= KEY_STEP1 && key < KEY_STEP1 + CELLS) {
        int c = (int)(key - KEY_STEP1);
        if (eng == E_SNAKE && pg == PG_GATES) {
            digiseq_snake[t].gate ^= (unsigned char)(1u << c);
            edited(t);
            return 1;
        }
        if (eng == E_185 && pg == PG8_MODE) {
            struct stages *g = &digiseq_trk[t].st;
            g->mode[c] = (unsigned char)((g->mode[c] + 1) % MODES);
            edited(t);
            POPUP("Stage %d=%s", c + 1, mode_names[g->mode[c]]);
            return 1;
        }
    }
    return key == KEY_YES || key == KEY_NO;
}

int digiseq_key(void *brain, void *ev)
{
    unsigned long key = EV_KEY(ev), flags = EV_FLAGS(ev);
    int t, down = (flags & F_DOWN) && !(flags & F_REPEAT);
    digiseq_brain = brain;
    /* FUNC and TRK, followed always, never taken: a release is 0x10, or 0 when
     * another key went down meanwhile */
    if (key == KEY_FUNC) {
        if (!(flags & F_REPEAT))
            func_held = (unsigned char)(flags & F_DOWN);
        return 0;
    }
    if (key == KEY_TRK) {
        if (!(flags & F_REPEAT))
            trk_held = (unsigned char)(flags & F_DOWN);
        return 0;
    }
    if (flags & F_FUNC)
        func_held = 1;
    t = ui_track();
    if (digiseq_open)
        return t < 0 ? (close_page(), 0) : page_key(t, key, flags);
    /* TRIG on the TRIG page: ours, so the page opens however long TRIG was
     * held; another key while it is down leaves it to that key. */
    if (stock_held) {
        if (key == KEY_TRIG) {
            if ((flags & F_UP) || !(flags & F_DOWN)) {
                stock_held = 0;
                if (!stock_used && t >= 0)
                    open_page();
            }
            return 1;
        }
        if (flags & F_DOWN)
            stock_used = 1;
        return 0;
    }
    if (key == KEY_TRIG && down && t >= 0 && page_view_key(brain) == KEY_TRIG &&
        !(flags & F_FUNC) && !func_held) {  /* FUNC + TRIG: the stock's QUANTIZE */
        stock_held = 1;
        stock_used = 0;
        return 1;
    }
    return 0;
}

static void snake_enc(int t, struct snake *s, int pg, int c, long d)
{
    switch (pg) {
    case PG_NOTES:
        if (func_held) {
            s->note[c] = (signed char)next_stop(s->note[c], d, note_stops, 5);
            POPUP("Note %c=%c%d", 'A' + c, s->note[c] < 0 ? '-' : '+',
                  s->note[c] < 0 ? -s->note[c] : s->note[c]);
        } else {
            s->note[c] = (signed char)clampi(s->note[c] + d, -NOTE_MAX, NOTE_MAX);
        }
        break;
    case PG_GATES:
        if (d > 0)
            s->gate |= (unsigned char)(1u << c);
        else
            s->gate &= (unsigned char)~(1u << c);
        break;
    case PG_VEL:
        if (func_held) {
            s->vel[c] = (unsigned char)next_stop(s->vel[c], d, vel_stops, 5);
            POPUP("Velocity %c=%d", 'A' + c, s->vel[c]);
        } else {
            s->vel[c] = (unsigned char)clampi(s->vel[c] + d, 1, 127);
        }
        break;
    default:
        if (c == P_ON) {
            int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
            set_type(t, ty);
            POPUP("Type=%s", type_names[ty]);
        } else if (func_held && c == P_RANGE) {
            s->set[c] = (unsigned char)next_stop(s->set[c], d, range_stops, 3);
            POPUP("Range=+-%d", s->set[c]);
        } else if (c == P_DIV) {
            s->set[c] = (unsigned char)div_turn(s->set[c], d);
            if (s->set[c] == 0 && !per_track_mode())
                POPUP("2X needs SCALE per track");
            else
                POPUP("Clock=%s", div_name(s->set[c]));
        } else {
            s->set[c] = (unsigned char)clampi(s->set[c] + d, set_min[c], set_max[c]);
        }
        if (c == P_DIR || c == P_LEN || c == P_SHAPE)
            reset_req[t] = 1;
        break;
    }
}

static void st_enc(int t, struct stages *g, int pg, int c, long d)
{
    switch (pg) {
    case PG8_PITCH:
        if (func_held) {
            g->pitch[c] = (signed char)next_stop(g->pitch[c], d, note_stops, 5);
            POPUP("Note %d=%c%d", c + 1, g->pitch[c] < 0 ? '-' : '+',
                  g->pitch[c] < 0 ? -g->pitch[c] : g->pitch[c]);
        } else {
            g->pitch[c] = (signed char)clampi(g->pitch[c] + d, -NOTE_MAX, NOTE_MAX);
        }
        break;
    case PG8_VEL:
        if (func_held) {
            g->vel[c] = (unsigned char)next_stop(g->vel[c], d, vel_stops, 5);
            POPUP("Velocity %d=%d", c + 1, g->vel[c]);
        } else {
            g->vel[c] = (unsigned char)clampi(g->vel[c] + d, 1, 127);
        }
        break;
    case PG8_PULSES:
        g->pulses[c] = (unsigned char)clampi(g->pulses[c] + d, 1, 8);
        break;
    case PG8_MODE:
        g->mode[c] = (unsigned char)clampi(g->mode[c] + d, 0, MODES - 1);
        break;
    case PG8_DIV:
        g->div[c] = (unsigned char)clampi(g->div[c] + d, 1, 4);
        break;
    default:
        if (c == S_TYPE) {
            int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
            set_type(t, ty);
            POPUP("Type=%s", type_names[ty]);
        } else if (func_held && c == S_RANGE) {
            g->set[c] = (unsigned char)next_stop(g->set[c], d, range_stops, 3);
            POPUP("Range=+-%d", g->set[c]);
        } else if (c == S_CLOCK) {
            g->set[c] = (unsigned char)div_turn(g->set[c], d);
            if (g->set[c] == 0 && !per_track_mode())
                POPUP("2X needs SCALE per track");
            else
                POPUP("Clock=%s", div_name(g->set[c]));
        } else {
            g->set[c] = (unsigned char)clampi(g->set[c] + d, sset_min[c], sset_max[c]);
        }
        if (c == S_DIR || c == S_STAGES || c == S_SHIFT)
            reset_req[t] = 1;
        break;
    }
}

static void arp_enc(int t, struct arpset *a, int c, long d)
{
    if (c == A_TYPE) {
        int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
        set_type(t, ty);
        POPUP("Type=%s", type_names[ty]);
        return;
    }
    if (c == A_SPD)                         /* CLOCK: right is faster, as the others' */
        d = -d;
    a->set[c] = (unsigned char)clampi(a->set[c] + d, aset_min[c], aset_max[c]);
    switch (c) {
    case A_MODE: POPUP("Arp Mode=%s", amode_names[a->set[c] % AMODES]); break;
    case A_SPD:  POPUP("Arp Clock=%s", spd_names[a->set[c] % SPDS]); break;
    case A_RNG:
        if (a->set[A_INT] == INT_OCT)
            POPUP("Arp Range=%d Oct", a->set[c]);
        else
            POPUP("Arp Range=%d Steps", a->set[c]);
        break;
    case A_SCALE: POPUP("Arp Scale=%s", scale_names[a->set[c] % SCALES]); break;
    case A_RPT:  POPUP("Arp Repeat=%dX", a->set[c]); break;
    case A_INT:
        if (a->set[c] == INT_OCT)
            POPUP("Arp Interval=Octave");
        else
            POPUP("Arp Interval=+%d", a->set[c]);
        break;
    default:     POPUP("Arp Hold=%s", a->set[c] ? "ON" : "OFF"); break;
    }
}

static void euc_enc(int t, struct euclid *u, int pg, int c, long d)
{
    if (pg == PGU_EUC) {
        int v;
        if (func_held && (c == U_VEL || c == U_AVL))
            v = next_stop(u->set[c], d, vel_stops, 5);
        else if (func_held && c == U_NOTE)
            v = NOTE_MAX + next_stop(u->set[c] - NOTE_MAX, d, note_stops, 5);
        else
            v = clampi(u->set[c] + d, uset_min[c], euc_max(u, c));
        u->set[c] = (unsigned char)v;
        euc_check(u);
        if (c == U_STEPS)
            reset_req[t] = 1;
        if (c == U_NOTE) {
            int o = u->set[c] - NOTE_MAX;
            POPUP("Note=%c%d", o < 0 ? '-' : '+', o < 0 ? -o : o);
        } else {
            POPUP("%s=%d", uset_long[c], u->set[c]);
        }
        return;
    }
    switch (c) {
    case V_TYPE: {
        int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
        set_type(t, ty);
        POPUP("Type=%s", type_names[ty]);
        break;
    }
    case V_CLOCK:
        u->setup[c] = (unsigned char)div_turn(u->setup[c], d);
        if (u->setup[c] == 0 && !per_track_mode())
            POPUP("2X needs SCALE per track");
        else
            POPUP("Clock=%s", div_name(u->setup[c]));
        break;
    case V_LEN:
        u->setup[c] = (unsigned char)clampi(u->setup[c] + d, 0, NLENS - 1);
        POPUP("Note Length=%s", nlen_names[u->setup[c]]);
        break;
    case V_ANOT: {
        int o;
        if (func_held)
            u->setup[c] = (unsigned char)(NOTE_MAX + next_stop(u->setup[c] - NOTE_MAX, d, note_stops, 5));
        else
            u->setup[c] = (unsigned char)clampi(u->setup[c] + d, 0, 2 * NOTE_MAX);
        o = u->setup[c] - NOTE_MAX;
        POPUP("Accent Note=%c%d", o < 0 ? '-' : '+', o < 0 ? -o : o);
        break;
    }
    case V_CHNC:
        u->setup[c] = (unsigned char)clampi(u->setup[c] + (func_held ? 10 : 1) * d, 0, 100);
        POPUP("Hit Chance=%d%%", u->setup[c]);
        break;
    case V_GHST:
        if (func_held)
            u->setup[c] = (unsigned char)next_stop(u->setup[c], d, ghost_stops, 5);
        else
            u->setup[c] = (unsigned char)clampi(u->setup[c] + d, 0, 127);
        if (u->setup[c])
            POPUP("Ghost Velocity=%d", u->setup[c]);
        else
            POPUP("Ghost Notes=OFF");
        break;
    case V_VSPR:
        u->setup[c] = (unsigned char)clampi(u->setup[c] + d, 0, vset_max[V_VSPR]);
        POPUP("Velocity Spread=%d", u->setup[c]);
        break;
    case V_SPIN:
        u->setup[c] = (unsigned char)clampi(u->setup[c] + d, 0, euc_steps(u) - 1);
        POPUP("Spin=%d a cycle", u->setup[c]);
        break;
    default:
        break;
    }
}

static void tur_enc(int t, struct turing *r, int pg, int c, long d)
{
    if (pg == PGR_TUR) {
        int v;
        if (func_held && c == R_VEL)
            v = next_stop(r->set[c], d, vel_stops, 5);
        else if (func_held && c == R_NOTE)
            v = NOTE_MAX + next_stop(r->set[c] - NOTE_MAX, d, note_stops, 5);
        else if (func_held && (c == R_CHG || c == R_DENS))
            v = clampi(r->set[c] + 10 * d, rset_min[c], rset_hi(c));
        else
            v = clampi(r->set[c] + d, rset_min[c], rset_hi(c));
        r->set[c] = (unsigned char)v;
        switch (c) {
        case R_NOTE:
            v -= NOTE_MAX;
            POPUP("Note=%c%d", v < 0 ? '-' : '+', v < 0 ? -v : v);
            break;
        case R_SCALE: POPUP("Scale=%s", scale_names[v % SCALES]); break;
        case R_CHG:
        case R_DENS:  POPUP("%s=%d%%", rset_long[c], v); break;
        default:      POPUP("%s=%d", rset_long[c], v); break;
        }
        return;
    }
    switch (c) {
    case V_TYPE: {
        int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
        set_type(t, ty);
        POPUP("Type=%s", type_names[ty]);
        break;
    }
    case V_CLOCK:
        r->setup[c] = (unsigned char)div_turn(r->setup[c], d);
        if (r->setup[c] == 0 && !per_track_mode())
            POPUP("2X needs SCALE per track");
        else
            POPUP("Clock=%s", div_name(r->setup[c]));
        break;
    case V_LEN:
        r->setup[c] = (unsigned char)clampi(r->setup[c] + d, 0, NLENS - 1);
        POPUP("Note Length=%s", nlen_names[r->setup[c]]);
        break;
    default:
        break;
    }
}

static void grd_enc(int t, struct grids *g, int pg, int c, long d)
{
    if (pg == PGG_GRD) {
        int v;
        if (func_held && (c == G_VEL || c == G_AVL))
            v = next_stop(g->set[c], d, vel_stops, 5);
        else if (func_held && c == G_NOTE)
            v = NOTE_MAX + next_stop(g->set[c] - NOTE_MAX, d, note_stops, 5);
        else if (func_held && c >= G_X && c <= G_CHAOS)
            v = clampi(g->set[c] + 8 * d, gset_min[c], gset_max[c]);
        else
            v = clampi(g->set[c] + d, gset_min[c], gset_max[c]);
        g->set[c] = (unsigned char)v;
        if (c == G_PART)
            POPUP("Part=%s", part_long[v % GRD_PARTS]);
        else if (c == G_NOTE)
            POPUP("Note=%c%d", v < NOTE_MAX ? '-' : '+', v < NOTE_MAX ? NOTE_MAX - v : v - NOTE_MAX);
        else
            POPUP("%s=%d", gset_long[c], v);
        return;
    }
    switch (c) {
    case V_TYPE: {
        int ty = clampi(type_of(t) + d, 0, NTYPES - 1);
        set_type(t, ty);
        POPUP("Type=%s", type_names[ty]);
        break;
    }
    case V_CLOCK:
        g->setup[c] = (unsigned char)div_turn(g->setup[c], d);
        if (g->setup[c] == 0 && !per_track_mode())
            POPUP("2X needs SCALE per track");
        else
            POPUP("Clock=%s", div_name(g->setup[c]));
        break;
    case V_LEN:
        g->setup[c] = (unsigned char)clampi(g->setup[c] + d, 0, NLENS - 1);
        POPUP("Note Length=%s", nlen_names[g->setup[c]]);
        break;
    default:
        break;
    }
}

int digiseq_enc(void *brain, void *ev)
{
    unsigned long e = EV_ENC(ev);
    long d;
    int t, eng, pg;
    digiseq_brain = brain;
    if (!digiseq_open || e < 1 || e > 8)
        return 0;
    t = ui_track();
    if (t < 0)
        return 0;
    enc_acc[e - 1] += EV_DELTA(ev);
    d = enc_acc[e - 1] / COUNTS;
    enc_acc[e - 1] -= d * COUNTS;
    if (d == 0)
        return 1;
    eng = eng_of(t);
    pg = *pg_of(eng);
    if (pg >= pages_of(eng))
        pg = 0;
    redraw = 1;
    if (!digiseq_trk[t].on) {               /* OFF: A is TYPE, the rest do nothing */
        if (e == 1) {
            int ty = clampi(d, 0, NTYPES - 1);
            set_type(t, ty);
            edited(t);
            POPUP("Type=%s", type_names[ty]);
        }
        return 1;
    }
    edited(t);
    if (eng == E_GRD)
        grd_enc(t, &digiseq_grd[t], pg, (int)e - 1, d);
    else if (eng == E_TUR)
        tur_enc(t, &digiseq_tur[t], pg, (int)e - 1, d);
    else if (eng == E_EUC)
        euc_enc(t, &digiseq_euc[t], pg, (int)e - 1, d);
    else if (eng == E_ARP)
        arp_enc(t, &digiseq_arp[t], (int)e - 1, d);
    else if (eng == E_185)
        st_enc(t, &digiseq_trk[t].st, pg, (int)e - 1, d);
    else
        snake_enc(t, &digiseq_snake[t], pg, (int)e - 1, d);
    return 1;
}

/* ==== drawing (ev_tick, ev_draw): 128 x 64, y = 0 the bottom row ========= */
typedef void (*rect_fn)(void *bmp, int x0, int y0, int x1, int y1, int colour);
typedef int (*text_fn)(void *bmp, const void *font, int x, int y, int maxlen, const char *fmt, ...);
#define FILLRECT  ((rect_fn)fw_fillrect)
#define FRAMERECT ((rect_fn)fw_framerect)
#define TEXTF     ((text_fn)fw_textf)
#define FONT5     ((const void *)fw_font5)

static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void digiseq_tick(void *ctrl)
{
    int t;
    frames++;
    if (!data_ready)
        put_together();
    if (TRANSPORT == 0)
        for (t = 0; t < TRACKS; t++)
            rewind[t] = 1;
    if (msg_ticks && !--msg_ticks)
        redraw = 1;
    {                                       /* the sequencer's state, from a page view */
        uint32_t k = page_view_key(digiseq_brain);
        if (k >= KEY_TRIG && k <= KEY_LFO) {
            void *o = popup_obj();
            if (o)
                seq_obj = o;
        }
    }
    {                                       /* a live recording ended: TYPE OFF */
        static unsigned char was_rec;
        int r = rec_live();
        if (was_rec && !r && arp_recorded) {
            uint16_t m = arp_recorded;
            arp_recorded = 0;
            for (t = 0; t < TRACKS; t++)
                if ((m >> t) & 1 && digiseq_trk[t].on) {
                    if (mod_pat[t]) {       /* keep the 2X it was recorded at, as PRINT does */
                        mod_pat[t] = 0;
                        *spd_keep(t, mod_eng[t]) = 0;
                    }
                    set_type(t, 0);
                    POPUP("T%d recorded: Type=OFF", t + 1);
                }
        }
        was_rec = (unsigned char)r;
    }
    speed_sync();
    live_tidy();
    if (digiseq_open) {
        /* a menu over the pages, or another view (a page key we passed on):
         * the page closes */
        if (page_view_key(digiseq_brain) != KEY_TRIG || ui_track() < 0) {
            close_page();
        } else {
            static int last_pos = -1, last_play = -1, last_t = -1;
            int ut = ui_track(), playing = (int)TRANSPORT;
            int e = eng_of(ut);
            int pos = e == E_GRD ? digiseq_gpos[ut] + 64 * digiseq_gpert[ut]
                    : e == E_TUR ? ptur[ut].reg
                    : e == E_EUC ? digiseq_epos[ut] + 32 * digiseq_espin[ut]
                    : e == E_185 ? digiseq_stage[ut] * 8 + digiseq_pulse[ut]
                    : e == E_ARP ? parp[ut].active : digiseq_cell[ut];
            if (pos != last_pos || playing != last_play || ut != last_t) {
                last_pos = pos;
                last_play = playing;
                last_t = ut;
                redraw = 1;
            }
        }
    }
    if (redraw) {
        redraw = 0;
        *((unsigned char *)ctrl + 0x20) = 1;
    }
}

static int path_order(const struct snake *s, int cell)
{
    int len = s->set[P_LEN], k;
    for (k = 0; k < len; k++)
        if (cell_of(s, k, len) == cell)
            return k + 1;
    return 0;
}

static void draw_note(void *bmp, int x0, int y0, int n, int off)
{
    n = clamp_note(n);
    TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "%s%d", note_names[n % 12], n / 12);
    TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%c%d", off < 0 ? '-' : '+', off < 0 ? -off : off);
}

static void draw_vel(void *bmp, int x0, int y0, int v)
{
    int h = (v * 14) / 127;
    FILLRECT(bmp, x0 + 3, y0 + 4, x0 + 6, y0 + 4 + h, 1);
    TEXTF(bmp, FONT5, x0 + 10, y0 + 8, -1, "%d", v);
}

/* 185: a stage's pulses as a row of ticks under a mode: ONE a tick on the
 * first, ALL on every DIV-th, HOLD a bar across, OFF dots */
static void draw_pulses(void *bmp, int x0, int y0, const struct stages *g, int s, int cur)
{
    int n = g->pulses[s], m = g->mode[s], dv = g->div[s], i;
    int w = 3, x = x0 + 3;                  /* a pulse a slot: the stage's length shows */
    for (i = 0; i < n; i++) {
        int xl = x + i * w, xr = xl + 1;
        int hit = m == M_ALL ? (i % dv == 0) : (m == M_ONE || m == M_HOLD) ? i == 0 : 0;
        if (i == cur)
            FILLRECT(bmp, xl, y0 + 2, xr, y0 + 2, 1);       /* the playing pulse */
        if (m == M_HOLD) {
            FILLRECT(bmp, xl, y0 + 5, xr + (i < n - 1 ? 2 : 0), y0 + 7, 1);
            if (i == 0)
                FILLRECT(bmp, xl, y0 + 5, xl, y0 + 10, 1);
        } else if (hit) {
            FILLRECT(bmp, xl, y0 + 4, xr, y0 + 10, 1);
        } else {
            FILLRECT(bmp, xl, y0 + 5, xl, y0 + 5, 1);
        }
    }
}

static void draw_snake(void *bmp, int t, int base, int root)
{
    const struct snake *s = &digiseq_snake[t];
    int pg = digiseq_pg_snake % SNAKE_PAGES, c;
    int playing = digiseq_trk[t].on && TRANSPORT == 1;
    for (c = 0; c < CELLS; c++) {
        int x0 = 32 * (c & 3), y0 = (c < 4) ? 31 : 8;
        int x1 = x0 + 30, y1 = y0 + 21, order;
        FRAMERECT(bmp, x0, y0, x1, y1, 1);
        if (pg == PG_PATH) {
            int v = s->set[c];
            TEXTF(bmp, FONT5, x0 + 3, y0 + 14, -1, "%s", set_labels[c]);
            switch (c) {
            case P_ON:    TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", type_names[type_of(t)]); break;
            case P_SHAPE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", shape_names[v % SHAPES]); break;
            case P_DIR:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", dir_names[v % DIRS]); break;
            case P_SCALE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", scale_names[v % SCALES]); break;
            case P_RANGE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "+-%d", v); break;
            case P_DIV:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", div_name(v)); break;
            default:      TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%d", v); break;
            }
            continue;
        }
        order = path_order(s, c);
        if (order)
            TEXTF(bmp, FONT5, x1 - 6, y1 - 6, -1, "%d", order);
        if (playing && digiseq_cell[t] == c) {
            FRAMERECT(bmp, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);
            FRAMERECT(bmp, x0 + 2, y0 + 2, x1 - 2, y1 - 2, 1);
        }
        if (pg == PG_NOTES) {
            draw_note(bmp, x0, y0, quantise(base + s->note[c], root, s->set[P_SCALE]), s->note[c]);
        } else if (pg == PG_GATES) {
            if ((s->gate >> c) & 1)
                FILLRECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
            else
                FRAMERECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
        } else {
            draw_vel(bmp, x0, y0, s->vel[c]);
        }
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", snake_pages[pg], pg + 1, SNAKE_PAGES);
}

static int in_sequence(const struct stages *g, int s)
{
    int k;
    for (k = 0; k < stages_n(g); k++)
        if (stage_of(g, k) == s)
            return 1;
    return 0;
}

static int total_steps(const struct stages *g)
{
    int k, n = 0;
    for (k = 0; k < stages_n(g); k++)
        n += g->pulses[stage_of(g, k)];
    return n * steps_of(g->set[S_CLOCK]);
}

static void draw_185(void *bmp, int t, int base, int root)
{
    const struct stages *g = &digiseq_trk[t].st;
    int pg = digiseq_pg_185 % PAGES_185, c;
    int playing = digiseq_trk[t].on && TRANSPORT == 1;
    for (c = 0; c < CELLS; c++) {
        int x0 = 32 * (c & 3), y0 = (c < 4) ? 31 : 8;
        int x1 = x0 + 30, y1 = y0 + 21;
        int cur = playing && digiseq_stage[t] == c;
        if (pg == PG8_SETUP) {
            int v = g->set[c];
            FRAMERECT(bmp, x0, y0, x1, y1, 1);
            TEXTF(bmp, FONT5, x0 + 3, y0 + 14, -1, "%s", sset_labels[c]);
            switch (c) {
            case S_TYPE:  TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", type_names[type_of(t)]); break;
            case S_DIR:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", dir_names[v % DIRS]); break;
            case S_CLOCK: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", div_name(v)); break;
            case S_SCALE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", scale_names[v % SCALES]); break;
            case S_RANGE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "+-%d", v); break;
            case S_NLEN:  TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", nlen_names[v % NLENS]); break;
            default:      TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%d", v); break;
            }
            continue;
        }
        if (in_sequence(g, c)) {
            FRAMERECT(bmp, x0, y0, x1, y1, 1);
            TEXTF(bmp, FONT5, x1 - 6, y1 - 6, -1, "%d", c + 1);
        } else {                            /* past STAGES: corners only */
            FILLRECT(bmp, x0, y0, x0 + 2, y0, 1);
            FILLRECT(bmp, x1 - 2, y0, x1, y0, 1);
            FILLRECT(bmp, x0, y1, x0 + 2, y1, 1);
            FILLRECT(bmp, x1 - 2, y1, x1, y1, 1);
        }
        if (cur) {
            FRAMERECT(bmp, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);
            FRAMERECT(bmp, x0 + 2, y0 + 2, x1 - 2, y1 - 2, 1);
        }
        switch (pg) {
        case PG8_PITCH:
            draw_note(bmp, x0, y0, quantise(base + g->pitch[c], root, g->set[S_SCALE]), g->pitch[c]);
            break;
        case PG8_VEL:
            draw_vel(bmp, x0, y0, g->vel[c]);
            break;
        case PG8_PULSES:
            TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "%d", g->pulses[c]);
            draw_pulses(bmp, x0, y0, g, c, cur ? digiseq_pulse[t] : -1);
            break;
        case PG8_MODE:
            TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "%s", mode_names[g->mode[c] % MODES]);
            draw_pulses(bmp, x0, y0, g, c, cur ? digiseq_pulse[t] : -1);
            break;
        default:                            /* DIV */
            if (g->mode[c] == M_ALL)
                TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "1/%d", g->div[c]);
            else
                TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "(1/%d)", g->div[c]);
            draw_pulses(bmp, x0, y0, g, c, cur ? digiseq_pulse[t] : -1);
            break;
        }
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", pages185[pg], pg + 1, PAGES_185);
}

/* A knob's box, as SNAKE's PATH page: knob c (A-D the top row, E-H the
 * bottom), its label above, its value (drawn by the caller at *vx, *vy)
 * below. mini: the shorter boxes under a row of steps (0.12). */
static void knob_box(void *bmp, int c, int mini, const char *label, int *vx, int *vy)
{
    int x0 = 32 * (c & 3), y0 = mini ? (c < 4 ? 23 : 7) : (c < 4 ? 31 : 8);
    FRAMERECT(bmp, x0, y0, x0 + 30, y0 + (mini ? 14 : 21), 1);
    TEXTF(bmp, FONT5, x0 + 3, y0 + (mini ? 8 : 14), -1, "%s", label);
    *vx = x0 + 3;
    *vy = y0 + (mini ? 2 : 4);
}

static void draw_offset(void *bmp, int x, int y, int v)    /* NOTE_MAX + semitones */
{
    TEXTF(bmp, FONT5, x, y, -1, "%c%d", v < NOTE_MAX ? '-' : '+', v < NOTE_MAX ? NOTE_MAX - v : v - NOTE_MAX);
}

/* A row of steps, 8 pixels a step as the trig keys (EUCLID, TURING, GRIDS):
 * kind 0 past the pattern (a dot), 1 a rest (a frame), 2 a hit (filled), 3
 * an accent (filled, taller); the step playing underlined. y: the row's
 * foot, h: a step's height, ha: an accent's. */
static void draw_step(void *bmp, int c, int y, int h, int ha, int kind, int cur)
{
    int x0 = 8 * c;
    if (kind == 0)
        FILLRECT(bmp, x0 + 3, y + h / 2 - 1, x0 + 4, y + h / 2, 1);
    else if (kind == 1)
        FRAMERECT(bmp, x0 + 1, y, x0 + 6, y + h - 1, 1);
    else
        FILLRECT(bmp, x0 + 1, y, x0 + 6, y + (kind == 3 ? ha : h) - 1, 1);
    if (cur)
        FILLRECT(bmp, x0 + 1, y - 2, x0 + 6, y - 2, 1);
}

/* SETUP for EUCLID, TURING and GRIDS: TYPE (A), LEN (B) and CLOCK (E) as
 * SNAKE's TYPE and DIV; EUCLID's extras on the rest. */
static void draw_setup(void *bmp, int t, const unsigned char *setup, int all)
{
    int c, x, y;
    for (c = 0; c < NVSETS; c++) {
        int v = setup[c];
        if (!all && c != V_TYPE && c != V_LEN && c != V_CLOCK)
            continue;
        knob_box(bmp, c, 0, vset_labels[c], &x, &y);
        switch (c) {
        case V_TYPE:  TEXTF(bmp, FONT5, x, y, -1, "%s", type_names[type_of(t)]); break;
        case V_CLOCK: TEXTF(bmp, FONT5, x, y, -1, "%s", div_name(v)); break;
        case V_LEN:   TEXTF(bmp, FONT5, x, y, -1, "%s", nlen_names[v % NLENS]); break;
        case V_ANOT:  draw_offset(bmp, x, y, v); break;
        case V_CHNC:  TEXTF(bmp, FONT5, x, y, -1, "%d%%", v); break;
        case V_GHST:
            if (v)
                TEXTF(bmp, FONT5, x, y, -1, "%d", v);
            else
                TEXTF(bmp, FONT5, x, y, -1, "OFF");
            break;
        default:      TEXTF(bmp, FONT5, x, y, -1, "%d", v); break;
        }
    }
}

/* ARP: the settings, a box a knob, as SNAKE's PATH page */
static void draw_arp(void *bmp, int t)
{
    const struct arpset *a = &digiseq_arp[t];
    int c, x, y;
    for (c = 0; c < NASETS; c++) {
        int v = a->set[c];
        knob_box(bmp, c, 0, aset_labels[c], &x, &y);
        switch (c) {
        case A_TYPE: TEXTF(bmp, FONT5, x, y, -1, "%s", type_names[type_of(t)]); break;
        case A_MODE: TEXTF(bmp, FONT5, x, y, -1, "%s", amode_names[v % AMODES]); break;
        case A_SPD:  TEXTF(bmp, FONT5, x, y, -1, "%s", spd_names[v % SPDS]); break;
        case A_RNG:
            if (a->set[A_INT] == INT_OCT)
                TEXTF(bmp, FONT5, x, y, -1, "%d OCT", v);
            else
                TEXTF(bmp, FONT5, x, y, -1, "%d STEP", v);
            break;
        case A_SCALE: TEXTF(bmp, FONT5, x, y, -1, "%s", scale_names[v % SCALES]); break;
        case A_RPT:  TEXTF(bmp, FONT5, x, y, -1, "%dX", v); break;
        case A_INT:
            if (v == INT_OCT)
                TEXTF(bmp, FONT5, x, y, -1, "OCT");
            else
                TEXTF(bmp, FONT5, x, y, -1, "+%d", v);
            break;
        default:     TEXTF(bmp, FONT5, x, y, -1, "%s", v ? "ON" : "OFF"); break;
        }
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", arp_pages[0], 1, ARP_PAGES);
}

/* EUCLID. EUCLID: the pattern as a row of STEPS steps over the trig keys
 * (as it plays: SPIN turns it), a hit filled, an accent taller, a rest a
 * frame, past STEPS a dot, the step playing underlined; under it the knobs'
 * boxes. SETUP: as SNAKE's PATH page. */
static void draw_euc(void *bmp, int t)
{
    const struct euclid *u = &digiseq_euc[t];
    int pg = digiseq_pg_euc % EUC_PAGES, c, n = euc_steps(u), x, y;
    if (pg == PGU_EUC) {
        int playing = digiseq_trk[t].on && TRANSPORT == 1;
        int cur = playing ? digiseq_epos[t] : -1, spin = playing ? digiseq_espin[t] : 0;
        for (c = 0; c < EUC_MAX; c++) {
            int h = c < n ? euc_hit(u, c, spin) : -1;
            draw_step(bmp, c, 41, 8, 12, c >= n ? 0 : h < 0 ? 1 : euc_accent(u, h) ? 3 : 2, c == cur);
        }
        for (c = 0; c < NUSETS; c++) {
            knob_box(bmp, c, 1, uset_labels[c], &x, &y);
            if (c == U_NOTE)
                draw_offset(bmp, x, y, u->set[c]);
            else
                TEXTF(bmp, FONT5, x, y, -1, "%d", u->set[c]);
        }
    } else {
        draw_setup(bmp, t, u->setup, 1);
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", euc_pages[pg], pg + 1, EUC_PAGES);
}

/* TURING. TURING: the register's loop, its LEN bits from bit 0 (the newest)
 * on the left, a 1 filled, a 0 framed, past LEN a dot; the bit that comes
 * round next underlined; under it the knobs' boxes (the note it plays now
 * is on the title line). SETUP: TYPE, LEN, CLOCK. */
static void draw_tur(void *bmp, int t)
{
    const struct turing *r = &digiseq_tur[t];
    int pg = digiseq_pg_tur % TUR_PAGES, c, n = tur_len(r), x, y;
    uint16_t reg = digiseq_trk[t].on && TRANSPORT == 1 ? ptur[t].reg : r->reg;
    if (pg == PGR_TUR) {
        for (c = 0; c < 16; c++)
            draw_step(bmp, c, 41, 10, 10, c >= n ? 0 : (reg >> c) & 1 ? 2 : 1, c == n - 1);
        for (c = 0; c < NRSETS; c++) {
            int v = r->set[c];
            knob_box(bmp, c, 1, rset_labels[c], &x, &y);
            switch (c) {
            case R_NOTE:  draw_offset(bmp, x, y, v); break;
            case R_SCALE: TEXTF(bmp, FONT5, x, y, -1, "%s", scale_names[v % SCALES]); break;
            case R_CHG:
            case R_DENS:  TEXTF(bmp, FONT5, x, y, -1, "%d%%", v); break;
            default:      TEXTF(bmp, FONT5, x, y, -1, "%d", v); break;
            }
        }
    } else {
        draw_setup(bmp, t, r->setup, 0);
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", tur_pages[pg], pg + 1, TUR_PAGES);
}

/* GRIDS. GRIDS: the part's 32 steps as EUCLID's, in two rows over the trig
 * keys (1-16 the top, 17-32 under it); under them the knobs' boxes. SETUP:
 * TYPE, LEN, CLOCK. */
static void draw_grd(void *bmp, int t)
{
    const struct grids *g = &digiseq_grd[t];
    int pg = digiseq_pg_grd % GRD_PAGES, c, x, y;
    if (pg == PGG_GRD) {
        int playing = digiseq_trk[t].on && TRANSPORT == 1;
        int cur = playing ? digiseq_gpos[t] : -1, pert = playing ? digiseq_gpert[t] : 0;
        for (c = 0; c < GRD_STEPS; c++) {
            int h = grd_hit(g, c, pert);
            draw_step(bmp, c & 15, c < 16 ? 48 : 41, 3, 4, h > 1 ? 3 : h ? 2 : 1, c == cur);
        }
        for (c = 0; c < NGSETS; c++) {
            int v = g->set[c];
            knob_box(bmp, c, 1, gset_labels[c], &x, &y);
            if (c == G_PART)
                TEXTF(bmp, FONT5, x, y, -1, "%s", part_names[v % GRD_PARTS]);
            else if (c == G_NOTE)
                draw_offset(bmp, x, y, v);
            else
                TEXTF(bmp, FONT5, x, y, -1, "%d", v);
        }
    } else {
        draw_setup(bmp, t, g->setup, 0);
    }
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/%d", grd_pages[pg], pg + 1, GRD_PAGES);
}

/* TYPE OFF (0.12): TYPE alone, on A */
static void draw_off(void *bmp, int t)
{
    int x, y;
    knob_box(bmp, 0, 0, "TYPE", &x, &y);
    TEXTF(bmp, FONT5, x, y, -1, "%s", type_names[type_of(t)]);
    TEXTF(bmp, FONT5, 36, 44, -1, "TURN A FOR A TYPE");
}

/* The stock drop-down is drawn in the title bar (rows 54-63) before the page:
 * while one shows, the page keeps those rows as the stock drew them. The
 * bitmap: +4 width, +8 height, +12 longs a column, +16 the pixels, 128
 * columns of two longs, the second's bits 9..0 rows 54..63 (as PUMP reads it). */
#define TITLE_BITS 0x3ffu
static uint32_t title_save[128];
static int title_keep(void *bmp, int restore)
{
    char *b = (char *)bmp;
    uint32_t *px;
    int x;
    if (*(int32_t *)(b + 4) != 128 || *(int32_t *)(b + 8) != 64 || *(int32_t *)(b + 12) != 2 ||
        !(px = *(uint32_t **)(b + 16)))
        return 0;
    for (x = 0; x < 128; x++) {
        if (restore)
            px[2 * x + 1] = (px[2 * x + 1] & ~TITLE_BITS) | title_save[x];
        else
            title_save[x] = px[2 * x + 1] & TITLE_BITS;
    }
    return 1;
}

void digiseq_draw(void *bmp, void *ctrl)
{
    int t, base = 60, root, kept = 0;
    struct sq_track info;
    (void)ctrl;
    if (!digiseq_open)
        return;
    t = ui_track();
    if (msg_stock && msg_ticks)
        kept = title_keep(bmp, 0);
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    if (t < 0)
        return;
    if (sq_track_info(t, &info, sizeof info) == 0)
        base = info.note;
    root = base % 12;
    /* the title line: the track, its type, the page */
    if (!digiseq_trk[t].on) {
        draw_off(bmp, t);
        TEXTF(bmp, FONT5, 1, 57, -1, "%s%d OFF", t < SQ_AUDIO ? "T" : "MIDI ", (t & 7) + 1);
    } else if (eng_of(t) == E_GRD) {
        draw_grd(bmp, t);
        TEXTF(bmp, FONT5, 1, 57, -1, "%s%d GRIDS %s", t < SQ_AUDIO ? "T" : "M", (t & 7) + 1,
              part_names[digiseq_grd[t].set[G_PART] % GRD_PARTS]);
    } else if (eng_of(t) == E_TUR) {
        const struct turing *r = &digiseq_tur[t];
        uint16_t reg = digiseq_trk[t].on && TRANSPORT == 1 ? ptur[t].reg : r->reg;
        int nn = clamp_note(tur_note(r, reg, base, root));
        draw_tur(bmp, t);
        if (tur_fires(r, reg))              /* the note the register plays now */
            TEXTF(bmp, FONT5, 1, 57, -1, "%s%d TURING %s%d", t < SQ_AUDIO ? "T" : "M", (t & 7) + 1,
                  note_names[nn % 12], nn / 12);
        else
            TEXTF(bmp, FONT5, 1, 57, -1, "%s%d TURING REST", t < SQ_AUDIO ? "T" : "M", (t & 7) + 1);
    } else if (eng_of(t) == E_EUC) {
        const struct euclid *u = &digiseq_euc[t];
        draw_euc(bmp, t);
        TEXTF(bmp, FONT5, 1, 57, -1, "%s%d E(%d,%d)", t < SQ_AUDIO ? "T" : "M", (t & 7) + 1,
              u->set[U_HITS], euc_steps(u));
    } else if (eng_of(t) == E_ARP) {
        draw_arp(bmp, t);
        if (t < SQ_AUDIO && live[t].n)          /* the keys held */
            TEXTF(bmp, FONT5, 1, 57, -1, "T%d ARP KB%d", t + 1, live[t].n);
        else
            TEXTF(bmp, FONT5, 1, 57, -1, "%s%d ARP", t < SQ_AUDIO ? "T" : "MIDI ", (t & 7) + 1);
    } else if (eng_of(t) == E_185) {
        draw_185(bmp, t, base, root);
        TEXTF(bmp, FONT5, 1, 57, -1, "%s%d 185 %dST", t < SQ_AUDIO ? "T" : "M", (t & 7) + 1,
              total_steps(&digiseq_trk[t].st));
    } else {
        draw_snake(bmp, t, base, root);
        TEXTF(bmp, FONT5, 1, 57, -1, "%s%d SNAKE", t < SQ_AUDIO ? "T" : "MIDI ", (t & 7) + 1);
    }
    FILLRECT(bmp, 0, 54, 127, 54, 1);
    if (kept)
        title_keep(bmp, 1);                 /* the drop-down, over the title */
    if (msg_ticks && msgbuf[0] && !msg_stock)
        TEXTF(bmp, FONT5, 1, 1, -1, "%s", msgbuf);
    else if (!digiseq_trk[t].on)
        TEXTF(bmp, FONT5, 1, 1, -1, "TYPE OFF        TRIG BACK");
    else if (pages_of(eng_of(t)) < 2)
        TEXTF(bmp, FONT5, 1, 1, -1, "TYPE %s       TRIG BACK", type_names[type_of(t)]);
    else
        TEXTF(bmp, FONT5, 1, 1, -1, "TYPE %s  <> PAGE  TRIG BACK", type_names[type_of(t)]);
}
