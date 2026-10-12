/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* CPU cost of one second of audio in the heaviest case: all 8 tracks hit on every 1/16 with 1/4 sub
 * steps (32nds x 2) on metal / long-decay sounds, all sends up, master effect on. Counted under
 * callgrind (host/run_tests.sh compares it with SLOOP's heaviest mix, which runs on the FM-1). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
static uint32_t plat_ms(void) { return 0; }
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { (void)x, (void)y, (void)w, (void)h, (void)px; }
static void plat_sync(void) {}
static void plat_led(uint32_t id, uint32_t level) { (void)id, (void)level; }
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len) { (void)s, (void)d1, (void)d2, (void)len; }
static int plat_flash_read(uint32_t off, void *dst, uint32_t n) { (void)off; memset(dst, 0xFF, n); return 0; }
static int plat_flash_erase(uint32_t off) { (void)off; return -1; }
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n) { (void)off, (void)src, (void)n; return -1; }
#define PZ_POOL
#include "pz_app.c"

static int32_t out[2 * PZ_CTL];
__attribute__((noinline)) static void cpu_counted(uint32_t nb)
{
    uint32_t k;
    for (k = 0; k < nb; k++)
        mix_block(out, PZ_CTL);
}

int main(int argc, char **argv)
{
    static const uint8_t HEAVY[PZ_NTRK] = {I_BD909, I_SD909, I_CY808, I_RD909, I_CP909, I_OH909, I_CH808, I_OH808};
    uint32_t t, s, kitn = argc > 1 ? (uint32_t)atoi(argv[1]) : 99u;
    store_load();
    seq_init();
    kit_cur = 0;
    if (kitn < PZ_NKIT) {
        kit_cur = (uint8_t)kitn;
    } else {
        for (t = 0; t < PZ_NTRK; t++) {
            kit.t[t].inst = HEAVY[t];
            kit.t[t].rev = 80;
            kit.t[t].dly = 60;
            kit.t[t].decay = 127;
        }
        kit.mfx_on = 1;
        kit.mfx_type = MFX_COMP;
        kit.choke = 0;
    }
    for (t = 0; t < PZ_NTRK; t++)
        for (s = 0; s < PZ_NSTEP; s++) {
            pats[0].s[0][t][s].f = ST_ON;
            pats[0].s[0][t][s].sub = 3;      /* 1/4: 64th notes */
        }
    sys.tempo10 = 1400;
    tempo_apply();
    sq.req = TP_START;
    cpu_counted(PZ_FS / PZ_CTL / 2);         /* settle */
    cpu_counted(PZ_FS / PZ_CTL);             /* the counted second */
    printf("peak meter %d\n", vc[0].meter);
    return 0;
}
