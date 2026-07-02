/* pdrift_opm.c -- lean YM2151 path (Jarek Burczynski core, opm/ym2151.c).
 *
 * Drop-in replacement for the C++ ymfm wrapper (pdrift_ym2151.cpp), selected at
 * build time with -DPDR_LEAN_YM. The Jarek core is tight C that JITs far better
 * than ymfm on the Amiga 030, AND it does internal rate conversion -- so we
 * generate DIRECTLY at PDR_OUT_RATE (pitch/envelope calibrated from the chip
 * clock) instead of the native clock/64 rate, i.e. ~2.85x fewer synthesis
 * samples. Pure synth: timer_cb=NULL so the core never drives IRQ/timers --
 * pdrift_audio.c owns those. */
#include <stdint.h>
#include "opm/ym2151.h"
#include "pdrift_audio.h"

static int inited;
static uint8_t addr;

static void ensure(void)
{
    if (!inited) {
        YM2151Init(1, 0, PDR_YM2151_CLOCK, PDR_OUT_RATE, 0);
        inited = 1;
    }
}

void pdr_ym2151_reset(void) { ensure(); YM2151ResetChip(0); addr = 0; }
void pdr_ym2151_write_addr(uint8_t v) { ensure(); addr = v; }
void pdr_ym2151_write_data(uint8_t v) { ensure(); YM2151WriteReg(0, addr, v); }

/* Fill n mono samples at PDR_OUT_RATE. */
void pdr_ym2151_generate(int16_t *out, int n)
{
    ensure();
    static int16_t L[1024], R[1024];
    int done = 0;
    while (done < n) {
        int c = n - done;
        if (c > 1024) c = 1024;
        INT16 *bufs[2] = { L, R };
        YM2151UpdateOne(0, bufs, c);
        for (int i = 0; i < c; i++)
            out[done + i] = (int16_t)((L[i] + R[i]) >> 1);
        done += c;
    }
}
