/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* PULSAR-8 control surface: how the FM-1's keys, buttons and knobs play the TR workflow.
 *
 *   16 white keys   the 16 step pads (TR-REC), patterns (PTN SELECT), instrument pads (INST PLAY),
 *                   steps to loop (STEP LOOP)
 *   black keys 1-8  instrument select BD..T8 (TR-8S [BD]..[RC]); C#5 LAST, D#5 CLEAR, F#5 FILL
 *   PLAY REC SEQ ARP  START/STOP, TR-REC, PTN SELECT, INST PLAY (ARP again: INST REC)
 *   OCT- = SHIFT, OCT+ = VARIATION (+ keys 1-8: A..H, several = a chain; 9, 10: FILL IN 1, 2)
 *   SEL = ACCENT, ENV = SUB (SHIFT: FLAM), LFO = MOTION, GLO = MUTE (tap: MIXER),
 *   EDIT = INST, FX = EFFECTS, SAVE = WRITE (SHIFT: COPY), HOME = main page (hold: UTILITY)
 *   knobs           SELECT = VALUE / TEMPO, ALGORITHM = SHUFFLE, PRESETS = KIT,
 *                   1-4 = TUNE DECAY CTRL LEVEL of the selected instrument (TR-8S INST knobs);
 *                   a step held: VELOCITY, START, PROBABILITY, CYCLE (TR-1000 step edit) */

enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_SHIFT, B_VAR, NB };
enum { E_VALUE, E_ALGO, E_PRESET, E_K1, E_K2, E_K3, E_K4, NE };
static const char *const B_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                       "PLAY", "REC", "OCT-", "OCT+"};
/* keys: note index 0 = F3 .. 26 = G5 */
static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_KEY[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
enum { BK_LAST = 8, BK_CLEAR = 9, BK_FILL = 10 };

enum { M_TRREC, M_PTN, M_PLAY, M_LOOP, M_N };
static const char *const MODE_NAME[M_N] = {"TR-REC", "PTN SELECT", "INST PLAY", "STEP LOOP"};
enum { PG_MAIN, PG_INST, PG_FX, PG_MIX, PG_TRK, PG_PTN, PG_UTIL, PG_N };

typedef struct {
    uint8_t mode, page, trk;
    uint8_t inst_rec;
    uint8_t sub_latch, flam_latch, acc_latch;
    uint8_t subtype;                          /* 1 = 1/2, 2 = 1/3, 3 = 1/4 */
    uint16_t held_w, edited_w, created_w;     /* white keys held; steps edited while held; steps made on press */
    uint16_t held_b;                          /* black keys held */
    uint16_t btn;                             /* buttons held */
    uint16_t btn_used;                        /* a held button was used as a modifier (no tap action) */
    uint32_t btn_t[NB];
    uint8_t var_fresh;                        /* the first variation key under VAR replaces the chain */
    uint8_t copy_arm;                         /* SHIFT + SAVE: copy armed */
    uint8_t cursor;                           /* list pages */
    int8_t pat_first;                         /* PTN SELECT: first key held (range) */
    uint8_t clear_pat;                        /* PTN SELECT + CLEAR: the pattern armed for clearing */
    uint8_t roll;                             /* INST PLAY: roll key held (1, 2, 4), 0 none */
    uint8_t pad_mod;                          /* INST PLAY: 1 flam, 2 weak, 3 accent held */
    uint8_t snap_layer;                       /* INST PLAY: 2 = key 15 (recall) held, 3 = key 16 (store) held */
    uint8_t loop_latch;
    uint8_t mix_bank;                         /* mixer: tracks 1-4 / 5-8 */
    uint16_t pat_dirty, kit_dirty;
    uint8_t save_pending;
    uint32_t last_input;
    uint32_t tap_t[4];
    uint8_t tap_n;
    char toast[28];
    uint32_t toast_t;
    uint8_t motion_full;
    uint8_t force;                            /* redraw everything */
} ui_t;
static ui_t ui;

static void toast(const char *a, const char *b)
{
    sz_cpy(ui.toast, a, sizeof ui.toast);
    if (b)
        sz_cat(ui.toast, b);
    ui.toast_t = plat_ms() | 1u;
}

static int held(uint32_t b) { return (ui.btn >> b) & 1u; }
static int black_held(uint32_t k) { return (ui.held_b >> k) & 1u; }
static void mods_used(void) { ui.btn_used = ui.btn; }

static void mark_pat(void) { ui.pat_dirty |= (uint16_t)(1u << sq.pat); }
static void mark_kit(void) { ui.kit_dirty |= (uint16_t)(1u << kit_cur); }

/* ------------------------------------------------------------------ values --- */
static int32_t clampv(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static void add_u8(uint8_t *p, int32_t d, int32_t lo, int32_t hi) { *p = (uint8_t)clampv((int32_t)*p + d, lo, hi); }
static void add_i8(int8_t *p, int32_t d, int32_t lo, int32_t hi) { *p = (int8_t)clampv((int32_t)*p + d, lo, hi); }
/* knob acceleration: a fast turn moves further */
static int32_t accel(int32_t d)
{
    int32_t a = d < 0 ? -d : d;
    return a >= 3 ? d * 3 : a == 2 ? d * 2 : d;
}

/* the kit store: switch kit (the edits of every kit stay in RAM until power off, as on a TR) */
static void kit_select(uint32_t k)
{
    kit_cur = (uint8_t)(k % PZ_NKIT);
    sys.kitno = kit_cur;
}

static void pattern_select(uint32_t n)
{
    n %= PZ_NPAT;
    if (sq.playing && n != sq.pat) {
        sq.pat_next = (uint8_t)n;             /* at the end of the cycle */
    } else {
        sq.pat = (uint8_t)n;
        sq.pat_next = 0xFF;
        chain_default();
        sq.sec = 0;
        sq.rec_sec = 0;
        tempo_apply();
    }
    sq.pat_lo = sq.pat_hi = (uint8_t)n;
    sys.pat = (uint8_t)n;
    if (pats[n].kit_link)
        kit_select(pats[n].pkit);
}

/* ------------------------------------------------------------- step edit --- */
static step_t *step_at(uint32_t t, uint32_t s) { return &PAT->s[sq.rec_sec][t][s]; }

static void step_toggle(uint32_t s)
{
    step_t *st = step_at(ui.trk, s);
    if (st->f & ST_ON) {
        st->f = 0;
        st->sub = st->vel = st->prob = st->cyc = 0;
        st->micro = 0;
        motion_clear(PAT, sq.rec_sec, (int)s, ui.trk);
    } else {
        st->f = ST_ON;
    }
    mark_pat();
}

static void sub_cycle(uint32_t s)              /* SUB: none -> 1/2 -> 1/3 -> 1/4 -> none (or the SUB type) */
{
    step_t *st = step_at(ui.trk, s);
    uint32_t d = st->sub & 3u;
    if (!(st->f & ST_ON)) {
        st->f |= ST_ON;
        d = 0;
    }
    if (ui.sub_latch && ui.subtype)
        d = d == ui.subtype ? 0u : ui.subtype;
    else
        d = (d + 1u) & 3u;
    st->sub = (uint8_t)d;
    mark_pat();
}

/* the TR-8S CLEAR held during playback: the selected track's steps go as the playhead passes */
static void clear_sweep(void)
{
    static uint8_t lastpos = 0xFF;
    uint32_t p;
    if (!sq.playing || !black_held(BK_CLEAR) || ui.held_w || (ui.held_b & 0xFFu)) {
        lastpos = 0xFF;
        return;
    }
    p = sq.tpos[ui.trk];
    if (p != lastpos) {
        step_t *st = step_at(ui.trk, p);
        if (st->f) {
            *st = (step_t){0, 0, 0, 0, 0, 0};
            mark_pat();
        }
        lastpos = (uint8_t)p;
    }
}

/* ------------------------------------------------------------- variations --- */
static void var_key(uint32_t v)                /* VAR + key 1..8 = A..H; 9, 10 = FILL IN 1, 2 */
{
    if (v >= 8u) {                             /* the fill-ins are edited (TR-REC target), not chained */
        sq.rec_sec = (uint8_t)(v == 8u ? PZ_SEC_FILL1 : PZ_SEC_FILL2);
        toast(v == 8u ? "EDIT FILL IN 1" : "EDIT FILL IN 2", 0);
        return;
    }
    if (held(B_REC)) {                         /* TR-REC + variation: record target only */
        sq.rec_sec = (uint8_t)v;
        return;
    }
    if (ui.var_fresh) {
        ui.var_fresh = 0;
        sq.chain[0] = (uint8_t)v;
        sq.chain_n = 1;
    } else if (sq.chain_n < PZ_CHAIN_MAX) {
        sq.chain[sq.chain_n++] = (uint8_t)v;   /* the order pressed (TR-1000 VARIATION CHAIN) */
    }
    sq.rec_sec = (uint8_t)v;
    if (!sq.playing) {
        sq.chain_i = 0;
        sq.sec = sq.chain[0];
    } else {
        sq.chain_i = (uint8_t)(sq.chain_n - 1u);   /* the next cycle starts the new chain */
    }
}

/* ------------------------------------------------------------------- tap --- */
static void tap_tempo(void)
{
    uint32_t now = plat_ms(), i, sum = 0;
    if (ui.tap_n && now - ui.tap_t[(ui.tap_n - 1u) & 3u] > 2000u)
        ui.tap_n = 0;
    ui.tap_t[ui.tap_n & 3u] = now;
    ui.tap_n++;
    if (ui.tap_n >= 3u) {
        uint32_t n = ui.tap_n > 4u ? 3u : ui.tap_n - 1u;
        for (i = 0; i < n; i++)
            sum += ui.tap_t[(ui.tap_n - 1u - i) & 3u] - ui.tap_t[(ui.tap_n - 2u - i) & 3u];
        if (sum)
            sys.tempo10 = (uint16_t)clampv((int32_t)(600000u * n / sum), 400, 3000);
        tempo_apply();
    }
    toast("TAP", 0);
}

/* --------------------------------------------------------------- storage --- */
static int store_pattern(uint32_t n);
static int store_kit(uint32_t n);
static int store_sys(void);
static void write_now(void)
{
    uint32_t i, ok = 1;
    for (i = 0; i < PZ_NPAT; i++)
        if (ui.pat_dirty & (1u << i)) {
            if (store_pattern(i) == 0)
                ui.pat_dirty &= (uint16_t)~(1u << i);
            else
                ok = 0;
        }
    for (i = 0; i < PZ_NKIT; i++)
        if (ui.kit_dirty & (1u << i)) {
            if (store_kit(i) == 0)
                ui.kit_dirty &= (uint16_t)~(1u << i);
            else
                ok = 0;
        }
    store_sys();
    toast(ok ? "WRITE COMPLETED" : "WRITE ERROR", 0);
}
static void write_req(void)
{
    if (sq.playing) {                          /* the flash stops the audio while it erases: at STOP */
        ui.save_pending = 1;
        toast("WRITE AT STOP", 0);
    } else {
        write_now();
    }
}

/* ------------------------------------------------------------ list pages --- */
typedef struct {
    const char *name;
    int32_t (*get)(void);
    void (*add)(int32_t d);
    void (*show)(char *b);
} item_t;

static const char *const SCALE_NAME[SC_N] = {"1/16", "1/32", "1/16T", "1/8T"};
static const char *const DIR_NAME[DIR_N] = {"FWD", "BWD", "P-P", "R-L", "L-R", "RND", "DNK"};
static const uint8_t AF_BARS[7] = {0, 2, 4, 8, 12, 16, 32};
static const char *const FILLT_NAME[3] = {"FILL IN 1", "FILL IN 2", "SCATTER"};
static const char *const REV_NAME[REV_N] = {"ROOM", "HALL", "PLATE"};
static const char *const THEME_NAME[2] = {"NOIR (TR-8S)", "ALLOY (TR-1000)"};
static const char *const SYNC_NAME[3] = {"INTERNAL", "USB MIDI", "TRS MIDI"};
static const char *const OFFON[2] = {"OFF", "ON"};

static void show_tempo(char *b, uint32_t t10)
{
    char d[3] = {'.', (char)('0' + t10 % 10u), 0};
    fmt_u(b, t10 / 10u);
    sz_cat(b, d);
}

/* pattern settings (TR-8S PTN SETTING) */
static int32_t g_ptempo(void) { return PAT->tempo10; }
static void a_ptempo(int32_t d) { PAT->tempo10 = (uint16_t)clampv(PAT->tempo10 + d * 10, 400, 3000); tempo_apply(); mark_pat(); }
static void s_ptempo(char *b) { show_tempo(b, PAT->tempo10); }
static int32_t g_shuf(void) { return PAT->shuffle; }
static void a_shuf(int32_t d) { add_i8(&PAT->shuffle, d, -100, 100); mark_pat(); }
static void s_shuf(char *b) { fmt_i(b, PAT->shuffle, 1); }
static int32_t g_scale(void) { return PAT->scale; }
static void a_scale(int32_t d) { PAT->scale = (uint8_t)clampv(PAT->scale + d, 0, SC_N - 1); mark_pat(); }
static void s_scale(char *b) { sz_cpy(b, SCALE_NAME[PAT->scale % SC_N], 12); }
static int32_t g_flam(void) { return PAT->flam; }
static void a_flam(int32_t d) { add_u8(&PAT->flam, d, 0, 8); mark_pat(); }
static void s_flam(char *b) { fmt_u(b, PAT->flam); }
static int32_t g_klink(void) { return PAT->kit_link; }
static void a_klink(int32_t d) { PAT->kit_link = d > 0; if (d > 0) PAT->pkit = kit_cur; mark_pat(); }
static void s_klink(char *b)
{
    if (!PAT->kit_link) {
        sz_cpy(b, "OFF", 8);
        return;
    }
    sz_cpy(b, "KIT ", 8);
    fmt_u(b + 4, PAT->pkit + 1u);
}
static int32_t g_mprob(void) { return PAT->mprob; }
static void a_mprob(int32_t d) { add_i8(&PAT->mprob, d * 5, -100, 100); mark_pat(); }
static void s_mprob(char *b) { fmt_i(b, PAT->mprob, 1); sz_cat(b, "%"); }
static int32_t g_acc(void) { return PAT->acc_lvl; }
static void a_acc(int32_t d) { add_u8(&PAT->acc_lvl, d, 0, 127); mark_pat(); }
static void s_acc(char *b) { fmt_u(b, PAT->acc_lvl); }
static int32_t g_af(void) { return PAT->autofill; }
static void a_af(int32_t d) { PAT->autofill = (uint8_t)clampv(PAT->autofill + d, 0, 6); mark_pat(); }
static void s_af(char *b)
{
    if (!PAT->autofill) {
        sz_cpy(b, "OFF", 8);
        return;
    }
    fmt_u(b, AF_BARS[PAT->autofill % 7u]);
    sz_cat(b, " BARS");
}
static int32_t g_ft(void) { return PAT->filltype; }
static void a_ft(int32_t d) { PAT->filltype = (uint8_t)clampv(PAT->filltype + d, 0, 2); mark_pat(); }
static void s_ft(char *b) { sz_cpy(b, FILLT_NAME[PAT->filltype % 3u], 12); }
static const item_t PTN_ITEMS[] = {
    {"TEMPO", g_ptempo, a_ptempo, s_ptempo}, {"SHUFFLE", g_shuf, a_shuf, s_shuf}, {"SCALE", g_scale, a_scale, s_scale},
    {"FLAM SPACING", g_flam, a_flam, s_flam}, {"KIT LINK", g_klink, a_klink, s_klink},
    {"MASTER PROB", g_mprob, a_mprob, s_mprob}, {"ACCENT LEVEL", g_acc, a_acc, s_acc},
    {"AUTO FILL IN", g_af, a_af, s_af}, {"FILL TYPE", g_ft, a_ft, s_ft},
};

/* utility (system settings) */
static void util_changed(void) { ui.force = 1; }
static int32_t g_theme(void) { return sys.theme; }
static void a_theme(int32_t d) { sys.theme = (uint8_t)clampv(sys.theme + d, 0, 1); util_changed(); }
static void s_theme(char *b) { sz_cpy(b, THEME_NAME[sys.theme & 1u], 20); }
static int32_t g_sync(void) { return sys.sync; }
static void a_sync(int32_t d) { sys.sync = (uint8_t)clampv(sys.sync + d, 0, 2); }
static void s_sync(char *b) { sz_cpy(b, SYNC_NAME[sys.sync % 3u], 12); }
static int32_t g_clk(void) { return sys.clk_out; }
static void a_clk(int32_t d) { sys.clk_out = d > 0; }
static void s_clk(char *b) { sz_cpy(b, OFFON[sys.clk_out & 1u], 4); }
static int32_t g_nout(void) { return sys.note_out; }
static void a_nout(int32_t d) { sys.note_out = d > 0; }
static void s_nout(char *b) { sz_cpy(b, OFFON[sys.note_out & 1u], 4); }
static int32_t g_ch(void) { return sys.midi_ch; }
static void a_ch(int32_t d) { sys.midi_ch = (uint8_t)clampv(sys.midi_ch + d, 0, 15); }
static void s_ch(char *b) { sz_cpy(b, "CH ", 8); fmt_u(b + 3, sys.midi_ch + 1u); }
static int32_t g_tsrc(void) { return sys.tempo_src; }
static void a_tsrc(int32_t d) { sys.tempo_src = d > 0; tempo_apply(); }
static void s_tsrc(char *b) { sz_cpy(b, sys.tempo_src ? "PATTERN" : "SYSTEM", 10); }
static int32_t g_aud(void) { return sys.audition; }
static void a_aud(int32_t d) { sys.audition = d > 0; }
static void s_aud(char *b) { sz_cpy(b, OFFON[sys.audition & 1u], 4); }
static int32_t g_asave(void) { return sys.autosave; }
static void a_asave(int32_t d) { sys.autosave = d > 0; }
static void s_asave(char *b) { sz_cpy(b, OFFON[sys.autosave & 1u], 4); }
static int32_t g_quant(void) { return sys.quant; }
static void a_quant(int32_t d) { sys.quant = d > 0; }
static void s_quant(char *b) { sz_cpy(b, sys.quant ? "ON (GRID)" : "OFF (FREE)", 12); }
static int32_t g_ufull(void) { return sys.usb_full; }
static void a_ufull(int32_t d) { sys.usb_full = d > 0; }
static void s_ufull(char *b) { sz_cpy(b, sys.usb_full ? "FULL" : "VOLUME", 8); }
static void s_ver(char *b) { sz_cpy(b, PZ_VERSION, 16); }
static int32_t g_none(void) { return 0; }
static void a_none(int32_t d) { (void)d; }
static uint8_t init_arm;
static void a_init(int32_t d)
{
    (void)d;
    init_arm = 1;
}
static void s_init(char *b) { sz_cpy(b, init_arm ? "PRESS HOME" : "TURN TO ARM", 14); }
static const item_t UTIL_ITEMS[] = {
    {"THEME", g_theme, a_theme, s_theme}, {"SYNC", g_sync, a_sync, s_sync}, {"CLOCK OUT", g_clk, a_clk, s_clk},
    {"NOTE OUT", g_nout, a_nout, s_nout}, {"MIDI CH", g_ch, a_ch, s_ch}, {"TEMPO SRC", g_tsrc, a_tsrc, s_tsrc},
    {"PAD PREVIEW", g_aud, a_aud, s_aud}, {"AUTO SAVE", g_asave, a_asave, s_asave},
    {"REC QUANTIZE", g_quant, a_quant, s_quant}, {"USB AUDIO", g_ufull, a_ufull, s_ufull},
    {"INIT PATTERN", g_none, a_init, s_init}, {"VERSION", g_none, a_none, s_ver},
};

/* ------------------------------------------------------------------ input --- */
static void live_hit(uint32_t t, uint32_t vel)
{
    live_vel[t] = (uint8_t)vel;
    live_flam = ui.pad_mod == 1u;
    live_req |= 1u << t;
    if (ui.mode == M_PLAY && ui.inst_rec && sq.playing) {
        rec_live(t, vel);
        mark_pat();
    }
}

static void ui_btn(uint32_t b, int down)
{
    uint32_t now = plat_ms();
    ui.last_input = now;
    if (down) {
        ui.btn |= (uint16_t)(1u << b);
        ui.btn_used &= (uint16_t)~(1u << b);
        ui.btn_t[b] = now;
        if (b != B_SHIFT && held(B_SHIFT))
            ui.btn_used |= 1u << B_SHIFT;
        switch (b) {
        case B_PLAY:
            if (held(B_SHIFT)) {
                if (sq.playing)
                    sq.req = TP_RESTART;
                else {
                    uint32_t t;
                    for (t = 0; t < PZ_NTRK; t++)
                        voice_choke(&vc[t]);
                }
            } else {
                sq.req = sq.playing ? TP_STOP : TP_START;
            }
            break;
        case B_REC:                            /* TR-REC, from any mode */
            ui.mode = M_TRREC;
            ui.inst_rec = 0;
            sq.loop_step = 0xFF;
            break;
        case B_SEQ:
            if (held(B_SHIFT))
                ui.page = ui.page == PG_PTN ? PG_MAIN : PG_PTN, ui.cursor = 0;
            break;
        case B_ARP:                            /* INST PLAY; pressed again: INST REC on / off */
            if (held(B_SHIFT))
                ui.mode = M_LOOP;
            else if (ui.mode != M_PLAY) {
                ui.mode = M_PLAY;
                ui.inst_rec = 0;
            } else {
                ui.inst_rec ^= 1u;
            }
            break;
        case B_EDIT:
            if (held(B_SHIFT))
                ui.page = ui.page == PG_TRK ? PG_MAIN : PG_TRK;
            else
                ui.page = ui.page == PG_INST ? PG_MAIN : PG_INST;
            break;
        case B_FX:
            if (held(B_SHIFT)) {
                kit.mfx_on ^= 1u;
                mark_kit();
                toast("MASTER FX ", OFFON[kit.mfx_on & 1u]);
            } else {
                ui.page = ui.page == PG_FX ? PG_MAIN : PG_FX;
            }
            break;
        case B_ENV:
            if (held(B_SHIFT)) {
                ui.flam_latch ^= 1u;
                ui.sub_latch = 0;
                toast("FLAM ", OFFON[ui.flam_latch]);
            }
            break;
        case B_LFO:
            if (held(B_SHIFT)) {
                sq.motion_rec ^= 1u;
                if (sq.motion_rec)
                    sq.motion_on = 1;
                toast("MOTION REC ", OFFON[sq.motion_rec]);
            } else if (black_held(BK_CLEAR)) {
                motion_clear(PAT, sq.sec, -1, -1);
                mark_pat();
                toast("MOTION CLEARED", 0);
                ui.btn_used |= 1u << b;
            }
            break;
        case B_GLO:
            if (held(B_SHIFT) || black_held(BK_CLEAR)) {
                sq.mute = sq.solo = 0;
                toast("MUTE CLEARED", 0);
                ui.btn_used |= 1u << b;
            }
            break;
        case B_HOME:
            if (held(B_SHIFT))
                tap_tempo();
            else if (ui.page == PG_UTIL && init_arm) {
                init_arm = 0;
                pat_init(PAT, sq.pat);
                mark_pat();
                toast("PATTERN INITIALIZED", 0);
            }
            break;
        case B_SAVE:
            if (held(B_SHIFT)) {
                ui.copy_arm ^= 1u;
                toast(ui.copy_arm ? "COPY: PICK A TARGET" : "COPY CANCELLED", 0);
                ui.btn_used |= 1u << b;
            }
            break;
        case B_VAR:
            ui.var_fresh = 1;
            break;
        }
        return;
    }
    /* release: taps */
    {
        uint32_t tap = !((ui.btn_used >> b) & 1u), dur = now - ui.btn_t[b];
        ui.btn &= (uint16_t)~(1u << b);
        if (!tap || held(B_SHIFT))
            return;
        switch (b) {
        case B_SEQ:
            ui.mode = ui.mode == M_PTN ? M_TRREC : M_PTN;
            ui.pat_first = -1;
            break;
        case B_GLO:
            ui.page = ui.page == PG_MIX ? PG_MAIN : PG_MIX;
            break;
        case B_LFO:
            sq.motion_on ^= 1u;
            if (!sq.motion_on)
                sq.motion_rec = 0;
            toast("MOTION ", OFFON[sq.motion_on]);
            break;
        case B_ENV:
            ui.sub_latch ^= 1u;
            ui.flam_latch = 0;
            if (!ui.subtype)
                ui.subtype = 1;
            toast("SUB STEP ", OFFON[ui.sub_latch]);
            break;
        case B_SEL:
            ui.acc_latch ^= 1u;
            toast("ACCENT STEP ", OFFON[ui.acc_latch]);
            break;
        case B_HOME:
            if (dur > 600u)
                ui.page = PG_UTIL, ui.cursor = 0;
            else {
                ui.page = PG_MAIN;
                ui.sub_latch = ui.flam_latch = ui.acc_latch = 0;
                ui.copy_arm = 0;
                if (ui.mode == M_LOOP)
                    ui.mode = M_TRREC;
            }
            break;
        case B_SAVE:
            write_req();
            break;
        }
    }
}

static void ui_white(uint32_t k, int down)
{
    ui.last_input = plat_ms();
    if (down)
        ui.held_w |= (uint16_t)(1u << k);
    else
        ui.held_w &= (uint16_t)~(1u << k);
    /* modifiers that work in every mode */
    if (down && held(B_VAR)) {
        mods_used();
        if (black_held(BK_CLEAR) && k < 10u) {
            uint32_t sec = k < 8u ? k : PZ_SEC_FILL1 + (k - 8u), t, s;
            for (t = 0; t < PZ_NTRK; t++)
                for (s = 0; s < PZ_NSTEP; s++)
                    PAT->s[sec][t][s] = (step_t){0, 0, 0, 0, 0, 0};
            for (s = 0; s < PZ_NSTEP; s++)
                PAT->acc[sec][s] = 0;
            motion_clear(PAT, (int)sec, -1, -1);
            mark_pat();
            toast("VARIATION CLEARED", 0);
        } else if (ui.copy_arm && k < 10u) {
            uint32_t dst = k < 8u ? k : PZ_SEC_FILL1 + (k - 8u), t, s, i;
            for (t = 0; t < PZ_NTRK; t++)
                for (s = 0; s < PZ_NSTEP; s++)
                    PAT->s[dst][t][s] = PAT->s[sq.rec_sec][t][s];
            for (s = 0; s < PZ_NSTEP; s++)
                PAT->acc[dst][s] = PAT->acc[sq.rec_sec][s];
            PAT->last[dst] = PAT->last[sq.rec_sec];
            for (i = 0; i < PZ_NMOTION; i++) {          /* the motion too */
                motion_t m = PAT->mo[i];
                if (m.where != 0xFF && (m.where >> 4) == sq.rec_sec)
                    motion_set(PAT, dst, m.where & 15u, m.what >> 4, m.what & 15u, m.val);
            }
            ui.copy_arm = 0;
            mark_pat();
            toast("VARIATION COPIED", 0);
        } else if (k < 10u) {
            var_key(k);
        } else if (k == 15u) {
            PAT->autofill = PAT->autofill ? 0u : 3u;
            mark_pat();
        }
        return;
    }
    if (down && held(B_SEQ) && ui.mode != M_PTN) {     /* hold SEQ: a pattern from any mode */
        ui.btn_used |= 1u << B_SEQ;
        pattern_select(k);
        return;
    }
    if (down && held(B_SAVE)) {                        /* SAVE + key: write the pattern into slot k */
        ui.btn_used |= 1u << B_SAVE;
        if (k != sq.pat) {
            pats[k] = *PAT;
            ui.pat_dirty |= (uint16_t)(1u << k);
        }
        toast("SAVED AS PATTERN ", 0);
        {
            char b[4];
            fmt_u(b, k + 1u);
            sz_cat(ui.toast, b);
        }
        write_req();
        return;
    }
    switch (ui.mode) {
    case M_TRREC: {
        step_t *st = step_at(ui.trk, k);
        if (!down) {
            if ((ui.created_w >> k) & 1u)
                ui.created_w &= (uint16_t)~(1u << k);
            else if (!((ui.edited_w >> k) & 1u) && (st->f & ST_ON) && !held(B_SHIFT) && !ui.sub_latch &&
                     !ui.flam_latch && !ui.acc_latch && !held(B_SEL) && !held(B_ENV))
                step_toggle(k);                        /* tapped an existing step: off */
            ui.edited_w &= (uint16_t)~(1u << k);
            return;
        }
        if (black_held(BK_LAST)) {                     /* LAST + key: the last step (with a track held: its own) */
            uint32_t t;
            int any = 0;
            for (t = 0; t < PZ_NTRK; t++)
                if (black_held(t)) {
                    PAT->trk[t].last = (uint8_t)(k + 1u);
                    any = 1;
                }
            if (!any)
                PAT->last[sq.rec_sec] = (uint8_t)(k + 1u);
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (black_held(BK_CLEAR)) {
            *st = (step_t){0, 0, 0, 0, 0, 0};
            motion_clear(PAT, sq.rec_sec, (int)k, ui.trk);
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (ui.acc_latch || held(B_SEL)) {             /* ACCENT */
            PAT->acc[sq.rec_sec][k] ^= 1u;
            ui.btn_used |= 1u << B_SEL;
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (held(B_ENV) || ui.sub_latch) {             /* SUB */
            ui.btn_used |= 1u << B_ENV;
            sub_cycle(k);
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (ui.flam_latch) {
            st->f = (uint8_t)((st->f | ST_ON) ^ ST_FLAM);
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (held(B_SHIFT)) {                           /* WEAK BEAT */
            ui.btn_used |= 1u << B_SHIFT;
            st->f = (uint8_t)((st->f | ST_ON) ^ ST_WEAK);
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (ui.held_b & 0xFFu) {                       /* track held + step: ALT INST (TR-8S) */
            uint32_t t;
            for (t = 0; t < PZ_NTRK; t++)
                if (black_held(t))
                    PAT->s[sq.rec_sec][t][k].f = (uint8_t)((PAT->s[sq.rec_sec][t][k].f | ST_ON) ^ ST_ALT);
            mark_pat();
            ui.edited_w |= (uint16_t)(1u << k);
            return;
        }
        if (!(st->f & ST_ON)) {
            step_toggle(k);
            ui.created_w |= (uint16_t)(1u << k);
        }
        return;
    }
    case M_PTN:
        if (!down) {
            if ((int)k == ui.pat_first)
                ui.pat_first = -1;
            return;
        }
        if (black_held(BK_CLEAR)) {                    /* CLEAR + pattern: twice to confirm */
            if (ui.clear_pat == k + 1u) {
                pat_init(&pats[k], k);
                ui.pat_dirty |= (uint16_t)(1u << k);
                ui.clear_pat = 0;
                toast("PATTERN CLEARED", 0);
            } else {
                ui.clear_pat = (uint8_t)(k + 1u);
                toast("AGAIN TO CLEAR", 0);
            }
            return;
        }
        if (ui.pat_first >= 0 && (uint32_t)ui.pat_first != k) {   /* two keys: a chain of patterns */
            uint32_t lo = (uint32_t)ui.pat_first < k ? (uint32_t)ui.pat_first : k, hi = lo == k ? (uint32_t)ui.pat_first : k;
            sq.pat_lo = (uint8_t)lo;
            sq.pat_hi = (uint8_t)hi;
            toast("PATTERN CHAIN", 0);
            return;
        }
        ui.pat_first = (int8_t)k;
        pattern_select(k);
        return;
    case M_PLAY:
        if (k < 8u && ui.snap_layer && down) {         /* TR-1000 SNAPSHOT: keys 1-8 */
            snap_t *sn = &PAT->snap[k];
            if (ui.snap_layer == 3u) {                 /* store: the knobs of every track */
                uint32_t t;
                for (t = 0; t < PZ_NTRK; t++) {
                    sn->v[t][MP_TUNE] = (uint8_t)(kit.t[t].tune + 64);
                    sn->v[t][MP_DECAY] = kit.t[t].decay;
                    sn->v[t][MP_CTRL] = kit.t[t].ctrl;
                    sn->v[t][MP_LEVEL] = kit.t[t].level;
                }
                sn->used = 1;
                mark_pat();
                toast("SNAPSHOT STORED", 0);
            } else if (sn->used) {                     /* recall: all the way to it; again: back to the kit */
                if (sq.morph_to == k && sq.morph == 127u) {
                    sq.morph = 0;
                    toast("SNAPSHOT OFF", 0);
                } else {
                    sq.morph_to = (uint8_t)k;
                    sq.morph = 127;
                    toast("SNAPSHOT ", 0);
                    {
                        char b[3] = {(char)('1' + k), 0, 0};
                        sz_cat(ui.toast, b);
                    }
                }
            } else {
                toast("EMPTY SNAPSHOT", 0);
            }
            return;
        }
        if (k < 8u) {
            if (down) {
                uint32_t vel = ui.pad_mod == 2u ? 48u : ui.pad_mod == 3u ? 127u : 100u;
                if (ui.roll) {
                    live_vel[k] = (uint8_t)vel;
                    sq.roll_div = ui.roll;
                    sq.roll_trk |= (uint8_t)(1u << k);
                }
                live_hit(k, vel);
                ui.trk = (uint8_t)k;
            } else {
                sq.roll_trk &= (uint8_t)~(1u << k);
            }
        } else if (k >= 8u && k <= 10u) {              /* ROLL 1/16 1/32 1/64 */
            static const uint8_t RD[3] = {1, 2, 4};
            if (down) {
                ui.roll = RD[k - 8u];
                sq.roll_div = ui.roll;
            } else {
                ui.roll = 0;
                sq.roll_trk = 0;
            }
        } else if (k >= 11u && k <= 13u) {             /* FLAM, WEAK, ACCENT pads */
            ui.pad_mod = down ? (uint8_t)(k - 10u) : 0u;
        } else if (k >= 14u) {                         /* SNAPSHOT layers: 15 recall, 16 store */
            ui.snap_layer = down ? (uint8_t)(k - 12u) : 0u;
        }
        return;
    case M_LOOP:
        if (down) {
            sq.loop_step = (uint8_t)k;
            ui.loop_latch = (ui.held_b & 0xFFu) != 0u;
        } else if (sq.loop_step == k && !ui.loop_latch) {
            sq.loop_step = 0xFF;
            sq.loop_div = 1;
        }
        return;
    }
}

static void ui_black(uint32_t k, int down)
{
    ui.last_input = plat_ms();
    if (!down) {
        ui.held_b &= (uint16_t)~(1u << k);
        return;
    }
    ui.held_b |= (uint16_t)(1u << k);
    if (k == BK_FILL) {
        if (held(B_SHIFT)) {
            PAT->autofill = PAT->autofill ? 0u : 3u;
            mark_pat();
            toast("AUTO FILL ", PAT->autofill ? "ON" : "OFF");
        } else {
            sq.fill_req = 1;
        }
        return;
    }
    if (k >= PZ_NTRK)
        return;
    /* an instrument key */
    if (ui.loop_latch && ui.mode == M_LOOP) {
        sq.loop_step = 0xFF;
        ui.loop_latch = 0;
    }
    if (held(B_GLO)) {                         /* MUTE (+ SHIFT: SOLO) */
        ui.btn_used |= 1u << B_GLO;
        if (held(B_SHIFT))
            sq.solo ^= (uint16_t)(1u << k);
        else
            sq.mute ^= (uint16_t)(1u << k);
        return;
    }
    if (black_held(BK_CLEAR)) {
        uint32_t s;
        for (s = 0; s < PZ_NSTEP; s++)
            PAT->s[sq.rec_sec][k][s] = (step_t){0, 0, 0, 0, 0, 0};
        motion_clear(PAT, sq.rec_sec, -1, (int)k);
        mark_pat();
        toast("TRACK CLEARED", 0);
        return;
    }
    if (black_held(BK_LAST)) {
        if (held(B_SHIFT)) {
            PAT->trk[k].last = 0;
            mark_pat();
        }
        ui.trk = (uint8_t)k;
        return;
    }
    if (held(B_LFO)) {                         /* MOTION + track: clear its motion */
        ui.btn_used |= 1u << B_LFO;
        motion_clear(PAT, sq.sec, -1, (int)k);
        mark_pat();
        toast("TRACK MOTION CLEARED", 0);
        return;
    }
    if (ui.copy_arm) {                         /* COPY: this track's steps <- the selected track's */
        uint32_t s;
        if (k != ui.trk) {
            for (s = 0; s < PZ_NSTEP; s++)
                PAT->s[sq.rec_sec][k][s] = PAT->s[sq.rec_sec][ui.trk][s];
            mark_pat();
        }
        ui.copy_arm = 0;
        toast("TRACK COPIED", 0);
        return;
    }
    ui.trk = (uint8_t)k;
    if (ui.mode == M_PLAY || (sys.audition && !sq.playing))
        live_hit(k, ui.pad_mod == 2u ? 48u : 100u);
}

/* knob turns */
static void knob_track(uint32_t par, int32_t d)
{
    kittrk_t *k = &kit.t[ui.trk];
    int32_t v = 0;
    uint32_t t = ui.trk;
    switch (par) {
    case MP_TUNE:
        add_i8(&k->tune, d, -64, 63);
        v = k->tune + 64;
        break;
    case MP_DECAY:
        add_u8(&k->decay, d, 0, 127);
        v = k->decay;
        break;
    case MP_CTRL:
        add_u8(&k->ctrl, d, 0, 127);
        v = k->ctrl;
        break;
    case MP_LEVEL:
        add_u8(&k->level, d, 0, 127);
        v = k->level;
        break;
    }
    mark_kit();
    /* MOTION REC (latched, or LFO held): the value goes into the step playing */
    if (sq.playing && (sq.motion_rec || held(B_LFO))) {
        ui.btn_used |= 1u << B_LFO;
        if (motion_set(PAT, sq.sec, sq.tpos[t], t, par, (uint32_t)v) == 0) {
            sq.motion_on = 1;
            mark_pat();
        } else {
            toast("MOTION FULL", 0);
        }
    }
}

static void knob_step(uint32_t e, int32_t d)   /* steps held: VEL, START, PROB, CYCLE (SHIFT: motion lock) */
{
    uint32_t s;
    for (s = 0; s < PZ_NSTEP; s++) {
        step_t *st;
        if (!((ui.held_w >> s) & 1u))
            continue;
        st = step_at(ui.trk, s);
        ui.edited_w |= (uint16_t)(1u << s);
        if (held(B_SHIFT) && e <= E_K4) {      /* parameter lock: TUNE DECAY CTRL LEVEL at this step */
            uint32_t par = e - E_K1;
            int i = motion_find(PAT, sq.rec_sec, s, ui.trk, par);
            int32_t base = par == MP_TUNE ? kit.t[ui.trk].tune + 64 : par == MP_DECAY ? kit.t[ui.trk].decay
                         : par == MP_CTRL ? kit.t[ui.trk].ctrl : kit.t[ui.trk].level;
            int32_t v = clampv((i >= 0 ? PAT->mo[i].val : base) + d, 0, 127);
            ui.btn_used |= 1u << B_SHIFT;
            if (motion_set(PAT, sq.rec_sec, s, ui.trk, par, (uint32_t)v))
                toast("MOTION FULL", 0);
            sq.motion_on = 1;
            continue;
        }
        st->f |= ST_ON;
        switch (e) {
        case E_K1:
            st->vel = (uint8_t)clampv((st->vel ? st->vel : (st->f & ST_WEAK) ? 48 : 100) + d, 1, 127);
            break;
        case E_K2:
            add_i8(&st->micro, d, -50, 50);
            break;
        case E_K3:
            st->prob = (uint8_t)clampv((st->prob ? st->prob : 100) + d, 1, 100) % 100u;
            break;
        case E_K4:
            st->cyc = (uint8_t)clampv(st->cyc + (d > 0 ? 1 : -1), 0, CYC_N - 1);
            break;
        case E_VALUE: {                        /* SUB: none 1/2 1/3 1/4, then the TR-1000 patterns */
            static const uint8_t SUBS[] = {0x00, 0x01, 0x02, 0x03, 0x52, 0x32, 0x62, 0xB3, 0x73, 0x93, 0xD3, 0x13 | 0x40};
            uint32_t i, cur = 0;
            for (i = 0; i < NELEM(SUBS); i++)
                if (SUBS[i] == st->sub)
                    cur = i;
            cur = (uint32_t)clampv((int32_t)cur + (d > 0 ? 1 : -1), 0, NELEM(SUBS) - 1);
            st->sub = SUBS[cur];
            break;
        }
        case E_ALGO:
            st->f ^= ST_FLAM;
            break;
        }
    }
    mark_pat();
}

static void list_turn(const item_t *it, uint32_t n, uint32_t e, int32_t d)
{
    if (e == E_VALUE) {
        ui.cursor = (uint8_t)clampv((int32_t)ui.cursor + (d > 0 ? 1 : -1), 0, (int32_t)n - 1);
    } else if (ui.cursor < n) {
        it[ui.cursor].add(d);
    }
}

static void ui_enc(uint32_t e, int32_t d)
{
    if (!d)
        return;
    ui.last_input = plat_ms();
    mods_used();
    if (ui.held_w && ui.mode == M_TRREC) {     /* step edit */
        knob_step(e, d);
        return;
    }
    if (held(B_SEL) && e == E_VALUE) {         /* ACCENT + VALUE: accent level */
        add_u8(&PAT->acc_lvl, d, 0, 127);
        mark_pat();
        return;
    }
    if ((held(B_ENV) || ui.sub_latch) && e == E_VALUE) {
        ui.subtype = (uint8_t)clampv(ui.subtype + (d > 0 ? 1 : -1), 1, 3);
        return;
    }
    if (black_held(BK_FILL) && e == E_VALUE) {
        PAT->autofill = (uint8_t)clampv(PAT->autofill + (d > 0 ? 1 : -1), 0, 6);
        mark_pat();
        return;
    }
    if (ui.mode == M_LOOP && e == E_VALUE) {
        sq.loop_div = sq.loop_div == 1u ? (d > 0 ? 2u : 1u) : sq.loop_div == 2u ? (d > 0 ? 4u : 1u) : (d > 0 ? 4u : 2u);
        return;
    }
    switch (ui.page) {
    case PG_PTN:
        list_turn(PTN_ITEMS, NELEM(PTN_ITEMS), e, accel(d));
        return;
    case PG_UTIL:
        list_turn(UTIL_ITEMS, NELEM(UTIL_ITEMS), e, d);
        return;
    case PG_TRK: {
        trkset_t *ts = &PAT->trk[ui.trk];
        switch (e) {
        case E_K1: add_u8(&ts->last, d, 0, 16); break;
        case E_K2: add_u8(&ts->dir, d > 0 ? 1 : -1, 0, DIR_N - 1); break;
        case E_K3: add_i8(&ts->rotate, d > 0 ? 1 : -1, -8, 8); break;
        case E_K4: add_i8(&ts->start, d, -50, 50); break;
        case E_ALGO: add_i8(&ts->shuffle, d, -100, 100); break;
        case E_VALUE: ui.trk = (uint8_t)clampv(ui.trk + (d > 0 ? 1 : -1), 0, PZ_NTRK - 1); return;
        default: return;
        }
        mark_pat();
        return;
    }
    case PG_FX: {
        uint32_t t, any = 0;
        for (t = 0; t < PZ_NTRK; t++)          /* a track held: its sends (TR-8S: hold [BD] + REVERB LEVEL) */
            if (black_held(t) && (e == E_K1 || e == E_K2)) {
                add_u8(e == E_K1 ? &kit.t[t].rev : &kit.t[t].dly, accel(d), 0, 127);
                any = 1;
            }
        if (!any)
            switch (e) {
            case E_K1: add_u8(&kit.rev_lvl, accel(d), 0, 127); break;
            case E_K2: add_u8(&kit.dly_lvl, accel(d), 0, 127); break;
            case E_K3: add_u8(&kit.dly_time, d > 0 ? 1 : -1, 0, DT_N - 1); break;
            case E_K4: add_u8(&kit.dly_fb, accel(d), 0, 127); break;
            case E_ALGO: add_u8(&kit.mfx_type, d > 0 ? 1 : -1, 0, MFX_N - 1); break;
            case E_PRESET: add_u8(&kit.mfx_ctrl, accel(d), 0, 127); break;
            case E_VALUE: add_u8(&kit.rev_type, d > 0 ? 1 : -1, 0, REV_N - 1); break;
            }
        if (sq.playing && (sq.motion_rec || held(B_LFO)) && !any) {
            static const int8_t MF[NE] = {-1, -1, MF_MFXCTRL, MF_REVLVL, MF_DLYLVL, MF_DLYTIME, MF_DLYFB};
            int32_t m = MF[e];
            if (m >= 0) {
                uint32_t v = m == MF_REVLVL ? kit.rev_lvl : m == MF_DLYLVL ? kit.dly_lvl : m == MF_DLYFB ? kit.dly_fb
                           : m == MF_DLYTIME ? kit.dly_time * 128u / DT_N : kit.mfx_ctrl;
                motion_set(PAT, sq.sec, sq.sec_step, MOTION_FX, (uint32_t)m, v);
                sq.motion_on = 1;
                mark_pat();
            }
        }
        mark_kit();
        return;
    }
    case PG_INST: {
        kittrk_t *k = &kit.t[ui.trk];
        switch (e) {
        case E_PRESET: {                       /* instrument select, browsing within the category first */
            int32_t i = (int32_t)k->inst + (d > 0 ? 1 : -1);
            if (i < 0)
                i = I_N - 1;
            if (i >= (int32_t)I_N)
                i = 0;
            k->inst = (uint8_t)i;
            if (!sq.playing)
                live_hit(ui.trk, 100);
            break;
        }
        case E_K1: add_i8(&k->pan, accel(d), -64, 63); break;
        case E_K2: add_u8(&k->rev, accel(d), 0, 127); break;
        case E_K3: add_u8(&k->dly, accel(d), 0, 127); break;
        case E_K4: add_u8(&k->color, d > 0 ? 1 : -1, 0, 7); break;
        case E_VALUE: ui.trk = (uint8_t)clampv(ui.trk + (d > 0 ? 1 : -1), 0, PZ_NTRK - 1); return;
        case E_ALGO: {                         /* category jump */
            uint32_t c = INST[k->inst].cat, i = k->inst, n;
            for (n = 0; n < I_N; n++) {
                i = (i + (d > 0 ? 1u : I_N - 1u)) % I_N;
                if (INST[i].cat != c)
                    break;
            }
            c = INST[i].cat;                   /* go to the first instrument of that category */
            while (d < 0 && i > 0 && INST[i - 1u].cat == c)
                i--;
            k->inst = (uint8_t)i;
            if (!sq.playing)
                live_hit(ui.trk, 100);
            break;
        }
        }
        mark_kit();
        return;
    }
    case PG_MIX:
        if (e >= E_K1) {
            uint32_t t = (e - E_K1) + (ui.mix_bank ? 4u : 0u);
            add_u8(&kit.t[t].level, accel(d), 0, 127);
            mark_kit();
        } else if (e == E_ALGO || e == E_VALUE) {
            ui.mix_bank = d > 0;
        } else if (e == E_PRESET) {
            add_i8(&kit.t[ui.trk].pan, accel(d), -64, 63);
            mark_kit();
        }
        return;
    default:
        break;
    }
    if (ui.mode == M_PLAY && e == E_ALGO) {     /* MORPH: the kit <-> the snapshot last recalled */
        sq.morph = (uint8_t)clampv((int32_t)sq.morph + accel(d) * 2, 0, 127);
        return;
    }
    /* the main page */
    switch (e) {
    case E_VALUE:
        sys.tempo10 = (uint16_t)clampv(sys.tempo10 + (held(B_SHIFT) ? d : accel(d) * 10), 400, 3000);
        if (sys.tempo_src) {
            PAT->tempo10 = sys.tempo10;
            mark_pat();
        }
        tempo_apply();
        return;
    case E_ALGO:
        add_i8(&PAT->shuffle, d, -100, 100);
        mark_pat();
        return;
    case E_PRESET:
        kit_select((kit_cur + (d > 0 ? 1u : PZ_NKIT - 1u)) % PZ_NKIT);
        toast("KIT ", kit.name);
        return;
    default:
        knob_track(e - E_K1, accel(d));
        return;
    }
}

/* the main loop's housekeeping: deferred writes, autosave, pattern-linked kits */
static void ui_tick(void)
{
    static uint8_t last_pat = 0xFF;
    uint32_t now = plat_ms();
    clear_sweep();
    if (sq.pat != last_pat) {                  /* a queued pattern started: its kit, if linked */
        last_pat = sq.pat;
        if (PAT->kit_link)
            kit_select(PAT->pkit);
        sys.pat = sq.pat;
    }
    if (!sq.playing && ui.save_pending) {
        ui.save_pending = 0;
        write_now();
    }
    if (sys.autosave && !sq.playing && (ui.pat_dirty || ui.kit_dirty) && now - ui.last_input > 4000u &&
        now - ui.last_input < 4200u) {
        write_now();
        toast("AUTO SAVED", 0);
    }
    if (ui.toast_t && now - ui.toast_t > 1300u)
        ui.toast_t = 0;
}
