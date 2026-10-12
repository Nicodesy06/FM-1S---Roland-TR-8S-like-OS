/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* PULSAR-8 core definitions: tracks, steps, patterns, kits, instruments.
 *
 * The application is hardware-independent C: the target glue (pulsar.c) and the host simulator
 * (host/sim.c) both include it as one compilation unit and provide the platform hooks declared at
 * the bottom of this file. Everything is fixed point (the pi32v2 has no FPU). */
#pragma once
#include <stdint.h>

#define PZ_FS 44100u             /* nominal; the codec clock runs at ~44118 Hz (FM-1-RE) */
#define PZ_CTL 32u               /* audio block: control-rate updates every 32 samples */
#define PZ_NTRK 8u               /* sequencer tracks = voices (each instrument is monophonic, as on a TR) */
#define PZ_NSTEP 16u             /* steps per variation (TR-8S / TR-1000: 16, longer by chaining) */
#define PZ_NVAR 8u               /* variations A..H */
#define PZ_NSEC 10u              /* A..H + FILL IN 1, FILL IN 2 */
#define PZ_SEC_FILL1 8u
#define PZ_SEC_FILL2 9u
#define PZ_NPAT 16u              /* patterns in memory and in flash */
#define PZ_NKIT 16u              /* kit slots (factory kits are the defaults) */
#define PZ_NMOTION 384u          /* motion entries per pattern (sparse) */
#define PZ_CHAIN_MAX 16u
#define PZ_NSNAP 8u              /* snapshots (scenes) per pattern */
#ifndef NELEM
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* compiler barrier: data before the index */
#endif

/* ---------------------------------------------------------------- time --- */
/* Ticks: 960 per quarter note. A 1/16 step is 240 ticks, 1/32 = 120, 1/16T = 160, 1/8T = 320.
 * MIDI clock (24 PPQN) = 40 ticks. The transport keeps the position as Q32.32 ticks. */
#define PZ_PPQ 960u
enum { SC_16, SC_32, SC_16T, SC_8T, SC_N };          /* pattern SCALE (TR-8S PTN SETTING: Scale) */
static const uint16_t SCALE_TICKS[SC_N] = {240, 120, 160, 320};

/* ---------------------------------------------------------------- steps --- */
#define ST_ON 0x01u
#define ST_WEAK 0x02u            /* WEAK BEAT: a ghost note */
#define ST_FLAM 0x04u
#define ST_ALT 0x08u             /* ALT INST: the instrument's alternate voicing */
/* sub: low 2 bits = division (0 none, 1 = 1/2, 2 = 1/3, 3 = 1/4); bits 4..7 = which sub steps sound
 * (bit i = sub step i; 0 is read as "all", the TR-8S behaviour; TR-1000 triplet / quadruplet
 * patterns are masks) */
typedef struct {
    uint8_t f;                   /* ST_* */
    uint8_t sub;
    uint8_t vel;                 /* 0 = default (TR-REC DEF), 1..127 */
    uint8_t prob;                /* 0 = 100 %, 1..99 % */
    int8_t micro;                /* start timing, -50..+50 % of a step (TR-1000 off-grid) */
    uint8_t cyc;                 /* note cycle condition (CYC_*) */
} step_t;

/* note cycle (TR-1000 CYCLE) + fill conditions: 0 always; 1..35 A:B (B = 2..8, A = 1..B);
 * then 1ST, NOT 1ST, FILL, NOT FILL */
#define CYC_AB_N 35u
enum { CYC_ALWAYS = 0, CYC_FIRST = 36, CYC_NFIRST, CYC_FILL, CYC_NFILL, CYC_N };

enum { DIR_FWD, DIR_BWD, DIR_PP, DIR_RL, DIR_LR, DIR_RND, DIR_DNK, DIR_N };

typedef struct {                 /* per track, shared by all variations (TR-8S track LAST is shared) */
    uint8_t last;                /* 0 = follow the variation's last step, else 1..16 */
    uint8_t dir;                 /* DIR_* (TR-1000 DIRECTION) */
    int8_t rotate;               /* -8..8 (TR-1000 ROTATE) */
    int8_t start;                /* -50..50 % of a step (TR-1000 START) */
    int8_t shuffle;              /* -100..100, scaled by the pattern shuffle (TR-1000 per-track SHUFFLE) */
    uint8_t rsv[3];
} trkset_t;

typedef struct {                 /* motion (TR-8S MOTION, TR-1000 parameter per step), sparse */
    uint8_t where;               /* section << 4 | step; 0xFF = free */
    uint8_t what;                /* track << 4 | MP_*  (track 8 = the effects) */
    uint8_t val;
    uint8_t rsv;
} motion_t;
enum { MP_TUNE, MP_DECAY, MP_CTRL, MP_LEVEL, MP_N };
enum { MF_REVLVL, MF_DLYLVL, MF_DLYTIME, MF_DLYFB, MF_MFXCTRL, MF_N };   /* track 8 */
#define MOTION_FX 8u

typedef struct {                 /* a snapshot: the four knobs of every track (TR-1000 SNAPSHOT) */
    uint8_t used;
    uint8_t v[PZ_NTRK][MP_N];
} snap_t;

#define PZ_NAME 12u
typedef struct {
    uint32_t magic;
    char name[PZ_NAME];
    uint16_t tempo10;            /* 40.0 .. 300.0 BPM x 10 */
    int8_t shuffle;              /* -100..100 */
    uint8_t scale;               /* SC_* */
    uint8_t flam;                /* flam spacing 0..8 */
    uint8_t pkit;                /* kit used by the pattern (with kit_link) */
    uint8_t kit_link;            /* 1: selecting the pattern selects its kit */
    int8_t mprob;                /* master probability -100..100 */
    uint8_t last[PZ_NSEC];       /* each variation's last step 1..16 */
    uint8_t acc_lvl;             /* accent level 0..127 */
    uint8_t autofill;            /* index into AUTOFILL_BARS, 0 = off */
    uint8_t filltype;            /* 0 FILL IN 1, 1 FILL IN 2, 2 SCATTER */
    uint8_t rsv[5];
    trkset_t trk[PZ_NTRK];
    step_t s[PZ_NSEC][PZ_NTRK][PZ_NSTEP];
    uint8_t acc[PZ_NSEC][PZ_NSTEP];   /* the accent track: 0 off, else on */
    motion_t mo[PZ_NMOTION];
    snap_t snap[PZ_NSNAP];
} pattern_t;
#define PAT_MAGIC 0x31544150u    /* "PAT1" */

/* ---------------------------------------------------------- instruments --- */
enum { CAT_BD, CAT_SD, CAT_TOM, CAT_RS, CAT_CP, CAT_HH, CAT_OH, CAT_CY, CAT_PC, CAT_N };

typedef struct {                 /* one kit track: what plays and how */
    uint8_t inst;                /* index into INST[] */
    int8_t tune;                 /* -64..63 */
    uint8_t decay, ctrl, level;  /* 0..127 */
    int8_t pan;                  /* -64..63 */
    uint8_t rev, dly;            /* sends 0..127 */
    uint8_t color;               /* track colour index (TR-8S KIT: COLOR) */
    uint8_t rsv[3];
} kittrk_t;

typedef struct {
    uint32_t magic;
    char name[PZ_NAME];
    kittrk_t t[PZ_NTRK];
    uint8_t rev_type, rev_lvl, rev_time;     /* reverb: ROOM / HALL / PLATE, level, time */
    uint8_t dly_lvl, dly_time, dly_fb;       /* delay: level, time (synced index), feedback */
    uint8_t mfx_type, mfx_ctrl, mfx_on;      /* master FX */
    uint8_t choke;                           /* bit t: track t is choked by track t - 1 (CH -> OH) */
    uint8_t rsv[6];
} kit_t;
#define KIT_MAGIC 0x3154494Bu    /* "KIT1" */

enum { MFX_FILTER, MFX_DRIVE, MFX_CRUSH, MFX_COMP, MFX_LOFI, MFX_ISO, MFX_N };
enum { REV_ROOM, REV_HALL, REV_PLATE, REV_N };

/* --------------------------------------------------------------- hooks --- */
/* provided by the target glue or the host simulator */
static uint32_t plat_ms(void);
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);   /* px: RGB565 (may be
                                                                                              * modified until plat_sync) */
static void plat_sync(void);                             /* the last blit is done with its pixels */
static void plat_led(uint32_t id, uint32_t level);       /* id: PZ_LED_*; level 0 off, 1 dim, 2 lit */
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len);   /* from the audio ISR */
static int plat_flash_read(uint32_t off, void *dst, uint32_t n);
static int plat_flash_erase(uint32_t off);
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n);
