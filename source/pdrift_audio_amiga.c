/* pdrift_audio_amiga.c -- Paula playback for the Power Drift sound board.
 *
 * Continuous-ring model (the proven family pattern, see st_audio_amiga.c):
 * Paula free-runs over one chip-RAM ring looping forever; each game frame we
 * advance an estimate of how many samples Paula has consumed (wall-clock EClock)
 * and render exactly that many NEW samples a fixed LEAD ahead of the read point.
 * The Z80 + YM2151 + SegaPCM are advanced PROPORTIONALLY to samples rendered
 * (audio-clocked), so music/SFX tempo stays correct regardless of video fps.
 * Two channels play the same mono stream, centred. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <stdint.h>
#include "pdrift_audio.h"

#define CUSTOM    ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_AUD0LCH (0x0a0/2)
#define R_AUD0LEN (0x0a4/2)
#define R_AUD0PER (0x0a6/2)
#define R_AUD0VOL (0x0a8/2)
#define R_AUD1LCH (0x0b0/2)
#define R_AUD1LEN (0x0b4/2)
#define R_AUD1PER (0x0b6/2)
#define R_AUD1VOL (0x0b8/2)
#define R_AUD2LEN (0x0c4/2)
#define R_AUD2VOL (0x0c8/2)
#define R_AUD3LEN (0x0d4/2)
#define R_AUD3VOL (0x0d8/2)

#define PDR_SR   PDR_OUT_RATE
#define PDR_PER  (3546895 / PDR_OUT_RATE)            /* PAL Paula period */
#define PDR_SPF  (PDR_OUT_RATE / 50 + 64)            /* per-frame refill unit (+margin) */
#define PDR_CYC_PER_SAMPLE (PDR_Z80_CLOCK / PDR_OUT_RATE)  /* Z80 cycles per output sample */

#define LEAD_FR  6
#define RING_FR  16
#define PDR_LEAD (LEAD_FR * PDR_SPF)
#define PDR_RING (RING_FR * PDR_SPF)

static signed char *ring = 0;
static unsigned long p_play = 0, p_wrote = 0;

/* self-contained EClock timer for wall-clock pacing. TimerBase is the library
 * base the timer.device proto stubs (ReadEClock) reference -- must be a plain
 * global, not static. */
struct Device *TimerBase = 0;
static struct MsgPort *tport = 0;
static struct timerequest *treq = 0;
static unsigned long aud_rate, aud_last, aud_frac;
static int aud_wallclock;

static void aud_setup(volatile uint16_t *c)
{
    uint32_t a = (uint32_t)ring;
    c[R_AUD0LCH] = (uint16_t)(a >> 16); c[R_AUD0LCH + 1] = (uint16_t)a;
    c[R_AUD1LCH] = (uint16_t)(a >> 16); c[R_AUD1LCH + 1] = (uint16_t)a;
    c[R_AUD0LEN] = PDR_RING / 2; c[R_AUD1LEN] = PDR_RING / 2;
    c[R_AUD0PER] = PDR_PER;      c[R_AUD1PER] = PDR_PER;
    c[R_AUD0VOL] = 64;           c[R_AUD1VOL] = 64;
}

/* Forward any pending sound commands, then advance the sound board + render. */
extern int pdrift_machine_sound_latch_take(uint8_t *cmd);
static void ring_render(unsigned long n)
{
    while (n) {
        unsigned long idx = p_wrote % PDR_RING;
        unsigned long chunk = PDR_RING - idx;
        if (chunk > n) chunk = n;
        uint8_t cmd;
        while (pdrift_machine_sound_latch_take(&cmd)) pdr_audio_command(cmd);
        pdr_audio_run_cycles((int)(chunk * PDR_CYC_PER_SAMPLE));
        pdr_audio_render(ring + idx, (int)chunk);
        p_wrote += chunk;
        n -= chunk;
    }
}

void pdr_audio_amiga_open(void)
{
    volatile uint16_t *c = CUSTOM;
    ring = (signed char *)AllocMem(PDR_RING, MEMF_CHIP | MEMF_CLEAR);
    if (!ring) return;
    p_play = p_wrote = 0; aud_wallclock = 0; aud_frac = 0;

    tport = CreateMsgPort();
    if (tport) {
        treq = (struct timerequest *)CreateIORequest(tport, sizeof *treq);
        if (treq && OpenDevice("timer.device", UNIT_ECLOCK, (struct IORequest *)treq, 0) == 0) {
            TimerBase = (struct Device *)treq->tr_node.io_Device;
            struct EClockVal ev;
            aud_rate = ReadEClock(&ev);
            if (aud_rate) { aud_last = ev.ev_lo; aud_wallclock = 1; }
        }
    }

    ring_render(PDR_LEAD);
    CacheClearU();

    c[R_DMACON] = 0x000f;                 /* stop AUD0-3 (clean 4-channel open) */
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0; c[R_AUD2VOL] = 0; c[R_AUD3VOL] = 0;
    c[R_AUD2LEN] = 0; c[R_AUD3LEN] = 0;
    aud_setup(c);
    c[R_DMACON] = 0x8203;                 /* SET|DMAEN|AUD0|AUD1 */
}

void pdr_audio_amiga_frame(void)
{
    if (!ring) return;

    if (aud_wallclock) {
        struct EClockVal ev; ReadEClock(&ev);
        unsigned long dt = ev.ev_lo - aud_last;
        aud_last = ev.ev_lo;
        unsigned long clamp = aud_rate / 10u;
        if (dt > clamp) dt = clamp;
        aud_frac += dt * (unsigned long)PDR_SR;
        p_play += aud_frac / aud_rate;
        aud_frac %= aud_rate;
    } else {
        p_play += PDR_SR / 60;            /* fallback: assume ~60Hz */
    }

    unsigned long target = p_play + PDR_LEAD;
    unsigned long cap = p_play + (PDR_RING - PDR_SPF);
    if (target > cap) target = cap;
    if ((long)(target - p_wrote) > 0) {
        ring_render(target - p_wrote);
        CacheClearU();
    }
}

unsigned long pdr_audio_eclock(unsigned long *rate)
{
    if (rate) *rate = aud_wallclock ? aud_rate : 0;
    if (!aud_wallclock) return 0;
    struct EClockVal ev;
    ReadEClock(&ev);
    return ev.ev_lo;
}

void pdr_audio_amiga_close(void)
{
    volatile uint16_t *c = CUSTOM;
    c[R_DMACON] = 0x0003;
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0;
    for (volatile unsigned i = 0; i < 50000; i++) ;
    if (treq) {
        if (TimerBase) CloseDevice((struct IORequest *)treq);
        DeleteIORequest((struct IORequest *)treq); treq = 0;
    }
    if (tport) { DeleteMsgPort(tport); tport = 0; }
    TimerBase = 0;
    if (ring) { FreeMem(ring, PDR_RING); ring = 0; }
    p_play = p_wrote = 0;
}
