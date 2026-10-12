/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* PULSAR-8 application: one compilation unit, hardware independent (see pz_core.h).
 * The platform calls pz_init() once, mix_block() from the audio interrupt, ui_btn() / ui_white() /
 * ui_black() / ui_enc() for the controls, midi_in() for received MIDI, and pz_frame() from the
 * main loop. */
#include "pz_core.h"
#ifndef PZ_VERSION
#define PZ_VERSION "1.0"
#endif
#include "pz_dsp.c"
#include "pz_kits.c"
#include "pz_seq.c"
#include "pz_gfx.c"
#include "pz_ui.c"
#include "pz_store.c"
#include "pz_draw.c"

static void pz_init(void)
{
    store_load();                              /* patterns, kits, settings (missing: factory) */
    seq_init();
    kit_select(sys.kitno);
    sq.pat = sys.pat;
    sq.pat_lo = sq.pat_hi = sys.pat;
    tempo_apply();
    ui.subtype = 1;
    ui.pat_first = -1;
    ui.force = 1;
    reverb_config(kit.rev_type, kit.rev_time);
}

static void pz_frame(void)
{
    ui_tick();
    ui_draw();
    ui_leds();
}
