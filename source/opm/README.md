# Lean YM2151 (OPM) core — Jarek Burczynski, from FBNeo

Fetched from FBNeo (src/burn/snd/ym2151.c) to replace the heavy C++ ymfm on the
Amiga 030 (ymfm JITs poorly; audio ~halved fps, block-gen recovered some).

## Integration plan (float-free, family standard)
1. Strip FBNeo deps: `driver.h`/`state.h` includes, `bprintf`, the savestate
   `BurnYM2151Scan_int` + `SCAN_VAR`; keep `USE_MAME_TIMERS` UNDEFINED (pdrift_audio.c
   owns timers/IRQ). Provide INT8/16/32 + UINT8/16/32 typedefs.
2. PRE-GENERATE the sin/pow/eg tables on host (the init uses pow/sin/log doubles)
   and embed as `ym2151_tables_pregen.h` — like Ikari's fmopl_tables_pregen.h — so
   the Amiga runtime is float-free (my mathstubs return 0 and would break live init).
3. Wire behind `-DPDR_LEAN_YM`: pdr_ym2151_reset/write_addr/write_data/
   generate_native map to YM2151ResetChip/WriteReg/YM2151UpdateOne. Keep ymfm
   (pdrift_ym2151.cpp) as the fallback.
4. VALIDATE vs MAME `pdrift -wavwrite` (FFT centroid ~2401Hz) before shipping — the
   St Dragon detune lesson. Generate at native clock/64 then resample (already done).
5. Amiga build + hw fps check.
