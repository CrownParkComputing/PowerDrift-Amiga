#!/bin/bash
# Build the host validator for the shared machine + render modules.
set -e
cd "$(dirname "$0")"
M68K="cores/musashi"
CC="gcc -O2 -I$M68K -I. -Wall -Wextra -Wno-unused-parameter"
mkdir -p build/host

$CC -c "$M68K/m68kcpu.c" -o build/host/m68kcpu.o
$CC -c "$M68K/m68kops.c" -o build/host/m68kops.o
$CC -c "$M68K/m68kdasm.c" -o build/host/m68kdasm.o
$CC -c "$M68K/softfloat/softfloat.c" -o build/host/softfloat.o
$CC -c pdrift_machine.c -o build/host/pdrift_machine.o
$CC -c pdrift_render.c -o build/host/pdrift_render.o
$CC -c tools/pdrift_host_live.c -o build/host/pdrift_host_live.o
gcc -o /tmp/pdrift_host_live \
    build/host/m68kcpu.o build/host/m68kops.o build/host/m68kdasm.o build/host/softfloat.o \
    build/host/pdrift_machine.o build/host/pdrift_render.o build/host/pdrift_host_live.o -lm
echo "built /tmp/pdrift_host_live"
