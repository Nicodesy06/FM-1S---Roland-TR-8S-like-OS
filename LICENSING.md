# Licensing

PULSAR-8 is free software: **GPL-3.0-only** (full text in `LICENSE`).

| What | Licence | Copyright |
| --- | --- | --- |
| The application: `firmware/src/` (`pz_*`, `pulsar.c`), `host/`, `tools/gen_tables.py`, `tools/gen_samples.py`, `tools/gen_fonts.py`, `tools/level_insts.py`, `tests/ui_tour.txt`, `tests/run_tests.sh`, `docs/` | GPL-3.0-only | PULSAR-8 contributors |
| The hardware layer and update path: `firmware/hal/`, `firmware/platform/` (USB, OTA, rescue, LCD, MIDI UART, libc), `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`, `tools/build.py`, `tools/fm1pkg_make.py`, `tools/lz4blk.py`, `tools/fm1_install.py`, `tools/get_toolchain.sh`, `tests/ldr_test.c`, `tests/ota_test.c`, `tests/recovery_test.c`, `docs/installer/fm1ota.js`, `docs/installer/fm1pkg.js` | GPL-3.0-only | Leo Kuroshita (@kurogedelic), Hügelton Instruments, and the SLOOP contributors (from SLOOP / Felucca); modified for PULSAR-8 where noted |
| Drum recordings `assets/samples-cc0/KIT/` | CC0 1.0 | Versilian Studios (VCSL); see `ATTRIBUTION.txt` |
| Font `assets/fonts/InterDisplay-*.otf` | SIL OFL 1.1 | The Inter Project Authors; `assets/fonts/Inter-LICENSE.txt` |
| JieLi AC79 SDK files (`uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`) placed in the package at build time; not in this tree | Apache-2.0 | Zhuhai JieLi Technology |

If you distribute PULSAR-8, or firmware derived from it, give your recipients its complete source under
the GPL.

## Trademarks

Roland, TR-8S, TR-1000, TR-808, TR-909, TR-606, CR-78, AIRA; LinnDrum; Oberheim DMX; E-mu Drumulator,
SP-12; M-VAVE and FM-1 are trademarks of their respective owners. PULSAR-8 is an independent project,
not affiliated with or endorsed by them. Names of instruments and kits describe a style of sound; no
proprietary code, samples, artwork or logos of those products are included.
