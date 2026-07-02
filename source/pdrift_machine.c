/*
 * pdrift_machine.c -- shared Sega Y-board machine model.
 *
 * Extracted verbatim (minus the probe's coverage/env/snapshot tooling) from
 * tools/pdrift_host_probe.c. Provides the Musashi memory interface symbols
 * (m68k_read_memory_* / m68k_write_memory_*) so it must be the ONLY translation
 * unit in a link that defines them -- the host probe keeps its own copy and is
 * never linked together with this module.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "pdrift_machine.h"

enum cpu_id { CPU_MAIN, CPU_SUBX, CPU_SUBY, CPU_COUNT };

typedef struct {
    const char *name;
    uint8_t *rom;
    uint32_t rom_size;
    void *ctx;
    int reset_held;
    int reset_pending;
    uint16_t mult_regs[2];
    uint16_t div_regs[8];
    unsigned long spin_skips;
} cpu_t;

static cpu_t cpus[CPU_COUNT];
static cpu_t *cur;

uint8_t pdm_maincpu[PDM_MAINCPU_SIZE];
uint8_t pdm_subx[PDM_SUBX_SIZE];
uint8_t pdm_suby[PDM_SUBY_SIZE];

uint8_t pdm_ysprite_ram[0x10000];
uint8_t pdm_rotate_ram[0x0800];
uint8_t pdm_rotate_buffer[0x0800];
uint8_t pdm_bsprite_ram[0x1000];
uint8_t pdm_palette_ram[0x4000];
int pdm_palette_dirty = 1;

pdm_inputs_t pdm_in;

static uint8_t shared_ram[0x10000];
static uint8_t backup_ram[0x4000];
static uint8_t main_ram[0x10000];
static uint8_t subx_ram[0x4000];
static uint8_t suby_ram[0x10000];

static uint8_t sound_latch;
static int sound_latch_pending;
static uint8_t io_dir, io_cnt, io_latch[8], misc_io_data;
static uint8_t adc_shift;   /* msm6253 serial output register (MSB-first) */

static int active_irq_level;
static int spin_tripped;
static const int wait_hook_enabled = 1;
static const int max_slice_cycles = 20000;

static void init_cpu(int id);

void pdrift_machine_default_inputs(void)
{
    pdm_in.p1 = 0xff;
    pdm_in.general = 0xdf;   /* active-low bits high, active-high gear low */
    pdm_in.limitsw = 0x00;   /* pdrift limit switches are active-high */
    pdm_in.dsw = 0xea;       /* upright, demo on, 1 start/continue, continue yes */
    pdm_in.coinage = 0xff;
    for (int i = 0; i < 7; i++) pdm_in.adc[i] = 0x00;
    pdm_in.adc[5] = 0x80;    /* steering centred */
}

uint8_t pdrift_machine_sound_latch(void) { return sound_latch; }

int pdrift_machine_sound_latch_take(uint8_t *cmd)
{
    if (!sound_latch_pending) return 0;
    sound_latch_pending = 0;
    if (cmd) *cmd = sound_latch;
    return 1;
}

/* ---- known busy-wait spots: yield the timeslice so the peer CPU can run ---- */
static int cpu_waiting_at(int id, unsigned pc)
{
    if (id == CPU_MAIN) {
        if ((pc == 0x033084 || pc == 0x03308a) && !(shared_ram[0xff0e] & 0x80))
            return 1;
    } else if (id == CPU_SUBX) {
        if (pc == 0x00db90 && shared_ram[0xff0c])
            return 1;
    } else if (id == CPU_SUBY) {
        if (pc == 0x01188e && suby_ram[0xf9f0])
            return 1;
        if ((pc == 0x011c64 || pc == 0x011c6c) && (shared_ram[0xff12] & 0x01))
            return 1;
    }
    return 0;
}

static void instr_hook(unsigned int pc)
{
    if (!cur) return;
    int id = (int)(cur - cpus);
    if (id < 0 || id >= CPU_COUNT) return;
    if (wait_hook_enabled && !active_irq_level && cpu_waiting_at(id, pc)) {
        cur->spin_skips++;
        spin_tripped = 1;
        m68k_end_timeslice();
    }
}

/* ---- Sega 315-5248 multiplier / 315-5249 divider ---- */
static unsigned read_reg16(uint16_t w, unsigned addr, int size)
{
    if (size == 1) return (addr & 1) ? (w & 0xff) : (w >> 8);
    return w;
}

static uint16_t combine_reg16(uint16_t old, unsigned addr, unsigned val, int size)
{
    if (size == 1) {
        if (addr & 1) return (old & 0xff00) | (val & 0x00ff);
        return (old & 0x00ff) | ((val & 0x00ff) << 8);
    }
    return val & 0xffff;
}

static unsigned multiplier_read(cpu_t *cpu, unsigned addr, int size)
{
    unsigned off = ((addr - 0x080000) >> 1) & 3;
    int32_t product = (int16_t)cpu->mult_regs[0] * (int16_t)cpu->mult_regs[1];
    uint16_t w;
    switch (off) {
    case 0: w = cpu->mult_regs[0]; break;
    case 1: w = cpu->mult_regs[1]; break;
    case 2: w = (uint16_t)(product >> 16); break;
    default: w = (uint16_t)product; break;
    }
    return read_reg16(w, addr, size);
}

static void multiplier_write(cpu_t *cpu, unsigned addr, unsigned val, int size)
{
    unsigned off = ((addr - 0x080000) >> 1) & 1;
    cpu->mult_regs[off] = combine_reg16(cpu->mult_regs[off], addr, val, size);
}

static int32_t div_s32(uint16_t hi, uint16_t lo)
{
    uint32_t raw = ((uint32_t)hi << 16) | lo;
    if (raw & 0x80000000u) return (int32_t)((int64_t)raw - 0x100000000LL);
    return (int32_t)raw;
}

static void divider_execute(cpu_t *cpu, int mode)
{
    cpu->div_regs[6] = 0;
    if (mode == 0) {
        int32_t dividend = div_s32(cpu->div_regs[0], cpu->div_regs[1]);
        int16_t divisor = (int16_t)cpu->div_regs[2];
        int32_t quotient;
        if (divisor == 0) {
            quotient = dividend;
            cpu->div_regs[6] |= 0x4000;
        } else {
            quotient = dividend / divisor;
        }
        if (quotient < -32768) {
            quotient = -32768;
            cpu->div_regs[6] |= 0x8000;
        } else if (quotient > 32767) {
            quotient = 32767;
            cpu->div_regs[6] |= 0x8000;
        }
        cpu->div_regs[4] = (uint16_t)(int16_t)quotient;
        cpu->div_regs[5] = (uint16_t)(int16_t)(dividend - quotient * divisor);
    } else {
        uint32_t dividend = ((uint32_t)cpu->div_regs[0] << 16) | cpu->div_regs[1];
        uint32_t divisor = cpu->div_regs[2];
        uint32_t quotient;
        if (divisor == 0) {
            quotient = dividend;
            cpu->div_regs[6] |= 0x4000;
        } else {
            quotient = dividend / divisor;
        }
        cpu->div_regs[4] = (uint16_t)(quotient >> 16);
        cpu->div_regs[5] = (uint16_t)quotient;
    }
}

static unsigned divider_read(cpu_t *cpu, unsigned addr, int size)
{
    unsigned off = ((addr - 0x084000) >> 1) & 7;
    uint16_t w = 0xffff;
    switch (off) {
    case 0: case 1: case 2: case 4: case 5: case 6:
        w = cpu->div_regs[off];
        break;
    default:
        break;
    }
    return read_reg16(w, addr, size);
}

static void divider_write(cpu_t *cpu, unsigned addr, unsigned val, int size)
{
    unsigned off = ((addr - 0x084000) >> 1) & 0xf;
    switch (off & 3) {
    case 0: case 1: case 2:
        cpu->div_regs[off & 3] = combine_reg16(cpu->div_regs[off & 3], addr, val, size);
        break;
    default:
        break;
    }
    if (off & 8) divider_execute(cpu, off & 4);
}

/* ---- Y-board rotation control: swaps the double-buffered rotate RAM ---- */
static unsigned rotate_control_read(unsigned addr, int size)
{
    (void)addr;
    for (unsigned i = 0; i < sizeof pdm_rotate_ram; i += 4) {
        uint8_t tmp[4];
        memcpy(tmp, pdm_rotate_ram + i, sizeof tmp);
        memcpy(pdm_rotate_ram + i, pdm_rotate_buffer + i, sizeof tmp);
        memcpy(pdm_rotate_buffer + i, tmp, sizeof tmp);
    }
    return size == 1 ? 0xff : 0xffff;
}

static uint8_t *region_ptr(cpu_t *cpu, unsigned addr, unsigned *limit)
{
    addr &= 0x1fffff;
    switch (cpu - cpus) {
    case CPU_MAIN:
        if (addr < cpu->rom_size) { *limit = cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x1f0000 && addr < 0x200000) { *limit = 0x10000; return main_ram + (addr - 0x1f0000); }
        break;
    case CPU_SUBX:
        if (addr < cpu->rom_size) { *limit = cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x180000 && addr < 0x190000) { *limit = 0x10000; return pdm_ysprite_ram + (addr - 0x180000); }
        if (addr >= 0x1f8000 && addr < 0x1fc000) { *limit = 0x4000; return subx_ram + (addr - 0x1f8000); }
        if (addr >= 0x1fc000 && addr < 0x200000) { *limit = 0x4000; return backup_ram + (addr - 0x1fc000); }
        break;
    case CPU_SUBY:
        if (addr < cpu->rom_size) { *limit = cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x180000 && addr < 0x188000) { *limit = 0x0800; return pdm_rotate_ram + ((addr - 0x180000) & 0x07ff); }
        if (addr >= 0x188000 && addr < 0x190000) { *limit = 0x1000; return pdm_bsprite_ram + ((addr - 0x188000) & 0x0fff); }
        if (addr >= 0x190000 && addr < 0x198000) { *limit = 0x4000; return pdm_palette_ram + ((addr - 0x190000) & 0x3fff); }
        if (addr >= 0x1f0000 && addr < 0x200000) { *limit = 0x10000; return suby_ram + (addr - 0x1f0000); }
        break;
    }
    *limit = 0;
    return NULL;
}

static unsigned read_mapped(cpu_t *cpu, unsigned addr, int size)
{
    unsigned a = addr & 0x1fffff;
    uint8_t *p;
    unsigned limit;

    if (a >= 0x080000 && a <= 0x080007) return multiplier_read(cpu, a, size);
    if (a >= 0x084000 && a <= 0x08401f) return divider_read(cpu, a, size);
    if (cpu == &cpus[CPU_MAIN]) {
        if (a >= 0x100000 && a <= 0x10001f) {
            unsigned off = ((a - 0x100000) >> 1) & 0x3f;
            unsigned val = 0xff;
            if (off < 8) {
                if ((io_dir >> off) & 1) val = io_latch[off];
                else switch (off) {
                case 0: val = pdm_in.p1; break;
                case 1: val = pdm_in.general; break;
                case 2: val = pdm_in.limitsw; break;
                case 5: val = pdm_in.dsw; break;
                case 6: val = pdm_in.coinage; break;
                default: val = 0xff; break;
                }
            } else if (off >= 8 && off <= 11) {
                static const char sega[4] = { 'S', 'E', 'G', 'A' };
                val = sega[off - 8];
            } else if (off == 12 || off == 14) {
                val = io_cnt;
            } else if (off == 13 || off == 15) {
                val = io_dir;
            }
            return size == 1 ? (val & 0xff) : (0xff00 | (val & 0xff));
        }
        if (a >= 0x100040 && a <= 0x100047) {
            /* msm6253 d7_r: bit7 = next serial bit (MSB-first), low 7 = open bus */
            unsigned bit = (adc_shift & 0x80) ? 0x80 : 0;
            adc_shift = (uint8_t)(adc_shift << 1);
            unsigned val = bit | 0x7f;
            return size == 1 ? val : (0xff00 | val);
        }
    } else if (cpu == &cpus[CPU_SUBY]) {
        if (a >= 0x198000 && a < 0x1a0000) return rotate_control_read(a, size);
    }

    p = region_ptr(cpu, a, &limit);
    if (!p)
        return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
    if (size == 1) return p[0];
    if (size == 2) return ((unsigned)p[0] << 8) | p[1];
    return (((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]);
}

static void write_mapped(cpu_t *cpu, unsigned addr, unsigned val, int size)
{
    unsigned a = addr & 0x1fffff;
    uint8_t *p;
    unsigned limit;

    if (a >= 0x080000 && a <= 0x080007) { multiplier_write(cpu, a, val, size); return; }
    if (a >= 0x084000 && a <= 0x08401f) { divider_write(cpu, a, val, size); return; }
    if (cpu == &cpus[CPU_MAIN]) {
        if (a == 0x082001 || (a >= 0x082000 && a <= 0x083fff)) {
            sound_latch = val & 0xff;
            sound_latch_pending = 1;
            return;
        }
        if (a >= 0x100000 && a <= 0x10001f) {
            unsigned off = ((a - 0x100000) >> 1) & 0x3f;
            uint8_t data = val & 0xff;
            if (off < 8) {
                io_latch[off] = data;
                if ((io_dir >> off) & 1) {
                    if (off == 4) {
                        int old_x = cpus[CPU_SUBX].reset_held;
                        int old_y = cpus[CPU_SUBY].reset_held;
                        misc_io_data = data;
                        cpus[CPU_SUBX].reset_held = (data & 0x08) ? 1 : 0;
                        cpus[CPU_SUBY].reset_held = (data & 0x04) ? 1 : 0;
                        if (old_x && !cpus[CPU_SUBX].reset_held) cpus[CPU_SUBX].reset_pending = 1;
                        if (old_y && !cpus[CPU_SUBY].reset_held) cpus[CPU_SUBY].reset_pending = 1;
                    }
                }
            } else if (off == 14) {
                io_cnt = data;
            } else if (off == 15) {
                uint8_t old = io_dir;
                io_dir = data;
                for (unsigned i = 0; i < 8; i++) {
                    if (((old ^ data) >> i) & 1) {
                        if (((data >> i) & 1) && i == 4) {
                            int old_x = cpus[CPU_SUBX].reset_held;
                            int old_y = cpus[CPU_SUBY].reset_held;
                            misc_io_data = io_latch[i];
                            cpus[CPU_SUBX].reset_held = (misc_io_data & 0x08) ? 1 : 0;
                            cpus[CPU_SUBY].reset_held = (misc_io_data & 0x04) ? 1 : 0;
                            if (old_x && !cpus[CPU_SUBX].reset_held) cpus[CPU_SUBX].reset_pending = 1;
                            if (old_y && !cpus[CPU_SUBY].reset_held) cpus[CPU_SUBY].reset_pending = 1;
                        }
                    }
                }
            }
            return;
        }
        if (a >= 0x100040 && a <= 0x100047) {
            /* msm6253 address_w: latch the shift register from the HC4052 mux,
             * which selects analog channel 3+(misc_io_data&3) = brake/gas/steer. */
            unsigned idx = 3 + (misc_io_data & 3);
            adc_shift = (idx < 7) ? pdm_in.adc[idx] : 0x80;
            return;
        }
    } else if (cpu == &cpus[CPU_SUBY]) {
        if (a >= 0x198000 && a < 0x1a0000) return;
    }

    p = region_ptr(cpu, a, &limit);
    if (!p) return;
    if (p >= pdm_palette_ram && p < pdm_palette_ram + sizeof pdm_palette_ram)
        pdm_palette_dirty = 1;
    for (int i = size - 1; i >= 0; i--) {
        p[i] = val & 0xff;
        val >>= 8;
    }
}

unsigned int m68k_read_memory_8(unsigned int a) { return read_mapped(cur, a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { return read_mapped(cur, a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { write_mapped(cur, a, v, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { write_mapped(cur, a, v, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }

static int int_ack(int level)
{
    (void)level;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

static void init_cpu(int id)
{
    cur = &cpus[id];
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack);
    m68k_pulse_reset();
    m68k_get_context(cur->ctx);
}

static void execute_cpu(int id, int cycles, int irq_level)
{
    cur = &cpus[id];
    if (cur->reset_held) return;
    if (cur->reset_pending) {
        cur->reset_pending = 0;
        init_cpu(id);
        cur = &cpus[id];
    }
    m68k_set_context(cur->ctx);
    if (!irq_level) {
        unsigned pc = (unsigned)m68k_get_reg(cur->ctx, M68K_REG_PC);
        if (cpu_waiting_at(id, pc)) {
            cur->spin_skips++;
            m68k_get_context(cur->ctx);
            return;
        }
    }
    if (irq_level)
        m68k_set_irq(irq_level);
    while (cycles > 0) {
        int slice = cycles > max_slice_cycles ? max_slice_cycles : cycles;
        active_irq_level = irq_level;
        spin_tripped = 0;
        int used = m68k_execute(slice);
        active_irq_level = 0;
        if (spin_tripped)
            break;
        cycles -= used > 0 ? used : slice;
    }
    m68k_set_irq(0);
    m68k_get_context(cur->ctx);
}

static void execute_phase_all(int cycles, int irq_level)
{
    execute_cpu(CPU_MAIN, cycles, irq_level);
    execute_cpu(CPU_SUBX, cycles, irq_level);
    execute_cpu(CPU_SUBY, cycles, irq_level);
}

void pdrift_machine_run_frame(void)
{
    const int cycles_per_scanline = 12500000 / 60 / 262;
    const int irq2_scanline = 170;
    const int vblank_scanline = 223;
    execute_phase_all(cycles_per_scanline * irq2_scanline, 0);
    execute_phase_all(cycles_per_scanline, 2);
    execute_phase_all(cycles_per_scanline * (vblank_scanline - irq2_scanline - 1), 0);
    execute_phase_all(cycles_per_scanline, 4);
    execute_phase_all(cycles_per_scanline * (262 - vblank_scanline - 1), 0);
}

void pdrift_machine_init(void)
{
    cpus[CPU_MAIN].name = "main";
    cpus[CPU_SUBX].name = "subx";
    cpus[CPU_SUBY].name = "suby";
    cpus[CPU_MAIN].rom = pdm_maincpu; cpus[CPU_MAIN].rom_size = PDM_MAINCPU_SIZE;
    cpus[CPU_SUBX].rom = pdm_subx;    cpus[CPU_SUBX].rom_size = PDM_SUBX_SIZE;
    cpus[CPU_SUBY].rom = pdm_suby;    cpus[CPU_SUBY].rom_size = PDM_SUBY_SIZE;

    m68k_init();
    m68k_set_instr_hook_callback(instr_hook);
    unsigned ctx_size = m68k_context_size();
    for (int i = 0; i < CPU_COUNT; i++) {
        cpus[i].ctx = calloc(1, ctx_size);
        init_cpu(i);
    }
}
