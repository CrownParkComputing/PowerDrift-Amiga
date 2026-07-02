#!/bin/bash
# Host build of the Power Drift sound-board test (machine + Z80 + lean YM2151 +
# SegaPCM). The ymfm C++ core was retired once the lean Jarek YM2151 was validated
# spectrally identical to it; this now builds the shipping (lean) path only.
set -e
cd "$(dirname "$0")"
M68K="cores/musashi"
CC="gcc -O2 -I$M68K -I. -Iopm -Wall -Wextra -Wno-unused-parameter"
mkdir -p build/host

$CC -c "$M68K/m68kcpu.c"          -o build/host/m68kcpu.o
$CC -c "$M68K/m68kops.c"          -o build/host/m68kops.o
$CC -c "$M68K/m68kdasm.c"         -o build/host/m68kdasm.o
$CC -c "$M68K/softfloat/softfloat.c" -o build/host/softfloat.o
$CC -c pdrift_machine.c           -o build/host/pdrift_machine.o
$CC -c pdrift_segapcm.c           -o build/host/pdrift_segapcm.o
$CC -c pdrift_z80.c               -o build/host/pdrift_z80.o
$CC -c pdrift_audio.c             -o build/host/pdrift_audio.o
$CC -c pdrift_opm.c               -o build/host/pdrift_opm.o
$CC -c opm/ym2151.c               -o build/host/ym2151.o
$CC -c tools/pdrift_audiotest.c   -o build/host/pdrift_audiotest.o

gcc -o /tmp/pdrift_audiotest \
    build/host/m68kcpu.o build/host/m68kops.o build/host/m68kdasm.o build/host/softfloat.o \
    build/host/pdrift_machine.o build/host/pdrift_segapcm.o build/host/pdrift_z80.o \
    build/host/pdrift_audio.o build/host/pdrift_opm.o build/host/ym2151.o \
    build/host/pdrift_audiotest.o -lm
echo "built /tmp/pdrift_audiotest (lean Jarek YM2151)"
