/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* The screen. Four strips, each redrawn only when what it shows changed (a hash of its state):
 *
 *     0 ..  33  header: pattern + variation, mode + kit, the red seven-segment tempo
 *    34 ..  51  variations A..H, FILL IN 1 / 2, transport and record status
 *    52 .. 143  the pattern: accent lane + 8 tracks x 16 steps (or the 16 patterns in PTN SELECT),
 *               with the TR-808 red / orange / yellow / white step-group stripe
 *   144 .. 239  the selected instrument and its four knobs, or the page in use
 *
 * Two themes share every layout and differ only in their palette and a few surface details:
 * NOIR after the TR-8S (black, lime-green glow, colour-coded tracks) and ALLOY after the TR-1000
 * (silver-grey panel, graphite displays, restrained orange). */

typedef struct {
    uint16_t bg, panel, panel_hi, line, text, dim, label, accent, accent_txt;
    uint16_t seg_on, seg_off, seg_bg;
    uint16_t cell, cell_edge, play, rec, head_bg, head_bg2, head_text, head_label, knob_bg;
    uint16_t grp[4];
    uint16_t trk[8];
    uint8_t light;
} theme_t;

static const theme_t THEMES[2] = {
    {   /* NOIR: TR-8S */
        HEX(0x000000), HEX(0x0E0F11), HEX(0x1C1F22), HEX(0x2A2E32), HEX(0xF4F4F2), HEX(0x5E646A), HEX(0x9AA1A8),
        HEX(0x9BE22B), HEX(0x0A0C08),
        HEX(0xFF3A22), HEX(0x2E0B07), HEX(0x0A0303),
        HEX(0x1A1D20), HEX(0x2B3034), HEX(0xFFFFFF), HEX(0xFF2E2E), HEX(0x000000), HEX(0x0E0F11), HEX(0xF4F4F2),
        HEX(0x8A9198), HEX(0x24282C),
        {HEX(0xE8382D), HEX(0xF2892B), HEX(0xF5CE2A), HEX(0xEDE8DA)},
        {HEX(0xFF3B30), HEX(0xFF8A1F), HEX(0xFFD21A), HEX(0x9BE22B), HEX(0x23D8B0), HEX(0x2BA8FF), HEX(0x9A6BFF), HEX(0xFF4FA8)},
        0},
    {   /* ALLOY: TR-1000 */
        HEX(0xC3C6C8), HEX(0xD3D6D8), HEX(0xE4E6E7), HEX(0x8D9296), HEX(0x15171A), HEX(0x5F6468), HEX(0x2C3033),
        HEX(0xE5481B), HEX(0xFFFFFF),
        HEX(0xFF4226), HEX(0x3A1511), HEX(0x16181A),
        HEX(0x2B2E31), HEX(0x4A4F53), HEX(0x101214), HEX(0xD8261B), HEX(0x25282B), HEX(0x34383C), HEX(0xF2F2F0),
        HEX(0x9EA4A9), HEX(0xA9AEB2),
        {HEX(0xD63A2A), HEX(0xE7832B), HEX(0xE7C02A), HEX(0xEFEBE0)},
        {HEX(0xD8432E), HEX(0xE07B2A), HEX(0xD9AE1F), HEX(0x76A33A), HEX(0x2E9C8A), HEX(0x3C7FC0), HEX(0x7A5FC2), HEX(0xC2487E)},
        1},
};
#define TH (&THEMES[sys.theme & 1u])

static uint16_t trk_color(uint32_t t) { return TH->trk[kit.t[t].color & 7u]; }

/* --------------------------------------------------------------- hashing --- */
static uint32_t hv;
static void h_begin(void) { hv = 2166136261u; }
static void h_u(uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        hv ^= (v >> (8u * i)) & 255u;
        hv *= 16777619u;
    }
}
static void h_mem(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n--) {
        hv ^= *b++;
        hv *= 16777619u;
    }
}
static uint32_t region_hash[4];

static uint32_t blink(uint32_t ms) { return (plat_ms() / ms) & 1u; }

/* ---------------------------------------------------------------- header --- */
static void draw_header(void)
{
    const theme_t *T = TH;
    char b[24];
    uint32_t t10 = sq.playing && sys.sync && sq.ext_period ? (uint32_t)(600ull * 44100u * 256u / (24ull * sq.ext_period)) : tempo_now();
    int32_t x, i;
    h_begin();
    h_u(sys.theme), h_u(sq.pat), h_u(sq.sec), h_u(sq.pat_next), h_u(ui.mode), h_u(ui.inst_rec), h_u(kit_cur);
    h_u(t10), h_u(ui.pat_dirty), h_u(ui.kit_dirty), h_u(sq.pat_next != 0xFF ? blink(250) : 0), h_u(sq.fill_on);
    h_mem(kit.name, PZ_NAME), h_u(ui.force);
    if (hv == region_hash[0])
        return;
    region_hash[0] = hv;
    cv_begin(0, 34, T->head_bg);
    if (T->light)
        grad(0, 0, SCR_W, 34, T->head_bg2, T->head_bg);
    /* pattern */
    text(6, 1, &F7, "PATTERN", T->head_label);
    fmt_u(b, (sq.pat_next != 0xFF && blink(250) ? sq.pat_next : sq.pat) + 1u);
    if (b[1] == 0)
        b[2] = 0, b[1] = b[0], b[0] = '0';
    text(6, 12, &F12, b, sq.pat_next != 0xFF ? T->accent : T->head_text);
    {
        uint32_t sec = sq.sec;
        char v[3] = {(char)(sec < 8u ? 'A' + sec : 'F'), sec < 8u ? 0 : (char)('1' + sec - 8u), 0};
        pad(34, 14, 20, 17, sq.fill_on ? T->rec : T->accent);
        text_c(44, 14, &F12, v[1] ? "F" : v, T->accent_txt);
        if (v[1])
            text(49, 22, &F7, v + 1, T->accent_txt);
    }
    if (ui.pat_dirty & (1u << sq.pat))
        disc(29, 15, 2, T->accent);            /* edited, not written */
    /* mode and kit */
    x = 64;
    vline(x - 4, 4, 26, T->light ? HEX(0x4A4F53) : T->line);
    text(x, 1, &F7, ui.mode == M_PLAY && ui.inst_rec ? "INST REC" : MODE_NAME[ui.mode],
         ui.mode == M_PLAY && ui.inst_rec ? T->rec : T->accent);
    sz_cpy(b, "KIT ", sizeof b);
    fmt_u(b + 4, kit_cur + 1u);
    text(x, 13, &F7, b, T->head_label);
    sz_cpy(b, kit.name, 12);
    if (ui.kit_dirty & (1u << kit_cur))
        sz_cat(b, "*");
    text(x + 30, 10, &F12, b, T->head_text);
    /* tempo: the red LED display */
    {
        int32_t dx = 150, dw = 86, digits[4], n;
        pad(dx, 3, dw, 28, T->seg_bg);
        n = (int32_t)(t10 > 9999u ? 9999u : t10);
        digits[0] = n >= 1000 ? n / 1000 : 11;
        digits[1] = n >= 100 ? (n / 100) % 10 : 11;
        digits[2] = (n / 10) % 10;
        digits[3] = n % 10;
        for (i = 0; i < 4; i++) {
            int32_t gx = dx + 5 + i * 19 + (i == 3 ? 4 : 0);
            seg_digit(gx, 7, 15, 20, 3, (uint32_t)digits[i], T->seg_on, T->seg_off);
        }
        fill(dx + 5 + 3 * 19 - 1, 24, 3, 3, T->seg_on);   /* the decimal point */
    }
    cv_end();
}

/* -------------------------------------------------------------- var bar --- */
static void draw_varbar(void)
{
    const theme_t *T = TH;
    uint32_t i, chain = 0;
    h_begin();
    for (i = 0; i < sq.chain_n; i++)
        chain |= 1u << sq.chain[i];
    h_u(sys.theme), h_u(chain), h_u(sq.sec), h_u(sq.rec_sec), h_u(sq.playing), h_u(ui.mode), h_u(ui.force);
    h_u(sq.motion_on | sq.motion_rec << 1), h_u(sq.fill_on), h_u(PAT->autofill), h_u(sq.playing ? sq.beat : 0);
    h_u(sq.motion_rec ? blink(300) : 0), h_u(sys.sync);
    for (i = 0; i < PZ_NSEC; i++)
        h_u(pat_sec_used(PAT, i));
    if (hv == region_hash[1])
        return;
    region_hash[1] = hv;
    cv_begin(34, 18, T->bg);
    if (T->light)
        fill(0, 34, SCR_W, 18, T->panel);
    for (i = 0; i < PZ_NSEC; i++) {
        int32_t x = i < 8u ? 3 + (int32_t)i * 20 : 166 + (int32_t)(i - 8u) * 18, w = i < 8u ? 18 : 17;
        uint32_t playing = sq.sec == i, inchain = (chain >> i) & 1u, used = pat_sec_used(PAT, i);
        char s[3] = {(char)(i < 8u ? 'A' + i : 'F'), i < 8u ? 0 : (char)('1' + i - 8u), 0};
        uint16_t bg = T->cell, fg = used ? T->text : T->dim;
        if (T->light)
            fg = used ? HEX(0xF2F2F0) : HEX(0x7A7F83);
        if (playing) {
            bg = i >= 8u ? T->rec : T->accent;
            fg = T->accent_txt;
        } else if (inchain) {
            bg = T->light ? HEX(0x4A4F53) : T->panel_hi;
            fg = T->accent;
        }
        pad(x, 36, w, 14, bg);
        if (inchain && !playing)
            pad_frame(x, 36, w, 14, T->accent);
        if (sq.rec_sec == i && ui.mode == M_TRREC)
            fill(x + 2, 48, w - 4, 2, T->rec);
        text_c(x + w / 2, i < 8u ? 36 : 38, i < 8u ? &F9 : &F7, s, fg);
    }
    /* status: play / stop, motion, sync */
    {
        int32_t x = 204;
        uint16_t c = sq.playing ? (sq.beat ? T->accent : (T->light ? T->text : T->text)) : T->dim;
        if (sq.playing) {
            int32_t k;
            for (k = 0; k < 9; k++)
                vline(x + k / 1, 38 + k / 2, 10 - k, c);   /* a play triangle */
        } else {
            fill(x, 39, 8, 8, c);
        }
        if (sq.motion_on || sq.motion_rec) {
            uint16_t mc = sq.motion_rec ? (blink(300) ? T->rec : T->dim) : (T->light ? T->text : T->label);
            text(x + 12, 37, &F9, "M", mc);
        }
        if (sys.sync)
            text(x + 26, 37, &F7, "S", T->accent);
    }
    cv_end();
}

/* ------------------------------------------------------------------ grid --- */
#define GRID_Y 52
#define ROW_Y(r) (62 + (r) * 10)
#define COL_X(s) (30 + (s) * 12 + ((s) / 4) * 2)

static void draw_cell(int32_t x, int32_t y, const step_t *st, uint16_t on, int32_t active, int32_t ph, int32_t held, int32_t mot)
{
    const theme_t *T = TH;
    uint16_t off = active ? T->cell : mix565(T->cell, T->light ? T->panel : T->bg, 9);
    int32_t w = 11, h = 9;
    if (!(st->f & ST_ON)) {
        pad(x, y, w, h, off);
    } else {
        uint32_t div = st->sub & 3u, mask = st->sub >> 4 ? st->sub >> 4 : 15u;
        uint32_t vel = st->vel ? st->vel : (st->f & ST_WEAK) ? 48u : 100u;
        uint16_t c = (st->f & ST_WEAK) ? mix565(off, on, 8) : mix565(off, on, vel >= 100u ? 15u : 6u + vel * 9u / 100u);
        if (!active)
            c = mix565(c, off, 9);
        if (div) {                             /* sub steps: the cell in slices */
            uint32_t n = div + 1u, i;
            int32_t sw = (w - (int32_t)n + 1) / (int32_t)n;
            pad(x, y, w, h, off);
            for (i = 0; i < n; i++)
                fill(x + (int32_t)i * (sw + 1), y + 1, sw, h - 2, (mask >> i) & 1u ? c : mix565(off, c, 4));
        } else {
            pad(x, y, w, h, c);
        }
        if (st->f & ST_FLAM)
            fill(x + 1, y + 1, 2, h - 2, mix565(c, T->bg, 10));
        if (st->prob) {                        /* probability: a dotted cell */
            int32_t i, j;
            for (j = 1; j < h - 1; j += 2)
                for (i = 1 + (j & 2) / 2; i < w - 1; i += 2)
                    px_set(x + i, y + j, mix565(c, off, 10));
        }
        if (st->cyc)
            fill(x + w - 3, y, 3, 2, T->light ? T->text : HEX(0xFFFFFF));
        if (st->micro)
            fill(st->micro < 0 ? x : x + w - 1, y + 2, 1, h - 4, T->light ? T->text : HEX(0xFFFFFF));
        if (st->f & ST_ALT)
            fill(x + 3, y + h - 2, w - 6, 1, T->light ? T->text : HEX(0xFFFFFF));
    }
    if (mot)
        fill(x + 4, y + h - 1, 3, 1, T->accent);
    if (ph)
        pad_frame(x - 1, y - 1, w + 2, h + 2, T->play);
    if (held)
        pad_frame(x - 1, y - 1, w + 2, h + 2, T->accent);
}

static void draw_patterns(void)                /* PTN SELECT: the 16 patterns as 4 x 4 pads */
{
    const theme_t *T = TH;
    uint32_t i;
    for (i = 0; i < PZ_NPAT; i++) {
        int32_t x = 4 + (int32_t)(i % 4u) * 59, y = GRID_Y + 2 + (int32_t)(i / 4u) * 22, used = 0;
        uint32_t s;
        char b[8];
        uint16_t bg = T->cell, fg = T->light ? HEX(0xF2F2F0) : T->text;
        for (s = 0; s < PZ_NSEC && !used; s++)
            used = pat_sec_used(&pats[i], s) != 0u;
        if (i == sq.pat) {
            bg = T->accent;
            fg = T->accent_txt;
        } else if (i == sq.pat_next && blink(200)) {
            bg = T->accent;
            fg = T->accent_txt;
        }
        pad(x, y, 55, 19, bg);
        if (sq.pat_lo != sq.pat_hi && i >= sq.pat_lo && i <= sq.pat_hi)
            pad_frame(x, y, 55, 19, T->rec);
        fmt_u(b, i + 1u);
        text(x + 4, y + 1, &F9, b, fg);
        if (used)
            fill(x + 46, y + 7, 5, 5, i == sq.pat ? T->accent_txt : T->grp[i / 4u]);
        if (ui.pat_dirty & (1u << i))
            text(x + 22, y + 1, &F9, "*", fg);
    }
}

static void draw_grid(void)
{
    const theme_t *T = TH;
    const pattern_t *p = PAT;
    uint32_t t, s, sec = sq.playing && ui.mode != M_TRREC ? sq.sec : sq.rec_sec;
    uint32_t follow = sq.playing && sq.sec == sec;
    uint32_t vlast = sec_last(sec);
    h_begin();
    h_u(sys.theme), h_u(ui.mode), h_u(sec), h_u(ui.trk), h_u(ui.held_w), h_u(sq.mute), h_u(sq.solo), h_u(ui.force);
    h_u(follow), h_u(kit_cur), h_u(sq.loop_step);
    if (ui.mode == M_PTN) {
        h_u(sq.pat), h_u(sq.pat_next), h_u(sq.pat_next != 0xFF ? blink(200) : 0), h_u(sq.pat_lo), h_u(sq.pat_hi), h_u(ui.pat_dirty);
    } else {
        h_mem(p->s[sec], sizeof p->s[sec]);
        h_mem(p->acc[sec], PZ_NSTEP);
        h_mem(p->trk, sizeof p->trk);
        h_u(vlast);
        for (t = 0; t < PZ_NTRK; t++)
            h_u(kit.t[t].color | kit.t[t].inst << 8), h_u(follow ? sq.tpos[t] : 0xFF), h_u(sq.hit_flash[t] > 3);
        h_u(motion_count(p));
    }
    if (hv == region_hash[2])
        return;
    region_hash[2] = hv;
    cv_begin(GRID_Y, 92, T->bg);
    if (T->light)
        fill(0, GRID_Y, SCR_W, 92, T->panel);
    if (ui.mode == M_PTN) {
        draw_patterns();
        cv_end();
        return;
    }
    /* accent lane */
    text(6, GRID_Y - 2, &F7, "ACC", T->light ? T->label : T->dim);
    for (s = 0; s < PZ_NSTEP; s++) {
        int32_t x = COL_X(s);
        uint16_t c = p->acc[sec][s] ? T->rec : (s < vlast ? T->cell : mix565(T->cell, T->bg, 9));
        fill(x, GRID_Y + 2, 11, 4, c);
    }
    /* tracks */
    for (t = 0; t < PZ_NTRK; t++) {
        int32_t y = ROW_Y(t), sel = t == ui.trk, muted = trk_muted(t);
        uint32_t len = p->trk[t].last ? p->trk[t].last : vlast;
        uint16_t tc = trk_color(t);
        const char *lbl = INST[kit.t[t].inst].lbl;
        if (sel) {
            pad(1, y - 1, 26, 11, tc);
            text_c(14, y - 3, &F7, lbl, T->light ? HEX(0xFFFFFF) : HEX(0x000000));
        } else {
            text_c(14, y - 3, &F7, lbl, muted ? T->dim : tc);
            if (sq.hit_flash[t] > 3)
                fill(25, y + 2, 2, 5, tc);
        }
        if (muted)
            hline(4, y + 4, 20, T->rec);
        for (s = 0; s < PZ_NSTEP; s++) {
            const step_t *st = &p->s[sec][t][s];
            int32_t ph = follow && sq.tpos[t] == s && sq.playing;
            int32_t mot = 0;
            if (st->f & ST_ON) {
                uint32_t k;
                for (k = 0; k < MP_N && !mot; k++)
                    mot = motion_find(p, sec, s, t, k) >= 0;
            }
            draw_cell(COL_X(s), y, st, muted ? mix565(tc, T->cell, 10) : tc, s < len, ph,
                      sel && ((ui.held_w >> s) & 1u) && ui.mode == M_TRREC, mot);
        }
    }
    /* the TR step-group stripe (red, orange, yellow, white) */
    for (s = 0; s < 4u; s++)
        fill(COL_X(s * 4u), 142, 4 * 12 - 1, 2, T->grp[s]);
    if (sq.loop_step < PZ_NSTEP)
        fill(COL_X(sq.loop_step), 52, 11, 2, T->accent);
    cv_end();
}

/* ----------------------------------------------------------------- knobs --- */
static void knob(int32_t cx, int32_t cy, int32_t v, int32_t lo, int32_t hi, int32_t bip, const char *val,
                 const char *label, uint16_t c)
{
    const theme_t *T = TH;
    int32_t a = (v - lo) * 270 / (hi - lo > 0 ? hi - lo : 1), a0, a1;
    if (T->light) {
        disc(cx, cy, 17, HEX(0x2B2E31));       /* a graphite cap on silver */
        disc(cx, cy, 13, HEX(0x3A3E42));
    } else {
        disc(cx, cy, 17, T->knob_bg);
        disc(cx, cy, 13, HEX(0x111315));
    }
    arc(cx, cy, 20, 3, 225, 359, T->light ? HEX(0x9EA3A7) : T->line);
    arc(cx, cy, 20, 3, 0, 135, T->light ? HEX(0x9EA3A7) : T->line);
    if (bip) {
        a0 = a < 135 ? 225 + a : 360;
        a1 = a < 135 ? 360 : 225 + a;
    } else {
        a0 = 225;
        a1 = 225 + a;
    }
    if (a1 > a0) {
        if (a0 < 360)
            arc(cx, cy, 20, 3, a0, a1 < 359 ? a1 : 359, c);
        if (a1 > 360)
            arc(cx, cy, 20, 3, a0 > 360 ? a0 - 360 : 0, a1 - 360, c);
    }
    {   /* the pointer */
        int32_t ang = 225 + a, sx = isin((uint32_t)ang), cx2 = isin((uint32_t)ang + 90u);
        int32_t px = cx + sx * 10 / 32768, py = cy - cx2 * 10 / 32768;
        disc(px, py, 2, c);
    }
    text_c(cx, cy + 21, &F9, val, T->light ? T->text : T->text);
    text_c(cx, cy + 35, &F7, label, T->light ? T->label : T->dim);
}

static void param_title(const char *chip, uint16_t chipc, const char *name, const char *right)
{
    const theme_t *T = TH;
    if (chip) {
        int32_t w = text_w(&F9, chip) + 8;
        pad(4, 147, w, 15, chipc);
        text(8, 146, &F9, chip, T->light ? HEX(0xFFFFFF) : HEX(0x000000));
        text(w + 9, 144, &F12, name, T->text);
    } else {
        text(5, 144, &F12, name, T->text);
    }
    if (right && text_w(&F12, name) + (chip ? text_w(&F9, chip) + 17 : 5) + text_w(&F7, right) + 8 < SCR_W)
        text_r(235, 148, &F7, right, T->light ? T->label : T->dim);
}

static void ctrl_label(char *b, uint32_t t)
{
    const inst_t *in = &INST[kit.t[t].inst];
    if (in->kind == K_SYN)
        sz_cpy(b, CT_NAME[SYN[in->idx].ctrl], 10);
    else
        sz_cpy(b, "TONE", 10);
}

static void draw_list(const item_t *it, uint32_t n, const char *title)
{
    const theme_t *T = TH;
    uint32_t first = ui.cursor > 2u ? ui.cursor - 2u : 0u, i;
    char b[24];
    if (first + 5u > n)
        first = n > 5u ? n - 5u : 0u;
    param_title(0, 0, title, "SELECT  KNOB 1");
    for (i = first; i < first + 5u && i < n; i++) {
        int32_t y = 164 + (int32_t)(i - first) * 15;
        int sel = i == ui.cursor;
        if (sel)
            pad(2, y, 236, 14, T->light ? HEX(0x2B2E31) : T->panel_hi);
        text(8, y, &F9, it[i].name, sel ? T->accent : (T->light ? T->label : T->label));
        b[0] = 0;
        it[i].show(b);
        text_r(232, y, &F9, b, sel ? (T->light ? HEX(0xFFFFFF) : T->text) : (T->light ? T->text : T->text));
    }
}

static void draw_param(void)
{
    const theme_t *T = TH;
    uint32_t t = ui.trk;
    const kittrk_t *k = &kit.t[t];
    const inst_t *in = &INST[k->inst];
    uint16_t tc = trk_color(t);
    char b[4][14];
    char right[24];
    h_begin();
    h_u(sys.theme), h_u(ui.page), h_u(t), h_mem(k, sizeof *k), h_u(ui.held_w), h_u(ui.mode), h_u(ui.force);
    h_u(ui.toast_t ? 1u : 0u), h_mem(ui.toast, sizeof ui.toast), h_u(ui.cursor), h_u(ui.mix_bank);
    h_u(ui.sub_latch | ui.flam_latch << 1 | ui.acc_latch << 2 | ui.subtype << 4), h_u(init_arm);
    h_u(sq.morph | sq.morph_to << 8 | ui.snap_layer << 16);
    if (ui.held_w)
        h_mem(PAT->s[sq.rec_sec][t], sizeof PAT->s[0][0]), h_mem(PAT->mo, sizeof PAT->mo);
    if (ui.page == PG_FX)
        h_u(kit.rev_lvl | kit.dly_lvl << 8 | kit.dly_time << 16 | kit.dly_fb << 24), h_u(kit.mfx_type | kit.mfx_ctrl << 8 | kit.mfx_on << 16 | kit.rev_type << 24), h_u(ui.held_b);
    if (ui.page == PG_PTN)
        h_mem(PAT, 64), h_u(sys.tempo10);
    if (ui.page == PG_UTIL)
        h_mem(&sys, sizeof sys);
    if (ui.page == PG_TRK)
        h_mem(&PAT->trk[t], sizeof(trkset_t));
    if (ui.page == PG_MIX) {
        uint32_t i;
        for (i = 0; i < PZ_NTRK; i++)
            h_u(kit.t[i].level), h_u((uint32_t)vc[i].meter >> 11);
        h_u(sq.mute), h_u(sq.solo);
    }
    if (hv == region_hash[3])
        return;
    region_hash[3] = hv;
    cv_begin(144, 96, T->bg);
    if (T->light) {
        grad(0, 144, SCR_W, 96, T->panel_hi, T->bg);
        hline(0, 144, SCR_W, HEX(0xF4F5F6));
    } else {
        hline(0, 144, SCR_W, T->line);
    }
    if (ui.toast_t) {                          /* a message over the title */
        pad(2, 146, 236, 17, T->accent);
        text_c(120, 146, &F12, ui.toast, T->accent_txt);
    }
    right[0] = 0;
    if (ui.held_w && ui.mode == M_TRREC && ui.page == PG_MAIN) {     /* step edit */
        uint32_t s = 0;
        const step_t *st;
        char nm[20];
        while (!((ui.held_w >> s) & 1u))
            s++;
        st = &PAT->s[sq.rec_sec][t][s];
        sz_cpy(nm, "STEP ", sizeof nm);
        fmt_u(nm + 5, s + 1u);
        if (st->sub & 3u) {
            static const char *const SD[4] = {"", " 1/2", " 1/3", " 1/4"};
            sz_cat(nm, SD[st->sub & 3u]);
        }
        if (st->f & ST_FLAM)
            sz_cat(nm, " FLAM");
        if (!ui.toast_t)
            param_title(in->lbl, tc, nm, held(B_SHIFT) ? "SHIFT: MOTION" : "VALUE: SUB  ALGO: FLAM");
        if (held(B_SHIFT)) {
            static const char *const ML[MP_N] = {"TUNE", "DECAY", "CTRL", "LEVEL"};
            uint32_t i;
            for (i = 0; i < MP_N; i++) {
                int m = motion_find(PAT, sq.rec_sec, s, t, i);
                if (m >= 0)
                    fmt_i(b[i], i == MP_TUNE ? (int32_t)PAT->mo[m].val - 64 : PAT->mo[m].val, i == MP_TUNE);
                else
                    sz_cpy(b[i], "----", 6);
                knob(30 + (int32_t)i * 60, 186, m >= 0 ? PAT->mo[m].val : 0, 0, 127, 0, b[i], ML[i], m >= 0 ? tc : T->dim);
            }
        } else {
            uint32_t vel = st->vel ? st->vel : (st->f & ST_WEAK) ? 48u : 100u;
            fmt_u(b[0], vel);
            fmt_i(b[1], st->micro, 1);
            fmt_u(b[2], st->prob ? st->prob : 100u);
            sz_cat(b[2], "%");
            cond_name(st->cyc, b[3]);
            knob(30, 186, (int32_t)vel, 0, 127, 0, b[0], "VELOCITY", tc);
            knob(90, 186, st->micro, -50, 50, 1, b[1], "START", tc);
            knob(150, 186, st->prob ? st->prob : 100, 0, 100, 0, b[2], "PROB", tc);
            knob(210, 186, (int32_t)st->cyc, 0, CYC_N - 1, 0, b[3], "CYCLE", tc);
        }
        cv_end();
        return;
    }
    switch (ui.page) {
    case PG_PTN:
        draw_list(PTN_ITEMS, NELEM(PTN_ITEMS), "PATTERN SETTING");
        break;
    case PG_UTIL:
        draw_list(UTIL_ITEMS, NELEM(UTIL_ITEMS), "UTILITY");
        break;
    case PG_TRK: {
        const trkset_t *ts = &PAT->trk[t];
        if (!ui.toast_t) {
            sz_cpy(right, "SHUFFLE ", sizeof right);
            fmt_i(right + 8, ts->shuffle, 1);
            param_title(in->lbl, tc, "TRACK SETTING", right);
        }
        if (ts->last)
            fmt_u(b[0], ts->last);
        else
            sz_cpy(b[0], "VAR", 4);
        sz_cpy(b[1], DIR_NAME[ts->dir % DIR_N], 6);
        fmt_i(b[2], ts->rotate, 1);
        fmt_i(b[3], ts->start, 1);
        knob(30, 186, ts->last, 0, 16, 0, b[0], "LAST", tc);
        knob(90, 186, ts->dir, 0, DIR_N - 1, 0, b[1], "DIRECTION", tc);
        knob(150, 186, ts->rotate, -8, 8, 1, b[2], "ROTATE", tc);
        knob(210, 186, ts->start, -50, 50, 1, b[3], "START", tc);
        break;
    }
    case PG_FX: {
        if (!ui.toast_t) {
            sz_cpy(right, MFX_NAME[kit.mfx_type % MFX_N], sizeof right);
            sz_cat(right, kit.mfx_on ? " ON " : " OFF ");
            fmt_u(right + strlen_s(right), kit.mfx_ctrl);
            param_title(0, 0, REV_NAME[kit.rev_type % REV_N], right);
            text(text_w(&F12, REV_NAME[kit.rev_type % REV_N]) + 9, 148, &F7, "REVERB", T->light ? T->label : T->dim);
        }
        if (ui.held_b & 0xFFu) {               /* a track held: its sends */
            uint32_t i = 0;
            while (!((ui.held_b >> i) & 1u))
                i++;
            fmt_u(b[0], kit.t[i].rev);
            fmt_u(b[1], kit.t[i].dly);
            knob(30, 186, kit.t[i].rev, 0, 127, 0, b[0], "REV", trk_color(i));
            knob(90, 186, kit.t[i].dly, 0, 127, 0, b[1], "DLY", trk_color(i));
        } else {
            fmt_u(b[0], kit.rev_lvl);
            fmt_u(b[1], kit.dly_lvl);
            fmt_u(b[3], kit.dly_fb);
            knob(30, 186, kit.rev_lvl, 0, 127, 0, b[0], "REVERB", T->accent);
            knob(90, 186, kit.dly_lvl, 0, 127, 0, b[1], "DELAY", T->accent);
            knob(150, 186, kit.dly_time, 0, DT_N - 1, 0, DT_NAME[kit.dly_time % DT_N], "TIME", T->accent);
            knob(210, 186, kit.dly_fb, 0, 127, 0, b[3], "FEEDBK", T->accent);
        }
        break;
    }
    case PG_INST: {
        static const char *const CAT_NAME[CAT_N] = {"KICK", "SNARE", "TOM", "RIM", "CLAP", "HI-HAT", "OPEN HAT",
                                                    "CYMBAL", "PERC"};
        if (!ui.toast_t) {
            sz_cpy(right, CAT_NAME[in->cat], sizeof right);
            sz_cat(right, in->kind == K_PCM ? " / SAMPLE" : " / ANALOG");
            param_title(in->lbl, tc, in->name, right);
        }
        fmt_i(b[0], k->pan, 1);
        fmt_u(b[1], k->rev);
        fmt_u(b[2], k->dly);
        fmt_u(b[3], k->color + 1u);
        knob(30, 186, k->pan, -64, 63, 1, b[0], "PAN", tc);
        knob(90, 186, k->rev, 0, 127, 0, b[1], "REV", tc);
        knob(150, 186, k->dly, 0, 127, 0, b[2], "DLY", tc);
        knob(210, 186, k->color, 0, 7, 0, b[3], "COLOR", tc);
        break;
    }
    case PG_MIX: {
        uint32_t i;
        if (!ui.toast_t)
            param_title(0, 0, "MIXER", ui.mix_bank ? "KNOBS: 5-8" : "KNOBS: 1-4");
        for (i = 0; i < PZ_NTRK; i++) {
            int32_t x = 8 + (int32_t)i * 29, h = kit.t[i].level * 52 / 127, m = vc[i].meter * 52 / 32768;
            uint16_t c = trk_color(i);
            int on_knobs = (i >= 4u) == (ui.mix_bank != 0u);
            pad(x, 166, 22, 56, T->light ? HEX(0x2B2E31) : T->cell);
            fill(x + 3, 220 - h, 7, h, trk_muted(i) ? T->dim : c);
            if (m > 52)
                m = 52;
            fill(x + 13, 220 - m, 6, m, mix565(c, HEX(0xFFFFFF), 6));
            text_c(x + 11, 224, &F7, INST[kit.t[i].inst].lbl, on_knobs ? c : (T->light ? T->label : T->dim));
            if (i == ui.trk)
                hline(x, 237, 22, c);
        }
        break;
    }
    default: {                                 /* MAIN: the TR-8S instrument knobs */
        char ct[12];
        if (!ui.toast_t) {
            if (ui.sub_latch) {
                static const char *const SD[4] = {"", "SUB 1/2", "SUB 1/3", "SUB 1/4"};
                sz_cpy(right, SD[ui.subtype & 3u], sizeof right);
            } else if (ui.flam_latch) {
                sz_cpy(right, "FLAM INPUT", sizeof right);
            } else if (ui.acc_latch) {
                sz_cpy(right, "ACCENT INPUT", sizeof right);
            } else if (ui.mode == M_PLAY && (sq.morph || ui.snap_layer)) {
                sz_cpy(right, "MORPH S", sizeof right);
                right[7] = (char)('1' + sq.morph_to);
                right[8] = ' ';
                fmt_u(right + 9, sq.morph * 100u / 127u);
                sz_cat(right, "%");
            } else {
                sz_cpy(right, "SHUFFLE ", sizeof right);
                fmt_i(right + 8, PAT->shuffle, 1);
            }
            param_title(in->lbl, tc, in->name, right);
        }
        ctrl_label(ct, t);
        fmt_i(b[0], k->tune, 1);
        fmt_u(b[1], k->decay);
        fmt_u(b[2], k->ctrl);
        fmt_u(b[3], k->level);
        knob(30, 186, k->tune, -64, 63, 1, b[0], "TUNE", tc);
        knob(90, 186, k->decay, 0, 127, 0, b[1], "DECAY", tc);
        knob(150, 186, k->ctrl, 0, 127, 0, b[2], ct, tc);
        knob(210, 186, k->level, 0, 127, 0, b[3], "LEVEL", tc);
        if (sq.morph && PAT->snap[sq.morph_to % PZ_NSNAP].used) {   /* where the morph has taken them */
            static const int16_t KX[MP_N] = {30, 90, 150, 210};
            uint32_t i;
            for (i = 0; i < MP_N; i++) {
                int32_t v = (int32_t)knob_val(t, i), a = v * 270 / 127, ang = 225 + a;
                int32_t px = KX[i] + isin((uint32_t)ang) * 16 / 32768, py = 186 - isin((uint32_t)ang + 90u) * 16 / 32768;
                disc(px, py, 2, T->light ? HEX(0xFFFFFF) : HEX(0xFFFFFF));
            }
        }
        break;
    }
    }
    cv_end();
}

static void ui_draw(void)
{
    uint32_t t;
    if (ui.force) {
        region_hash[0] = region_hash[1] = region_hash[2] = region_hash[3] = 0;
    }
    draw_header();
    draw_varbar();
    draw_grid();
    draw_param();
    ui.force = 0;
    for (t = 0; t < PZ_NTRK; t++)
        if (sq.hit_flash[t])
            sq.hit_flash[t]--;
}

/* ------------------------------------------------------------------ LEDs --- */
/* ids: 0..15 white keys, 16..26 black keys, 27.. buttons (B_*) */
#define LED_WHITE(i) (i)
#define LED_BLACK(i) (16u + (i))
#define LED_BTN(b) (27u + (b))
static void ui_leds(void)
{
    uint32_t i, sec = sq.playing && ui.mode != M_TRREC ? sq.sec : sq.rec_sec;
    for (i = 0; i < 16u; i++) {
        uint32_t lv = 0;
        switch (ui.mode) {
        case M_TRREC: {
            const step_t *st = &PAT->s[sec][ui.trk][i];
            if (ui.acc_latch || held(B_SEL))
                lv = PAT->acc[sec][i] ? 2u : 0u;
            else
                lv = (st->f & ST_ON) ? 2u : 0u;
            if (sq.playing && sq.sec == sec && sq.tpos[ui.trk] == i)
                lv = lv ? 1u : 2u;             /* the running light: inverted on a lit step */
            if (held(B_VAR))
                lv = i < 8u ? (sq.sec == i ? 2u : pat_sec_used(PAT, i) ? 1u : 0u) : 0u;
            break;
        }
        case M_PTN:
            lv = i == sq.pat ? 2u : (i == sq.pat_next && blink(200)) ? 2u : 0u;
            break;
        case M_PLAY:
            if (ui.snap_layer)                 /* the snapshots: stored dim, the one in force lit */
                lv = i < 8u ? (PAT->snap[i].used ? (sq.morph && sq.morph_to == i ? 2u : 1u) : 0u) : i >= 14u ? 2u : 0u;
            else
                lv = i < 8u ? (sq.hit_flash[i] ? 2u : 1u) : (i < 11u && ui.roll) ? 2u : 0u;
            break;
        case M_LOOP:
            lv = sq.loop_step == i ? 2u : (sq.playing && sq.tpos[0] == i) ? 1u : 0u;
            break;
        }
        plat_led(LED_WHITE(i), lv);
    }
    for (i = 0; i < 11u; i++) {
        uint32_t lv = 0;
        if (i < PZ_NTRK)
            lv = i == ui.trk ? 2u : trk_muted(i) ? 0u : sq.hit_flash[i] ? 2u : 0u;
        else if (i == BK_FILL)
            lv = sq.fill_on ? 2u : PAT->autofill ? 1u : 0u;
        plat_led(LED_BLACK(i), lv);
    }
    plat_led(LED_BTN(B_PLAY), sq.playing ? (sq.beat ? 2u : 1u) : 0u);
    plat_led(LED_BTN(B_REC), ui.mode == M_TRREC ? 2u : (ui.mode == M_PLAY && ui.inst_rec) ? (blink(250) ? 2u : 0u) : 0u);
    plat_led(LED_BTN(B_SEQ), ui.mode == M_PTN ? 2u : 0u);
    plat_led(LED_BTN(B_ARP), ui.mode == M_PLAY ? 2u : ui.mode == M_LOOP ? (blink(250) ? 2u : 0u) : 0u);
    plat_led(LED_BTN(B_LFO), sq.motion_rec ? (blink(250) ? 2u : 0u) : sq.motion_on ? 2u : 0u);
    plat_led(LED_BTN(B_ENV), ui.sub_latch || ui.flam_latch ? 2u : 0u);
    plat_led(LED_BTN(B_SEL), ui.acc_latch ? 2u : 0u);
    plat_led(LED_BTN(B_FX), ui.page == PG_FX ? 2u : kit.mfx_on ? 1u : 0u);
    plat_led(LED_BTN(B_EDIT), ui.page == PG_INST || ui.page == PG_TRK ? 2u : 0u);
    plat_led(LED_BTN(B_GLO), ui.page == PG_MIX ? 2u : (sq.mute || sq.solo) ? 1u : 0u);
    plat_led(LED_BTN(B_SAVE), ui.copy_arm ? (blink(200) ? 2u : 0u) : (ui.pat_dirty || ui.kit_dirty) ? 1u : 0u);
    plat_led(LED_BTN(B_HOME), ui.page == PG_UTIL ? 2u : 0u);
    plat_led(LED_BTN(B_VAR), sq.chain_n > 1u ? 1u : 0u);
    plat_led(LED_BTN(B_SHIFT), 0);
}
