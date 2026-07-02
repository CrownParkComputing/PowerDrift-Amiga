/* pdrift_audio.c -- Power Drift Y-board sound board.
 *
 * Z80 sound CPU (z80emu) + YM2151 (ymfm, pure synth) + SegaPCM. Wiring per
 * segaybd.cpp: soundlatch write -> Z80 NMI; YM2151 timer IRQ -> Z80 INT (RST38);
 * SegaPCM at 0xf000-0xf7ff; YM2151 on I/O ports 0x00/0x01; soundlatch read on
 * port 0x40. Audio-clocked (family standard): pdr_audio_run_cycles() is pulled
 * from the Paula refill, and the YM2151 timers are advanced in Z80 cycles so the
 * music tempo is independent of video fps. Float-free on the C side. */
#include <string.h>
#include "z80emu.h"
#include "pdrift_audio.h"
#include "pdrift_segapcm.h"

/* YM2151 synth: ymfm (pdrift_ym2151.cpp) or the lean Jarek core (pdrift_opm.c,
 * -DPDR_LEAN_YM). Both are pure synths (pdrift_audio.c owns timers/IRQ) and both
 * expose pdr_ym2151_generate() filling n MONO samples at PDR_OUT_RATE. */
extern void pdr_ym2151_reset(void);
extern void pdr_ym2151_write_addr(uint8_t v);
extern void pdr_ym2151_write_data(uint8_t v);
extern void pdr_ym2151_generate(int16_t *out, int n);

static MY_LITTLE_Z80 z;
static int booted;

/* soundlatch: main CPU write -> data pending -> Z80 NMI; Z80 reads it on port 0x40 */
static uint8_t latch_cmd;
static int nmi_edge;

/* YM2151 timers/IRQ modeled clock-correct (ymfm is a pure synth). YM2151 shares
 * the Z80 clock (both SOUND_CLOCK/8), so 1 YM clock == 1 Z80 cycle. Timer A =
 * 64*(1024-TA) clocks, Timer B = 1024*(256-TB) clocks. */
static uint8_t ym_addr, ym_regs[256], ym_status;
static int ym_irqen_a, ym_irqen_b, ym_ta_load, ym_tb_load, ym_irq_line;
static long ym_ta_count, ym_tb_count;

static unsigned long dbg_nmis, dbg_ym_writes, dbg_pcm_writes;
static int dbg_out_peak;

static long ym_periodA(void) { int ta = (ym_regs[0x10] << 2) | (ym_regs[0x11] & 3); return (long)(1024 - ta) * 64; }
static long ym_periodB(void) { int tb = ym_regs[0x12]; return (long)(256 - tb) * 1024; }

static int ym_irq_active(void)
{
    return ((ym_status & 0x01) && ym_irqen_a) || ((ym_status & 0x02) && ym_irqen_b);
}

static void ym_write_addr(uint8_t v) { ym_addr = v; pdr_ym2151_write_addr(v); }

static void ym_write_data(uint8_t v)
{
    dbg_ym_writes++;
    ym_regs[ym_addr] = v;
    pdr_ym2151_write_data(v);
    if (ym_addr == 0x14) {                 /* timer control */
        ym_irqen_a = (v >> 2) & 1;
        ym_irqen_b = (v >> 3) & 1;
        if (v & 0x01) { if (!ym_ta_load) ym_ta_count = ym_periodA(); ym_ta_load = 1; } else ym_ta_load = 0;
        if (v & 0x02) { if (!ym_tb_load) ym_tb_count = ym_periodB(); ym_tb_load = 1; } else ym_tb_load = 0;
        if (v & 0x10) ym_status &= ~0x01;  /* reset flag A */
        if (v & 0x20) ym_status &= ~0x02;  /* reset flag B */
        ym_irq_line = ym_irq_active();
    }
}

static void ym_advance(long cyc)
{
    if (ym_ta_load) {
        ym_ta_count -= cyc;
        while (ym_ta_count <= 0) { ym_status |= 0x01; ym_ta_count += ym_periodA(); }
    }
    if (ym_tb_load) {
        ym_tb_count -= cyc;
        while (ym_tb_count <= 0) { ym_status |= 0x02; ym_tb_count += ym_periodB(); }
    }
    ym_irq_line = ym_irq_active();
}

/* ---- z80emu callbacks ---- */
unsigned char machine_rd(MY_LITTLE_Z80 *zz, unsigned a)
{
    if (a >= 0xf000 && a <= 0xf7ff) return pdr_segapcm_read(a & 0xff);
    return zz->memory[a & 0xffff];
}

void machine_wr(MY_LITTLE_Z80 *zz, unsigned a, unsigned char v)
{
    (void)zz;
    if (a >= 0xf000 && a <= 0xf7ff) { pdr_segapcm_write(a & 0xff, v); dbg_pcm_writes++; return; }
    /* a < 0xf000 = ROM: ignore */
}

unsigned char in_impl(MY_LITTLE_Z80 *zz, int port)
{
    (void)zz;
    if (port & 0x40) return latch_cmd;      /* 0x40-0x7f: soundlatch read */
    return ym_status;                        /* 0x00-0x3f: YM2151 status */
}

void out_impl(MY_LITTLE_Z80 *zz, int port, unsigned char v)
{
    (void)zz;
    if ((port & 0x40) == 0) {                /* 0x00-0x3f: YM2151, bit0 = addr/data */
        if (port & 1) ym_write_data(v);
        else ym_write_addr(v);
    }
}

/* ---- public API ---- */
void pdr_audio_init(const uint8_t *soundcpu, const uint8_t *pcm, uint32_t pcm_size)
{
    memset(&z, 0, sizeof z);
    memcpy(z.memory, soundcpu, 0xf000);      /* ROM 0x0000-0xefff */
    z.opcodes = 0; z.opcodes_len = 0;
    pdr_segapcm_init(pcm, pcm_size);
    pdr_audio_reset();
}

void pdr_audio_reset(void)
{
    memset(ym_regs, 0, sizeof ym_regs);
    ym_addr = ym_status = 0;
    ym_irqen_a = ym_irqen_b = ym_ta_load = ym_tb_load = ym_irq_line = 0;
    ym_ta_count = ym_tb_count = 0;
    latch_cmd = 0; nmi_edge = 0;
    dbg_nmis = dbg_ym_writes = dbg_pcm_writes = 0; dbg_out_peak = 0;
    pdr_segapcm_reset();
    pdr_ym2151_reset();
    Z80Reset(&z.state);
    booted = 1;
}

void pdr_audio_command(uint8_t cmd)
{
    latch_cmd = cmd;
    nmi_edge = 1;      /* data-pending -> NMI (edge) */
}

void pdr_audio_run_cycles(int total)
{
    if (!booted) return;
    const int SLICE = 512;
    while (total > 0) {
        if (nmi_edge) { Z80NonMaskableInterrupt(&z.state, &z); nmi_edge = 0; dbg_nmis++; }
        if (ym_irq_line) Z80Interrupt(&z.state, 0xFF, &z);   /* IM1 -> RST38 */
        int n = total < SLICE ? total : SLICE;
        Z80Emulate(&z.state, n, &z);
        ym_advance(n);
        total -= n;
    }
}

/* SegaPCM native rate (31461Hz) resampled to PDR_OUT_RATE by decimating averager. */
static int segapcm_out(void)
{
    static uint32_t step = 0, frac = 0;
    if (step == 0) step = (uint32_t)(((uint64_t)PDR_SEGAPCM_RATE << 16) / PDR_OUT_RATE);
    frac += step;
    int count = (int)(frac >> 16);
    frac &= 0xffff;
    if (count < 1) count = 1;
    int32_t l = 0, r = 0;
    for (int i = 0; i < count; i++) pdr_segapcm_render(&l, &r);
    return (int)((l + r) / count);           /* mono, averaged over consumed native samples */
}

void pdr_audio_render(signed char *out, int n)
{
    /* Both YM cores emit n mono samples straight at PDR_OUT_RATE -- no resampling
     * here anymore. Mix with per-sample SegaPCM to signed-8 Paula. */
    enum { MAXOUT = 2048 };
    static int16_t ym[MAXOUT];
    if (n > MAXOUT) n = MAXOUT;
    pdr_ym2151_generate(ym, n);

    for (int i = 0; i < n; i++) {
        int fm = ym[i];                          /* +-32767 */
        int pcm = segapcm_out();                 /* sum v*vol over active channels */
        /* FM ~ full-scale/9, PCM ~ full-scale/64 per active channel. */
        int s = (fm >> 9) + (pcm >> 8);
        if (s > 127) s = 127; else if (s < -128) s = -128;
        int a = s < 0 ? -s : s; if (a > dbg_out_peak) dbg_out_peak = a;
        out[i] = (signed char)s;
    }
}

int pdr_audio_dbg_booted(void) { return booted; }
unsigned long pdr_audio_dbg_nmis(void) { return dbg_nmis; }
unsigned long pdr_audio_dbg_ym_writes(void) { return dbg_ym_writes; }
unsigned long pdr_audio_dbg_pcm_writes(void) { return dbg_pcm_writes; }
int pdr_audio_dbg_out_peak(void) { return dbg_out_peak; }
