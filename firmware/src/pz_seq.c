/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* PULSAR-8 sequencer and mixer (runs in the audio interrupt, block by block).
 *
 * Time is kept in ticks (960 per quarter) as Q32.32, advanced per sample from the tempo or
 * slaved to MIDI clock. Every 1/16 (or the pattern's SCALE) a grid step falls; each track
 * evaluates its own step half a grid step ahead, so steps played early (START, micro timing,
 * flam grace notes, negative shuffle) can still be scheduled, then fires its hits at the exact
 * sample inside the block (voices are rendered in segments around the hit).
 *
 *   pattern: variations A..H + FILL IN 1 / 2, each 1..16 steps (its LAST)
 *   playback: a chain of variations (TR-8S: the lit ones in A..H order; TR-1000: any order),
 *             a fill-in on demand (FILL) or every N bars (AUTO FILL), scatter
 *   track: own LAST (polymeter), DIRECTION, ROTATE, START, SHUFFLE (TR-1000)
 *   step: sub steps 1/2 1/3 1/4 (with a mask: TR-1000 triplet / quadruplet patterns), flam,
 *         weak beat, alt inst, velocity, probability, note cycle A:B / 1ST / FILL, micro timing
 *   accent track, motion (per step knob values), step loop, rolls, mute / solo */

/* ---------------------------------------------------------- state --- */
static pattern_t pats[PZ_NPAT] PZ_POOL;
static kit_t kits[PZ_NKIT];                    /* the kit store (RAM copy of flash) */
static uint8_t kit_cur;                        /* the kit playing: edited in place in kits[] ('*' when changed) */
#define kit (kits[kit_cur])

typedef struct {
    uint8_t theme;                             /* 0 NOIR (TR-8S style), 1 ALLOY (TR-1000 style) */
    uint8_t sync;                              /* 0 INT, 1 USB, 2 TRS MIDI */
    uint8_t clk_out;                           /* MIDI clock + start / stop out on USB */
    uint8_t note_out;                          /* the steps as notes on USB MIDI */
    uint8_t midi_ch;                           /* 0..15 (10 = 9) */
    uint8_t tempo_src;                         /* 0 SYSTEM, 1 PATTERN */
    uint8_t audition;                          /* track select plays the sound (stopped) */
    uint8_t autosave;                          /* write edited patterns / kit when stopped and idle */
    uint8_t led;                               /* key LED brightness 0..2 */
    uint8_t quant;                             /* INST REC quantize: 1 on (to the step), 0 off (keeps micro) */
    uint8_t usb_full;                          /* USB audio: 0 follows VOLUME, 1 full level */
    uint8_t rsv[5];
    uint16_t tempo10;                          /* the system tempo */
    uint8_t pat, kitno;                        /* last pattern / kit (restored at power-on) */
} sys_t;
static sys_t sys = {0, 0, 1, 0, 9, 0, 1, 1, 2, 1, 0, {0}, 1200, 0, 0};

/* the notes of the tracks (MIDI in and out), TR-8S default map order */
static const uint8_t TRK_NOTE[PZ_NTRK] = {36, 38, 43, 50, 39, 37, 42, 46};

typedef struct {
    uint64_t t;                                /* tick (Q32.32) */
    hit_t h;
} ev_t;
#define EVQ 16u

typedef struct {
    /* transport */
    volatile uint8_t playing;
    uint8_t pat;                               /* pattern playing */
    volatile uint8_t pat_next;                 /* 0xFF none: switches at the end of the cycle */
    uint8_t pat_lo, pat_hi;                    /* pattern chain (two pads at once) */
    uint64_t pos, inc;                         /* ticks Q32.32 and per sample */
    uint32_t gstep;                            /* grid steps evaluated (the next to evaluate) */
    /* variations */
    uint8_t chain[PZ_CHAIN_MAX], chain_n, chain_i;
    uint8_t sec;                               /* the section playing (A..H, FILL 1/2) */
    uint8_t sec_step;                          /* its step 0..last-1 */
    uint8_t rec_sec;                           /* TR-REC target */
    uint16_t cyc[PZ_NSEC];                     /* cycles played per section (note cycle) */
    uint32_t bars;                             /* cycles played since PLAY (auto fill) */
    volatile uint8_t fill_req;                 /* manual trig: the rest of this cycle is the fill */
    uint8_t fill_on, fill_sec, fill_back;      /* fill playing; its section; the section to return to */
    /* tracks */
    uint16_t tcnt[PZ_NTRK];                    /* steps since the track's loop started */
    uint8_t tpos[PZ_NTRK];                     /* the step each track plays now (UI) */
    uint8_t pp_dir[PZ_NTRK];
    uint32_t rnd;
    ev_t evq[PZ_NTRK][EVQ];
    uint8_t ev_n[PZ_NTRK];
    uint32_t rskip[PZ_NTRK];                   /* live recording: skip this grid step's hit (sounded live) */
    /* performance */
    volatile uint16_t mute, solo;
    volatile uint8_t loop_step;                /* STEP LOOP: the step held (0xFF none) */
    volatile uint8_t loop_div;                 /* 1, 2, 4 */
    volatile uint8_t roll_trk;                 /* bit per track rolling */
    volatile uint8_t roll_div;                 /* 1 = 1/16, 2 = 1/32, 4 = 1/64 */
    uint8_t motion_on, motion_rec;
    /* clock */
    uint32_t clk_next;                         /* the next MIDI clock tick (40 ticks) */
    uint32_t ext_clocks;                       /* MIDI clocks received since START */
    uint32_t ext_period;                       /* samples per clock (smoothed, Q8) */
    uint32_t ext_last;                         /* sample counter at the last clock */
    uint8_t ext_run;
    uint32_t samples;                          /* sample counter */
    /* the live (motion / knob) values */
    uint8_t mval[PZ_NTRK][MP_N];               /* motion override for the step playing, 0xFF none */
    uint8_t fxval[MF_N];
    /* metering */
    uint8_t hit_flash[PZ_NTRK];                /* UI: frames to show a hit */
    uint8_t beat;                              /* toggles on every quarter (UI: tempo LED) */
    uint32_t cpu;                              /* load estimate, 1/256 */
    volatile uint8_t req;                      /* from the UI: TP_* (the audio interrupt acts on it) */
    volatile uint8_t morph;                    /* SNAPSHOT / MORPH: 0 = the kit, 127 = snapshot morph_to */
    volatile uint8_t morph_to;
    volatile uint16_t overload;                /* blocks left with the reverb bypassed (the render ran late) */
} seq_t;
enum { TP_NONE, TP_START, TP_STOP, TP_RESTART, TP_CONT };
static seq_t sq;

#define PAT (&pats[sq.pat])

static uint32_t sq_rand(void) { return xrand(&sq.rnd); }

/* ------------------------------------------------------------ pattern ops --- */
static void pat_init(pattern_t *p, uint32_t n)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)p;
    for (i = 0; i < sizeof *p; i++)
        b[i] = 0;
    p->magic = PAT_MAGIC;
    p->name[0] = 'P';
    p->name[1] = 'T';
    p->name[2] = 'N';
    p->name[3] = ' ';
    p->name[4] = (char)('0' + (n + 1u) / 10u);
    p->name[5] = (char)('0' + (n + 1u) % 10u);
    p->tempo10 = 1200;
    p->scale = SC_16;
    p->flam = 3;
    p->pkit = (uint8_t)n;
    p->acc_lvl = 64;
    for (i = 0; i < PZ_NSEC; i++)
        p->last[i] = 16;
    for (i = 0; i < PZ_NMOTION; i++)
        p->mo[i].where = 0xFF;
}

static uint32_t pat_sec_used(const pattern_t *p, uint32_t sec)
{
    uint32_t t, s;
    for (t = 0; t < PZ_NTRK; t++)
        for (s = 0; s < PZ_NSTEP; s++)
            if (p->s[sec][t][s].f & ST_ON)
                return 1;
    return 0;
}

/* --------------------------------------------------------------- motion --- */
static int motion_find(const pattern_t *p, uint32_t sec, uint32_t step, uint32_t trk, uint32_t par)
{
    uint32_t i, w = sec << 4 | step, wh = trk << 4 | par;
    for (i = 0; i < PZ_NMOTION; i++)
        if (p->mo[i].where == w && p->mo[i].what == wh)
            return (int)i;
    return -1;
}
static int motion_set(pattern_t *p, uint32_t sec, uint32_t step, uint32_t trk, uint32_t par, uint32_t val)
{
    int i = motion_find(p, sec, step, trk, par);
    uint32_t k;
    if (i < 0)
        for (k = 0; k < PZ_NMOTION; k++)
            if (p->mo[k].where == 0xFF) {
                i = (int)k;
                break;
            }
    if (i < 0)
        return -1;                             /* MOTION FULL */
    p->mo[i].what = (uint8_t)(trk << 4 | par);
    p->mo[i].val = (uint8_t)val;
    RING_PUBLISH();
    p->mo[i].where = (uint8_t)(sec << 4 | step);
    return 0;
}
static void motion_clear(pattern_t *p, int sec, int step, int trk)   /* -1 = any */
{
    uint32_t i;
    for (i = 0; i < PZ_NMOTION; i++) {
        motion_t *m = &p->mo[i];
        if (m->where == 0xFF)
            continue;
        if ((sec < 0 || (m->where >> 4) == (uint32_t)sec) && (step < 0 || (m->where & 15u) == (uint32_t)step) &&
            (trk < 0 || (m->what >> 4) == (uint32_t)trk))
            m->where = 0xFF;
    }
}
static uint32_t motion_count(const pattern_t *p)
{
    uint32_t i, n = 0;
    for (i = 0; i < PZ_NMOTION; i++)
        n += p->mo[i].where != 0xFF;
    return n;
}

/* ------------------------------------------------------------ transport --- */
static uint32_t step_ticks(void) { return SCALE_TICKS[PAT->scale % SC_N]; }

static uint16_t tempo_now(void) { return sys.tempo_src ? PAT->tempo10 : sys.tempo10; }

static void tempo_apply(void)
{
    uint32_t t = tempo_now();
    if (t < 200u)
        t = 200u;
    if (t > 3000u)
        t = 3000u;
    sq.inc = ((uint64_t)t * 39891578u) >> 8;   /* t x PPQ x 2^32 / (600 x FS), in Q8 */
}

static void chain_default(void)                /* variation A */
{
    sq.chain[0] = 0;
    sq.chain_n = 1;
    sq.chain_i = 0;
}

static uint32_t sec_last(uint32_t sec)
{
    uint32_t l = PAT->last[sec];
    return l >= 1u && l <= PZ_NSTEP ? l : PZ_NSTEP;
}

static void tracks_reset(void)
{
    uint32_t t;
    for (t = 0; t < PZ_NTRK; t++) {
        sq.tcnt[t] = 0;
        sq.pp_dir[t] = 0;
    }
}

static void seq_start(void)
{
    uint32_t t, k;
    sq.pos = 0;
    sq.gstep = 0;
    sq.chain_i = 0;
    sq.sec = sq.chain[0];
    sq.sec_step = 0;
    sq.bars = 0;
    sq.fill_on = 0;
    sq.fill_req = 0;
    sq.clk_next = 0;
    for (k = 0; k < PZ_NSEC; k++)
        sq.cyc[k] = 0;
    for (t = 0; t < PZ_NTRK; t++) {
        sq.ev_n[t] = 0;
        sq.rskip[t] = 0xFFFFFFFFu;
        for (k = 0; k < MP_N; k++)
            sq.mval[t][k] = 0xFF;
    }
    for (k = 0; k < MF_N; k++)
        sq.fxval[k] = 0xFF;
    tracks_reset();
    tempo_apply();
    sq.playing = 1;
}

static void seq_init(void)                     /* power-on: nothing plays, no motion in force */
{
    uint32_t t, k;
    sq.pat_next = 0xFF;
    sq.loop_step = 0xFF;
    sq.loop_div = 1;
    sq.roll_div = 1;
    sq.rnd = 0x9E3779B9u;
    for (t = 0; t < PZ_NTRK; t++) {
        sq.rskip[t] = 0xFFFFFFFFu;
        for (k = 0; k < MP_N; k++)
            sq.mval[t][k] = 0xFF;
    }
    for (k = 0; k < MF_N; k++)
        sq.fxval[k] = 0xFF;
    chain_default();
    tempo_apply();
}

static void seq_stop(void)
{
    uint32_t t, k;
    sq.playing = 0;
    for (t = 0; t < PZ_NTRK; t++) {
        sq.ev_n[t] = 0;
        for (k = 0; k < MP_N; k++)
            sq.mval[t][k] = 0xFF;
    }
    for (k = 0; k < MF_N; k++)
        sq.fxval[k] = 0xFF;
}

/* ------------------------------------------------------------- the step --- */
/* the step position of track t for its n-th step since its loop start (DIRECTION, ROTATE) */
static uint32_t track_pos(uint32_t t, uint32_t n, uint32_t len)
{
    const trkset_t *ts = &PAT->trk[t];
    uint32_t p;
    switch (ts->dir) {
    default:
    case DIR_FWD:
        p = n % len;
        break;
    case DIR_BWD:
        p = len - 1u - n % len;
        break;
    case DIR_PP: {                             /* 1..L..1 without repeating the ends */
        uint32_t per = len > 1u ? 2u * len - 2u : 1u, k = n % per;
        p = k < len ? k : per - k;
        break;
    }
    case DIR_RL:                               /* 2 1 4 3 ... */
        p = (n % len) ^ 1u;
        if (p >= len)
            p = len - 1u;
        break;
    case DIR_LR:                               /* 15 16 13 14 ... */
        p = len - 1u - ((n % len) ^ 1u);
        if (p >= len)
            p = 0;
        break;
    case DIR_RND:
        p = sq_rand() % len;
        break;
    case DIR_DNK: {                            /* a drunk walk: one step either way */
        uint32_t cur = sq.tpos[t];
        if (n == 0u)
            p = 0;
        else if (sq_rand() & 1u)
            p = cur + 1u >= len ? cur : cur + 1u;
        else
            p = cur == 0u ? 0u : cur - 1u;
        break;
    }
    }
    if (ts->rotate) {                          /* ROTATE shifts the pattern: the step read moves the other way */
        int32_t r = ((int32_t)p - ts->rotate) % (int32_t)len;
        p = (uint32_t)(r < 0 ? r + (int32_t)len : r);
    }
    return p;
}

static int cond_pass(uint32_t cyc, uint32_t cycle, uint32_t fill)
{
    if (cyc == CYC_ALWAYS)
        return 1;
    if (cyc <= CYC_AB_N) {                     /* 1..35: A:B, B = 2..8 */
        uint32_t k = cyc - 1u, b = 2, a;
        while (k >= b) {
            k -= b;
            b++;
        }
        a = k + 1u;
        return cycle % b == a - 1u;
    }
    switch (cyc) {
    case CYC_FIRST:
        return cycle == 0u;
    case CYC_NFIRST:
        return cycle != 0u;
    case CYC_FILL:
        return fill != 0u;
    case CYC_NFILL:
        return fill == 0u;
    }
    return 1;
}
static void cond_name(uint32_t cyc, char *b)
{
    if (cyc == CYC_ALWAYS) {
        b[0] = '1', b[1] = ':', b[2] = '1', b[3] = 0;
        return;
    }
    if (cyc <= CYC_AB_N) {
        uint32_t k = cyc - 1u, bb = 2;
        while (k >= bb) {
            k -= bb;
            bb++;
        }
        b[0] = (char)('1' + k);
        b[1] = ':';
        b[2] = (char)('0' + bb);
        b[3] = 0;
        return;
    }
    {
        static const char *const N[] = {"1ST", "!1ST", "FILL", "!FILL"};
        const char *s = N[(cyc - CYC_FIRST) & 3u];
        while ((*b++ = *s++))
            ;
    }
}

static void ev_push(uint32_t t, int64_t when, const hit_t *h)
{
    uint32_t n = sq.ev_n[t];
    if (when < (int64_t)sq.pos)
        when = (int64_t)sq.pos;                /* (early hits at the very start: now) */
    if (n < EVQ) {
        sq.evq[t][n].t = (uint64_t)when;
        sq.evq[t][n].h = *h;
        sq.ev_n[t] = (uint8_t)(n + 1u);
    }
}

static int trk_muted(uint32_t t) { return ((sq.mute >> t) & 1u) || (sq.solo && !((sq.solo >> t) & 1u)); }

/* a knob value of track t (MP_*, 0..127) as the kit has it, morphed towards the snapshot (TR-1000) */
static uint32_t knob_val(uint32_t t, uint32_t par)
{
    const kittrk_t *k = &kit.t[t];
    int32_t a = par == MP_TUNE ? k->tune + 64 : par == MP_DECAY ? k->decay : par == MP_CTRL ? k->ctrl : k->level;
    const snap_t *sn = &PAT->snap[sq.morph_to % PZ_NSNAP];
    if (sq.morph && sn->used)
        a += ((int32_t)sn->v[t][par] - a) * sq.morph / 127;
    return (uint32_t)a;
}

/* the hit parameters of track t from the kit, any morph and any motion */
static void hit_make(hit_t *h, uint32_t t, uint32_t vel, uint32_t alt)
{
    const kittrk_t *k = &kit.t[t];
    h->inst = k->inst;
    h->alt = (uint8_t)alt;
    h->tune = (int8_t)((int32_t)(sq.mval[t][MP_TUNE] != 0xFF ? sq.mval[t][MP_TUNE] : knob_val(t, MP_TUNE)) - 64);
    h->decay = (uint8_t)(sq.mval[t][MP_DECAY] != 0xFF ? sq.mval[t][MP_DECAY] : knob_val(t, MP_DECAY));
    h->ctrl = (uint8_t)(sq.mval[t][MP_CTRL] != 0xFF ? sq.mval[t][MP_CTRL] : knob_val(t, MP_CTRL));
    h->vel = (uint8_t)(vel > 127u ? 127u : vel < 1u ? 1u : vel);
}

/* scatter (fill type 3): steps exchanged / reversed within the cycle (TR-8S SCATTER) */
static uint32_t scatter(uint32_t s, uint32_t cyc)
{
    switch (cyc & 3u) {
    case 0:
        return s ^ 1u;
    case 1:
        return (s & ~3u) | (3u - (s & 3u));
    case 2:
        return s & ~1u;
    default:
        return (s & 8u) ? 15u - s : s;
    }
}

/* evaluate grid step g for every track (called half a step before its time) */
static void eval_step(uint32_t g)
{
    pattern_t *p = PAT;
    uint32_t st = step_ticks(), t;
    uint32_t sec = sq.sec, sstep = sq.sec_step, last = sec_last(sec);
    int64_t grid = (int64_t)((uint64_t)g * st << 32);
    int32_t pshuf = p->shuffle;
    uint32_t loop = sq.loop_step;
    uint32_t fill = sq.fill_on;
    /* motion of the effects: on the variation step */
    if (sq.motion_on) {
        uint32_t k;
        for (k = 0; k < MF_N; k++) {
            int i = motion_find(p, sec, sstep, MOTION_FX, k);
            sq.fxval[k] = i >= 0 ? p->mo[i].val : 0xFF;
        }
    }
    for (t = 0; t < PZ_NTRK; t++) {
        const trkset_t *ts = &p->trk[t];
        uint32_t len = ts->last ? ts->last : last, pos;
        const step_t *s;
        int32_t off;
        uint32_t k, vel, div, mask, n;
        if (len > PZ_NSTEP)
            len = PZ_NSTEP;
        pos = track_pos(t, sq.tcnt[t], len);
        if (loop < PZ_NSTEP)
            pos = loop;                        /* STEP LOOP: every track plays the held step */
        else if (fill && p->filltype == 2u)
            pos = scatter(pos, sq.cyc[sec]) % len;
        sq.tpos[t] = (uint8_t)pos;
        sq.tcnt[t]++;
        /* motion of the track */
        for (k = 0; k < MP_N; k++) {
            int i = sq.motion_on ? motion_find(p, sec, pos, t, k) : -1;
            sq.mval[t][k] = i >= 0 ? p->mo[i].val : 0xFF;
        }
        if (sq.roll_trk & (1u << t))
            continue;                          /* the roll plays this track */
        s = &p->s[sec][t][pos];
        if (!(s->f & ST_ON) || trk_muted(t))
            continue;
        if (sq.rskip[t] == g) {                /* recorded live into this step: it sounded already */
            sq.rskip[t] = 0xFFFFFFFFu;
            continue;
        }
        if (!cond_pass(s->cyc, sq.cyc[sec], fill))
            continue;
        if (s->prob) {
            int32_t pr = (int32_t)s->prob + p->mprob;
            if (pr < 100 && (int32_t)(sq_rand() % 100u) >= pr)
                continue;
        }
        vel = s->vel ? s->vel : (s->f & ST_WEAK) ? 48u : 100u;
        if (p->acc[sec][loop < PZ_NSTEP ? loop : sstep])   /* the accent lane runs with the variation */
            vel += (127u - vel) * p->acc_lvl / 127u + p->acc_lvl / 8u;
        /* timing: START (track), micro (step), shuffle on the off-beat 16ths */
        off = ((int32_t)ts->start + s->micro) * (int32_t)st / 100;
        if (g & 1u) {
            int32_t sh = pshuf + ts->shuffle;
            sh = sh > 100 ? 100 : sh < -100 ? -100 : sh;
            off += sh * (int32_t)st / 200;
        }
        if (off < -(int32_t)st / 2)
            off = -(int32_t)st / 2;
        div = loop < PZ_NSTEP ? sq.loop_div : (s->sub & 3u) ? (s->sub & 3u) + 1u : 1u;
        mask = loop < PZ_NSTEP ? 15u : s->sub >> 4 ? s->sub >> 4 : 15u;
        {
            hit_t h;
            hit_make(&h, t, vel, (s->f & ST_ALT) != 0u);
            for (n = 0; n < div; n++) {
                if (!(mask & (1u << n)))
                    continue;
                ev_push(t, grid + ((int64_t)(off + (int32_t)(n * st / div)) << 32), &h);
            }
            if ((s->f & ST_FLAM) && div == 1u) {   /* grace note before the hit (TR-8S flam spacing) */
                hit_t gr = h;
                uint32_t ms = 3u + (uint32_t)p->flam * 3u;
                int64_t gap = (int64_t)(sq.inc * (PZ_FS / 1000u) * ms);
                gr.vel = (uint8_t)(h.vel * 2u / 3u);
                ev_push(t, grid + ((int64_t)off << 32) - gap, &gr);
            }
        }
    }
    /* advance the variation (also under STEP LOOP: the pattern runs on underneath, as on a TR) */
    if (++sq.sec_step >= last) {
        uint32_t was_fill = sq.fill_on, old = sq.sec;
        static const uint8_t AF[] = {0, 2, 4, 8, 12, 16, 32};
        uint32_t nbar = AF[p->autofill % NELEM(AF)];
        sq.sec_step = 0;
        sq.cyc[sec]++;
        sq.bars++;
        sq.fill_on = 0;
        if (sq.pat_next != 0xFF && sq.chain_i + 1u >= sq.chain_n) {
            sq.pat = sq.pat_next;              /* (the kit follows in the main loop) */
            sq.pat_next = 0xFF;
            chain_default();
            sq.sec = 0;
            p = PAT;
            tempo_apply();
            tracks_reset();
        } else {
            sq.chain_i = (uint8_t)((sq.chain_i + 1u) % sq.chain_n);
            sq.sec = sq.chain[sq.chain_i];
            if (sq.chain_i == 0u && sq.pat_hi != sq.pat_lo) {      /* pattern chain: the next pattern */
                sq.pat = (uint8_t)(sq.pat + 1u > sq.pat_hi || sq.pat < sq.pat_lo ? sq.pat_lo : sq.pat + 1u);
                p = PAT;
                sq.sec = sq.chain[0];
                tempo_apply();
                tracks_reset();
            } else if (sq.sec != old || was_fill) {
                tracks_reset();
            }
        }
        if (nbar && (sq.bars + 1u) % nbar == 0u)
            sq.fill_req = 1;                   /* AUTO FILL IN: the last bar of every N */
        if (sq.fill_req) {
            sq.fill_req = 0;
            sq.fill_on = 1;
            sq.fill_back = sq.sec;
            sq.fill_sec = p->filltype == 1u ? PZ_SEC_FILL2 : PZ_SEC_FILL1;
            if (p->filltype < 2u && pat_sec_used(p, sq.fill_sec)) {
                sq.sec = sq.fill_sec;
                tracks_reset();
            }
        }
    } else if (sq.fill_req) {
        /* MANUAL TRIG: the fill takes over now, for the rest of the cycle */
        sq.fill_req = 0;
        sq.fill_on = 1;
        sq.fill_back = sq.sec;
        sq.fill_sec = p->filltype == 1u ? PZ_SEC_FILL2 : PZ_SEC_FILL1;
        if (p->filltype < 2u && pat_sec_used(p, sq.fill_sec))
            sq.sec = sq.fill_sec;
    }
}

/* ------------------------------------------------------------- live input --- */
static volatile uint32_t live_req;             /* bit per track: trigger now (pads) */
static volatile uint8_t live_vel[PZ_NTRK];
static volatile uint8_t live_flam;

/* realtime record (INST REC) of a pad hit on track t: the nearest step of the record section */
static void rec_live(uint32_t t, uint32_t vel)
{
    pattern_t *p = PAT;
    uint32_t st = step_ticks(), last = sec_last(sq.rec_sec);
    uint32_t now = (uint32_t)(sq.pos >> 32);
    uint32_t g = (now + st / 2u) / st;         /* the nearest grid step */
    int32_t micro = (int32_t)now - (int32_t)(g * st), d = (int32_t)g - (int32_t)sq.gstep, pos;
    step_t *s;
    /* sec_step is the variation step of grid step gstep (the next to evaluate) */
    pos = ((int32_t)sq.sec_step + d) % (int32_t)last;
    if (pos < 0)
        pos += (int32_t)last;
    s = &p->s[sq.rec_sec][t][pos];
    s->f = (uint8_t)((s->f & ~ST_WEAK) | ST_ON);
    s->vel = (uint8_t)(vel == 100u ? 0u : vel);
    s->micro = sys.quant ? 0 : (int8_t)(micro * 100 / (int32_t)st);
    if (d >= 0)
        sq.rskip[t] = g;                       /* not evaluated yet: it sounded live, skip it once */
}

/* ------------------------------------------------------------------ MIDI --- */
static volatile uint32_t midi_rx[32];          /* raw 3-byte messages (status | d1 << 8 | d2 << 16) */
static volatile uint32_t mrx_w, mrx_r;
static void midi_in(uint32_t msg)              /* from the USB / UART pollers */
{
    if (mrx_w - mrx_r < NELEM(midi_rx)) {
        midi_rx[mrx_w % NELEM(midi_rx)] = msg;
        RING_PUBLISH();
        mrx_w++;
    }
}

static void midi_note_on(uint32_t note, uint32_t vel)
{
    uint32_t t;
    for (t = 0; t < PZ_NTRK; t++) {
        if (note == TRK_NOTE[t] || (t == 0u && note == 35u) || (t == 1u && note == 40u) ||
            (t == 2u && (note == 41u || note == 45u)) || (t == 3u && (note == 47u || note == 48u)) ||
            (t == 7u && (note == 49u || note == 51u))) {
            live_vel[t] = (uint8_t)vel;
            live_req |= 1u << t;
        }
    }
}

static void midi_service(void)                 /* audio ISR, once per block */
{
    while (mrx_r != mrx_w) {
        uint32_t m = midi_rx[mrx_r % NELEM(midi_rx)], st = m & 0xFFu;
        mrx_r++;
        if (st == 0xF8u) {                     /* clock */
            if (sys.sync) {
                uint32_t dt = sq.samples - sq.ext_last;
                sq.ext_last = sq.samples;
                if (dt > 50u && dt < 20000u)
                    sq.ext_period = sq.ext_period ? (sq.ext_period * 7u + (dt << 8)) / 8u : dt << 8;
                if (sq.playing)
                    sq.ext_clocks++;
            }
        } else if (st == 0xFAu || st == 0xFBu) {
            if (sys.sync) {
                if (st == 0xFAu) {
                    seq_start();
                    sq.ext_clocks = 0;
                } else {
                    sq.playing = 1;
                }
            }
        } else if (st == 0xFCu) {
            if (sys.sync)
                seq_stop();
        } else if ((st & 0xF0u) == 0x90u && (st & 15u) == sys.midi_ch && (m >> 16) & 0x7Fu) {
            midi_note_on((m >> 8) & 0x7Fu, (m >> 16) & 0x7Fu);
        }
    }
}

static void midi_send(uint8_t s, uint8_t a, uint8_t b, uint8_t n) { plat_midi_out(s, a, b, n); }

/* ---------------------------------------------------------------- mixer --- */
static int32_t trk_buf[PZ_CTL];
static int32_t lvl_g[PZ_NTRK];                 /* level gain, ramped */
static uint8_t note_off_pending;

static void fx_config(void)
{
    uint32_t rt = sq.fxval[MF_DLYTIME] != 0xFF ? sq.fxval[MF_DLYTIME] * DT_N / 128u : kit.dly_time;
    uint32_t fb = sq.fxval[MF_DLYFB] != 0xFF ? sq.fxval[MF_DLYFB] : kit.dly_fb;
    if (rv.type != kit.rev_type || rv.time != kit.rev_time || !rv.len[0][0])
        reverb_config(kit.rev_type, kit.rev_time);
    delay_config(rt, fb, tempo_now());
}

/* one block: events, voices, sends, effects, master. out: L R interleaved Q15 */
static void mix_block(int32_t *out, uint32_t n)
{
    static int32_t rev_in[PZ_CTL], dly_in[PZ_CTL];
    uint64_t end;
    uint32_t t, i;
    if (sq.req) {
        uint32_t r = sq.req;
        sq.req = TP_NONE;
        if (r == TP_STOP) {
            seq_stop();
            if (sys.clk_out && !sys.sync)
                midi_send(0xFC, 0, 0, 1);
        } else if (r == TP_CONT) {
            sq.playing = 1;
            if (sys.clk_out && !sys.sync)
                midi_send(0xFB, 0, 0, 1);
        } else {
            seq_start();
            if (sys.clk_out && !sys.sync)
                midi_send(0xFA, 0, 0, 1);
        }
    }
    midi_service();
    if (note_off_pending) {
        for (t = 0; t < PZ_NTRK; t++)
            if (note_off_pending & (1u << t))
                midi_send((uint8_t)(0x80u | sys.midi_ch), TRK_NOTE[t], 0, 3);
        note_off_pending = 0;
    }
    /* the transport */
    if (sq.playing) {
        uint64_t adv = sq.inc * n;
        if (sys.sync) {                        /* MIDI clock: follow its tempo, never run ahead of it */
            uint64_t lim = (uint64_t)sq.ext_clocks * 40u << 32;   /* the first clock after START is tick 0:
                                                                      * the k-th one allows up to tick 40 k */
            if (sq.ext_period)
                sq.inc = ((uint64_t)40u << 40) / sq.ext_period;   /* 40 ticks per clock period (Q8 samples) */
            adv = sq.inc * n;
            if (sq.pos + adv > lim)
                adv = sq.pos < lim ? lim - sq.pos : 0;
            if (sq.pos + (80ull << 32) < lim)
                adv = lim - sq.pos - (40ull << 32);   /* far behind (a dropped clock): catch up */
        }
        end = sq.pos + adv;
        /* evaluate the steps whose time (minus half a step) falls in this block */
        for (;;) {                             /* grid step g is evaluated at (g - 1/2) steps */
            uint32_t st = step_ticks();
            int64_t due = (int64_t)((2ull * sq.gstep * st) << 31) - ((int64_t)st << 31);
            if (due >= (int64_t)end)
                break;
            eval_step(sq.gstep);
            sq.gstep++;
        }
        /* rolls (performance, TR-8S ROLL): retrigger on the roll grid */
        if (sq.roll_trk) {
            uint32_t rt = step_ticks() / (sq.roll_div ? sq.roll_div : 1u);
            uint64_t a = sq.pos >> 32, b = end >> 32, k;
            for (k = (a + rt - 1u) / rt * rt; k < b; k += rt)
                for (t = 0; t < PZ_NTRK; t++)
                    if ((sq.roll_trk >> t) & 1u) {
                        hit_t h;
                        hit_make(&h, t, live_vel[t] ? live_vel[t] : 100u, 0);
                        ev_push(t, k << 32, &h);
                    }
        }
        /* MIDI clock out */
        if (sys.clk_out && !sys.sync)
            while (((uint64_t)sq.clk_next << 32) < end) {
                midi_send(0xF8, 0, 0, 1);
                sq.clk_next += 40u;
            }
        if (((sq.pos >> 32) / PZ_PPQ) != ((end >> 32) / PZ_PPQ))
            sq.beat ^= 1u;
    } else {
        end = sq.pos + sq.inc * n;             /* (a free clock while stopped: live flams and rolls) */
    }
    /* live pads */
    if (live_req) {
        uint32_t r = live_req;
        live_req = 0;
        for (t = 0; t < PZ_NTRK; t++)
            if (r & (1u << t)) {
                hit_t h;
                hit_make(&h, t, live_vel[t] ? live_vel[t] : 100u, 0);
                if (live_flam) {
                    hit_t g = h;
                    g.vel = (uint8_t)(h.vel * 2u / 3u);
                    ev_push(t, sq.pos, &g);
                    ev_push(t, sq.pos + sq.inc * (PZ_FS / 1000u) * (3u + PAT->flam * 3u), &h);
                } else {
                    ev_push(t, sq.pos, &h);
                }
            }
    }
    fx_config();
    for (i = 0; i < n; i++)
        out[2u * i] = out[2u * i + 1u] = rev_in[i] = dly_in[i] = 0;
    for (t = 0; t < PZ_NTRK; t++) {
        voice_t *v = &vc[t];
        uint32_t done = 0, e;
        const kittrk_t *k = &kit.t[t];
        int32_t lv, gl, gr, sr, sd;
        for (i = 0; i < n; i++)
            trk_buf[i] = 0;
        /* fire the hits due in this block, in time order */
        for (;;) {
            uint32_t best = EVQ, off;
            for (e = 0; e < sq.ev_n[t]; e++)
                if (sq.evq[t][e].t < end && (best == EVQ || sq.evq[t][e].t < sq.evq[t][best].t))
                    best = e;
            if (best == EVQ)
                break;
            {
                ev_t ev = sq.evq[t][best];
                sq.evq[t][best] = sq.evq[t][--sq.ev_n[t]];
                off = ev.t <= sq.pos || !(sq.inc >> 8) ? 0u : (uint32_t)((ev.t - sq.pos) >> 8) / (uint32_t)(sq.inc >> 8);
                if (off >= n)
                    off = n - 1u;
                if (off < done)
                    off = done;
                voice_render(v, trk_buf + done, off - done);
                done = off;
                voice_trigger(v, &ev.h);
                sq.hit_flash[t] = 6;
#ifdef PZ_TEST_HOOK
                PZ_TEST_HOOK(t, sq.pos + (uint64_t)off * sq.inc, &ev.h);
#endif
                /* choke: the closed hat closes the open one */
                if (t + 1u < PZ_NTRK && ((kit.choke >> (t + 1u)) & 1u) && INST[kit.t[t + 1u].inst].cat == CAT_OH &&
                    INST[k->inst].cat == CAT_HH)
                    voice_choke(&vc[t + 1u]);
                if (sys.note_out) {
                    midi_send((uint8_t)(0x90u | sys.midi_ch), TRK_NOTE[t], ev.h.vel, 3);
                    note_off_pending |= (uint8_t)(1u << t);
                }
            }
        }
        if (!v->on && !v->xf && !done) {      /* silent all block: nothing to mix */
            v->meter -= v->meter >> 3;
            continue;
        }
        voice_render(v, trk_buf + done, n - done);
        /* level (motion), pan, sends */
        lv = sq.mval[t][MP_LEVEL] != 0xFF ? sq.mval[t][MP_LEVEL] : (int32_t)knob_val(t, MP_LEVEL);
        {
            int32_t target = LVL_Q15[lv & 127];
            int32_t step = (target - lvl_g[t]) / (int32_t)n;
            uint32_t p = (uint32_t)((int32_t)k->pan + 64);
            if (p > 127u)
                p = 127u;
            gl = PAN_L[p];
            gr = PAN_R[p];
            sr = (int32_t)k->rev * 258;
            sd = (int32_t)k->dly * 258;
            for (i = 0; i < n; i++) {
                int32_t x = mulq15(trk_buf[i], lvl_g[t]);
                lvl_g[t] += step;
                out[2u * i] += mulq15(x, gl);
                out[2u * i + 1u] += mulq15(x, gr);
                if (sr)
                    rev_in[i] += mulq15(x, sr);
                if (sd)
                    dly_in[i] += mulq15(x, sd);
            }
            lvl_g[t] = target;
        }
    }
    /* effects returns */
    {
        int32_t rl = (int32_t)(sq.fxval[MF_REVLVL] != 0xFF ? sq.fxval[MF_REVLVL] : kit.rev_lvl) * 258;
        int32_t dlv = (int32_t)(sq.fxval[MF_DLYLVL] != 0xFF ? sq.fxval[MF_DLYLVL] : kit.dly_lvl) * 258;
        for (i = 0; i < n; i++) {
            int32_t l = 0, r = 0, d;
            if (!sq.overload)
                reverb_run(rev_in[i], &l, &r);     /* (an overloaded render sheds the reverb first) */
            d = delay_run(dly_in[i]);
            out[2u * i] += mulq15(l, rl) + mulq15(d, dlv);
            out[2u * i + 1u] += mulq15(r, rl) + mulq15(d, dlv);
        }
    }
    {
        static int32_t l[PZ_CTL], r[PZ_CTL];
        uint32_t ctrl = sq.fxval[MF_MFXCTRL] != 0xFF ? sq.fxval[MF_MFXCTRL] : kit.mfx_ctrl;
        for (i = 0; i < n; i++) {
            l[i] = out[2u * i];
            r[i] = out[2u * i + 1u];
        }
        mfx_run(l, r, n, kit.mfx_type, ctrl, kit.mfx_on);
        for (i = 0; i < n; i++) {             /* the bus limiter: soft knee above -6 dBFS */
            int32_t a = mulq15(l[i], 21000), b = mulq15(r[i], 21000);   /* -3.9 dB of bus headroom */
            out[2u * i] = soft_clip(a > 16384 || a < -16384 ? (a > 0 ? 16384 + (a - 16384) / 2 : -16384 + (a + 16384) / 2) : a);
            out[2u * i + 1u] = soft_clip(b > 16384 || b < -16384 ? (b > 0 ? 16384 + (b - 16384) / 2 : -16384 + (b + 16384) / 2) : b);
        }
    }
    sq.pos = end;
    sq.samples += n;
    if (sq.overload)
        sq.overload--;
}
