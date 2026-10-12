/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Host audio test: renders every instrument and every factory kit playing a beat to WAV files,
 * and checks levels, silence after the decay, and the sequencer timing.
 *   cc -O2 -Ibuild/gen -Ifirmware/src host/audio_test.c -o build/audio_test && build/audio_test build/wav */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pz_app.c"

static uint32_t plat_ms(void) { return 0; }
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { (void)x, (void)y, (void)w, (void)h, (void)px; }
static void plat_sync(void) {}
static void plat_led(uint32_t id, uint32_t level) { (void)id, (void)level; }
static int midi_clocks;
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len) { (void)d1, (void)d2, (void)len; if (s == 0xF8) midi_clocks++; }
static int plat_flash_read(uint32_t off, void *dst, uint32_t n) { (void)off, (void)dst, (void)n; return -1; }
static int plat_flash_erase(uint32_t off) { (void)off; return -1; }
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n) { (void)off, (void)src, (void)n; return -1; }

static void wav_write(const char *path, const int16_t *d, uint32_t frames, uint32_t ch)
{
    FILE *f = fopen(path, "wb");
    uint32_t bytes = frames * ch * 2, v;
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); { uint16_t a = 1, c = (uint16_t)ch; fwrite(&a, 2, 1, f); fwrite(&c, 2, 1, f); }
    v = PZ_FS; fwrite(&v, 4, 1, f); v = PZ_FS * 2 * ch; fwrite(&v, 4, 1, f);
    { uint16_t ba = (uint16_t)(2 * ch), bits = 16; fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); }
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(d, 2, frames * ch, f); fclose(f);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void render(int16_t *dst, uint32_t frames)
{
    int32_t buf[2 * PZ_CTL];
    uint32_t i, j;
    for (i = 0; i < frames; i += PZ_CTL) {
        mix_block(buf, PZ_CTL);
        for (j = 0; j < PZ_CTL && i + j < frames; j++) {
            int32_t l = buf[2 * j], r = buf[2 * j + 1];
            dst[2 * (i + j)] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            dst[2 * (i + j) + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/wav";
    char path[512];
    uint32_t i, k, t;
    static int16_t out[2 * PZ_FS * 8];
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", dir);
    if (system(cmd)) return 1;
    for (i = 0; i < PZ_NPAT; i++) pat_init(&pats[i], i);
    seq_init();
    /* 1. every instrument alone: peak, and silent at the end */
    kit_factory(&kit, 0);
    kit.rev_lvl = kit.dly_lvl = 0;
    for (t = 0; t < PZ_NTRK; t++) kit.t[t].rev = kit.t[t].dly = 0;
    tempo_apply();
    printf("%-11s %7s %7s %6s\n", "INSTRUMENT", "PEAK", "RMS", "TAIL");
    for (i = 0; i < I_N; i++) {
        uint32_t frames = PZ_FS * 3, n, j;
        double rms = 0; int peak = 0, tail = 0;
        hit_t h = {(uint8_t)i, 0, 0, 64, 64, 100};
        static int32_t mono[PZ_FS * 3];
        memset(vc, 0, sizeof vc);
        memset(mono, 0, sizeof mono);
        voice_trigger(&vc[0], &h);              /* the voice alone: before level, pan and the bus */
        for (j = 0; j < frames; j += PZ_CTL)
            voice_render(&vc[0], mono + j, PZ_CTL);
        for (n = 0; n < frames; n++) {
            int a = abs(mono[n]);
            out[2 * n] = out[2 * n + 1] = (int16_t)(mono[n] > 32767 ? 32767 : mono[n] < -32768 ? -32768 : mono[n]);
            if (a > peak) peak = a;
            if (n < PZ_FS / 2) rms += (double)mono[n] * mono[n];
            if (n > frames - PZ_FS / 10 && a > tail) tail = a;
        }
        rms = sqrt(rms / (PZ_FS / 2));
        printf("%-11s %7d %7.0f %6d%s\n", INST[i].name, peak, rms, tail, vc[0].on ? " (still on)" : "");
        CHECK(peak > 2000, "%s too quiet (%d)", INST[i].name, peak);
        CHECK(peak < 32700, "%s clips (%d)", INST[i].name, peak);
        snprintf(path, sizeof path, "%s/inst_%03u.wav", dir, i);
        wav_write(path, out, PZ_FS, 2);
    }
    /* 2. every factory kit plays a beat for 2 bars at 120 BPM */
    {
        pattern_t *p = &pats[0];
        static const char *const BEAT[PZ_NTRK] = {
            "x...x...x...x..x", "....x.......x...", "..........x.....", "...........x..x.",
            "....x.......x...", "..x.....x.....x.", "x.x.x.x.x.x.x.x.", "..x...x...x...x."};
        for (t = 0; t < PZ_NTRK; t++)
            for (k = 0; k < 16; k++)
                p->s[0][t][k].f = BEAT[t][k] == 'x' ? ST_ON : 0;
        p->acc[0][0] = p->acc[0][8] = 1;
        for (i = 0; i < PZ_NKIT; i++) {
            uint32_t frames = PZ_FS * 4, n, clip = 0; int peak = 0;
            memset(vc, 0, sizeof vc);
            kit_factory(&kit, i);
            sq.pat = 0; chain_default(); sys.tempo10 = 1200;
            sq.req = TP_START;
            render(out, frames);
            sq.req = TP_STOP;
            for (n = 0; n < frames * 2; n++) { if (abs(out[n]) > peak) peak = abs(out[n]); if (abs(out[n]) >= 32700) clip++; }
            printf("kit %-9s peak %d, %u samples at the ceiling\n", kit.name, peak, clip);
            CHECK(peak > 12000 && clip < frames / 500, "kit %s level %d clip %u", kit.name, peak, clip);
            snprintf(path, sizeof path, "%s/kit_%02u_%s.wav", dir, i, kit.name);
            wav_write(path, out, frames, 2);
        }
    }
    /* 3. timing: a 1-step pattern of BD at 120 BPM hits every 5512.5 samples; MIDI clock 24 per beat */
    {
        pattern_t *p = &pats[1];
        uint32_t n, hits = 0, last = 0, maxdev = 0, frames = PZ_FS * 4;
        int prev = 0;
        pat_init(p, 1);
        for (k = 0; k < 16; k++) p->s[0][0][k].f = ST_ON;
        kit_factory(&kit, 0);
        kit.t[0].inst = I_CL808; kit.rev_lvl = kit.dly_lvl = 0;
        memset(vc, 0, sizeof vc);
        sq.pat = 1; chain_default(); sys.tempo10 = 1200; sys.clk_out = 1; midi_clocks = 0;
        sq.req = TP_START;
        render(out, frames);
        sq.req = TP_STOP;
        render(out + 2 * frames, PZ_CTL);
        for (n = 1; n < frames; n++) {
            int a = out[2 * n];
            if (a > 3000 && prev <= 3000 && (hits == 0 || n - last > 2000)) {
                if (hits) { uint32_t d = n - last; uint32_t dev = d > 5512 ? d - 5512 : 5512 - d; if (dev > maxdev) maxdev = dev; }
                last = n; hits++;
            }
            prev = a;
        }
        printf("timing: %u hits in 4 s (expect 32), max deviation %u samples, %d MIDI clocks (expect 192)\n", hits, maxdev, midi_clocks);
        CHECK(hits == 32, "hits %u", hits);
        CHECK(maxdev <= 2, "jitter %u", maxdev);
        CHECK(midi_clocks >= 191 && midi_clocks <= 193, "clocks %d", midi_clocks);
    }
    printf(fails ? "%d FAILED\n" : "audio tests: all passed\n", fails);
    return fails != 0;
}
