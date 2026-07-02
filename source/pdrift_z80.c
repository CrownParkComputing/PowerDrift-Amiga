/* pdrift_z80.c -- compiles the shared z80emu core with the Power Drift sound-CPU
 * memory map baked into the inline fast path. Z80 sound map (segaybd.cpp):
 *   0x0000-0xefff ROM, 0xf000-0xf7ff SegaPCM (mirror of f000-f0ff), 0xf800-0xffff RAM.
 * Fast (direct memory[]) = ROM reads + RAM R/W; everything else -> machine_rd/wr. */
#define Z80_FAST_READ(a)  (((a) < 0xf000u) || ((a) >= 0xf800u))
#define Z80_FAST_WRITE(a) ((a) >= 0xf800u)
#include "z80.c"
