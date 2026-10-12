# Research and design notes

How PULSAR-8 was designed: what the Roland instruments do, what the FM-1 is, and the decisions that
follow. Sources are listed at the end; where a fact could not be verified it says so.

## 1. The reference instruments

Both reference manuals were read in full for the workflow sections:

- **TR-8S Reference Manual**, revision 05 (`TR-8S_Reference_eng05_W.pdf`), "applies to program version
  3.00 or later", © 2020.
- **TR-1000 Reference Manual**, revision 03 (`TR-1000_reference_eng03_W.pdf`), "Version 1.20 and later",
  © 2025.

### 1.1 The TR-8S workflow (the primary reference)

The TR-8S is organised around **modes of the 16 pads** and **always-live instrument knobs**:

- **PTN SELECT** – pads choose patterns (8 banks × 16); a change during playback waits for the pattern to
  return to its first step; two pads at once chain a range.
- **TR-REC** – choose an instrument with its select button, press pads to place steps. During TR-REC:
  *SUB* + pad = sub steps (1/2, 1/3, 1/4, chosen with SUB + VALUE); *SHIFT + SUB* toggles FLAM input;
  *SHIFT* + pad = weak beat; holding an instrument button + pad = ALT INST; ACCENT [STEP] + pads = the
  accent track, ACCENT [LEVEL] its amount; long-pressing a pad opens per-step velocity, probability
  (PROB, SUB PROB) and motion values. CLEAR held during playback erases the selected track as it plays.
- **INST PLAY / INST REC** – pads 1–11 play the instruments, pads 12 / 13 (and both) roll at 1/16, 1/32,
  1/64; INST REC records into the variation; SHIFT + pad = weak beat; flam and sub steps work too.
- **Variations** – each pattern has A–H (16 steps each, its own LAST) and two FILL INs. Several lit
  variations play in A → H order. TR-REC + a variation records into it without changing playback.
- **Fill-ins** – MANUAL TRIG inserts one; AUTO FILL IN every 2, 4, 8, 12, 16 or 32 bars; the fill type
  can be a FILL IN, another variation or SCATTER (steps exchanged / reversed, gate changes).
- **Track LAST** – per track, shared by the variations, overriding the variation LAST: polymeters.
- **MOTION** – movements of TUNE, DECAY, CTRL (and reverb, delay, master FX) are recorded per step while
  MOTION [REC] is lit, or only while it is held; values can be entered for a held step; MOTION [ON]
  plays them; several ways to clear (variation, track, knob, step).
- **Instrument edit** – each of the 11 instruments has TUNE, DECAY, CTRL (assignable per instrument via
  CTRL SELECT) and a level fader; INST / KIT screens choose sounds; mixer screens for level, tune, decay,
  pan, sends, LFO, gain.
- **Effects** – REVERB level, DELAY level / time / feedback, MASTER FX on + CTRL; per-instrument sends
  by holding the instrument button and turning the effect level.
- **Mute / solo**, tap tempo (SHIFT + ENTER), nudge, shuffle knob, step loop (SHIFT + INST PLAY, pads,
  VALUE / SUB for 1/2 and 1/4 rolls), patterns store a scale (1/16, 1/32, triplets), flam spacing,
  master probability, kit link.
- **Saving** – edits stay until power-off; WRITE saves, SHIFT + WRITE overwrites pattern and kit.

### 1.2 What the TR-1000 adds (the secondary reference)

- **Per-track settings:** DIRECTION (FWD, BWD, P-P, R-L, L-R, RND, DNK, DNK2), ROTATE −8…8, START
  −50…+50, per-track SHUFFLE scaled by the master shuffle, FIRST STEP / LAST STEP.
- **Per-step editing on knobs:** C1 velocity, MORPH / C2 start timing (off-grid), C4 probability,
  C5 **note cycle** (2/2, 1/3, 5/8, 1st only …).
- **Sub-step patterns:** 1/2, 1/3, 1/4, flam, triplet 1–4, quadruplet 1–5.
- **VARIATION CHAIN** in any order; song mode; layers A / B per track.
- **PLAY MODE** for the step keys: INST, 16 VELO, SLICE, **SNAPSHOT** (16 snapshots of knob positions
  per track, recalled from the keys, recordable as motion).
- **KNOB MORPH**: a slider interpolates many knobs between two states, 16 morph slots per kit.
- **Analog engine + digital models**, sampling with slicing and time-stretch, compressor and insert
  effects per instrument, analog FX, side chain, CUE.

### 1.3 Distinctions kept in mind

TR-8S: 11 tracks, ACB / FM / sample tones, step-level SUB PROB, scatter, CTRL SELECT. TR-1000: more
tracks with two layers, true analog circuits, off-grid start timing, note cycles, per-track direction /
rotate / shuffle, snapshots, morph, song mode, sampling. Features were not merged blindly: PULSAR-8 uses
the TR-8S's pad modes and key layout as the frame (its *immediacy*), and adds the TR-1000's
sequencer depth where it fits behind a held step or a page.

## 2. The FM-1 hardware

| | Finding | Source |
| --- | --- | --- |
| SoC | JieLi AC791N / WL82 ("JL-BR22" strings are library names) | FM-1-RE |
| CPU | pi32v2 (Blackfin-derived 32-bit), 240 MHz from a 24 MHz crystal, no FPU used | FM-1-RE architecture.md, SLOOP |
| RAM | the SLOOP map: 96 KiB .data/.bss + 336 KiB pool + stacks / no-init | SLOOP `firmware/app.ld` |
| Flash | 1 MiB SPI NOR (JEDEC 0x856014), XIP at 0x02000000, 4 KiB sectors | FM-1-RE, SLOOP |
| Display | 240 × 240 ST7789-class IPS, RGB565, SPI1 at 12 MHz, polled DMA (a full frame ≈ 77 ms) | SLOOP `lcd.c` |
| Audio | ALNK0 I²S to an external codec, ~44.1 kHz (44 118 Hz measured by FM-1-RE), 24-bit slots, double-buffered DMA | SLOOP, FM-1-RE |
| Controls | a 6 × 11 diode matrix behind two 74HC595: 27 keys (F3–G5: 16 white, 11 black), 14 buttons, 7 encoders, one LED per key and button (one colour, two brightness layers) | SLOOP `fm1_input.h` |
| Analogue | master VOLUME pot and battery on the ADC | SLOOP |
| USB | full-speed device: USB-MIDI (class compliant), UAC1 audio *input to the computer* (44.1 / 48 kHz), optional CDC | SLOOP `usb.c` |
| MIDI | TRS MIDI **in** on UART1; **no MIDI out jack** | SLOOP |
| Bluetooth / Wi-Fi | present in the SoC, not used | FM-1-RE |
| Update path | M-UPGRADE SysEx protocol → RAM update loader → app area; SPL `uboot.boot` from the SDK | FM-1-RE, SLOOP |
| Recovery | application USB rescue (OCT− at power-on, boot-loop guard), chip USB boot via FM-1-transporter | SLOOP, FM-1-RE |

Not verified here (no device in this environment): the exact codec part, real-world CPU headroom on
the device, LED brightness perception. These are covered by the same SLOOP code paths that run on
FM-1s today.

### What the hardware rules out

- **No audio input** (USB audio is FM-1 → computer only): no sampling, resampling or external input.
- **No MIDI out jack**: MIDI out is USB only.
- **No velocity-sensitive pads, no faders, no RGB LEDs:** dynamics come from weak / accent / held-key
  modifiers and step velocity; levels are knobs; the screen carries the colour.
- **RAM:** 16 patterns × 9.7 KiB of pattern data are kept in RAM (as a TR keeps edits until power-off);
  that, the 64 KiB delay line and the reverb fill most of the pool.

## 3. Foundation: SLOOP's platform layer

Two open firmware projects exist for the FM-1: **FM-1-RE** (reverse engineering, no firmware on its
main branch) and **SLOOP** (a complete GPL-3.0 groovebox firmware derived from Felucca, with a bare-metal
HAL, a RAM update loader, M-UPGRADE-compatible packaging, USB MIDI / audio, rescue mode and host tests).

Decision: **reuse SLOOP's hardware layer unchanged, replace the application entirely.** Reasons:

1. It is proven on devices (released, installed by users), including the hardest parts to get right
   without hardware: the update loader, the flash driver that runs from RAM, the boot-loop guard.
2. Its build checks (no hardware register access outside `hal/`, no calls from RAM code into flash, no
   ROM references, memory headroom) protect a new application from the classic bricking mistakes.
3. Its package is installable by its installer and its loader can be resumed, so a user can always go
   back.

SLOOP's application (4-track groovebox, synth engines, its UI) was not reused: a TR needs a different
data model (variations, per-step conditions, accent lane, motion), a different voice model (one
monophonic voice per track) and a different interface.

## 4. Architecture

```
TIMER5 10 kHz ─ key matrix, encoders, LEDs, ms, USB (2 kHz), TRS MIDI
ALNK0 audio   ─ per 32-sample block: MIDI in → transport → step evaluation → hits at their sample
                → 8 voices → level / pan / sends → reverb, delay → master FX → limiter → volume
main loop     ─ controls → UI state; screen strips (hash-checked); LEDs; WRITE / auto-save (stopped)
```

- **Time base:** 960 ticks per quarter, Q32.32 position, advanced per sample from the tempo or limited by
  incoming MIDI clock (24 PPQN = 40 ticks; never running ahead of the last clock). Every grid step is
  evaluated half a step early, so negative start timing, flam grace notes and negative shuffle can be
  scheduled; hits then fire at their exact sample inside the block (voices are rendered in segments
  around them). Measured on the host: hits within 1 sample of the grid, exactly 24 clocks per beat out.
- **Tracks vs voices:** eight sequencer tracks, eight voices, one per track, monophonic like a TR
  circuit: a retrigger restarts the voice with a 1.5 ms declick; the closed hat chokes the open hat.
  This keeps the CPU cost bounded no matter how dense the pattern is.
- **Sound:** fixed-point synthesis (Q15 / Q30, a zero-delay-feedback state-variable filter, a 1024-point
  sine table, xorshift noise); sample voices are 8-bit µ-law at 24 kHz with machine-dependent zero-order
  hold, quantisation, filter and drive. Instrument levels are calibrated automatically
  (`tools/level_insts.py` from the host render).
- **CPU budget:** the worst case (all eight tracks firing 1/64 notes on metallic and long-decay sounds
  with every send and the compressor on) counts ~113 M host instructions per audio second, the same as
  SLOOP's heaviest mix that plays on the FM-1 (~112 M). Normal patterns cost a fraction of that. If a
  render runs past 85 % of its deadline the reverb is shed for half a second rather than glitching.
- **Screen:** no frame buffer (RAM is for patterns): four horizontal strips, each redrawn only when a
  hash of what it shows changes, drawn into a 240 × 100 canvas and sent by DMA while the next is drawn.
- **Storage:** patterns in 16 slots + a journal slot; kits and settings in A / B sector pairs; CRCs on
  headers and payloads; written only while stopped (an erase stops the audio for ~40 ms).

## 5. The interface

**Physical mapping.** The 16 white keys are exactly the TR's 16 step pads; eight black keys are the
instrument select buttons; the three remaining black keys carry the three TR functions used constantly
while programming (LAST, CLEAR, FILL). Modes sit on the buttons whose FM-1 meaning is closest (REC =
TR-REC, SEQ = PTN SELECT, ARP = INST PLAY because rolls live there). OCT− / OCT+ become SHIFT and
VARIATION because they are the two buttons nearest the keys. The four knobs are always the selected
instrument's TUNE / DECAY / CTRL / LEVEL — the TR-8S's defining immediacy — and become the TR-1000's
step parameters while a step is held.

**Visual design.** Two themes from one layout:

- **NOIR** (TR-8S): black, the AIRA lime-green as the accent, per-track colours (the TR-8S colours its
  faders and pads per instrument), a red seven-segment tempo, high contrast.
- **ALLOY** (TR-1000): silver-grey surfaces, graphite "display windows" and knob caps, the same red LED
  tempo, restrained orange-red accents, dark text.

Both carry the TR-808's red / orange / yellow / white step-group colours under the grid. The project's
own name, typography (Inter Display) and drawings are used; no Roland logo, artwork or font.

## 6. Sound design and legality

The TR-808 / 909 / 606 / CR-78 voices are new synthesis models informed by published descriptions of
the circuits (the six 808 metal oscillator frequencies, the bridged-T kick and toms, the 238 / 476 Hz
snare resonators, the multi-burst clap, the 909's noise-clicked kick and bit-reduced cymbals). No
samples of those machines are used. The LM / DMX / DRMU / SP12 kits are CC0 acoustic recordings played
through an emulation of how those machines played samples; no ROM content of any machine is included.

## 7. Feature map

| Feature | TR-8S | TR-1000 | PULSAR-8 |
| --- | --- | --- | --- |
| TR-REC step entry, 16 pads | ✓ | ✓ | ✓ white keys |
| Instrument select, live TUNE / DECAY / CTRL / LEVEL | ✓ | ✓ | ✓ black keys, knobs 1–4 |
| Variations A–H, fill-ins 1 / 2 | ✓ | ✓ | ✓ |
| Variation chain | A→H order | any order | any order |
| Manual / auto fill, scatter | ✓ | ✓ (scatter is a master effect) | ✓ (scatter as a fill type) |
| Variation LAST, track LAST | ✓ | ✓ (+ FIRST STEP) | ✓ (no FIRST STEP) |
| Sub steps, flam, weak beats, ALT INST | ✓ | ✓ + patterns | ✓ + patterns |
| Accent track, step velocity | ✓ | ✓ | ✓ |
| Probability, master probability | ✓ (+ SUB PROB) | ✓ | ✓ (no SUB PROB) |
| Note cycle | – | ✓ | ✓ + 1ST / FILL conditions |
| Start timing / micro timing | – (nudge only) | ✓ | ✓ |
| Direction, rotate, start, per-track shuffle | – | ✓ | ✓ |
| Shuffle, scale, flam spacing | ✓ | ✓ | ✓ |
| Motion record / play / clear, step values | ✓ | ✓ | ✓ (384 points per pattern) |
| Step loop with rolls | ✓ | ✓ | ✓ |
| Rolls 1/16 1/32 1/64 | ✓ | ✓ | ✓ |
| Snapshots, knob morph | – | ✓ | ✓ (8 per pattern, one morph target) |
| Mute, solo | ✓ | ✓ (+ CUE) | ✓ |
| Reverb, delay, master FX, sends | ✓ | ✓ (+ AFX, IFX, CMP) | ✓ (no per-instrument FX) |
| Kits, instrument choice per track | ✓ | ✓ | ✓ |
| Write, copy, clear, reload | ✓ | ✓ | ✓ (reload = power-cycle) |
| Song mode | – | ✓ | pattern chains only |
| Sampling | import | record / slice | – (no audio input) |
| MIDI clock in / out, notes in / out | ✓ | ✓ | ✓ (out via USB) |
| USB audio | ✓ multi-channel | ✓ | ✓ stereo |

## 8. Verification

On the host (`tests/run_tests.sh`): every instrument rendered (level, decay to silence), every kit
mixed (no clipping), timing and MIDI clock counts, 16 sequencer behaviours (sub steps, masks, flams,
cycles, probability, chains, queued patterns, polymeter, variation LAST, manual and auto fills, accent /
weak velocities, start timing, shuffle, directions, step loop, mute, MIDI clock slave), a power cut at
every point of a pattern write, the UI driven from a script through every page in both themes with a
write and a reboot, the CPU worst case, and SLOOP's own tests of the update entry, the update loader
(PULSAR-8 over SLOOP and back) and the rescue mode. The firmware build adds SLOOP's static checks.

Not yet done: running on a physical FM-1. Things to watch first on hardware: the panel mapping (SLOOP's
default button table is assumed), audio load in dense patterns, LED readability, the LCD's colour
order in the ALLOY theme.

## Sources

- Roland, *TR-8S Reference Manual* (eng05), https://static.roland.com/assets/media/pdf/TR-8S_Reference_eng05_W.pdf
- Roland, *TR-1000 Reference Manual* (eng03), https://static.roland.com/assets/media/pdf/TR-1000_reference_eng03_W.pdf
- AL-255, *FM-1-RE*, https://github.com/AL-255/FM-1-RE (architecture, flash layout, OTA protocol)
- isod89 et al., *SLOOP*, https://github.com/isod89/sloop-fm1, and Leo Kuroshita, *Felucca*, https://github.com/hugelton/Felucca
- Versilian Studios, *VCSL* (CC0), https://github.com/sgossner/VCSL
