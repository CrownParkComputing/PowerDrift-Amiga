/*
 * pdrift_machine.h -- shared Sega Y-board machine model (3x MC68000 + custom
 * math/IO), factored out of tools/pdrift_host_probe.c so the host validator and
 * the Amiga live runtime execute the exact same core. Semantics are kept
 * byte-identical to the probe (wait-hook spin-skip, 20000-cycle slices, the same
 * scanline IRQ schedule), so a given frame count reproduces the probe snapshots.
 */
#ifndef PDRIFT_MACHINE_H
#define PDRIFT_MACHINE_H

#include <stdint.h>

/* ROM images: the caller fills these before pdrift_machine_init(). */
#define PDM_MAINCPU_SIZE 0x080000
#define PDM_SUBX_SIZE    0x040000
#define PDM_SUBY_SIZE    0x040000
extern uint8_t pdm_maincpu[PDM_MAINCPU_SIZE];
extern uint8_t pdm_subx[PDM_SUBX_SIZE];
extern uint8_t pdm_suby[PDM_SUBY_SIZE];

/* Video RAM the renderer reads; populated by the CPUs during run_frame(). */
extern uint8_t pdm_ysprite_ram[0x10000];
extern uint8_t pdm_rotate_ram[0x0800];
extern uint8_t pdm_rotate_buffer[0x0800];
extern uint8_t pdm_bsprite_ram[0x1000];
extern uint8_t pdm_palette_ram[0x4000];
/* Set to 1 whenever a CPU writes palette RAM; the renderer clears it after
 * rebuilding the palette caches, so a static palette skips the rebuild. */
extern int pdm_palette_dirty;

/*
 * Input port state, sampled at the top of every frame. Defaults match the
 * probe's pdrift attract configuration (no coin, demo on). Set p1/general bits
 * (active-low) to drive coin/start/gear; adc[] feeds the steering/pedal ADC.
 */
typedef struct {
    uint8_t p1;        /* IO port 0 */
    uint8_t general;   /* IO port 1: coin (0x40) START1 (0x08) SERVICE1 (0x04) all
                        * active-low; gear-shift (0x20) active-high */
    uint8_t limitsw;   /* IO port 2 */
    uint8_t dsw;       /* IO port 5: DIP switches */
    uint8_t coinage;   /* IO port 6 */
    /* Analog channels, indexed as MAME's ADC.n via the HC4052 mux (misc_io_data
     * low 2 bits select 3+n): [3]=brake [4]=gas [5]=steering. 0x00=released,
     * steering centre 0x80 (range 0x20..0xe0). */
    uint8_t adc[7];
} pdm_inputs_t;
extern pdm_inputs_t pdm_in;

/* Reset defaults into pdm_in (call before init if you want the attract config). */
void pdrift_machine_default_inputs(void);

/* Boot all three 68000s. ROM buffers must be loaded first. */
void pdrift_machine_init(void);

/* Advance the machine one 60Hz frame (scanline-phased IRQ2/IRQ4 schedule). */
void pdrift_machine_run_frame(void);

/* Latest byte written to the sound latch. */
uint8_t pdrift_machine_sound_latch(void);

/* generic_latch_8 data-pending: returns 1 (and clears) if the main CPU wrote a
 * new sound command since the last call, delivering it in *cmd. Drives the Z80
 * sound-board NMI. */
int pdrift_machine_sound_latch_take(uint8_t *cmd);

#endif
