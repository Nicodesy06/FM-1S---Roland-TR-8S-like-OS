/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Host simulator: the whole application (sequencer, sounds, UI, storage) on a PC, driven by a
 * script of control events. It renders the screen to PPM images, the audio to a WAV file and
 * emulates the SPI NOR (erase to 0xFF, program clears bits) in a file, so the flash format and a
 * power cut during a write can be tested too.
 *
 *   build/sim SCRIPT OUTDIR
 * Script lines (# comments):
 *   press NAME | release NAME | tap NAME     buttons: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+
 *   key N [down|up]                          white key 1..16 (no arg: a tap)
 *   black N [down|up]                        black key 1..11
 *   turn KNOB N                              VALUE ALGO PRESET K1 K2 K3 K4, N detents
 *   wait MS                                  run the audio and the UI for MS milliseconds
 *   shot NAME                                save OUTDIR/NAME.ppm
 *   theme N | reboot | cut N (power cut after N flash operations, then reboot) | expect ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t sim_ms;
static uint16_t fb[240 * 240];
static uint8_t flash[1 << 20];
static int flash_ops, flash_cut = -1;
static int16_t *wav;
static uint32_t wav_n, wav_cap;
static uint8_t leds[64];

static uint32_t plat_ms(void) { return sim_ms; }
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (y + j < 240 && x + i < 240)
                fb[(y + j) * 240 + x + i] = px[j * w + i];
}
static void plat_sync(void) {}
static void plat_led(uint32_t id, uint32_t level) { if (id < 64) leds[id] = (uint8_t)level; }
static int midi_out_n;
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len) { (void)s, (void)d1, (void)d2, (void)len; midi_out_n++; }
static int cut_check(void)
{
    if (flash_cut >= 0 && flash_ops++ >= flash_cut)
        return -1;                             /* the power is gone: nothing more is written */
    return 0;
}
static int plat_flash_read(uint32_t off, void *dst, uint32_t n)
{
    if (off + n > sizeof flash) return -1;
    memcpy(dst, flash + off, n);
    return 0;
}
static int plat_flash_erase(uint32_t off)
{
    if (cut_check()) return -1;
    if ((off & 0xFFF) || off < 0x97000 || off >= 0xE0000) { printf("FAIL: erase outside the data area %x\n", off); exit(1); }
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (cut_check()) return -1;
    if (off < 0x97000 || off + n > 0xE0000) { printf("FAIL: write outside the data area %x\n", off); exit(1); }
    for (i = 0; i < n; i++) flash[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}

#define PZ_POOL
#include "pz_app.c"

static void run_ms(uint32_t ms)
{
    uint32_t end = sim_ms + ms;
    static uint32_t frame_due, smp_acc;
    while (sim_ms < end) {
        int32_t buf[2 * PZ_CTL];
        uint32_t j;
        smp_acc += PZ_FS;                      /* 1 ms of audio */
        while (smp_acc >= 1000u * PZ_CTL) {
            smp_acc -= 1000u * PZ_CTL;
            mix_block(buf, PZ_CTL);
            if (wav_n + 2 * PZ_CTL > wav_cap) {
                wav_cap = wav_cap ? wav_cap * 2 : 1 << 20;
                wav = realloc(wav, wav_cap * 2);
            }
            for (j = 0; j < 2 * PZ_CTL; j++)
                wav[wav_n++] = (int16_t)(buf[j] > 32767 ? 32767 : buf[j] < -32768 ? -32768 : buf[j]);
        }
        sim_ms++;
        if (sim_ms >= frame_due) {
            frame_due = sim_ms + 16;
            pz_frame();
        }
    }
}

static void shot(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240 * 240; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    /* the LEDs too: one line per frame, for the panel mock-up */
    snprintf(path, sizeof path, "%s/%s.led", dir, name);
    f = fopen(path, "w");
    for (i = 0; i < 64; i++) fprintf(f, "%d", leds[i]);
    fprintf(f, "\n");
    fclose(f);
}

static int btn_id(const char *s)
{
    uint32_t i;
    for (i = 0; i < NB; i++)
        if (!strcmp(s, B_NAME[i])) return (int)i;
    if (!strcmp(s, "SHIFT")) return B_SHIFT;
    if (!strcmp(s, "VAR")) return B_VAR;
    printf("unknown button %s\n", s);
    exit(1);
}
static int enc_id(const char *s)
{
    static const char *const N[NE] = {"VALUE", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
    uint32_t i;
    for (i = 0; i < NE; i++)
        if (!strcmp(s, N[i])) return (int)i;
    printf("unknown knob %s\n", s);
    exit(1);
}

static void boot(void)
{
    memset(&sq, 0, sizeof sq);
    memset(&ui, 0, sizeof ui);
    memset(vc, 0, sizeof vc);
    pz_init();
    run_ms(50);
}

int main(int argc, char **argv)
{
    FILE *f;
    char line[256], dir[256];
    int fails = 0;
    if (argc < 3) { fprintf(stderr, "usage: sim SCRIPT OUTDIR\n"); return 2; }
    snprintf(dir, sizeof dir, "%s", argv[2]);
    snprintf(line, sizeof line, "mkdir -p %s", dir);
    if (system(line)) return 1;
    memset(flash, 0xFF, sizeof flash);
    boot();
    f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    while (fgets(line, sizeof line, f)) {
        char a[64] = "", b[64] = "", c[64] = "";
        int n = sscanf(line, "%63s %63s %63s", a, b, c);
        if (n < 1 || a[0] == '#') continue;
        if (!strcmp(a, "press")) { ui_btn((uint32_t)btn_id(b), 1); run_ms(20); }
        else if (!strcmp(a, "release")) { ui_btn((uint32_t)btn_id(b), 0); run_ms(20); }
        else if (!strcmp(a, "tap")) { ui_btn((uint32_t)btn_id(b), 1); run_ms(60); ui_btn((uint32_t)btn_id(b), 0); run_ms(20); }
        else if (!strcmp(a, "key")) {
            uint32_t k = (uint32_t)atoi(b) - 1u;
            if (n < 3 || !strcmp(c, "down")) ui_white(k, 1);
            if (n < 3) run_ms(40);
            if (n < 3 || !strcmp(c, "up")) ui_white(k, 0);
            run_ms(20);
        } else if (!strcmp(a, "black")) {
            uint32_t k = (uint32_t)atoi(b) - 1u;
            if (n < 3 || !strcmp(c, "down")) ui_black(k, 1);
            if (n < 3) run_ms(40);
            if (n < 3 || !strcmp(c, "up")) ui_black(k, 0);
            run_ms(20);
        } else if (!strcmp(a, "turn")) {
            int d = atoi(c), i, s = d < 0 ? -1 : 1;
            for (i = 0; i < (d < 0 ? -d : d); i++) { ui_enc((uint32_t)enc_id(b), s); run_ms(30); }
        } else if (!strcmp(a, "wait")) run_ms((uint32_t)atoi(b));
        else if (!strcmp(a, "shot")) { run_ms(40); shot(dir, b); }
        else if (!strcmp(a, "theme")) { sys.theme = (uint8_t)atoi(b); ui.force = 1; run_ms(40); }
        else if (!strcmp(a, "reboot")) boot();
        else if (!strcmp(a, "cut")) { flash_cut = atoi(b); flash_ops = 0; }
        else if (!strcmp(a, "power")) { flash_cut = -1; boot(); }
        else if (!strcmp(a, "expect")) {
            /* expect step TRK STEP 0|1 / expect playing 0|1 / expect pat N / expect tempo T10 / expect kit N */
            char d[64] = "", e[64] = "";
            sscanf(line, "%*s %*s %*s %63s %63s", d, e);
            if (!strcmp(b, "step")) {
                int on = (PAT->s[sq.rec_sec][atoi(c) - 1][atoi(d) - 1].f & ST_ON) != 0;
                if (on != atoi(e)) { printf("FAIL line: %s", line); fails++; }
            } else if (!strcmp(b, "playing")) { if (sq.playing != atoi(c)) { printf("FAIL line: %s", line); fails++; } }
            else if (!strcmp(b, "pat")) { if (sq.pat + 1 != atoi(c)) { printf("FAIL line: %s (pat %d)", line, sq.pat + 1); fails++; } }
            else if (!strcmp(b, "tempo")) { if (tempo_now() != (uint32_t)atoi(c)) { printf("FAIL line: %s (tempo %u)", line, tempo_now()); fails++; } }
            else if (!strcmp(b, "kit")) { if (kit_cur + 1 != atoi(c)) { printf("FAIL line: %s", line); fails++; } }
            else if (!strcmp(b, "sec")) { if (sq.sec + 1 != atoi(c)) { printf("FAIL line: %s (sec %d)", line, sq.sec + 1); fails++; } }
            else if (!strcmp(b, "dirty")) { if ((ui.pat_dirty != 0) != atoi(c)) { printf("FAIL line: %s", line); fails++; } }
            else if (!strcmp(b, "mute")) { if (sq.mute != atoi(c)) { printf("FAIL line: %s", line); fails++; } }
            else if (!strcmp(b, "sub")) {
                if ((PAT->s[sq.rec_sec][atoi(c) - 1][atoi(d) - 1].sub & 3) != atoi(e)) { printf("FAIL line: %s", line); fails++; }
            } else if (!strcmp(b, "acc")) {
                if ((PAT->acc[sq.rec_sec][atoi(c) - 1] != 0) != atoi(d)) { printf("FAIL line: %s", line); fails++; }
            } else if (!strcmp(b, "motion")) {
                if ((int)motion_count(PAT) < atoi(c)) { printf("FAIL line: %s (%u)", line, motion_count(PAT)); fails++; }
            } else { printf("unknown expect %s\n", b); fails++; }
        } else { printf("unknown command %s\n", a); fails++; }
    }
    fclose(f);
    {
        char path[512];
        FILE *w;
        uint32_t v, bytes = wav_n * 2;
        snprintf(path, sizeof path, "%s/audio.wav", dir);
        w = fopen(path, "wb");
        fwrite("RIFF", 1, 4, w); v = 36 + bytes; fwrite(&v, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
        v = 16; fwrite(&v, 4, 1, w); { uint16_t x = 1, ch = 2; fwrite(&x, 2, 1, w); fwrite(&ch, 2, 1, w); }
        v = PZ_FS; fwrite(&v, 4, 1, w); v = PZ_FS * 4; fwrite(&v, 4, 1, w);
        { uint16_t ba = 4, bits = 16; fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w); }
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(wav, 2, wav_n, w); fclose(w);
    }
    printf(fails ? "sim: %d FAILED\n" : "sim: all checks passed\n", fails);
    return fails != 0;
}
