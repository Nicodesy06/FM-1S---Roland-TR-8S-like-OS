/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* PULSAR-8 drum voices.
 *
 * Every track owns one monophonic voice, as each instrument of a TR is one circuit: a new hit
 * restarts it (with a 1.5 ms declick of the old sound), and the open hi-hat can be choked by
 * the closed one. A voice is either
 *
 *   SYN  an analogue-style model in fixed point: a tone (sine / triangle / square) with an
 *        exponential pitch drop and a second partial, a "metal" source (six square waves at
 *        the TR-808 cymbal / hi-hat frequencies 205.3, 304.4, 369.6, 522.7, 540, 800 Hz), white
 *        noise or a clap envelope (three bursts then a tail), a click, a resonant zero-delay-
 *        feedback state-variable filter (LP / BP / HP, on the noise or on everything), drive,
 *        and an optional bit / sample-rate reduction (the 6/8-bit digital machines);
 *   PCM  a mu-law recording played the way the early sample drum machines did: a fixed sample
 *        clock, no interpolation (zero-order hold, so the pitch knob changes the aliasing), a
 *        machine-specific bit depth and output filter, and a decay VCA.
 *
 * The TR-8S knob set drives every model: TUNE, DECAY, CTRL (what it does depends on the model:
 * ATTACK, SNAPPY, TONE, PITCH, COLOR, DRIVE or TAIL) and LEVEL, plus PAN and two sends. */
#include "pz_tables.h"
#include "pz_samples.h"

#ifndef PZ_POOL
#define PZ_POOL
#endif

enum { W_NONE, W_SINE, W_TRI, W_SQR };
enum { N_NONE, N_WHITE, N_METAL, N_METALW, N_CLAP };
enum { F_OFF, F_LP, F_BP, F_HP };
#define F_ALL 0x10u
enum { CT_ATTACK, CT_SNAPPY, CT_TONE, CT_PITCH, CT_COLOR, CT_DRIVE, CT_TAIL, CT_N };
static const char *const CT_NAME[CT_N] = {"ATTACK", "SNAPPY", "TONE", "PITCH", "COLOR", "DRIVE", "TAIL"};

typedef struct {
    uint16_t freq;               /* tone frequency, Hz */
    uint8_t wave;                /* W_* */
    uint8_t pe_amt, pe_dec;      /* pitch drop: 1/4 semitones at the hit, DECAY_K index */
    uint8_t hold, a_dec, o_lvl;  /* tone: hold (ms), DECAY_K index, level */
    uint8_t o2, o2_lvl;          /* second partial: ratio x64 (0 none), level; decays 1.6x faster */
    uint8_t src;                 /* N_* */
    uint8_t n_lvl, n_hold, n_dec;
    uint8_t flt;                 /* F_* | F_ALL */
    uint8_t cut, res, hp;        /* cutoff index, resonance 0..127, noise high-pass index (0 off) */
    int8_t fenv;                 /* cutoff offset at the hit, follows the pitch envelope */
    uint8_t mt;                  /* metal frequency scale x64 (64 = 808) */
    uint8_t ck, ck_dec;          /* click level, decay index */
    uint8_t drive;               /* 0..127 */
    uint8_t crush;               /* low nibble: bits dropped of 16; high nibble: sample-and-hold - 1 */
    uint8_t ctrl;                /* CT_* */
    uint8_t tune;                /* semitones for a full TUNE turn (each side) */
    uint8_t lvl;                 /* trim, 64 = unity (set by the level calibration of host/audio_test.c) */
} syn_t;

typedef struct {
    const char *name;
    uint8_t zoh;                 /* 1: zero-order hold (no interpolation) */
    uint8_t bitdrop;             /* bits dropped after decoding (16-bit scale) */
    uint8_t lp;                  /* output low-pass cutoff index (the machine's reconstruction filter) */
    uint8_t drive;
} mach_t;
enum { MA_LM, MA_DMX, MA_DRMU, MA_SP12, MA_CLEAN, MA_N };
static const mach_t MACH[MA_N] = {
    {"LM", 1, 0, 113, 0},        /* LinnDrum-style: 8-bit companded, bright */
    {"DMX", 1, 1, 108, 8},       /* DMX-style: 8-bit companded, a little darker, punchy */
    {"DRMU", 1, 3, 104, 0},      /* Drumulator-style: coarser */
    {"SP12", 1, 4, 116, 24},     /* SP-12-style: 12-bit grit, driven */
    {"CLEAN", 0, 0, 127, 0},     /* clean playback (interpolated) */
};

typedef struct {
    uint8_t smp;                 /* SMP_* */
    uint8_t mach;                /* MA_* */
    int8_t semi;                 /* the machine's default tuning */
    uint8_t dec;                 /* default decay character: 0 = natural, else DECAY_K index */
    uint8_t lvl;
} pcm_t;

enum { K_SYN, K_PCM };
typedef struct {
    const char *name;            /* up to 10 characters */
    char lbl[3];                 /* two-letter track label */
    uint8_t cat;                 /* CAT_* */
    uint8_t kind;                /* K_* */
    uint8_t idx;                 /* into SYN[] or PCMI[] */
    uint8_t alt;                 /* ALT INST: index into INST[] (0xFF = itself, retuned) */
} inst_t;

/* ---------------------------------------------------------------- models --- */
/* index helpers: decay index i -> 1.5 ms * 2^(i / 10); cutoff index i -> 20 Hz * 900^(i / 127) */
#define D(ms) ((uint8_t)(ms))    /* (documentation only) */
#define S(...) {__VA_ARGS__}

enum {
    Y_BD808, Y_BD909, Y_BD606, Y_BDCR, Y_BD7X7, Y_BDLONG, Y_BDDIST, Y_BDFM,
    Y_SD808, Y_SD909, Y_SD606, Y_SDCR, Y_SD7X7, Y_SDNZ,
    Y_LT808, Y_HT808, Y_LT909, Y_HT909, Y_LT606, Y_HT606, Y_LT7X7, Y_HT7X7,
    Y_RS808, Y_RS909, Y_RS7X7,
    Y_CP808, Y_CP909, Y_CP7X7,
    Y_CH808, Y_OH808, Y_CH909, Y_OH909, Y_CH606, Y_OH606, Y_CHCR, Y_OHCR, Y_CH7X7, Y_OH7X7,
    Y_CY808, Y_CY909, Y_RD909, Y_CY606, Y_CYCR,
    Y_CB808, Y_CL808, Y_MA808, Y_LC808, Y_HC808, Y_CBCR, Y_GUCR, Y_BGCR, Y_TB7X7, Y_BLIP, Y_ZAP,
    Y_N
};

static const syn_t SYN[Y_N] = {
    /* ---- bass drums ---- */
    [Y_BD808] = {.freq = 50, .wave = W_SINE, .pe_amt = 22, .pe_dec = 40, .a_dec = 86, .o_lvl = 127,
                 .ck = 40, .ck_dec = 8, .flt = F_LP, .cut = 100, .ctrl = CT_ATTACK, .tune = 7, .lvl = 50},
    [Y_BD909] = {.freq = 52, .wave = W_SINE, .pe_amt = 84, .pe_dec = 36, .hold = 4, .a_dec = 78, .o_lvl = 127,
                 .src = N_WHITE, .n_lvl = 30, .n_dec = 14, .flt = F_LP, .cut = 112, .ck = 90, .ck_dec = 9,
                 .drive = 34, .ctrl = CT_ATTACK, .tune = 7, .lvl = 63},
    [Y_BD606] = {.freq = 60, .wave = W_SINE, .pe_amt = 14, .pe_dec = 34, .a_dec = 70, .o_lvl = 127,
                 .ck = 50, .ck_dec = 6, .ctrl = CT_ATTACK, .tune = 7, .lvl = 54},
    [Y_BDCR] = {.freq = 64, .wave = W_SINE, .pe_amt = 6, .pe_dec = 30, .a_dec = 66, .o_lvl = 127,
                .ck = 20, .ck_dec = 5, .ctrl = CT_COLOR, .o2 = 96, .o2_lvl = 10, .tune = 7, .lvl = 54},
    [Y_BD7X7] = {.freq = 54, .wave = W_SINE, .pe_amt = 52, .pe_dec = 38, .hold = 10, .a_dec = 74, .o_lvl = 127,
                 .ck = 70, .ck_dec = 7, .drive = 20, .crush = 0x18, .ctrl = CT_ATTACK, .tune = 7, .lvl = 63},
    [Y_BDLONG] = {.freq = 45, .wave = W_SINE, .pe_amt = 20, .pe_dec = 42, .a_dec = 104, .o_lvl = 127,
                  .ck = 30, .ck_dec = 8, .drive = 16, .ctrl = CT_DRIVE, .tune = 12, .lvl = 63},
    [Y_BDDIST] = {.freq = 50, .wave = W_SINE, .pe_amt = 96, .pe_dec = 38, .hold = 6, .a_dec = 82, .o_lvl = 127,
                  .ck = 100, .ck_dec = 9, .drive = 110, .ctrl = CT_DRIVE, .tune = 7, .lvl = 63},
    [Y_BDFM] = {.freq = 48, .wave = W_SINE, .pe_amt = 64, .pe_dec = 44, .a_dec = 80, .o_lvl = 127,
                .o2 = 90, .o2_lvl = 60, .ck = 60, .ck_dec = 7, .drive = 40, .ctrl = CT_COLOR, .tune = 7, .lvl = 63},
    /* ---- snares ---- */
    [Y_SD808] = {.freq = 238, .wave = W_SINE, .pe_amt = 8, .pe_dec = 30, .a_dec = 60, .o_lvl = 92,
                 .o2 = 128, .o2_lvl = 64, .src = N_WHITE, .n_lvl = 92, .n_dec = 68, .flt = F_BP, .cut = 101,
                 .res = 0, .hp = 80, .ck = 20, .ck_dec = 4, .ctrl = CT_SNAPPY, .tune = 7, .lvl = 37},
    [Y_SD909] = {.freq = 182, .wave = W_TRI, .pe_amt = 20, .pe_dec = 34, .a_dec = 62, .o_lvl = 100,
                 .o2 = 116, .o2_lvl = 70, .src = N_WHITE, .n_lvl = 100, .n_dec = 72, .flt = F_LP, .cut = 109,
                 .res = 12, .hp = 61, .ck = 40, .ck_dec = 5, .drive = 20, .ctrl = CT_SNAPPY, .tune = 7, .lvl = 57},
    [Y_SD606] = {.freq = 200, .wave = W_SINE, .pe_amt = 10, .pe_dec = 28, .a_dec = 52, .o_lvl = 80,
                 .o2 = 96, .o2_lvl = 40, .src = N_WHITE, .n_lvl = 100, .n_dec = 60, .flt = F_BP, .cut = 104, .res = 0, .hp = 76,
                 .ctrl = CT_SNAPPY, .tune = 7, .lvl = 38},
    [Y_SDCR] = {.freq = 220, .wave = W_SINE, .pe_amt = 0, .a_dec = 48, .o_lvl = 50,
                .src = N_WHITE, .n_lvl = 100, .n_dec = 58, .flt = F_BP, .cut = 98, .res = 30,
                .ctrl = CT_TONE, .tune = 7, .lvl = 77},
    [Y_SD7X7] = {.freq = 190, .wave = W_TRI, .pe_amt = 16, .pe_dec = 30, .a_dec = 58, .o_lvl = 96,
                 .o2 = 120, .o2_lvl = 50, .src = N_WHITE, .n_lvl = 104, .n_dec = 66, .flt = F_LP, .cut = 104,
                 .hp = 70, .crush = 0x18, .ctrl = CT_SNAPPY, .tune = 7, .lvl = 60},
    [Y_SDNZ] = {.src = N_WHITE, .n_lvl = 127, .n_hold = 8, .n_dec = 62, .flt = F_BP, .cut = 92, .res = 50,
                .fenv = 20, .pe_dec = 40, .ck = 60, .ck_dec = 5, .ctrl = CT_TONE, .tune = 7, .lvl = 67},
    /* ---- toms ---- */
    [Y_LT808] = {.freq = 95, .wave = W_SINE, .pe_amt = 12, .pe_dec = 46, .a_dec = 82, .o_lvl = 127,
                 .src = N_WHITE, .n_lvl = 10, .n_dec = 40, .flt = F_LP, .cut = 80,
                 .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    [Y_HT808] = {.freq = 165, .wave = W_SINE, .pe_amt = 12, .pe_dec = 44, .a_dec = 78, .o_lvl = 127,
                 .src = N_WHITE, .n_lvl = 10, .n_dec = 38, .flt = F_LP, .cut = 86,
                 .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    [Y_LT909] = {.freq = 90, .wave = W_SINE, .pe_amt = 30, .pe_dec = 44, .a_dec = 78, .o_lvl = 120,
                 .src = N_WHITE, .n_lvl = 34, .n_dec = 44, .flt = F_LP, .cut = 96, .ck = 50, .ck_dec = 6,
                 .drive = 20, .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    [Y_HT909] = {.freq = 150, .wave = W_SINE, .pe_amt = 30, .pe_dec = 42, .a_dec = 74, .o_lvl = 120,
                 .src = N_WHITE, .n_lvl = 34, .n_dec = 42, .flt = F_LP, .cut = 100, .ck = 50, .ck_dec = 6,
                 .drive = 20, .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    [Y_LT606] = {.freq = 110, .wave = W_SINE, .pe_amt = 8, .pe_dec = 40, .a_dec = 68, .o_lvl = 127,
                 .ck = 30, .ck_dec = 5, .ctrl = CT_PITCH, .tune = 12, .lvl = 48},
    [Y_HT606] = {.freq = 180, .wave = W_SINE, .pe_amt = 8, .pe_dec = 38, .a_dec = 64, .o_lvl = 127,
                 .ck = 30, .ck_dec = 5, .ctrl = CT_PITCH, .tune = 12, .lvl = 49},
    [Y_LT7X7] = {.freq = 100, .wave = W_SINE, .pe_amt = 24, .pe_dec = 44, .a_dec = 74, .o_lvl = 127,
                 .src = N_WHITE, .n_lvl = 20, .n_dec = 36, .flt = F_LP, .cut = 90,
                 .crush = 0x18, .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    [Y_HT7X7] = {.freq = 160, .wave = W_SINE, .pe_amt = 24, .pe_dec = 42, .a_dec = 70, .o_lvl = 127,
                 .src = N_WHITE, .n_lvl = 20, .n_dec = 34, .flt = F_LP, .cut = 94,
                 .crush = 0x18, .ctrl = CT_PITCH, .tune = 12, .lvl = 54},
    /* ---- rims ---- */
    [Y_RS808] = {.freq = 1667, .wave = W_SINE, .a_dec = 38, .o_lvl = 110, .o2 = 17, .o2_lvl = 110,
                 .flt = F_HP | F_ALL, .cut = 48, .ck = 40, .ck_dec = 3, .drive = 70, .ctrl = CT_TONE,
                 .tune = 7, .lvl = 43},
    [Y_RS909] = {.freq = 820, .wave = W_TRI, .a_dec = 36, .o_lvl = 110, .o2 = 104, .o2_lvl = 80,
                 .src = N_WHITE, .n_lvl = 30, .n_dec = 20, .flt = F_HP | F_ALL, .cut = 60, .ck = 80, .ck_dec = 4,
                 .drive = 50, .ctrl = CT_TONE, .tune = 7, .lvl = 43},
    [Y_RS7X7] = {.freq = 900, .wave = W_SQR, .a_dec = 34, .o_lvl = 80, .o2 = 90, .o2_lvl = 60,
                 .flt = F_BP | F_ALL, .cut = 92, .res = 30, .ck = 60, .ck_dec = 4, .crush = 0x18,
                 .ctrl = CT_TONE, .tune = 7, .lvl = 61},
    /* ---- claps ---- */
    [Y_CP808] = {.src = N_CLAP, .n_lvl = 127, .n_dec = 66, .flt = F_BP, .cut = 75, .res = 40, .hp = 50,
                 .ctrl = CT_TONE, .tune = 7, .lvl = 154},
    [Y_CP909] = {.src = N_CLAP, .n_lvl = 127, .n_dec = 70, .flt = F_BP, .cut = 82, .res = 56, .hp = 60,
                 .ck = 30, .ck_dec = 4, .ctrl = CT_TONE, .tune = 7, .lvl = 126},
    [Y_CP7X7] = {.src = N_CLAP, .n_lvl = 127, .n_dec = 64, .flt = F_BP, .cut = 86, .res = 40, .hp = 60,
                 .crush = 0x18, .ctrl = CT_TONE, .tune = 7, .lvl = 122},
    /* ---- hi-hats ---- */
    [Y_CH808] = {.src = N_METAL, .n_lvl = 127, .n_dec = 50, .flt = F_BP, .cut = 110, .res = 30, .hp = 104,
                 .mt = 64, .ctrl = CT_TONE, .tune = 7, .lvl = 131},
    [Y_OH808] = {.src = N_METAL, .n_lvl = 127, .n_hold = 10, .n_dec = 82, .flt = F_BP, .cut = 110, .res = 30,
                 .hp = 104, .mt = 64, .ctrl = CT_TONE, .tune = 7, .lvl = 131},
    [Y_CH909] = {.src = N_METALW, .n_lvl = 127, .n_dec = 54, .flt = F_HP, .cut = 108, .res = 10, .hp = 96,
                 .mt = 90, .crush = 0x19, .ctrl = CT_TONE, .tune = 7, .lvl = 122},
    [Y_OH909] = {.src = N_METALW, .n_lvl = 127, .n_hold = 12, .n_dec = 80, .flt = F_HP, .cut = 106, .res = 10,
                 .hp = 96, .mt = 90, .crush = 0x19, .ctrl = CT_TONE, .tune = 7, .lvl = 105},
    [Y_CH606] = {.src = N_METAL, .n_lvl = 127, .n_dec = 46, .flt = F_BP, .cut = 112, .res = 10, .hp = 108,
                 .mt = 72, .ctrl = CT_TONE, .tune = 7, .lvl = 162},
    [Y_OH606] = {.src = N_METAL, .n_lvl = 127, .n_hold = 6, .n_dec = 76, .flt = F_BP, .cut = 112, .res = 10,
                 .hp = 108, .mt = 72, .ctrl = CT_TONE, .tune = 7, .lvl = 118},
    [Y_CHCR] = {.src = N_WHITE, .n_lvl = 110, .n_dec = 44, .flt = F_HP, .cut = 110, .res = 20,
                .ctrl = CT_TONE, .tune = 7, .lvl = 47},
    [Y_OHCR] = {.src = N_WHITE, .n_lvl = 110, .n_hold = 6, .n_dec = 74, .flt = F_HP, .cut = 110, .res = 20,
                .ctrl = CT_TONE, .tune = 7, .lvl = 37},
    [Y_CH7X7] = {.src = N_METALW, .n_lvl = 127, .n_dec = 50, .flt = F_HP, .cut = 110, .hp = 98, .mt = 100,
                 .crush = 0x18, .ctrl = CT_TONE, .tune = 7, .lvl = 163},
    [Y_OH7X7] = {.src = N_METALW, .n_lvl = 127, .n_hold = 10, .n_dec = 78, .flt = F_HP, .cut = 108, .hp = 98,
                 .mt = 100, .crush = 0x18, .ctrl = CT_TONE, .tune = 7, .lvl = 94},
    /* ---- cymbals ---- */
    [Y_CY808] = {.src = N_METALW, .n_lvl = 127, .n_hold = 10, .n_dec = 96, .flt = F_BP, .cut = 98, .res = 20,
                 .hp = 90, .mt = 64, .ctrl = CT_TONE, .tune = 7, .lvl = 90},
    [Y_CY909] = {.src = N_METALW, .n_lvl = 127, .n_hold = 20, .n_dec = 100, .flt = F_HP, .cut = 100, .res = 10,
                 .hp = 92, .mt = 118, .crush = 0x19, .ctrl = CT_TONE, .tune = 7, .lvl = 70},
    [Y_RD909] = {.src = N_METALW, .n_lvl = 120, .n_hold = 4, .n_dec = 96, .flt = F_BP, .cut = 104, .res = 30,
                 .hp = 96, .mt = 150, .ck = 50, .ck_dec = 4, .crush = 0x19, .ctrl = CT_TONE, .tune = 7, .lvl = 75},
    [Y_CY606] = {.src = N_METAL, .n_lvl = 127, .n_hold = 10, .n_dec = 90, .flt = F_BP, .cut = 106, .res = 30,
                 .hp = 98, .mt = 80, .ctrl = CT_TONE, .tune = 7, .lvl = 110},
    [Y_CYCR] = {.src = N_WHITE, .n_lvl = 100, .n_hold = 10, .n_dec = 88, .flt = F_BP, .cut = 104, .res = 30,
                .ctrl = CT_TONE, .tune = 7, .lvl = 44},
    /* ---- percussion ---- */
    [Y_CB808] = {.freq = 540, .wave = W_SQR, .a_dec = 58, .hold = 6, .o_lvl = 90, .o2 = 95, .o2_lvl = 90,
                 .flt = F_BP | F_ALL, .cut = 95, .res = 50, .ctrl = CT_TONE, .tune = 7, .lvl = 77},
    [Y_CL808] = {.freq = 2500, .wave = W_SINE, .a_dec = 40, .o_lvl = 127, .ctrl = CT_PITCH,
                 .pe_amt = 0, .tune = 12, .lvl = 43},
    [Y_MA808] = {.src = N_WHITE, .n_lvl = 120, .n_dec = 42, .flt = F_HP, .cut = 108, .ctrl = CT_TONE,
                 .tune = 7, .lvl = 57},
    [Y_LC808] = {.freq = 230, .wave = W_SINE, .pe_amt = 6, .pe_dec = 36, .a_dec = 66, .o_lvl = 127,
                 .ctrl = CT_PITCH, .tune = 12, .lvl = 43},
    [Y_HC808] = {.freq = 340, .wave = W_SINE, .pe_amt = 6, .pe_dec = 34, .a_dec = 62, .o_lvl = 127,
                 .ctrl = CT_PITCH, .tune = 12, .lvl = 43},
    [Y_CBCR] = {.freq = 800, .wave = W_SQR, .a_dec = 52, .o_lvl = 80, .o2 = 80, .o2_lvl = 80,
                .flt = F_BP | F_ALL, .cut = 100, .res = 40, .ctrl = CT_TONE, .tune = 7, .lvl = 55},
    [Y_GUCR] = {.src = N_CLAP, .n_lvl = 100, .n_dec = 40, .flt = F_BP, .cut = 102, .res = 60,
                .ctrl = CT_TONE, .tune = 7, .lvl = 109},
    [Y_BGCR] = {.freq = 420, .wave = W_SINE, .pe_amt = 4, .pe_dec = 30, .a_dec = 56, .o_lvl = 127,
                .ctrl = CT_PITCH, .tune = 12, .lvl = 43},
    [Y_TB7X7] = {.src = N_METALW, .n_lvl = 120, .n_hold = 4, .n_dec = 60, .flt = F_HP, .cut = 112, .mt = 140,
                 .crush = 0x18, .ctrl = CT_TONE, .tune = 7, .lvl = 128},
    [Y_BLIP] = {.freq = 880, .wave = W_SQR, .pe_amt = 48, .pe_dec = 30, .a_dec = 44, .o_lvl = 90,
                .flt = F_LP | F_ALL, .cut = 112, .res = 20, .crush = 0x16, .ctrl = CT_PITCH, .tune = 24, .lvl = 99},
    [Y_ZAP] = {.freq = 120, .wave = W_SINE, .pe_amt = 160, .pe_dec = 46, .a_dec = 60, .o_lvl = 127,
               .ctrl = CT_PITCH, .tune = 12, .lvl = 43},
};

/* ---------------------------------------------------------- PCM instruments --- */
enum {
    P_LM_BD, P_LM_SD, P_LM_LT, P_LM_HT, P_LM_CP, P_LM_RS, P_LM_CH, P_LM_OH, P_LM_CY, P_LM_RD, P_LM_CB, P_LM_TB,
    P_LM_SH, P_LM_CG, P_LM_CL,
    P_DX_BD, P_DX_SD, P_DX_LT, P_DX_HT, P_DX_CP, P_DX_RS, P_DX_CH, P_DX_OH, P_DX_RD, P_DX_TB, P_DX_SH,
    P_DR_BD, P_DR_SD, P_DR_LT, P_DR_HT, P_DR_CP, P_DR_RS, P_DR_CH, P_DR_OH, P_DR_CB, P_DR_CL,
    P_SP_BD, P_SP_SD, P_SP_LT, P_SP_HT, P_SP_CP, P_SP_RS, P_SP_CH, P_SP_OH, P_SP_RD, P_SP_WB,
    P_AC_BD, P_AC_SD, P_AC_LT, P_AC_HT, P_AC_RS, P_AC_CH, P_AC_OH, P_AC_CY, P_AC_RD, P_AC_CG, P_AC_WB,
    P_N
};
static const pcm_t PCMI[P_N] = {
    [P_LM_BD] = {SMP_BD, MA_LM, 0, 0, 77},  [P_LM_SD] = {SMP_SD, MA_LM, 1, 0, 64},
    [P_LM_LT] = {SMP_LT, MA_LM, 0, 0, 59},  [P_LM_HT] = {SMP_HT, MA_LM, 0, 0, 58},
    [P_LM_CP] = {SMP_CP, MA_LM, 0, 0, 59},  [P_LM_RS] = {SMP_RS, MA_LM, 0, 0, 50},
    [P_LM_CH] = {SMP_CH, MA_LM, 0, 0, 48},   [P_LM_OH] = {SMP_OH, MA_LM, 0, 0, 53},
    [P_LM_CY] = {SMP_CY, MA_LM, 0, 0, 51},   [P_LM_RD] = {SMP_RD, MA_LM, 0, 0, 46},
    [P_LM_CB] = {SMP_CB, MA_LM, 0, 0, 53},   [P_LM_TB] = {SMP_TB, MA_LM, 0, 0, 63},
    [P_LM_SH] = {SMP_SH, MA_LM, 0, 0, 90},   [P_LM_CG] = {SMP_CG, MA_LM, 0, 0, 51},
    [P_LM_CL] = {SMP_CL, MA_LM, 0, 0, 52},
    [P_DX_BD] = {SMP_BD, MA_DMX, -2, 74, 74}, [P_DX_SD] = {SMP_SD, MA_DMX, -1, 66, 75},
    [P_DX_LT] = {SMP_LT, MA_DMX, -2, 76, 58}, [P_DX_HT] = {SMP_HT, MA_DMX, -1, 74, 58},
    [P_DX_CP] = {SMP_CP, MA_DMX, 0, 64, 56},  [P_DX_RS] = {SMP_RS, MA_DMX, 0, 0, 43},
    [P_DX_CH] = {SMP_CH, MA_DMX, 1, 48, 52},   [P_DX_OH] = {SMP_OH, MA_DMX, 1, 76, 44},
    [P_DX_RD] = {SMP_RD, MA_DMX, 0, 0, 32},    [P_DX_TB] = {SMP_TB, MA_DMX, 0, 60, 63},
    [P_DX_SH] = {SMP_SH, MA_DMX, 0, 0, 55},
    [P_DR_BD] = {SMP_BD, MA_DRMU, -1, 70, 94}, [P_DR_SD] = {SMP_SD, MA_DRMU, 0, 64, 101},
    [P_DR_LT] = {SMP_LT, MA_DRMU, -3, 0, 61},  [P_DR_HT] = {SMP_HT, MA_DRMU, -1, 0, 59},
    [P_DR_CP] = {SMP_CP, MA_DRMU, 1, 0, 62},   [P_DR_RS] = {SMP_RS, MA_DRMU, 2, 0, 62},
    [P_DR_CH] = {SMP_CH, MA_DRMU, 0, 44, 128},   [P_DR_OH] = {SMP_OH, MA_DRMU, 0, 72, 98},
    [P_DR_CB] = {SMP_CB, MA_DRMU, 0, 0, 56},    [P_DR_CL] = {SMP_CL, MA_DRMU, 0, 0, 61},
    [P_SP_BD] = {SMP_BD, MA_SP12, -4, 0, 64}, [P_SP_SD] = {SMP_SD, MA_SP12, -3, 0, 58},
    [P_SP_LT] = {SMP_LT, MA_SP12, -4, 0, 55}, [P_SP_HT] = {SMP_HT, MA_SP12, -3, 0, 55},
    [P_SP_CP] = {SMP_CP, MA_SP12, -2, 0, 46}, [P_SP_RS] = {SMP_RS, MA_SP12, -2, 0, 43},
    [P_SP_CH] = {SMP_CH, MA_SP12, -2, 46, 39}, [P_SP_OH] = {SMP_OH, MA_SP12, -2, 0, 35},
    [P_SP_RD] = {SMP_RD, MA_SP12, -3, 0, 32},  [P_SP_WB] = {SMP_WB, MA_SP12, -3, 0, 46},
    [P_AC_BD] = {SMP_BD, MA_CLEAN, 0, 0, 77}, [P_AC_SD] = {SMP_SD, MA_CLEAN, 0, 0, 64},
    [P_AC_LT] = {SMP_LT, MA_CLEAN, 0, 0, 59}, [P_AC_HT] = {SMP_HT, MA_CLEAN, 0, 0, 58},
    [P_AC_RS] = {SMP_RS, MA_CLEAN, 0, 0, 48},  [P_AC_CH] = {SMP_CH, MA_CLEAN, 0, 0, 40},
    [P_AC_OH] = {SMP_OH, MA_CLEAN, 0, 0, 49},  [P_AC_CY] = {SMP_CY, MA_CLEAN, 0, 0, 51},
    [P_AC_RD] = {SMP_RD, MA_CLEAN, 0, 0, 37},  [P_AC_CG] = {SMP_CG, MA_CLEAN, 0, 0, 51},
    [P_AC_WB] = {SMP_WB, MA_CLEAN, 0, 0, 48},
};

/* ------------------------------------------------------- instrument list --- */
#define SY(n, l, c, y, a) {n, l, c, K_SYN, y, a}
#define PC(n, l, c, p, a) {n, l, c, K_PCM, p, a}
enum {
    I_BD808, I_BD909, I_BD606, I_BDCR, I_BD7X7, I_BDLONG, I_BDDIST, I_BDFM,
    I_LM_BD, I_DX_BD, I_DR_BD, I_SP_BD, I_AC_BD,
    I_SD808, I_SD909, I_SD606, I_SDCR, I_SD7X7, I_SDNZ,
    I_LM_SD, I_DX_SD, I_DR_SD, I_SP_SD, I_AC_SD,
    I_LT808, I_HT808, I_LT909, I_HT909, I_LT606, I_HT606, I_LT7X7, I_HT7X7,
    I_LM_LT, I_LM_HT, I_DX_LT, I_DX_HT, I_DR_LT, I_DR_HT, I_SP_LT, I_SP_HT, I_AC_LT, I_AC_HT,
    I_RS808, I_RS909, I_RS7X7, I_LM_RS, I_DX_RS, I_DR_RS, I_SP_RS, I_AC_RS,
    I_CP808, I_CP909, I_CP7X7, I_LM_CP, I_DX_CP, I_DR_CP, I_SP_CP,
    I_CH808, I_CH909, I_CH606, I_CHCR, I_CH7X7, I_LM_CH, I_DX_CH, I_DR_CH, I_SP_CH, I_AC_CH,
    I_OH808, I_OH909, I_OH606, I_OHCR, I_OH7X7, I_LM_OH, I_DX_OH, I_DR_OH, I_SP_OH, I_AC_OH,
    I_CY808, I_CY909, I_RD909, I_CY606, I_CYCR, I_LM_CY, I_LM_RD, I_DX_RD, I_SP_RD, I_AC_CY, I_AC_RD,
    I_CB808, I_CL808, I_MA808, I_LC808, I_HC808, I_CBCR, I_GUCR, I_BGCR, I_TB7X7, I_BLIP, I_ZAP,
    I_LM_CB, I_LM_TB, I_LM_SH, I_LM_CG, I_LM_CL, I_DX_TB, I_DX_SH, I_DR_CB, I_DR_CL, I_SP_WB, I_AC_CG,
    I_AC_WB,
    I_N
};
#define SAME 0xFFu
static const inst_t INST[I_N] = {
    [I_BD808] = SY("808 KICK", "BD", CAT_BD, Y_BD808, I_BDLONG),
    [I_BD909] = SY("909 KICK", "BD", CAT_BD, Y_BD909, I_BDDIST),
    [I_BD606] = SY("606 KICK", "BD", CAT_BD, Y_BD606, SAME),
    [I_BDCR] = SY("CR KICK", "BD", CAT_BD, Y_BDCR, SAME),
    [I_BD7X7] = SY("7X7 KICK", "BD", CAT_BD, Y_BD7X7, SAME),
    [I_BDLONG] = SY("BOOM KICK", "BD", CAT_BD, Y_BDLONG, SAME),
    [I_BDDIST] = SY("DIST KICK", "BD", CAT_BD, Y_BDDIST, SAME),
    [I_BDFM] = SY("FM KICK", "BD", CAT_BD, Y_BDFM, SAME),
    [I_LM_BD] = PC("LM KICK", "BD", CAT_BD, P_LM_BD, SAME),
    [I_DX_BD] = PC("DMX KICK", "BD", CAT_BD, P_DX_BD, SAME),
    [I_DR_BD] = PC("DRMU KICK", "BD", CAT_BD, P_DR_BD, SAME),
    [I_SP_BD] = PC("SP12 KICK", "BD", CAT_BD, P_SP_BD, SAME),
    [I_AC_BD] = PC("ACOU KICK", "BD", CAT_BD, P_AC_BD, SAME),
    [I_SD808] = SY("808 SNARE", "SD", CAT_SD, Y_SD808, SAME),
    [I_SD909] = SY("909 SNARE", "SD", CAT_SD, Y_SD909, I_SDNZ),
    [I_SD606] = SY("606 SNARE", "SD", CAT_SD, Y_SD606, SAME),
    [I_SDCR] = SY("CR SNARE", "SD", CAT_SD, Y_SDCR, SAME),
    [I_SD7X7] = SY("7X7 SNARE", "SD", CAT_SD, Y_SD7X7, SAME),
    [I_SDNZ] = SY("NOIZ SNARE", "SD", CAT_SD, Y_SDNZ, SAME),
    [I_LM_SD] = PC("LM SNARE", "SD", CAT_SD, P_LM_SD, SAME),
    [I_DX_SD] = PC("DMX SNARE", "SD", CAT_SD, P_DX_SD, SAME),
    [I_DR_SD] = PC("DRMU SNARE", "SD", CAT_SD, P_DR_SD, SAME),
    [I_SP_SD] = PC("SP12 SNARE", "SD", CAT_SD, P_SP_SD, SAME),
    [I_AC_SD] = PC("ACOU SNARE", "SD", CAT_SD, P_AC_SD, SAME),
    [I_LT808] = SY("808 LO TOM", "LT", CAT_TOM, Y_LT808, I_LC808),
    [I_HT808] = SY("808 HI TOM", "HT", CAT_TOM, Y_HT808, I_HC808),
    [I_LT909] = SY("909 LO TOM", "LT", CAT_TOM, Y_LT909, SAME),
    [I_HT909] = SY("909 HI TOM", "HT", CAT_TOM, Y_HT909, SAME),
    [I_LT606] = SY("606 LO TOM", "LT", CAT_TOM, Y_LT606, SAME),
    [I_HT606] = SY("606 HI TOM", "HT", CAT_TOM, Y_HT606, SAME),
    [I_LT7X7] = SY("7X7 LO TOM", "LT", CAT_TOM, Y_LT7X7, SAME),
    [I_HT7X7] = SY("7X7 HI TOM", "HT", CAT_TOM, Y_HT7X7, SAME),
    [I_LM_LT] = PC("LM LO TOM", "LT", CAT_TOM, P_LM_LT, SAME),
    [I_LM_HT] = PC("LM HI TOM", "HT", CAT_TOM, P_LM_HT, SAME),
    [I_DX_LT] = PC("DMX LO TOM", "LT", CAT_TOM, P_DX_LT, SAME),
    [I_DX_HT] = PC("DMX HI TOM", "HT", CAT_TOM, P_DX_HT, SAME),
    [I_DR_LT] = PC("DRMU LOTOM", "LT", CAT_TOM, P_DR_LT, SAME),
    [I_DR_HT] = PC("DRMU HITOM", "HT", CAT_TOM, P_DR_HT, SAME),
    [I_SP_LT] = PC("SP12 LOTOM", "LT", CAT_TOM, P_SP_LT, SAME),
    [I_SP_HT] = PC("SP12 HITOM", "HT", CAT_TOM, P_SP_HT, SAME),
    [I_AC_LT] = PC("ACOU LOTOM", "LT", CAT_TOM, P_AC_LT, SAME),
    [I_AC_HT] = PC("ACOU HITOM", "HT", CAT_TOM, P_AC_HT, SAME),
    [I_RS808] = SY("808 RIM", "RS", CAT_RS, Y_RS808, I_CL808),
    [I_RS909] = SY("909 RIM", "RS", CAT_RS, Y_RS909, SAME),
    [I_RS7X7] = SY("7X7 RIM", "RS", CAT_RS, Y_RS7X7, SAME),
    [I_LM_RS] = PC("LM RIM", "RS", CAT_RS, P_LM_RS, SAME),
    [I_DX_RS] = PC("DMX RIM", "RS", CAT_RS, P_DX_RS, SAME),
    [I_DR_RS] = PC("DRMU RIM", "RS", CAT_RS, P_DR_RS, SAME),
    [I_SP_RS] = PC("SP12 RIM", "RS", CAT_RS, P_SP_RS, SAME),
    [I_AC_RS] = PC("ACOU RIM", "RS", CAT_RS, P_AC_RS, SAME),
    [I_CP808] = SY("808 CLAP", "CP", CAT_CP, Y_CP808, I_MA808),
    [I_CP909] = SY("909 CLAP", "CP", CAT_CP, Y_CP909, SAME),
    [I_CP7X7] = SY("7X7 CLAP", "CP", CAT_CP, Y_CP7X7, SAME),
    [I_LM_CP] = PC("LM CLAP", "CP", CAT_CP, P_LM_CP, SAME),
    [I_DX_CP] = PC("DMX CLAP", "CP", CAT_CP, P_DX_CP, SAME),
    [I_DR_CP] = PC("DRMU CLAP", "CP", CAT_CP, P_DR_CP, SAME),
    [I_SP_CP] = PC("SP12 CLAP", "CP", CAT_CP, P_SP_CP, SAME),
    [I_CH808] = SY("808 CL HAT", "CH", CAT_HH, Y_CH808, SAME),
    [I_CH909] = SY("909 CL HAT", "CH", CAT_HH, Y_CH909, SAME),
    [I_CH606] = SY("606 CL HAT", "CH", CAT_HH, Y_CH606, SAME),
    [I_CHCR] = SY("CR HAT", "CH", CAT_HH, Y_CHCR, SAME),
    [I_CH7X7] = SY("7X7 CL HAT", "CH", CAT_HH, Y_CH7X7, SAME),
    [I_LM_CH] = PC("LM CL HAT", "CH", CAT_HH, P_LM_CH, SAME),
    [I_DX_CH] = PC("DMX CL HAT", "CH", CAT_HH, P_DX_CH, SAME),
    [I_DR_CH] = PC("DRMU CLHAT", "CH", CAT_HH, P_DR_CH, SAME),
    [I_SP_CH] = PC("SP12 CLHAT", "CH", CAT_HH, P_SP_CH, SAME),
    [I_AC_CH] = PC("ACOU CLHAT", "CH", CAT_HH, P_AC_CH, SAME),
    [I_OH808] = SY("808 OP HAT", "OH", CAT_OH, Y_OH808, SAME),
    [I_OH909] = SY("909 OP HAT", "OH", CAT_OH, Y_OH909, SAME),
    [I_OH606] = SY("606 OP HAT", "OH", CAT_OH, Y_OH606, SAME),
    [I_OHCR] = SY("CR OP HAT", "OH", CAT_OH, Y_OHCR, SAME),
    [I_OH7X7] = SY("7X7 OP HAT", "OH", CAT_OH, Y_OH7X7, SAME),
    [I_LM_OH] = PC("LM OP HAT", "OH", CAT_OH, P_LM_OH, SAME),
    [I_DX_OH] = PC("DMX OP HAT", "OH", CAT_OH, P_DX_OH, SAME),
    [I_DR_OH] = PC("DRMU OPHAT", "OH", CAT_OH, P_DR_OH, SAME),
    [I_SP_OH] = PC("SP12 OPHAT", "OH", CAT_OH, P_SP_OH, SAME),
    [I_AC_OH] = PC("ACOU OPHAT", "OH", CAT_OH, P_AC_OH, SAME),
    [I_CY808] = SY("808 CYMBAL", "CY", CAT_CY, Y_CY808, SAME),
    [I_CY909] = SY("909 CRASH", "CC", CAT_CY, Y_CY909, I_RD909),
    [I_RD909] = SY("909 RIDE", "RC", CAT_CY, Y_RD909, I_CY909),
    [I_CY606] = SY("606 CYMBAL", "CY", CAT_CY, Y_CY606, SAME),
    [I_CYCR] = SY("CR CYMBAL", "CY", CAT_CY, Y_CYCR, SAME),
    [I_LM_CY] = PC("LM CRASH", "CC", CAT_CY, P_LM_CY, I_LM_RD),
    [I_LM_RD] = PC("LM RIDE", "RC", CAT_CY, P_LM_RD, I_LM_CY),
    [I_DX_RD] = PC("DMX RIDE", "RC", CAT_CY, P_DX_RD, SAME),
    [I_SP_RD] = PC("SP12 RIDE", "RC", CAT_CY, P_SP_RD, SAME),
    [I_AC_CY] = PC("ACOU CRASH", "CC", CAT_CY, P_AC_CY, I_AC_RD),
    [I_AC_RD] = PC("ACOU RIDE", "RC", CAT_CY, P_AC_RD, I_AC_CY),
    [I_CB808] = SY("808 COWBEL", "CB", CAT_PC, Y_CB808, SAME),
    [I_CL808] = SY("808 CLAVES", "CL", CAT_PC, Y_CL808, SAME),
    [I_MA808] = SY("808 MARACA", "MA", CAT_PC, Y_MA808, SAME),
    [I_LC808] = SY("808 LO CNG", "LC", CAT_PC, Y_LC808, I_HC808),
    [I_HC808] = SY("808 HI CNG", "HC", CAT_PC, Y_HC808, I_LC808),
    [I_CBCR] = SY("CR COWBELL", "CB", CAT_PC, Y_CBCR, SAME),
    [I_GUCR] = SY("CR GUIRO", "GU", CAT_PC, Y_GUCR, SAME),
    [I_BGCR] = SY("CR BONGO", "BG", CAT_PC, Y_BGCR, SAME),
    [I_TB7X7] = SY("7X7 TAMB", "TB", CAT_PC, Y_TB7X7, SAME),
    [I_BLIP] = SY("BLIP", "BL", CAT_PC, Y_BLIP, SAME),
    [I_ZAP] = SY("ZAP", "ZP", CAT_PC, Y_ZAP, SAME),
    [I_LM_CB] = PC("LM COWBELL", "CB", CAT_PC, P_LM_CB, SAME),
    [I_LM_TB] = PC("LM TAMB", "TB", CAT_PC, P_LM_TB, SAME),
    [I_LM_SH] = PC("LM SHAKER", "SH", CAT_PC, P_LM_SH, SAME),
    [I_LM_CG] = PC("LM CONGA", "CG", CAT_PC, P_LM_CG, SAME),
    [I_LM_CL] = PC("LM CLAVES", "CL", CAT_PC, P_LM_CL, SAME),
    [I_DX_TB] = PC("DMX TAMB", "TB", CAT_PC, P_DX_TB, SAME),
    [I_DX_SH] = PC("DMX SHAKER", "SH", CAT_PC, P_DX_SH, SAME),
    [I_DR_CB] = PC("DRMU COWBL", "CB", CAT_PC, P_DR_CB, SAME),
    [I_DR_CL] = PC("DRMU CLAVE", "CL", CAT_PC, P_DR_CL, SAME),
    [I_SP_WB] = PC("SP12 BLOCK", "WB", CAT_PC, P_SP_WB, SAME),
    [I_AC_CG] = PC("ACOU CONGA", "CG", CAT_PC, P_AC_CG, SAME),
    [I_AC_WB] = PC("ACOU BLOCK", "WB", CAT_PC, P_AC_WB, SAME),
};

/* ----------------------------------------------------------- helpers --- */
static inline int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t mulq30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 2^(n / 192) in Q16, n in 1/16 semitones (any sign, |n| < 16 octaves) */
static uint32_t exp2_16(int32_t n)
{
    int32_t oct = n >= 0 ? n / 192 : -((-n + 191) / 192);
    uint32_t m = EXP2_16[n - oct * 192];
    return oct >= 0 ? m << oct : m >> -oct;
}

static inline int32_t sine_q15(uint32_t ph)
{
    uint32_t i = ph >> 22, f = (ph >> 7) & 0x7FFFu;
    int32_t a = SINE[i], b = SINE[i + 1u];
    return a + (((b - a) * (int32_t)f) >> 15);
}

static inline int32_t soft_clip(int32_t x)    /* Q15, cubic, |x| <= 1.0 -> 1.5x - 0.5x^3 */
{
    if (x >= 32768)
        return 32767;
    if (x <= -32768)
        return -32767;
    return (3 * x - (int32_t)(((int64_t)x * x * x) >> 30)) >> 1;
}

static uint32_t pz_rnd = 0x1234567u;
#define INC_PER_HZ 97392u                      /* 2^32 / 44100: the phase increment of 1 Hz */
static inline uint32_t xrand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* zero-delay-feedback SVF (Q16 coefficients, int64 products) */
typedef struct { int32_t ic1, ic2, a1, a2, a3, k; } svf_t;
static void svf_set(svf_t *f, int32_t cut_idx, int32_t res)   /* res 0..127 -> k 2.0 .. 0.08 */
{
    int64_t g = SVF_G[clampi(cut_idx, 0, 127)], k = 131072 - (int64_t)clampi(res, 0, 127) * 1000;
    uint32_t den = (uint32_t)(65536 + ((g * (g + k)) >> 16));   /* >= 65536: a1 <= 1.0 */
    f->k = (int32_t)k;
    f->a1 = (int32_t)(0xFFFFFFFFu / den);
    f->a2 = (int32_t)((g * f->a1) >> 16);
    f->a3 = (int32_t)((g * f->a2) >> 16);
}
static inline int32_t svf_run(svf_t *f, int32_t x, uint32_t mode)
{
    int32_t v3 = x - f->ic2;
    int32_t v1 = (int32_t)(((int64_t)f->a1 * f->ic1 + (int64_t)f->a2 * v3) >> 16);
    int32_t v2 = f->ic2 + (int32_t)(((int64_t)f->a2 * f->ic1 + (int64_t)f->a3 * v3) >> 16);
    f->ic1 = 2 * v1 - f->ic1;
    f->ic2 = 2 * v2 - f->ic2;
    if (mode == F_LP)
        return v2;
    if (mode == F_BP)
        return (int32_t)(((int64_t)f->k * v1) >> 16);   /* normalised: unity gain at the centre */
    return x - (int32_t)(((int64_t)f->k * v1) >> 16) - v2;
}

/* one-pole high-pass, coefficient from a cutoff index (Q15) */
static int32_t hp_coef(uint32_t idx)
{
    int32_t g = SVF_G[idx];                     /* tan(pi fc / fs) Q16 */
    return (int32_t)(0x80000000u / (uint32_t)(65536 + g));   /* a = 1 / (1 + g) approximately (bilinear) */
}

/* ------------------------------------------------------------ voices --- */
static const uint16_t METAL_HZ10[6] = {2053, 3044, 3696, 5227, 5400, 8000};

typedef struct {
    uint8_t on, kind, ft, ftall;
    const syn_t *sy;
    /* tone */
    uint32_t ph, ph2, inc, inc_base, o2r;      /* o2r: ratio x64 */
    uint32_t mph[6], minc[6];
    int32_t pe, pek, pe_amt16;                 /* pitch env Q30, decay, depth (1/16 semitones) */
    int32_t a, ak, a2, a2k, n, nk, ck, ckk;    /* envelopes Q30 and their per-sample multipliers */
    uint32_t ahold, nhold, t;
    int32_t olvl, o2lvl, nlvl, cklvl;          /* Q15 */
    uint8_t wave, src, sq_crush_b, sh;
    int32_t gain;                              /* velocity x trim, Q15 */
    int32_t drive;                             /* 0 = off, else gain Q8 */
    int32_t cut, fenv, res;                    /* cutoff index (Q8), fenv in index units */
    svf_t f;
    int32_t hpa, hpx, hpy;
    uint32_t rnd;
    uint32_t bitmask;
    uint8_t shn, shc;
    int32_t shv;
    int32_t clap_k;                            /* clap burst decay */
    /* pcm */
    const uint8_t *pd;
    uint32_t plen, pos, pinc;                  /* pos Q16 */
    uint8_t zoh;
    /* declick */
    int32_t last, xf;                          /* the previous sound's last sample, ramp left */
    int32_t meter;                             /* peak (UI) */
} voice_t;

static voice_t vc[PZ_NTRK];

typedef struct {                               /* the parameters a hit is played with */
    uint8_t inst, alt;
    int8_t tune;
    uint8_t decay, ctrl;
    uint8_t vel;                               /* 1..127 */
} hit_t;

static void voice_choke(voice_t *v)             /* ~6 ms fade (the closed hat closes the open one) */
{
    int32_t k = DECAY_K[22];
    if (!v->on)
        return;
    if (v->ak > k)
        v->ak = k;
    if (v->a2k > k)
        v->a2k = k;
    if (v->nk > k)
        v->nk = k;
    v->ahold = v->nhold = 0;
}

static void voice_trigger(voice_t *v, const hit_t *h)
{
    const inst_t *in = &INST[h->inst < I_N ? h->inst : 0];
    int32_t dofs = ((int32_t)h->decay - 64) * 22 / 64;
    int32_t c = h->ctrl, cc = c - 64;
    if (h->alt) {
        if (in->alt != SAME)
            in = &INST[in->alt];
    }
    if (v->on && !v->xf) {
        v->xf = 64;                            /* declick the old sound over 1.5 ms */
    } else if (!v->on) {
        v->last = 0;
        v->xf = 0;
    }
    v->on = 1;
    v->t = 0;
    v->kind = in->kind;
    v->gain = mulq15(VEL_Q15[h->vel & 127u], 32767);
    v->rnd = xrand(&pz_rnd) | 1u;
    v->f.ic1 = v->f.ic2 = 0;
    v->hpx = v->hpy = 0;
    v->shc = 0;
    v->shv = 0;
    if (in->kind == K_SYN) {
        const syn_t *s = &SYN[in->idx];
        int32_t tune16 = (int32_t)h->tune * s->tune * 16 / 64 + (h->alt && in->alt == SAME ? 5 * 16 : 0);
        uint32_t ratio = exp2_16(tune16), i;
        v->sy = s;
        v->wave = s->wave;
        v->src = s->src;
        v->inc_base = (uint32_t)s->freq * INC_PER_HZ;
        v->inc_base = (uint32_t)(((uint64_t)v->inc_base * ratio) >> 16);
        v->o2r = s->o2;
        v->ph = 0x40000000u;                   /* start at the peak of a cosine: the click of the circuit */
        v->ph2 = 0x40000000u;
        v->pe = 1 << 30;
        v->pek = DECAY_K[s->pe_dec];
        v->pe_amt16 = (int32_t)s->pe_amt * 4;
        if (s->ctrl == CT_PITCH)
            v->pe_amt16 = v->pe_amt16 * c / 64;
        v->inc = (uint32_t)(((uint64_t)v->inc_base * exp2_16(v->pe_amt16)) >> 16);
        v->a = s->o_lvl ? 1 << 30 : 0;
        v->ak = DECAY_K[clampi((int32_t)s->a_dec + dofs, 0, 127)];
        v->a2 = v->o2r ? 1 << 30 : 0;
        v->a2k = DECAY_K[clampi((int32_t)s->a_dec + dofs - 7, 0, 127)];
        v->ahold = (uint32_t)s->hold * (PZ_FS / 1000u);
        v->n = s->src ? 1 << 30 : 0;
        v->nk = DECAY_K[clampi((int32_t)s->n_dec + dofs + (s->ctrl == CT_TAIL ? cc / 3 : 0), 0, 127)];
        v->nhold = (uint32_t)s->n_hold * (PZ_FS / 1000u);
        v->ck = s->ck ? 1 << 30 : 0;
        v->ckk = DECAY_K[s->ck_dec];
        v->clap_k = DECAY_K[16];
        v->olvl = (int32_t)s->o_lvl * 258;
        v->o2lvl = (int32_t)s->o2_lvl * 258;
        v->nlvl = (int32_t)s->n_lvl * 258;
        v->cklvl = (int32_t)s->ck * 258;
        if (s->ctrl == CT_ATTACK)
            v->cklvl = v->cklvl * c / 64;
        else if (s->ctrl == CT_SNAPPY)
            v->nlvl = v->nlvl * c / 64;
        else if (s->ctrl == CT_COLOR)
            v->o2lvl = clampi(v->o2lvl * c / 48 + (c > 64 ? (c - 64) * 100 : 0), 0, 32767);
        v->drive = s->drive;
        if (s->ctrl == CT_DRIVE)
            v->drive = clampi((int32_t)s->drive + cc * 2, 0, 127);
        v->drive = v->drive ? 256 + v->drive * 24 : 0;
        v->ft = s->flt & 0x0Fu;
        v->ftall = (s->flt & F_ALL) != 0u;
        v->cut = ((int32_t)s->cut + (s->ctrl == CT_TONE ? cc * 2 / 3 : 0) + ((int32_t)h->vel - 100) / 6) << 8;
        v->fenv = s->fenv;
        v->res = s->res;
        if (v->ft)
            svf_set(&v->f, v->cut >> 8, v->res);
        v->hpa = s->hp ? hp_coef(s->hp) : 0;
        for (i = 0; i < 6u; i++) {
            uint32_t hz10 = (uint32_t)METAL_HZ10[i] * (s->mt ? s->mt : 64u) / 64u;
            v->mph[i] = xrand(&pz_rnd);
            v->minc[i] = (uint32_t)(((uint64_t)(hz10 * (INC_PER_HZ / 10u)) * ratio) >> 16);
        }
        v->bitmask = ~((1u << (s->crush & 15u)) - 1u);
        v->shn = (uint8_t)(s->crush >> 4);
        v->gain = (int32_t)((int64_t)v->gain * s->lvl / 64);
    } else {
        const pcm_t *p = &PCMI[in->idx];
        const mach_t *m = &MACH[p->mach];
        int32_t tune16 = (int32_t)h->tune * 12 * 16 / 64 + p->semi * 16 + (h->alt && in->alt == SAME ? 5 * 16 : 0);
        uint32_t i;
        v->sy = 0;
        v->pd = PCM_SMP[p->smp].d;
        v->plen = PCM_SMP[p->smp].n << 16;
        v->pos = 0;
        v->pinc = (uint32_t)((((uint64_t)PCM_RATE << 16) / PZ_FS * exp2_16(tune16)) >> 16);
        v->zoh = m->zoh;
        v->a = 1 << 30;
        i = (uint32_t)clampi((p->dec ? p->dec : 96) + dofs * 2, 20, 130);
        v->ak = i >= 128u ? 1 << 30 : DECAY_K[i];
        v->ahold = (uint32_t)(h->decay < 64 ? 0 : (h->decay - 64)) * 40u;
        v->bitmask = ~((1u << m->bitdrop) - 1u);
        v->drive = m->drive ? 256 + m->drive * 24 : 0;
        v->ft = cc < -2 ? F_LP : cc > 2 ? F_HP : (m->lp < 127 ? F_LP : F_OFF);
        v->cut = (cc < -2 ? (int32_t)m->lp + cc * 3 / 2 : cc > 2 ? cc * 3 / 2 + 10 : m->lp) << 8;
        v->res = 8;
        v->fenv = 0;
        if (v->ft)
            svf_set(&v->f, v->cut >> 8, v->res);
        v->gain = (int32_t)((int64_t)v->gain * p->lvl / 64);
        v->shn = 0;
    }
}

/* one block of one voice, added into out[] (mono, Q15 scale) */
static void voice_render(voice_t *v, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t peak = v->meter - (v->meter >> 3);
    if (!n)
        return;
    if (v->xf) {                               /* the declick of a retriggered sound */
        for (i = 0; i < n && v->xf; i++, v->xf--)
            out[i] += v->last * v->xf / 64;
    }
    if (!v->on) {
        v->meter = peak;
        return;
    }
    if (v->kind == K_SYN) {
        const syn_t *s = v->sy;
        uint32_t inc0 = v->inc, inc1;
        int32_t dinc;
        /* the pitch at the end of this block */
        int32_t pe_end;
        {                                      /* pek^n by squaring */
            int64_t k = v->pek, r = 1 << 30;
            uint32_t e = n;
            while (e) {
                if (e & 1u)
                    r = (r * k) >> 30;
                k = (k * k) >> 30;
                e >>= 1;
            }
            pe_end = (int32_t)(((int64_t)v->pe * r) >> 30);
        }
        inc1 = (uint32_t)(((uint64_t)v->inc_base * exp2_16((int32_t)(((int64_t)v->pe_amt16 * pe_end) >> 30))) >> 16);
        dinc = (int32_t)(inc1 - inc0) / (int32_t)n;
        if (v->ft && v->fenv) {
            int32_t c = (v->cut >> 8) + (int32_t)(((int64_t)v->fenv * v->pe) >> 30);
            svf_set(&v->f, c, v->res);
        }
        for (i = 0; i < n; i++) {
            int32_t tone = 0, nz = 0, x;
            /* tone */
            if (v->wave && v->a) {
                int32_t o;
                if (v->wave == W_SINE)
                    o = sine_q15(v->ph);
                else if (v->wave == W_TRI)
                    o = (int32_t)((v->ph ^ (uint32_t)((int32_t)v->ph >> 31)) >> 15) - 32768;
                else
                    o = (int32_t)v->ph < 0 ? -20000 : 20000;
                tone = mulq15(o, mulq15(v->olvl, v->a >> 15));
                if (v->o2r) {
                    int32_t o2;
                    if (v->wave == W_SQR)
                        o2 = (int32_t)v->ph2 < 0 ? -20000 : 20000;
                    else
                        o2 = sine_q15(v->ph2);
                    tone += mulq15(o2, mulq15(v->o2lvl, v->a2 >> 15));
                    v->ph2 += (uint32_t)(((uint64_t)v->inc * v->o2r) >> 6);
                    v->a2 = mulq30(v->a2, v->a2k);
                }
                v->ph += v->inc;
                if (v->ahold)
                    v->ahold--;
                else
                    v->a = mulq30(v->a, v->ak);
            }
            /* noise / metal / clap */
            if (v->src && v->n) {
                int32_t w = (int32_t)xrand(&v->rnd) >> 16, env;
                if (v->src == N_METAL || v->src == N_METALW) {
                    uint32_t j;
                    int32_t m = 0;
                    for (j = 0; j < 6u; j++) {
                        v->mph[j] += v->minc[j];
                        m += (int32_t)v->mph[j] < 0 ? -1 : 1;
                    }
                    w = v->src == N_METAL ? m * 5000 : m * 4000 + (w >> 2);
                }
                if (v->hpa) {                  /* one-pole high-pass before the filter */
                    v->hpy = mulq15(v->hpa, v->hpy + w - v->hpx);
                    v->hpx = w;
                    w = v->hpy;
                }
                if (v->src == N_CLAP) {
                    uint32_t sp = 441u, k = v->t / sp;
                    if (k < 3u) {
                        if (v->t % sp == 0u)
                            v->ck = 1 << 30;   /* (the clap reuses ck as its burst envelope) */
                        env = v->ck;
                        v->ck = mulq30(v->ck, v->clap_k);
                    } else {
                        env = (int32_t)(((int64_t)v->n * 23000) >> 15);   /* the tail, -3 dB */
                        v->n = mulq30(v->n, v->nk);
                    }
                } else {
                    env = v->n;
                    if (v->nhold)
                        v->nhold--;
                    else
                        v->n = mulq30(v->n, v->nk);
                }
                nz = mulq15(w, mulq15(v->nlvl, env >> 15));
            }
            /* filter */
            if (v->ft) {
                if (v->ftall)
                    x = svf_run(&v->f, tone + nz, v->ft);
                else
                    x = tone + svf_run(&v->f, nz, v->ft);
            } else {
                x = tone + nz;
            }
            /* click */
            if (v->src != N_CLAP && v->ck) {
                int32_t w = (int32_t)xrand(&v->rnd) >> 17;
                x += mulq15(w + (v->t < 8u ? 12000 : 0), mulq15(v->cklvl, v->ck >> 15));
                v->ck = mulq30(v->ck, v->ckk);
            }
            if (v->drive)
                x = soft_clip((x * v->drive) >> 8);
            if (s->crush) {
                if (v->shc == 0u)
                    v->shv = (int32_t)((uint32_t)(x + (int32_t)(~v->bitmask >> 1)) & v->bitmask);   /* rounded: no DC */
                x = v->shv;
                if (++v->shc > v->shn)
                    v->shc = 0;
            }
            x = mulq15(x, v->gain);
            out[i] += x;
            v->last = x;
            if (x > peak)
                peak = x;
            else if (-x > peak)
                peak = -x;
            v->t++;
            v->inc = (uint32_t)((int32_t)v->inc + dinc);
            v->pe = mulq30(v->pe, v->pek);
        }
        v->inc = inc1;
        if (v->a < 3000 && v->n < 3000 && v->a2 < 3000 && v->ck < 3000 && v->t > 64u)
            v->on = 0;                         /* below -110 dB */
    } else {
        for (i = 0; i < n; i++) {
            uint32_t k = v->pos >> 16;
            int32_t x;
            if (v->pos >= v->plen) {
                v->on = 0;
                break;
            }
            x = ULAW[v->pd[k]];
            if (!v->zoh && k + 1u < (v->plen >> 16)) {
                int32_t y = ULAW[v->pd[k + 1u]];
                x += (int32_t)(((int64_t)(y - x) * (v->pos & 0xFFFFu)) >> 16);
            }
            x = (int32_t)((uint32_t)(x + (int32_t)(~v->bitmask >> 1)) & v->bitmask);
            if (v->ft)
                x = svf_run(&v->f, x, v->ft);
            if (v->drive)
                x = soft_clip((x * v->drive) >> 8);
            x = mulq15(mulq15(x, v->a >> 15), v->gain);
            if (v->ahold)
                v->ahold--;
            else
                v->a = mulq30(v->a, v->ak);
            out[i] += x;
            v->last = x;
            if (x > peak)
                peak = x;
            else if (-x > peak)
                peak = -x;
            v->pos += v->pinc;
            v->t++;
        }
        if (v->a < 3000)
            v->on = 0;
    }
    v->meter = peak;
}

/* ---------------------------------------------------------------- FX --- */
/* Reverb: a Schroeder / Moorer network (Freeverb tunings) per side, damped combs, two allpasses.
 * TYPE sets the room scale (the comb lengths read shorter) and the damping; TIME the feedback. */
#define RV_NC 4u
#define RV_NA 2u
static const uint16_t RV_COMB[RV_NC] = {1116, 1277, 1422, 1557};
static const uint16_t RV_AP[RV_NA] = {556, 341};
#define RV_SPREAD 23u
#define RV_CBUF (1116 + 1277 + 1422 + 1557 + 4 * RV_SPREAD)
#define RV_ABUF (556 + 341 + 2 * RV_SPREAD)
static int16_t rv_comb[2][RV_CBUF] PZ_POOL;
static int16_t rv_ap[2][RV_ABUF] PZ_POOL;
typedef struct {
    uint16_t len[2][RV_NC], pos[2][RV_NC], alen[2][RV_NA], apos[2][RV_NA];
    uint16_t off[2][RV_NC], aoff[2][RV_NA];
    int32_t lp[2][RV_NC];
    int32_t fb, damp;
    uint8_t type, time;
    int32_t hp;                                /* input high-pass state */
} reverb_t;
static reverb_t rv;

static void reverb_config(uint32_t type, uint32_t time)
{
    static const uint8_t SCALE[REV_N] = {55, 100, 78};     /* % of the Freeverb lengths */
    static const uint8_t DAMP[REV_N] = {50, 30, 12};       /* % high damping */
    uint32_t c, k, o;
    if (type >= REV_N)
        type = REV_HALL;
    if (rv.type != type || !rv.len[0][0]) {
        for (c = 0; c < 2u; c++) {
            o = 0;
            for (k = 0; k < RV_NC; k++) {
                rv.off[c][k] = (uint16_t)o;
                rv.len[c][k] = (uint16_t)((RV_COMB[k] + c * RV_SPREAD) * SCALE[type] / 100u);
                rv.pos[c][k] = 0;
                rv.lp[c][k] = 0;
                o += RV_COMB[k] + RV_SPREAD;
            }
            o = 0;
            for (k = 0; k < RV_NA; k++) {
                rv.aoff[c][k] = (uint16_t)o;
                rv.alen[c][k] = (uint16_t)(RV_AP[k] + c * RV_SPREAD);
                rv.apos[c][k] = 0;
                o += RV_AP[k] + RV_SPREAD;
            }
        }
        for (c = 0; c < 2u; c++) {
            for (k = 0; k < RV_CBUF; k++)
                rv_comb[c][k] = 0;
            for (k = 0; k < RV_ABUF; k++)
                rv_ap[c][k] = 0;
        }
    }
    rv.type = (uint8_t)type;
    rv.time = (uint8_t)time;
    rv.fb = 22000 + (int32_t)time * 82;        /* 0.67 .. 0.99 Q15 */
    if (rv.fb > 32400)
        rv.fb = 32400;
    rv.damp = DAMP[type] * 327;
}

static inline void reverb_run(int32_t in, int32_t *ol, int32_t *or_)
{
    uint32_t c, k;
    int32_t outc[2];
    in >>= 2;                                  /* headroom: 8 combs sum */
    for (c = 0; c < 2u; c++) {
        int32_t acc = 0;
        for (k = 0; k < RV_NC; k++) {
            int16_t *b = &rv_comb[c][rv.off[c][k]];
            uint32_t p = rv.pos[c][k];
            int32_t y = b[p];
            rv.lp[c][k] = y + mulq15(rv.lp[c][k] - y, rv.damp);
            b[p] = (int16_t)clampi(in + mulq15(rv.lp[c][k], rv.fb), -32768, 32767);
            if (++p >= rv.len[c][k])
                p = 0;
            rv.pos[c][k] = (uint16_t)p;
            acc += y;
        }
        for (k = 0; k < RV_NA; k++) {
            int16_t *b = &rv_ap[c][rv.aoff[c][k]];
            uint32_t p = rv.apos[c][k];
            int32_t bo = b[p];
            int32_t y = bo - acc;
            b[p] = (int16_t)clampi(acc + (bo >> 1), -32768, 32767);
            if (++p >= rv.alen[c][k])
                p = 0;
            rv.apos[c][k] = (uint16_t)p;
            acc = y;
        }
        outc[c] = acc;
    }
    *ol += outc[0];
    *or_ += outc[1];
}

/* Delay: mono line (0.74 s), tempo-synced TIME, feedback through a damping low-pass,
 * output to both sides with the repeats alternating a little (wide). */
#define DLY_N 32768u
static int16_t dly_buf[DLY_N] PZ_POOL;
static struct {
    uint32_t w;
    uint32_t len, len_to;                      /* samples; glides to the new time */
    int32_t fb, lp;
} dl;
/* delay TIME choices (the TR-8S knob is free; synced divisions read better on this screen) */
#define DT_N 12u
static const char *const DT_NAME[DT_N] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D",
                                          "1/4", "1/2T", "1/4D", "1/2"};
static const uint16_t DT_TICKS[DT_N] = {120, 160, 240, 320, 360, 480, 640, 720, 960, 1280, 1440, 1920};

static void delay_config(uint32_t time_idx, uint32_t fb, uint32_t tempo10)
{
    uint32_t smp = (uint32_t)DT_TICKS[time_idx % DT_N] * (PZ_FS * 600u / PZ_PPQ * 2u) / (2u * tempo10);
    while (smp >= DLY_N - 64u)
        smp >>= 1;                             /* longer than the line: an octave shorter */
    dl.len_to = smp;
    if (!dl.len)
        dl.len = dl.len_to;
    dl.fb = (int32_t)fb * 240;                 /* 0 .. 0.93 */
}

static inline int32_t delay_run(int32_t in)
{
    uint32_t r;
    int32_t y;
    if (dl.len != dl.len_to)
        dl.len += dl.len < dl.len_to ? 1u : (uint32_t)-1;
    r = (dl.w - dl.len) & (DLY_N - 1u);
    y = dly_buf[r];
    dl.lp += (y - dl.lp) >> 2;
    dly_buf[dl.w] = (int16_t)clampi(in + mulq15(dl.lp, dl.fb), -32768, 32767);
    dl.w = (dl.w + 1u) & (DLY_N - 1u);
    return y;
}

/* master effect (TR-8S MASTER FX: one effect, ON and CTRL) */
static const char *const MFX_NAME[MFX_N] = {"FILTER", "DRIVE", "CRUSH", "COMP", "LO-FI", "ISOLATR"};
static struct {
    svf_t f[2], g[2];
    int32_t env, sh[2];
    uint32_t shc;
    int32_t xf;                                /* on / off crossfade, Q15 */
} mfx;

static void mfx_run(int32_t *l, int32_t *r, uint32_t n, uint32_t type, uint32_t ctrl, uint32_t on)
{
    uint32_t i, c;
    int32_t target = on ? 32767 : 0;
    int32_t cc = (int32_t)ctrl - 64;
    uint32_t mode = F_OFF;
    if (mfx.xf == 0 && !on)
        return;
    if (type == MFX_FILTER || type == MFX_LOFI || type == MFX_ISO) {
        int32_t cut = 127, res = 40;
        if (type == MFX_FILTER) {
            mode = cc < 0 ? F_LP : F_HP;
            cut = cc < 0 ? 127 + cc * 2 : cc * 2 - 4;
            if (cc == 0)
                mode = F_OFF;
        } else if (type == MFX_LOFI) {
            mode = F_LP;
            cut = 108 - (int32_t)ctrl / 4;
            res = 20;
        } else {
            mode = F_BP;
            cut = 30 + (int32_t)ctrl * 3 / 4;
            res = 60;
        }
        for (c = 0; c < 2u; c++)
            svf_set(&mfx.f[c], cut, res);
    }
    for (i = 0; i < n; i++) {
        int32_t x[2] = {l[i], r[i]}, y[2];
        for (c = 0; c < 2u; c++) {
            int32_t v = x[c];
            switch (type) {
            case MFX_FILTER:
                if (mode)
                    v = svf_run(&mfx.f[c], v, mode);
                break;
            case MFX_DRIVE:
                v = soft_clip(clampi(v * (256 + (int32_t)ctrl * 12) >> 8, -65536, 65536)) * 3 / 4;
                break;
            case MFX_CRUSH: {
                uint32_t bits = 16u - ctrl / 11u, hold = ctrl / 16u;
                if (c == 0u && ++mfx.shc > hold)
                    mfx.shc = 0;
                if (mfx.shc == 0u)
                    mfx.sh[c] = (int32_t)((uint32_t)v & ~((1u << (16u - bits)) - 1u));
                v = mfx.sh[c];
                break;
            }
            case MFX_COMP:                     /* gain applied below */
                break;
            case MFX_LOFI:
                v = svf_run(&mfx.f[c], v, F_LP);
                if (c == 0u && ++mfx.shc > 2u)
                    mfx.shc = 0;
                if (mfx.shc == 0u)
                    mfx.sh[c] = v & ~0x7F;
                v = mfx.sh[c];
                break;
            case MFX_ISO:
                v = v - svf_run(&mfx.f[c], v, F_BP);   /* a notch sweeping through the mix */
                break;
            }
            y[c] = v;
        }
        if (type == MFX_COMP) {                /* peak compressor: CTRL = amount (threshold and makeup) */
            int32_t a = y[0] > 0 ? y[0] : -y[0], b = y[1] > 0 ? y[1] : -y[1], pk = a > b ? a : b;
            int32_t thr = 32767 - (int32_t)ctrl * 200, g;
            mfx.env = pk > mfx.env ? mfx.env + ((pk - mfx.env) >> 4) : mfx.env - (mfx.env >> 11);
            g = mfx.env > thr ? (int32_t)(((int64_t)thr << 15) / mfx.env) : 32767;
            g = (int32_t)(((int64_t)g * (32768 + (int32_t)ctrl * 160)) >> 15);
            y[0] = mulq15(y[0], g);
            y[1] = mulq15(y[1], g);
        }
        if (mfx.xf != target)
            mfx.xf += mfx.xf < target ? 64 : -64;
        if (mfx.xf < 0)
            mfx.xf = 0;
        if (mfx.xf > 32767)
            mfx.xf = 32767;
        l[i] = x[0] + mulq15(y[0] - x[0], mfx.xf);
        r[i] = x[1] + mulq15(y[1] - x[1], mfx.xf);
    }
}
