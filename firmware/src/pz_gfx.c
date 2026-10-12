/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Drawing: one horizontal strip of the 240 x 240 screen at a time is drawn into a RAM canvas in
 * screen coordinates, then sent in one transfer (no full frame buffer: RAM is for patterns).
 * Colours are native RGB565 here; the platform swaps bytes for the panel. Text is anti-aliased
 * (4-bit coverage, blended onto what is below). */
#include "pz_fonts.h"

#define SCR_W 240
#define SCR_H 240
#define CV_ROWS 100u
static uint16_t cv[SCR_W * CV_ROWS] PZ_POOL;
static int32_t cv_y0, cv_h;                    /* the strip: rows cv_y0 .. cv_y0 + cv_h - 1 */
static int32_t clip_x0 = 0, clip_x1 = SCR_W;

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HEX(h) RGB(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)

static void cv_begin(int32_t y0, int32_t h, uint16_t bg)
{
    uint32_t i, n;
    if (h > (int32_t)CV_ROWS)
        h = CV_ROWS;
    plat_sync();
    cv_y0 = y0;
    cv_h = h;
    n = (uint32_t)(SCR_W * h);
    for (i = 0; i < n; i++)
        cv[i] = bg;
    clip_x0 = 0;
    clip_x1 = SCR_W;
}
static void cv_end(void) { plat_blit(0, (uint32_t)cv_y0, SCR_W, (uint32_t)cv_h, cv); }

static inline uint16_t blend(uint16_t d, uint16_t s, uint32_t a)   /* a 0..15 */
{
    int32_t dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    int32_t sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31, k = (int32_t)a;
    dr += ((sr - dr) * k + (sr > dr ? 7 : -7)) / 15;
    dg += ((sg - dg) * k + (sg > dg ? 7 : -7)) / 15;
    db += ((sb - db) * k + (sb > db ? 7 : -7)) / 15;
    return (uint16_t)(dr << 11 | dg << 5 | db);
}
static inline uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)  /* t 0..15: a -> b */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    ar += (br - ar) * (int32_t)t / 15;
    ag += (bg - ag) * (int32_t)t / 15;
    ab += (bb - ab) * (int32_t)t / 15;
    return (uint16_t)(ar << 11 | ag << 5 | ab);
}

static inline void px_set(int32_t x, int32_t y, uint16_t c)
{
    y -= cv_y0;
    if (x >= clip_x0 && x < clip_x1 && (uint32_t)y < (uint32_t)cv_h)
        cv[y * SCR_W + x] = c;
}
static inline void px_blend(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    y -= cv_y0;
    if (x >= clip_x0 && x < clip_x1 && (uint32_t)y < (uint32_t)cv_h)
        cv[y * SCR_W + x] = a >= 15u ? c : blend(cv[y * SCR_W + x], c, a);
}

static void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h, i, j;
    if (x < clip_x0)
        x = clip_x0;
    if (x1 > clip_x1)
        x1 = clip_x1;
    if (y < cv_y0)
        y = cv_y0;
    if (y1 > cv_y0 + cv_h)
        y1 = cv_y0 + cv_h;
    for (j = y; j < y1; j++) {
        uint16_t *p = &cv[(j - cv_y0) * SCR_W];
        for (i = x; i < x1; i++)
            p[i] = c;
    }
}
static void fill_a(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint32_t a)
{
    int32_t i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            px_blend(i, j, c, a);
}
static void hline(int32_t x, int32_t y, int32_t w, uint16_t c) { fill(x, y, w, 1, c); }
static void vline(int32_t x, int32_t y, int32_t h, uint16_t c) { fill(x, y, 1, h, c); }
static void frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}
/* a filled rectangle with 1-pixel cut corners (the look of a rubber pad / key cap) */
static void pad(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    fill(x + 1, y, w - 2, 1, c);
    fill(x, y + 1, w, h - 2, c);
    fill(x + 1, y + h - 1, w - 2, 1, c);
}
static void pad_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    hline(x + 1, y, w - 2, c);
    hline(x + 1, y + h - 1, w - 2, c);
    vline(x, y + 1, h - 2, c);
    vline(x + w - 1, y + 1, h - 2, c);
}
/* vertical gradient (panel shading) */
static void grad(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t top, uint16_t bot)
{
    int32_t j;
    for (j = 0; j < h; j++)
        fill(x, y + j, w, 1, mix565(top, bot, (uint32_t)(h > 1 ? j * 15 / (h - 1) : 0)));
}

/* ------------------------------------------------------------------ text --- */
static uint32_t glyph_of(const pzfont_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch < f->first || ch > f->last)
        ch = '?';
    return ch - f->first;
}
static int32_t text_w(const pzfont_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++)
        w += f->adv[glyph_of(f, (uint8_t)*s)];
    return w;
}
/* y = the top of the text box (cap height ~ 0.73 x size below the ascent line) */
static int32_t text(int32_t x, int32_t y, const pzfont_t *f, const char *s, uint16_t c)
{
    for (; *s; s++) {
        uint32_t g = glyph_of(f, (uint8_t)*s), w = f->w[g], bpr = (w + 1u) / 2u, gx, gy;
        const uint8_t *d = f->data + f->off[g];
        int32_t ox = x + f->xo[g];
        if (y + (int32_t)f->h >= cv_y0 && y < cv_y0 + cv_h)
            for (gy = 0; gy < f->h; gy++)
                for (gx = 0; gx < w; gx++) {
                    uint32_t v = d[gy * bpr + gx / 2u];
                    v = (gx & 1u) ? v & 15u : v >> 4;
                    if (v)
                        px_blend(ox + (int32_t)gx, y + (int32_t)gy, c, v);
                }
        x += f->adv[g];
    }
    return x;
}
static void text_c(int32_t cx, int32_t y, const pzfont_t *f, const char *s, uint16_t c)
{
    text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const pzfont_t *f, const char *s, uint16_t c)
{
    text(rx - text_w(f, s), y, f, s, c);
}
/* the cap-height offset to centre capitals in a box: top of the text box for a cap block at y */
static int32_t cap_top(const pzfont_t *f, int32_t y) { return y - (int32_t)(f->asc - f->asc * 73u / 100u); }

/* ------------------------------------------------------- number formats --- */
static void fmt_u(char *b, uint32_t v)
{
    char t[12];
    uint32_t n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void fmt_i(char *b, int32_t v, int plus)
{
    if (v < 0) {
        *b++ = '-';
        v = -v;
    } else if (plus && v > 0) {
        *b++ = '+';
    }
    fmt_u(b, (uint32_t)v);
}
static void sz_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while ((*d++ = *s++))
        ;
}
static uint32_t strlen_s(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void sz_cpy(char *d, const char *s, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 1u < n && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* ------------------------------------------------- seven-segment digits --- */
/* the red LED tempo display of a TR: a digit is w x h with segment thickness t, slanted a little */
static const uint8_t SEG7[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
static void seg_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, int horiz)
{
    int32_t i;
    if (horiz) {
        for (i = 0; i < h; i++) {
            int32_t inset = i < h / 2 ? h / 2 - i : i - h / 2;
            fill(x + inset, y + i, w - 2 * inset, 1, c);
        }
    } else {
        for (i = 0; i < w; i++) {
            int32_t inset = i < w / 2 ? w / 2 - i : i - w / 2;
            fill(x + i, y + inset, 1, h - 2 * inset, c);
        }
    }
}
static void seg_digit(int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, uint32_t d, uint16_t on, uint16_t off)
{
    uint32_t m = d < 10u ? SEG7[d] : d == 10u ? 0x40u : 0u;   /* 10 = '-', 11 = blank */
    int32_t hh = h / 2;
    seg_bar(x + 1, y, w - 2, t, (m & 0x01) ? on : off, 1);              /* a */
    seg_bar(x + w - t, y + 1, t, hh - 1, (m & 0x02) ? on : off, 0);     /* b */
    seg_bar(x + w - t, y + hh, t, hh - 1, (m & 0x04) ? on : off, 0);    /* c */
    seg_bar(x + 1, y + h - t, w - 2, t, (m & 0x08) ? on : off, 1);      /* d */
    seg_bar(x, y + hh, t, hh - 1, (m & 0x10) ? on : off, 0);            /* e */
    seg_bar(x, y + 1, t, hh - 1, (m & 0x20) ? on : off, 0);             /* f */
    seg_bar(x + 1, y + hh - t / 2, w - 2, t, (m & 0x40) ? on : off, 1); /* g */
}

/* -------------------------------------------------------------- shapes --- */
static int32_t isin(uint32_t deg)              /* sin, Q15, degrees */
{
    return SINE[(deg % 360u) * 1024u / 360u];
}
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y, x;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++) {
            int32_t d = x * x + y * y, r2 = r * r;
            if (d <= r2 - r)
                px_set(cx + x, cy + y, c);
            else if (d <= r2 + r)
                px_blend(cx + x, cy + y, c, (uint32_t)(15 - (d - (r2 - r)) * 15 / (2 * r + 1)));
        }
}
/* an arc from angle a0 to a1 (degrees, 0 = up, clockwise), thickness th, radius r */
static void arc(int32_t cx, int32_t cy, int32_t r, int32_t th, int32_t a0, int32_t a1, uint16_t c)
{
    int32_t y, x, ri = r - th;
    for (y = -r - 1; y <= r + 1; y++)
        for (x = -r - 1; x <= r + 1; x++) {
            int32_t d2 = x * x + y * y, ang, cov;
            if (d2 > (r + 1) * (r + 1) || d2 < (ri - 1) * (ri - 1))
                continue;
            /* angle 0..359 from up, clockwise (integer atan2 by octants) */
            {
                int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, t;
                t = ax > ay ? 90 - (ay * 45 / (ax ? ax : 1)) : (ax * 45 / (ay ? ay : 1));
                if (ax > ay)
                    t = 90 - (int32_t)((int64_t)ay * 4500 / ax / 100);
                if (x >= 0 && y < 0)
                    ang = t;
                else if (x >= 0)
                    ang = 180 - t;
                else if (y >= 0)
                    ang = 180 + t;
                else
                    ang = 360 - t;
            }
            if (ang < a0 || ang > a1)
                continue;
            cov = 15;
            if (d2 > r * r)
                cov = 15 - (d2 - r * r) * 15 / (2 * r + 1);
            else if (d2 < ri * ri)
                cov = 15 - (ri * ri - d2) * 15 / (2 * ri + 1);
            if (cov > 0)
                px_blend(cx + x, cy + y, c, (uint32_t)cov);
        }
}
