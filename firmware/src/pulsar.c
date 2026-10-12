/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors
 * Platform layer (firmware/hal, firmware/platform) from SLOOP / Felucca,
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, GPL-3.0-only. */
/* PULSAR-8 on the M-VAVE FM-1 (JieLi AC791N / WL82, pi32v2 at 240 MHz). One compilation unit.
 *
 *   TIMER5  10 kHz: key / encoder matrix and LEDs, milliseconds, USB (2 kHz), TRS MIDI (2 kHz)
 *   ALNK0   the audio interrupt: 256-frame halves rendered as 8 blocks of 32 (mix_block)
 *   main    controls -> UI, the screen, flash writes (stopped only), USB update requests
 *
 * The update path (M-UPGRADE protocol, ota.c + the RAM loader) and the USB rescue mode
 * (OCT- held at power-on, or two failed boots) are SLOOP's, unchanged. */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define PZ_POOL __attribute__((section(".pool")))
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_980"                 /* package identity (tools/build.py) */
#endif
#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1                          /* USB audio input: the master output */
#endif
#define FELUCCA_UART 1
#define HALF_FRAMES 256u
#define CTL PZ_CTL

#include "libc.c"
#include "pz_rt.c"
#include "lcd.c"
static volatile uint32_t fm1_ms;
#include "bootguard.h"
bootguard_t bootguard __attribute__((section(".noinit")));

/* ------------------------------------------------------------ app hooks --- */
static uint32_t plat_ms(void) { return fm1_ms; }
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint16_t *p = (uint16_t *)(uintptr_t)px;   /* the canvas: swapped in place, then sent by DMA */
    uint32_t i, n = w * h;
    for (i = 0; i < n; i++)
        p[i] = (uint16_t)(p[i] >> 8 | p[i] << 8);   /* the panel takes big-endian RGB565 */
    lcd_blit(x, y, w, h, p);                    /* runs on; cv_begin waits for it (plat_sync) */
}
static void plat_sync(void) { lcd_sync(); }
static void midi_out_event(uint32_t pkt);
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len)
{
    uint32_t cin = s >= 0xF8u ? 0xFu : (s >> 4);
    (void)len;
    midi_out_event(cin | (uint32_t)s << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}
static uint8_t flash_ok;
static int plat_flash_read(uint32_t off, void *dst, uint32_t n);
static int plat_flash_erase(uint32_t off);
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n);
static void plat_led(uint32_t id, uint32_t level);

#define PZ_VERSION "1.0 BETA"
#include "pz_app.c"

/* ------------------------------------------------------------- USB, MIDI --- */
#include "usb.c"
#include "midi_uart.c"

/* the received MIDI (USB cable 0, TRS = cable 1) into the sequencer's queue; clock only from the source
 * chosen in UTILITY > SYNC */
static void midi_pump(void)
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], cin = pkt & 15u, cable = (pkt >> 4) & 15u, st = (pkt >> 8) & 0xFFu;
        mi_r++;
        if (st >= 0xF8u) {
            if ((sys.sync == 1u && cable == 0u) || (sys.sync == 2u && cable == 1u))
                midi_in(st);
            continue;
        }
        if (cin >= 0x8u && cin <= 0xEu)
            midi_in(pkt >> 8);
    }
}

/* ------------------------------------------------------------------ audio --- */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 7                             /* Q15 -> 24 bit, -6 dBFS ceiling (as SLOOP) */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t master_q12 = 2048;
static volatile uint32_t audio_us, audio_max_us, audio_late;
static volatile uint8_t in_audio;
static volatile uint32_t t5_nested_ticks;
static uint8_t usb_full_now;

void fm1_alnk0_irq(void)
{
    uint8_t p;
    uint32_t t0;
    in_audio = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, i, us;
        int32_t *o = &abuf[half * HALF_WORDS];
        uac_render_start();
        usb_full_now = (uint8_t)(sys.usb_full && uac.feed);
        for (b = 0; b < HALF_FRAMES; b += CTL) {
            int32_t *x = o + 2u * b;
            midi_pump();
            mix_block(x, CTL);
            if (usb_full_now)
                uac_tap(x, CTL);                /* USB AUDIO = FULL: before the volume knob */
            for (i = 0; i < 2u * CTL; i++)
                x[i] = (int32_t)(((int64_t)x[i] * (int32_t)master_q12) >> 12);
            if (!usb_full_now)
                uac_tap(x, CTL);
            for (i = 0; i < 2u * CTL; i++)
                x[i] <<= OUT_SHIFT;
        }
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        audio_us = us;
        if (us > audio_max_us)
            audio_max_us = us;
        sq.cpu = (sq.cpu * 15u + us * 256u / (HALF_FRAMES * 1000000u / PZ_FS)) / 16u;
        if (fm1_audio_free_half() != half)
            audio_late++;
        if (us * 100u > (HALF_FRAMES * 1000000u / PZ_FS) * 85u)
            sq.overload = 700;                  /* over 85 % of the half: no reverb for ~0.5 s */
    }
    in_audio = 0;
}
extern void isr_alnk0(void);

/* ----------------------------------------------------------------- timer5 --- */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed, last, acc;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (usb_due)
        owed |= 1u;
    if (sub % 5u == 2u)
        owed |= 2u;
    if (++sub == 10u)
        sub = 0;
    if (in_audio) {                             /* nested in the render: only the USB audio stream */
        if (usb_due)
            uac_service();
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
    if (owed & 2u)
        uart_midi_poll();
    owed = 0;
}
extern void isr_timer5(void);

/* ------------------------------------------------------------------- LEDs --- */
/* id: 0..15 white keys, 16..26 black keys, 27..40 buttons (pz_draw.c); HAL key ids: buttons 0..13,
 * note keys 14 + note index */
static const uint8_t BTN_MATRIX[NB] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1};   /* SLOOP's panel map */
static uint8_t led_col[41], led_bit[41];
static void led_map_init(void)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++) {
            int8_t id = FM1_KEYMAP[r][p];
            if (id >= 0 && id < 41) {
                led_col[id] = (uint8_t)p;
                led_bit[id] = (uint8_t)(1u << r);
            }
        }
}
static void plat_led(uint32_t id, uint32_t level)
{
    uint32_t hw;
    if (id < 16u)
        hw = 14u + WHITE_KEY[id];
    else if (id < 27u)
        hw = 14u + BLACK_KEY[id - 16u];
    else if (id < 27u + NB)
        hw = BTN_MATRIX[id - 27u];
    else
        return;
    if (!led_bit[hw])
        return;
    if (level >= 2u)
        fm1_led[led_col[hw]] |= led_bit[hw];
    else
        fm1_led[led_col[hw]] &= (uint8_t)~led_bit[hw];
    if (level == 1u)
        fm1_led_dim[led_col[hw]] |= led_bit[hw];
    else
        fm1_led_dim[led_col[hw]] &= (uint8_t)~led_bit[hw];
}

/* ------------------------------------------------------------------ flash --- */
#include "fm1_flash.h"
static int plat_flash_read(uint32_t off, void *dst, uint32_t n)   /* 256-byte IRQ-off windows */
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static void audio_silence(void)                /* the DMA would loop a stale half during the erase */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
}
static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    uint32_t f = irq_save();
    int rc;
    audio_silence();
    rc = FL_FAR(fl_erase4k_ram)(off, took);
    irq_restore(f);
    return rc;
}
static int plat_flash_erase(uint32_t off)
{
    uint32_t took;
    if (!flash_ok || !FL_IN(off, 0x1000u, FL_DATA_LO, FL_DATA_HI))
        return -8;
    fm1_wdt_feed();
    return fl_erase4k_quiet(off, &took);
}
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!flash_ok || !FL_IN(off, n, FL_DATA_LO, FL_DATA_HI))
        return -8;
    fm1_wdt_feed();
    return fl_write(off, src, n);
}

/* ------------------------------------------------------- update, rescue --- */
static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_HI 0xFD20u
#define FONT_S F12
/* the one-line screens of the updater and the rescue mode (SLOOP's draw_text_box) */
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const pzfont_t *f, const char *s, uint16_t c, int align)
{
    (void)w;
    (void)x;
    cv_begin((int32_t)y, f->h, C_BLACK);
    if (align == 1)
        text_c(120, (int32_t)y, f, s, c);
    else
        text((int32_t)x, (int32_t)y, f, s, c);
    cv_end();
    lcd_sync();
}
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active)
        recovery_poll();
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k_quiet(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return plat_flash_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    char b[24];
    if (recovery_active)
        return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &F12, "UPDATE", C_WHITE, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &F12, STEP[step < 6u ? step : 0], C_HI, 1);
        return;
    }
    sz_cpy(b, "FAILED ", sizeof b);
    fmt_i(b + 7, code, 0);
    draw_text_box(0, 124, 240, &F12, b, C_HI, 1);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "recovery.c"

/* ------------------------------------------------------------------ boot --- */
static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}
static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, 0xA000u);
    draw_text_box(0, 8, 240, &F12, "PULSAR-8 CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 44, 220, &F12, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 66, 220, &F12, b, C_WHITE, 0);
    hexs(b, c->rets);
    draw_text_box(10, 88, 220, &F12, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void splash(void)                       /* the name, in the theme's colours */
{
    const theme_t *T = TH;
    uint32_t s;
    cv_begin(0, 100, T->bg);
    cv_end();
    cv_begin(100, 60, T->bg);
    text_c(120, 104, &F20, "PULSAR-8", T->light ? T->text : T->text);
    for (s = 0; s < 4u; s++)
        fill(42 + (int32_t)s * 40, 146, 36, 3, T->grp[s]);
    cv_end();
    cv_begin(160, 80, T->bg);
    text_c(120, 166, &F7, "RHYTHM FIRMWARE FOR THE FM-1", T->light ? T->label : T->dim);
    text_c(120, 184, &F7, PZ_VERSION, T->accent);
    cv_end();
    lcd_sync();
}

/* the controls -> the UI (the main loop). Key presses are latched by the scan (fm1_input_note_edges), so
 * a tap shorter than a screen redraw is still seen: down, then up if it is already released */
static void input_poll(void)
{
    static uint32_t held;
    uint32_t rel, pr = fm1_input_edges(&rel), b, i;
    uint32_t taps = fm1_input_note_edges(), notes = fm1_in.notes;
    static const uint8_t ENC_MATRIX[NE] = {0, 1, 6, 2, 3, 4, 5};   /* VALUE ALGO PRESET K1..K4 */
    for (b = 0; b < NB; b++) {
        uint32_t m = 1u << BTN_MATRIX[b];
        if (pr & m)
            ui_btn(b, 1);
        if (rel & m)
            ui_btn(b, 0);
    }
    for (i = 0; i < 27u; i++) {
        uint32_t m = 1u << i, down = (taps & m) || ((notes & m) && !(held & m)), up = (held & m || down) && !(notes & m);
        int w = -1, k;
        for (k = 0; k < 16; k++)
            if (WHITE_KEY[k] == i)
                w = k;
        if ((taps & m) && (held & m)) {        /* released and pressed again between two polls */
            if (w >= 0)
                ui_white((uint32_t)w, 0);
            else
                for (k = 0; k < 11; k++)
                    if (BLACK_KEY[k] == i)
                        ui_black((uint32_t)k, 0);
            held &= ~m;
        }
        if (down) {
            if (w >= 0)
                ui_white((uint32_t)w, 1);
            else
                for (k = 0; k < 11; k++)
                    if (BLACK_KEY[k] == i)
                        ui_black((uint32_t)k, 1);
            held |= m;
        }
        if (up) {
            if (w >= 0)
                ui_white((uint32_t)w, 0);
            else
                for (k = 0; k < 11; k++)
                    if (BLACK_KEY[k] == i)
                        ui_black((uint32_t)k, 0);
            held &= ~m;
        }
    }
    for (i = 0; i < NE; i++) {
        int32_t d = fm1_enc_take(ENC_MATRIX[i]);
        if (d)
            ui_enc(i, d);
    }
}

static void pz_main(void)
{
    int32_t knob = 512 * 16;
    uint32_t healthy_since, last_frame = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    if (flash_ok)
        ota_boot_cleanup();                     /* staging left by an update */
    lcd_init();
    fm1_input_init();
    led_map_init();
    fm1_adc_init();
    pz_init();                                  /* patterns, kits, settings from flash */
    splash();
    {
        uint32_t i;
        for (i = 0; i < 2u * HALF_WORDS; i++)
            abuf[i] = 0;
        fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    }
    usb_start();
    uart_midi_init();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(700);
    ui.force = 1;
    healthy_since = fm1_ms;
    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if ((uint32_t)(m - healthy_since) >= 30000u && bootguard.pending)
            bootguard_clear(&bootguard);
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                master_q12 = (k10 * k10) >> 8;  /* 0 .. ~4096, audio taper */
            }
        }
        ota_service();
        if (usb.ota_req) {                      /* M-UPGRADE: an update */
            usb.ota_req = 0;
            sq.req = TP_STOP;
            if (flash_ok)
                ota_session();                  /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, 0);
            ui.force = 1;
        }
        if (usb.uboot_req) {
            fm1_audio_stop();
            lcd_fill(0, 0, 240, 240, 0);
            draw_text_box(0, 110, 240, &F12, "UBOOT (USB)", C_WHITE, 1);
            fm1_delay_ms(20);
            usb_detach();
            fm1_delay_ms(30);
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
        input_poll();
        if (m - last_frame >= 16u) {            /* ~60 frames/s at most; a strip is redrawn only if it changed */
            last_frame = m;
            pz_frame();
        }
    }
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    uint32_t *s, *d, p3, src, wdt, boot_mode;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);            /* a power-on: no failed boot to count */
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* the flash driver, run from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    if (boot_mode == BOOT_RECOVERY || recovery_key())
        recovery_main();
    pz_main();
    for (;;)
        ;
}
