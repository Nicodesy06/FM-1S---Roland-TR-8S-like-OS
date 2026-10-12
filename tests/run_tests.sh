#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host test suite (no hardware): sounds, sequencer, UI flows, storage, update path.
#   tests/run_tests.sh            (after ./build.sh for the package tests; they are skipped otherwise)
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
OUT=build/tests
mkdir -p "$OUT" build/gen
[ -f build/gen/pz_tables.h ] || python3 tools/gen_tables.py build/gen/pz_tables.h
[ -f build/gen/pz_samples.h ] || python3 tools/gen_samples.py build/gen/pz_samples.h
[ -f build/gen/pz_fonts.h ] || python3 tools/gen_fonts.py build/gen/pz_fonts.h
fail=0
run() { name="$1"; shift; if "$@" > "$OUT/log.txt" 2>&1; then echo "ok    $name"; else echo "FAIL  $name"; cat "$OUT/log.txt"; fail=1; fi; }
F="-O2 -Wall -Wno-unused-function -Wno-format-truncation -Ibuild/gen -Ifirmware/src"
$CC $F host/audio_test.c -o "$OUT/audio_test" -lm
$CC $F host/seq_test.c -o "$OUT/seq_test" -lm
$CC $F host/sim.c -o "$OUT/sim" -lm
$CC $F host/cpu_test.c -o "$OUT/cpu_test" -lm
run "sounds: every instrument (level, decay), every kit (mix), clock timing" "$OUT/audio_test" "$OUT/wav"
run "sequencer: sub steps, flam, cycles, probability, chains, fills, timing, MIDI clock, power cuts" "$OUT/seq_test"
run "UI: a beat programmed with the keys, every page in both themes, write + reboot" "$OUT/sim" tests/ui_tour.txt "$OUT/shots"
if command -v valgrind >/dev/null 2>&1; then
    valgrind --tool=callgrind --collect-atstart=no '--toggle-collect=cpu_counted*' --callgrind-out-file="$OUT/cg.out" \
        "$OUT/cpu_test" > /dev/null 2>&1
    ir=$(sed -n 's/^summary: //p;s/^totals: //p' "$OUT/cg.out" | head -1)
    per_s=$((ir * 2 / 3 / 1000000))
    # SLOOP's heaviest mix, which plays on the FM-1, counts ~112 M host instructions per second (cc -O2)
    if [ "$per_s" -le 125 ]; then echo "ok    CPU worst case: ${per_s} M host instructions / s (SLOOP heavy mix: 112)"; \
    else echo "FAIL  CPU worst case: ${per_s} M host instructions / s"; fail=1; fi
fi
$CC -O1 -w -o "$OUT/recovery_test" tests/recovery_test.c
run "USB rescue and boot-loop guard (SLOOP)" "$OUT/recovery_test"
if [ -f build/pulsar.fwsc ]; then
    $CC -O1 -w -o "$OUT/ota_test" tests/ota_test.c
    $CC -O1 -w -o "$OUT/ldr_test" tests/ldr_test.c
    run "M-UPGRADE entry with build/pulsar.fwsc" "$OUT/ota_test" build/pulsar.fwsc
    run "update loader: PULSAR-8 over itself" "$OUT/ldr_test" build/pulsar.fwsc build/pulsar.fwsc
    python3 -c "import sys; sys.path.insert(0,'tools'); import fm1_install as f; d=open('build/pulsar.fwsc','rb').read(); assert f.product_of(d).startswith('FM-1_9'), f.product_of(d)" \
        && echo "ok    package identity" || { echo "FAIL  package identity"; fail=1; }
else
    echo "skip  package tests (run ./build.sh first)"
fi
[ $fail = 0 ] && echo "all tests passed" || { echo "SOME TESTS FAILED"; exit 1; }
