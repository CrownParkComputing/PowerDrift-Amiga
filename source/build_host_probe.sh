#!/bin/bash
set -e
cd "$(dirname "$0")"
M68K="cores/musashi"
CC="gcc -O2 -I$M68K -Wall -Wextra -Wno-unused-parameter"
mkdir -p build/host
$CC -c "$M68K/m68kcpu.c" -o build/host/m68kcpu.o
$CC -c "$M68K/m68kops.c" -o build/host/m68kops.o
$CC -c "$M68K/m68kdasm.c" -o build/host/m68kdasm.o
$CC -c "$M68K/softfloat/softfloat.c" -o build/host/softfloat.o
$CC -c tools/pdrift_host_probe.c -o build/host/pdrift_host_probe.o
$CC -c tools/pdrift_yview.c -o build/host/pdrift_yview.o
$CC -c tools/pdrift_bview.c -o build/host/pdrift_bview.o
$CC -c tools/pdrift_frameview.c -o build/host/pdrift_frameview.o
$CC -c tools/pdrift_cov_topdis.c -o build/host/pdrift_cov_topdis.o
gcc -o /tmp/pdrift_host_probe build/host/m68kcpu.o build/host/m68kops.o build/host/m68kdasm.o build/host/softfloat.o build/host/pdrift_host_probe.o -lm
gcc -o /tmp/pdrift_yview build/host/pdrift_yview.o
gcc -o /tmp/pdrift_bview build/host/pdrift_bview.o
gcc -o /tmp/pdrift_frameview build/host/pdrift_frameview.o
gcc -o /tmp/pdrift_cov_topdis build/host/m68kdasm.o build/host/pdrift_cov_topdis.o
echo "built /tmp/pdrift_host_probe"
echo "built /tmp/pdrift_yview"
echo "built /tmp/pdrift_bview"
echo "built /tmp/pdrift_frameview"
echo "built /tmp/pdrift_cov_topdis"
