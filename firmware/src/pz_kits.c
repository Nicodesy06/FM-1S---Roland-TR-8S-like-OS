/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* Factory kits. Kit names describe a style of sound, not a product: every sound here is either
 * synthesised by PULSAR-8's own models or a CC0 recording played through a vintage-style
 * sample path (pz_dsp.c). A kit is eight tracks (any instrument on any track, as on the TR-8S)
 * plus its reverb, delay and master effect. */
#define T(i, tu, de, ct, lv, pa, rv, dl, co) {i, tu, de, ct, lv, pa, rv, dl, co, {0, 0, 0}}
#define KD(i, co) T(i, 0, 64, 64, 100, 0, 0, 0, co)

typedef struct {
    const char *name;
    kittrk_t t[PZ_NTRK];
    uint8_t rev_type, rev_lvl, rev_time, dly_lvl, dly_time, dly_fb, mfx_type, mfx_ctrl, mfx_on;
} kit_def_t;

static const kit_def_t KIT_DEF[PZ_NKIT] = {
    {"808", {KD(I_BD808, 0), KD(I_SD808, 1), T(I_LT808, 0, 64, 64, 96, -20, 10, 0, 2), T(I_HT808, 0, 64, 64, 96, 20, 10, 0, 3),
             T(I_CP808, 0, 64, 64, 100, 0, 30, 0, 4), T(I_RS808, 0, 64, 64, 90, 10, 0, 20, 5), KD(I_CH808, 6),
             T(I_OH808, 0, 64, 64, 96, 0, 0, 0, 7)},
     REV_HALL, 60, 70, 50, 7, 45, MFX_FILTER, 64, 0},
    {"909", {T(I_BD909, 0, 64, 64, 104, 0, 0, 0, 0), KD(I_SD909, 1), T(I_LT909, 0, 64, 64, 96, -24, 0, 0, 2),
             T(I_HT909, 0, 64, 64, 96, 24, 0, 0, 3), T(I_CP909, 0, 64, 64, 100, 0, 24, 0, 4), T(I_RS909, 0, 64, 64, 92, -10, 0, 0, 5),
             KD(I_CH909, 6), KD(I_OH909, 7)},
     REV_ROOM, 50, 50, 50, 5, 40, MFX_DRIVE, 40, 0},
    {"606", {KD(I_BD606, 0), KD(I_SD606, 1), T(I_LT606, 0, 64, 64, 96, -20, 0, 0, 2), T(I_HT606, 0, 64, 64, 96, 20, 0, 0, 3),
             T(I_CY606, 0, 64, 64, 84, 10, 0, 0, 4), T(I_CB808, 0, 64, 64, 84, -10, 0, 20, 5), KD(I_CH606, 6), KD(I_OH606, 7)},
     REV_PLATE, 50, 60, 60, 4, 50, MFX_FILTER, 64, 0},
    {"7X7", {KD(I_BD7X7, 0), KD(I_SD7X7, 1), T(I_LT7X7, 0, 64, 64, 96, -20, 0, 0, 2), T(I_HT7X7, 0, 64, 64, 96, 20, 0, 0, 3),
             KD(I_CP7X7, 4), KD(I_RS7X7, 5), KD(I_CH7X7, 6), KD(I_OH7X7, 7)},
     REV_ROOM, 50, 50, 40, 5, 40, MFX_CRUSH, 30, 0},
    {"CR78", {KD(I_BDCR, 0), KD(I_SDCR, 1), T(I_BGCR, 0, 64, 64, 96, -20, 20, 0, 2), T(I_CBCR, 0, 64, 64, 90, 20, 0, 0, 3),
              T(I_GUCR, 0, 64, 64, 90, 0, 0, 0, 4), T(I_MA808, 0, 64, 64, 84, 10, 0, 0, 5), KD(I_CHCR, 6), KD(I_CYCR, 7)},
     REV_PLATE, 70, 60, 40, 7, 40, MFX_LOFI, 40, 0},
    {"LM", {KD(I_LM_BD, 0), T(I_LM_SD, 0, 64, 64, 100, 0, 30, 0, 1), T(I_LM_LT, 0, 64, 64, 96, -24, 20, 0, 2),
            T(I_LM_HT, 0, 64, 64, 96, 24, 20, 0, 3), T(I_LM_CP, 0, 64, 64, 100, 0, 30, 0, 4), KD(I_LM_CB, 5), KD(I_LM_CH, 6),
            KD(I_LM_OH, 7)},
     REV_PLATE, 70, 75, 40, 5, 40, MFX_COMP, 50, 0},
    {"DMX", {KD(I_DX_BD, 0), T(I_DX_SD, 0, 64, 64, 100, 0, 30, 0, 1), T(I_DX_LT, 0, 64, 64, 96, -24, 20, 0, 2),
             T(I_DX_HT, 0, 64, 64, 96, 24, 20, 0, 3), KD(I_DX_CP, 4), KD(I_DX_RS, 5), KD(I_DX_CH, 6), KD(I_DX_OH, 7)},
     REV_PLATE, 60, 60, 40, 5, 40, MFX_COMP, 60, 0},
    {"DRMU", {KD(I_DR_BD, 0), KD(I_DR_SD, 1), T(I_DR_LT, 0, 64, 64, 96, -24, 0, 0, 2), T(I_DR_HT, 0, 64, 64, 96, 24, 0, 0, 3),
              KD(I_DR_CP, 4), KD(I_DR_CB, 5), KD(I_DR_CH, 6), KD(I_DR_OH, 7)},
     REV_ROOM, 50, 50, 40, 5, 40, MFX_FILTER, 64, 0},
    {"SP12", {KD(I_SP_BD, 0), KD(I_SP_SD, 1), T(I_SP_LT, 0, 64, 64, 96, -24, 0, 0, 2), T(I_SP_HT, 0, 64, 64, 96, 24, 0, 0, 3),
              KD(I_SP_CP, 4), KD(I_SP_RS, 5), KD(I_SP_CH, 6), KD(I_SP_OH, 7)},
     REV_ROOM, 40, 40, 30, 5, 30, MFX_COMP, 60, 0},
    {"ACOUSTIC", {KD(I_AC_BD, 0), T(I_AC_SD, 0, 64, 64, 100, 0, 30, 0, 1), T(I_AC_LT, 0, 64, 64, 96, -24, 20, 0, 2),
                  T(I_AC_HT, 0, 64, 64, 96, 24, 20, 0, 3), T(I_AC_RD, 0, 64, 64, 84, 20, 10, 0, 4), KD(I_AC_RS, 5), KD(I_AC_CH, 6),
                  KD(I_AC_OH, 7)},
     REV_ROOM, 60, 50, 30, 5, 30, MFX_COMP, 40, 0},
    {"HOUSE", {T(I_BD909, -4, 70, 70, 106, 0, 0, 0, 0), T(I_SD808, 0, 64, 64, 90, 0, 20, 0, 1), KD(I_LT808, 2),
               T(I_CP909, 0, 64, 64, 100, 0, 40, 30, 4), KD(I_HT808, 3), T(I_RD909, 0, 64, 64, 80, 20, 10, 0, 5), KD(I_CH909, 6),
               KD(I_OH909, 7)},
     REV_HALL, 60, 70, 50, 7, 50, MFX_FILTER, 64, 0},
    {"TECHNO", {T(I_BD909, -8, 80, 90, 108, 0, 0, 0, 0), T(I_SD909, 0, 64, 40, 92, 0, 30, 0, 1), KD(I_LT909, 2),
                KD(I_HT909, 3), T(I_CP909, 0, 64, 64, 96, 0, 50, 40, 4), T(I_RD909, 0, 64, 64, 84, 20, 0, 0, 5),
                T(I_CH909, 0, 50, 70, 100, -10, 0, 0, 6), KD(I_OH909, 7)},
     REV_HALL, 70, 80, 60, 6, 60, MFX_DRIVE, 50, 0},
    {"ELECTRO", {T(I_BD808, 0, 90, 80, 104, 0, 0, 0, 0), T(I_SD808, 4, 64, 90, 100, 0, 20, 0, 1), KD(I_LT808, 2),
                 KD(I_HT808, 3), T(I_CP808, 0, 64, 64, 100, 0, 30, 0, 4), T(I_CB808, 0, 64, 64, 86, 10, 0, 30, 5),
                 KD(I_CH606, 6), KD(I_OH606, 7)},
     REV_PLATE, 50, 60, 60, 5, 50, MFX_FILTER, 64, 0},
    {"HIPHOP", {KD(I_SP_BD, 0), KD(I_DX_SD, 1), T(I_BDLONG, -12, 64, 64, 90, 0, 0, 0, 2), KD(I_HT808, 3),
                T(I_DX_CP, 0, 64, 64, 96, 0, 20, 0, 4), KD(I_RS808, 5), KD(I_DX_CH, 6), KD(I_DX_OH, 7)},
     REV_ROOM, 40, 40, 30, 5, 30, MFX_COMP, 60, 0},
    {"TRAP", {T(I_BDLONG, 0, 80, 64, 104, 0, 0, 0, 0), T(I_SD909, 4, 50, 80, 96, 0, 20, 0, 1), KD(I_LT808, 2),
              KD(I_HT808, 3), T(I_CP808, 0, 64, 64, 100, 0, 30, 0, 4), T(I_RS808, 0, 64, 64, 86, 10, 0, 20, 5),
              T(I_CH808, 0, 40, 64, 100, 0, 0, 0, 6), KD(I_OH808, 7)},
     REV_HALL, 50, 70, 40, 5, 40, MFX_DRIVE, 30, 0},
    {"LO-FI", {KD(I_DR_BD, 0), KD(I_LM_SD, 1), KD(I_LT606, 2), KD(I_HT606, 3), KD(I_LM_CP, 4), KD(I_RS7X7, 5),
               KD(I_DR_CH, 6), KD(I_DR_OH, 7)},
     REV_PLATE, 60, 60, 40, 5, 40, MFX_LOFI, 50, 1},
};

static void kit_factory(kit_t *k, uint32_t i)
{
    const kit_def_t *d = &KIT_DEF[i % PZ_NKIT];
    uint32_t t, c;
    for (c = 0; c < sizeof *k; c++)
        ((uint8_t *)k)[c] = 0;
    k->magic = KIT_MAGIC;
    for (c = 0; c < PZ_NAME - 1u && d->name[c]; c++)
        k->name[c] = d->name[c];
    for (t = 0; t < PZ_NTRK; t++)
        k->t[t] = d->t[t];
    k->rev_type = d->rev_type;
    k->rev_lvl = d->rev_lvl;
    k->rev_time = d->rev_time;
    k->dly_lvl = d->dly_lvl;
    k->dly_time = d->dly_time;
    k->dly_fb = d->dly_fb;
    k->mfx_type = d->mfx_type;
    k->mfx_ctrl = d->mfx_ctrl;
    k->mfx_on = d->mfx_on;
    k->choke = 1u << 7;                        /* track 8 (open hat) is choked by track 7 (closed hat) */
}
