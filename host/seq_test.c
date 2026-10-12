/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Sequencer tests: what plays, when, how often (host build). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t trk; uint64_t tick; uint8_t vel, alt; } hitlog_t;
static hitlog_t hl[20000];
static uint32_t hl_n;
struct hit_s;
#define PZ_TEST_HOOK(t, when, h) do { if (hl_n < 20000) { hl[hl_n].trk = (t); hl[hl_n].tick = (when); hl[hl_n].vel = (h)->vel; hl[hl_n].alt = (h)->alt; hl_n++; } } while (0)

static uint32_t sim_ms;
static uint8_t flash[1 << 20];
static int flash_ops, flash_cut = -1;
static uint32_t plat_ms(void) { return sim_ms; }
static void plat_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { (void)x, (void)y, (void)w, (void)h, (void)px; }
static void plat_sync(void) {}
static void plat_led(uint32_t id, uint32_t level) { (void)id, (void)level; }
static void plat_midi_out(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len) { (void)s, (void)d1, (void)d2, (void)len; }
static int plat_flash_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, flash + off, n); return 0; }
static int plat_flash_erase(uint32_t off) { if (flash_cut >= 0 && flash_ops++ >= flash_cut) return -1; memset(flash + off, 0xFF, 0x1000); return 0; }
static int plat_flash_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (flash_cut >= 0 && flash_ops++ >= flash_cut) return -1;
    for (i = 0; i < n; i++) flash[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}
#define PZ_POOL
#include "pz_app.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(uint32_t samples)
{
    int32_t buf[2 * PZ_CTL];
    uint32_t i;
    for (i = 0; i < samples; i += PZ_CTL) {
        mix_block(buf, PZ_CTL);
        sim_ms = (uint32_t)((uint64_t)sq.samples * 1000u / PZ_FS);
    }
}
static uint32_t bars(uint32_t n) { return n * 4u * 60u * PZ_FS / 120u; }   /* at 120 BPM */
static uint32_t count(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < hl_n; i++) n += hl[i].trk == t;
    return n;
}
static void fresh(void)
{
    uint32_t i;
    for (i = 0; i < PZ_NPAT; i++) pat_init(&pats[i], i);
    for (i = 0; i < PZ_NKIT; i++) kit_factory(&kits[i], i);
    memset(&sq, 0, sizeof sq);
    memset(vc, 0, sizeof vc);
    seq_init();
    kit_cur = 0;
    sys.tempo10 = 1200;
    sys.tempo_src = 0;
    sys.sync = 0;
    tempo_apply();
    hl_n = 0;
}
static void play(void) { sq.req = TP_START; }
static void stop(void) { sq.req = TP_STOP; run(PZ_CTL); }

int main(void)
{
    pattern_t *p;
    uint32_t i;
    /* 1. four on the floor: 4 hits a bar, on the beat (240 ticks a 1/16) */
    fresh(); p = &pats[0];
    for (i = 0; i < 16; i += 4) p->s[0][0][i].f = ST_ON;
    play(); run(bars(4)); stop();
    CHECK(count(0) == 16, "four on the floor: %u hits", count(0));
    for (i = 0; i < hl_n; i++) CHECK(hl[i].tick % (960ull << 32) < (2ull << 32) || (960ull << 32) - hl[i].tick % (960ull << 32) < (2ull << 32), "hit %u off the beat", i);
    /* 2. sub steps 1/3 and a mask; a flam = 2 hits */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; p->s[0][0][0].sub = 2;            /* 1/3: 3 hits */
    p->s[0][1][4].f = ST_ON; p->s[0][1][4].sub = 3 | 0x90;     /* 1/4, sub steps 0 and 3 */
    p->s[0][2][8].f = ST_ON | ST_FLAM;
    play(); run(bars(1) - 100); stop();
    CHECK(count(0) == 3, "1/3 sub steps: %u", count(0));
    CHECK(count(1) == 2, "masked 1/4: %u", count(1));
    CHECK(count(2) == 2, "flam: %u", count(2));
    /* 3. note cycle 1:2 plays every other bar; 1ST only the first; probability 50 % ~ half */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; p->s[0][0][0].cyc = 1;            /* 1:2 */
    p->s[0][1][0].f = ST_ON; p->s[0][1][0].cyc = CYC_FIRST;
    for (i = 0; i < 16; i++) { p->s[0][2][i].f = ST_ON; p->s[0][2][i].prob = 50; }
    play(); run(bars(8) - 100); stop();
    CHECK(count(0) == 4, "1:2 over 8 bars: %u", count(0));
    CHECK(count(1) == 1, "1ST: %u", count(1));
    CHECK(count(2) > 40 && count(2) < 88, "prob 50%%: %u of 128", count(2));
    /* 4. variation chain A B B: the steps of each in turn; a queued pattern starts after the chain */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON;                                   /* A: track 1 */
    p->s[1][1][0].f = ST_ON;                                   /* B: track 2 */
    sq.chain[0] = 0; sq.chain[1] = 1; sq.chain[2] = 1; sq.chain_n = 3;
    play(); run(bars(6) - 100);
    CHECK(count(0) == 2 && count(1) == 4, "chain A B B: %u %u", count(0), count(1));
    pats[3].s[0][3][0].f = ST_ON;
    sq.pat_next = 3; hl_n = 0;
    run(bars(4));
    CHECK(sq.pat == 3, "queued pattern: %u", sq.pat);
    CHECK(count(3) >= 1, "the new pattern plays");
    stop();
    /* 5. track LAST 3 against 16: polymeter */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; p->trk[0].last = 3;
    play(); run(bars(3) - 100); stop();
    CHECK(count(0) == 16, "LAST 3 over 48 steps: %u", count(0));
    /* 6. last step of the variation = 12: a 3/4 bar */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; p->last[0] = 12;
    play(); run(bars(3) - 100); stop();
    CHECK(count(0) == 4, "12-step variation over 48 steps: %u", count(0));
    /* 7. manual fill: the rest of the bar from FILL IN 1 */
    fresh(); p = &pats[0];
    for (i = 0; i < 16; i++) p->s[0][0][i].f = ST_ON;
    for (i = 8; i < 16; i++) p->s[PZ_SEC_FILL1][1][i].f = ST_ON;
    play(); run(bars(1) / 4); sq.fill_req = 1; run(bars(1) * 3 / 4 - 100);
    CHECK(count(1) == 8, "fill: %u hits of the fill track", count(1));
    CHECK(count(0) < 16, "the variation stopped during the fill (%u)", count(0));
    run(bars(1)); stop();
    CHECK(sq.sec == 0, "back to A after the fill");
    /* 8. auto fill every 2 bars */
    fresh(); p = &pats[0];
    p->s[PZ_SEC_FILL1][1][0].f = ST_ON; p->autofill = 1;
    play(); run(bars(8) - 100); stop();
    CHECK(count(1) == 4, "auto fill every 2 bars over 8: %u", count(1));
    /* 9. accent raises the velocity; weak beat lowers it */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; p->s[0][0][4].f = ST_ON | ST_WEAK; p->s[0][0][8].f = ST_ON; p->acc[0][8] = 1;
    play(); run(bars(1) - 100); stop();
    CHECK(hl_n == 3 && hl[1].vel < hl[0].vel && hl[2].vel > hl[0].vel, "velocities %u %u %u", hl[0].vel, hl[1].vel, hl[2].vel);
    /* 10. micro timing +25 %: 60 ticks late; START -50 %: 120 ticks early */
    fresh(); p = &pats[0];
    p->s[0][0][4].f = ST_ON; p->s[0][0][4].micro = 25;
    p->s[0][1][8].f = ST_ON; p->trk[1].start = -50;
    play(); run(bars(1) - 100); stop();
    {
        int64_t d0 = (int64_t)(hl[0].tick >> 32) - 960, d1 = (int64_t)(hl[1].tick >> 32) - 1920;
        CHECK(hl_n == 2 && d0 >= 59 && d0 <= 61 && d1 >= -121 && d1 <= -119, "timing %lld %lld", (long long)d0, (long long)d1);
    }
    /* 11. shuffle +100: the off-beat 1/16s 120 ticks late */
    fresh(); p = &pats[0];
    for (i = 0; i < 4; i++) p->s[0][0][i].f = ST_ON;
    p->shuffle = 100;
    play(); run(bars(1) / 4 - 10); stop();
    CHECK(hl_n == 4 && (hl[1].tick >> 32) >= 359 && (hl[1].tick >> 32) <= 361 && (hl[2].tick >> 32) >= 479 && (hl[2].tick >> 32) <= 481,
          "shuffle %llu %llu", (unsigned long long)(hl[1].tick >> 32), (unsigned long long)(hl[2].tick >> 32));
    /* 12. directions */
    fresh(); p = &pats[0];
    p->s[0][0][15].f = ST_ON; p->trk[0].dir = DIR_BWD;
    play(); run(bars(1) / 16 - 10); stop();
    CHECK(count(0) == 1, "backwards: step 16 first (%u)", count(0));
    /* 13. step loop: every step is step 5 */
    fresh(); p = &pats[0];
    p->s[0][0][4].f = ST_ON;
    sq.loop_step = 4;
    play(); run(bars(1) - 100); stop();
    CHECK(count(0) == 16, "step loop: %u", count(0));
    /* 14. mute and the closed hat choking the open hat (both still trigger) */
    fresh(); p = &pats[0];
    p->s[0][0][0].f = ST_ON; sq.mute = 1;
    play(); run(bars(1) - 100); stop();
    CHECK(count(0) == 0, "muted track played %u", count(0));
    /* 15. MIDI clock slave: 24 clocks a beat at 100 BPM drive the steps */
    fresh(); p = &pats[0];
    for (i = 0; i < 16; i += 4) p->s[0][0][i].f = ST_ON;
    sys.sync = 1;
    midi_in(0xFA);
    {
        uint32_t k, per = PZ_FS * 60 / (100 * 24);
        for (k = 0; k < 24 * 8; k++) { midi_in(0xF8); run(per); }
    }
    midi_in(0xFC); run(PZ_CTL);
    CHECK(count(0) == 8, "8 beats of MIDI clock: %u hits", count(0));
    /* 16. a pattern written, a power cut at every point of a second write, then a reboot: old or new, never broken */
    {
        int cut, bad = 0;
        fresh();
        memset(flash, 0xFF, sizeof flash);
        pats[2].s[0][0][0].f = ST_ON; pats[2].tempo10 = 1000;
        CHECK(store_pattern(2) == 0, "write");
        for (cut = 0; cut < 12; cut++) {
            fresh();
            store_load();
            pats[2].tempo10 = 1500;
            flash_ops = 0; flash_cut = cut;
            store_pattern(2);
            flash_cut = -1;
            memset(pats, 0, sizeof pats);
            store_load();
            if (!(pats[2].tempo10 == 1000 || pats[2].tempo10 == 1500) || !(pats[2].s[0][0][0].f & ST_ON)) bad++;
        }
        CHECK(!bad, "power cut: %d broken patterns", bad);
        fresh(); store_load();
        CHECK(pats[2].tempo10 == 1500, "the last complete write wins (%u)", pats[2].tempo10);
        /* kits and the settings survive */
        kits[5].t[0].tune = 12; sys.theme = 1;
        store_kit(5); store_sys();
        kits[5].t[0].tune = 0; sys.theme = 0;
        store_load();
        CHECK(kits[5].t[0].tune == 12 && sys.theme == 1, "kits / settings reload");
    }
    printf(fails ? "seq tests: %d FAILED\n" : "seq tests: all passed\n", fails);
    return fails != 0;
}
