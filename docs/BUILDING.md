# Building and testing PULSAR-8

## Requirements

- Linux x86-64 (the JieLi toolchain is Linux-only; on macOS run it in a `linux/amd64` container, as SLOOP's
  build does), Python 3 with Pillow, a host C compiler. Optional: valgrind (exact CPU counts), Node.js
  (installer checks).
- The JieLi pi32v2 toolchain (clang 4.0.1 based): `tools/get_toolchain.sh` installs it to `~/.jieli/toolchain`.
- Three files of the JieLi AC79 SDK, put into the package (Apache-2.0, not in this tree):

  ```sh
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 --filter=blob:none --sparse \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  cd ~/fw-AC79_AIoT_SDK && git sparse-checkout set --no-cone /cpu/wl82/tools/uboot.boot \
      /cpu/wl82/tools/cfg_tool.bin /cpu/wl82/tools/cfg/eq_cfg_hw.bin
  ```

## Build

```sh
./build.sh                       # build/pulsar.bin, build/loader/ota.bin, build/pulsar.fwsc (identity FM-1_980)
./build.sh --release 1.0-beta    # build/pulsar-1.0-beta.fwsc
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations. The build generates the DSP tables,
the drum recordings (`tools/gen_samples.py`, cached) and the fonts into `build/gen/`, builds the update
loader and the app, runs SLOOP's static checks (no register access outside `firmware/hal`, no calls
from RAM code into flash, no ROM references, memory headroom) and packs the `.fwsc`.

Layout:

| Path | |
| --- | --- |
| `firmware/src/pz_core.h` | data model: steps, patterns, kits, platform hooks |
| `firmware/src/pz_dsp.c` | drum models, sample voices, reverb, delay, master FX |
| `firmware/src/pz_kits.c` | factory kits |
| `firmware/src/pz_seq.c` | transport, sequencer, MIDI, mixer (audio interrupt) |
| `firmware/src/pz_ui.c`, `pz_draw.c`, `pz_gfx.c` | controls, screens and themes, drawing |
| `firmware/src/pz_store.c` | flash storage |
| `firmware/src/pulsar.c` | the FM-1 glue: boot, interrupts, LEDs, LCD, USB, update, rescue |
| `firmware/hal`, `firmware/platform`, `firmware/loader`, `firmware/crt0.S`, `firmware/app.ld` | SLOOP's hardware layer and update loader |
| `host/` | the simulator and the host tests |
| `tools/` | build, table / sample / font generators, installer, level calibration |

## Test

```sh
tests/run_tests.sh
```

Everything runs on the host. See [RESEARCH.md § Verification](RESEARCH.md#8-verification).

### The simulator

`host/sim.c` runs the whole application with an emulated flash and writes screenshots (PPM), the LED
states and the audio (WAV) while a script presses keys:

```
build/tests/sim tests/ui_tour.txt out/
```

Script commands: `press|release|tap BUTTON` (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+),
`key N [down|up]` (white key 1–16), `black N [down|up]` (1–11), `turn KNOB N` (VALUE ALGO PRESET K1–K4),
`wait MS`, `shot NAME`, `theme 0|1`, `reboot`, `cut N` (power cut after N flash operations),
`expect …` (see `host/sim.c`).

### Sound levels

After changing a model, `build/tests/audio_test out | python3 tools/level_insts.py` re-calibrates every
instrument's trim to its category target (run it two or three times until no trim changes).

## Install a build

`python3 tools/fm1_install.py build/pulsar.fwsc`, or the browser installer (`docs/installer/`) with a
locally built package chosen in its file field.
