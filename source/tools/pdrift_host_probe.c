/*
 * pdrift_host_probe.c -- first host-side Sega Y-board boot probe.
 *
 * Loads the flat blobs produced by pdrift_extract_roms.py, boots the three
 * MC68000 programs with Musashi, and records whether they start touching shared
 * RAM, Y-board sprite RAM, secondary sprite RAM, palette RAM, and the sound
 * latch. This is not gameplay; it is the bring-up harness for scheduler/MMIO
 * work before any Amiga RTG package exists.
 *
 * Build from source/:
 *   bash build_host_probe.sh
 *
 * Run:
 *   /tmp/pdrift_host_probe [build/pdrift] [frames] [snapshot-dir]
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "m68k.h"

enum cpu_id { CPU_MAIN, CPU_SUBX, CPU_SUBY, CPU_COUNT };

typedef struct {
    const char *name;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *work;
    size_t work_size;
    void *ctx;
    unsigned long unmapped_r;
    unsigned long unmapped_w;
    unsigned long mult_w;
    unsigned long div_w;
    unsigned long spin_skips;
    unsigned long irq4_raised;
    unsigned long irq2_raised;
    unsigned long irq_taken[8];
    int reset_held;
    int reset_pending;
    uint16_t mult_regs[2];
    uint16_t div_regs[8];
} cpu_t;

static cpu_t cpus[CPU_COUNT];
static cpu_t *cur;
static uint8_t shared_ram[0x10000];
static uint8_t ysprite_ram[0x10000];
static uint8_t rotate_ram[0x0800];
static uint8_t rotate_buffer[0x0800];
static uint8_t bsprite_ram[0x1000];
static uint8_t palette_ram[0x4000];
static uint8_t backup_ram[0x4000];
static uint8_t main_ram[0x10000];
static uint8_t subx_ram[0x4000];
static uint8_t suby_ram[0x10000];

static unsigned long shared_w, ysprite_w, rotate_w, bsprite_w, palette_w;
static unsigned long sound_latch_w, io_r, io_w, adc_r, adc_w, rotate_control_r;
static uint8_t sound_latch;
static uint8_t io_dir, io_cnt, io_latch[8], misc_io_data;
static uint8_t port_p1 = 0xff;
static uint8_t port_general = 0xdf;  /* pdrift: active-low bits high, active-high gear low */
static uint8_t port_limitsw = 0x00;  /* pdrift limit switches are active-high */
static uint8_t port_dsw = 0xea;      /* upright, demo on, 1 start/continue, continue yes, normal */
static uint8_t port_coinage = 0xff;
static uint8_t port_general_base = 0xdf;
static int coin_frame = -1, coin_hold = 2;
static int start_frame = -1, start_hold = 8;
static const char *coverage_path;
static uint32_t *pc_counts[CPU_COUNT];
static size_t pc_count_words[CPU_COUNT];
static int active_irq_level;
static int spin_tripped;
static int wait_hook_enabled = 1;
static int max_slice_cycles = 20000;

static uint16_t rd_be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static void init_cpu(int id);
static void die_errno(const char *what, const char *path);

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
    if (coverage_path && pc < cpus[id].rom_size) {
        size_t index = (pc & ~1u) >> 1;
        if (index < pc_count_words[id] && pc_counts[id][index] != UINT32_MAX)
            pc_counts[id][index]++;
    }
    if (wait_hook_enabled && !active_irq_level && cpu_waiting_at(id, pc)) {
        cur->spin_skips++;
        spin_tripped = 1;
        m68k_end_timeslice();
    }
}

static void init_coverage(void)
{
    if (!coverage_path || !*coverage_path) return;
    for (int i = 0; i < CPU_COUNT; i++) {
        pc_count_words[i] = (cpus[i].rom_size + 1) / 2;
        pc_counts[i] = calloc(pc_count_words[i], sizeof pc_counts[i][0]);
        if (!pc_counts[i]) {
            fprintf(stderr, "coverage allocation failed for %s\n", cpus[i].name);
            exit(1);
        }
    }
    m68k_set_instr_hook_callback(instr_hook);
}

static void write_coverage(void)
{
    if (!coverage_path || !*coverage_path) return;
    FILE *f = fopen(coverage_path, "w");
    if (!f) die_errno("open", coverage_path);
    for (int c = 0; c < CPU_COUNT; c++) {
        unsigned long distinct = 0;
        unsigned long long hits = 0;
        fprintf(f, "[%s]\n", cpus[c].name);
        for (size_t i = 0; i < pc_count_words[c]; i++) {
            if (pc_counts[c][i]) {
                fprintf(f, "%06zx %u\n", i * 2, pc_counts[c][i]);
                distinct++;
                hits += pc_counts[c][i];
            }
        }
        fprintf(stderr, "coverage %-5s distinct=%lu hits=%llu\n", cpus[c].name, distinct, hits);
    }
    fclose(f);
    fprintf(stderr, "coverage wrote %s\n", coverage_path);
}

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
    cpu->mult_w++;
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
    cpu->div_w++;
}

static unsigned rotate_control_read(unsigned addr, int size)
{
    (void)addr;
    rotate_control_r++;
    for (unsigned i = 0; i < sizeof rotate_ram; i += 4) {
        uint8_t tmp[4];
        memcpy(tmp, rotate_ram + i, sizeof tmp);
        memcpy(rotate_ram + i, rotate_buffer + i, sizeof tmp);
        memcpy(rotate_buffer + i, tmp, sizeof tmp);
    }
    return size == 1 ? 0xff : 0xffff;
}

static uint8_t *region_ptr(cpu_t *cpu, unsigned addr, unsigned *limit)
{
    addr &= 0x1fffff;
    switch (cpu - cpus) {
    case CPU_MAIN:
        if (addr < cpu->rom_size) { *limit = (unsigned)cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x1f0000 && addr < 0x200000) { *limit = 0x10000; return main_ram + (addr - 0x1f0000); }
        break;
    case CPU_SUBX:
        if (addr < cpu->rom_size) { *limit = (unsigned)cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x180000 && addr < 0x190000) { *limit = 0x10000; return ysprite_ram + (addr - 0x180000); }
        if (addr >= 0x1f8000 && addr < 0x1fc000) { *limit = 0x4000; return subx_ram + (addr - 0x1f8000); }
        if (addr >= 0x1fc000 && addr < 0x200000) { *limit = 0x4000; return backup_ram + (addr - 0x1fc000); }
        break;
    case CPU_SUBY:
        if (addr < cpu->rom_size) { *limit = (unsigned)cpu->rom_size; return cpu->rom + addr; }
        if (addr >= 0x0c0000 && addr < 0x0d0000) { *limit = 0x10000; return shared_ram + (addr - 0x0c0000); }
        if (addr >= 0x180000 && addr < 0x188000) { *limit = 0x0800; return rotate_ram + ((addr - 0x180000) & 0x07ff); }
        if (addr >= 0x188000 && addr < 0x190000) { *limit = 0x1000; return bsprite_ram + ((addr - 0x188000) & 0x0fff); }
        if (addr >= 0x190000 && addr < 0x198000) { *limit = 0x4000; return palette_ram + ((addr - 0x190000) & 0x3fff); }
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
            io_r++;
            if (off < 8) {
                if ((io_dir >> off) & 1) val = io_latch[off];
                else switch (off) {
                case 0: val = port_p1; break;
                case 1: val = port_general; break;
                case 2: val = port_limitsw; break;
                case 5: val = port_dsw; break;
                case 6: val = port_coinage; break;
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
            unsigned sel = misc_io_data & 3;
            unsigned val = (sel == 0) ? 0x00 : (sel == 1) ? 0x00 : 0x80;
            adc_r++;
            return size == 1 ? val : (0xff00 | val);
        }
    } else if (cpu == &cpus[CPU_SUBY]) {
        if (a >= 0x198000 && a < 0x1a0000) return rotate_control_read(a, size);
    }

    p = region_ptr(cpu, a, &limit);
    if (!p) {
        cpu->unmapped_r++;
        return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
    }
    if (size == 1) return p[0];
    if (size == 2) return ((unsigned)p[0] << 8) | p[1];
    return (((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]);
}

static void note_write(cpu_t *cpu, unsigned a)
{
    if (a >= 0x0c0000 && a < 0x0d0000) shared_w++;
    if (cpu == &cpus[CPU_SUBX] && a >= 0x180000 && a < 0x190000) ysprite_w++;
    if (cpu == &cpus[CPU_SUBY] && a >= 0x180000 && a < 0x188000) rotate_w++;
    if (cpu == &cpus[CPU_SUBY] && a >= 0x188000 && a < 0x190000) bsprite_w++;
    if (cpu == &cpus[CPU_SUBY] && a >= 0x190000 && a < 0x198000) palette_w++;
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
            sound_latch_w++;
            return;
        }
        if (a >= 0x100000 && a <= 0x10001f) {
            unsigned off = ((a - 0x100000) >> 1) & 0x3f;
            uint8_t data = val & 0xff;
            io_w++;
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
        if (a >= 0x100040 && a <= 0x100047) { adc_w++; return; }
    } else if (cpu == &cpus[CPU_SUBY]) {
        if (a >= 0x198000 && a < 0x1a0000) return;
    }

    p = region_ptr(cpu, a, &limit);
    if (!p) {
        cpu->unmapped_w++;
        return;
    }
    note_write(cpu, a);
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
    if (cur && level >= 0 && level < 8) cur->irq_taken[level]++;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

static void die_errno(const char *what, const char *path)
{
    fprintf(stderr, "%s %s: %s\n", what, path, strerror(errno));
    exit(1);
}

static uint8_t *load_blob(const char *dir, const char *name, size_t want)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) die_errno("open", path);
    uint8_t *buf = calloc(1, want);
    if (!buf) { fprintf(stderr, "out of memory for %s\n", name); exit(1); }
    size_t got = fread(buf, 1, want, f);
    if (ferror(f)) die_errno("read", path);
    fclose(f);
    if (got != want) {
        fprintf(stderr, "%s: got %zu bytes, expected %zu\n", path, got, want);
        exit(1);
    }
    return buf;
}

static void init_cpu(int id)
{
    cur = &cpus[id];
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack);
    m68k_pulse_reset();
    m68k_get_context(cur->ctx);
    fprintf(stderr, "%-5s reset SSP=%08x PC=%08x IRQ4=%08x\n",
        cur->name,
        (unsigned)m68k_get_reg(cur->ctx, M68K_REG_SP),
        (unsigned)m68k_get_reg(cur->ctx, M68K_REG_PC),
        (cur->rom[0x70] << 24) | (cur->rom[0x71] << 16) | (cur->rom[0x72] << 8) | cur->rom[0x73]);
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
    if (irq_level) {
        if (irq_level == 2) cur->irq2_raised++;
        if (irq_level == 4) cur->irq4_raised++;
        m68k_set_irq(irq_level);
    }
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

static void execute_frame_all(void)
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

#if 0
static void execute_frame_cpu_old(int id)
{
    cur = &cpus[id];
    if (cur->reset_held) return;
    m68k_set_context(cur->ctx);
    {
        cur->irq4_raised++;
        m68k_set_irq(4);
    }
    while (cycles > 0) {
        int slice = cycles > 20000 ? 20000 : cycles;
        int used = m68k_execute(slice);
        cycles -= used > 0 ? used : slice;
    }
    m68k_set_irq(0);
    m68k_get_context(cur->ctx);
}
#endif

static int count_words(const uint8_t *p, size_t bytes)
{
    int n = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2)
        if (p[i] || p[i + 1]) n++;
    return n;
}

static void dump_words(const char *label, const uint8_t *p, int words)
{
    fprintf(stderr, "%s", label);
    for (int i = 0; i < words; i++) fprintf(stderr, " %04x", rd_be16(p + i * 2));
    fprintf(stderr, "\n");
}

static void dump_summary(int frames)
{
    fprintf(stderr, "\nran %d frames\n", frames);
    for (int i = 0; i < CPU_COUNT; i++) {
        cpu_t *c = &cpus[i];
        fprintf(stderr, "%-5s PC=%06x IRQ2=%lu/%lu IRQ4=%lu/%lu reset=%d unmapped r/w=%lu/%lu mult/div writes=%lu/%lu\n",
            c->name,
            (unsigned)m68k_get_reg(c->ctx, M68K_REG_PC),
            c->irq2_raised,
            c->irq_taken[2],
            c->irq4_raised,
            c->irq_taken[4],
            c->reset_held,
            c->unmapped_r,
            c->unmapped_w,
            c->mult_w,
            c->div_w);
        fprintf(stderr, "      spin_skips=%lu\n", c->spin_skips);
    }
    fprintf(stderr, "\nactivity:\n");
    fprintf(stderr, "  shared_w=%lu ysprite_w=%lu rotate_w=%lu bsprite_w=%lu palette_w=%lu\n",
        shared_w, ysprite_w, rotate_w, bsprite_w, palette_w);
    fprintf(stderr, "  sound_latch_w=%lu last=%02x io r/w=%lu/%lu adc r/w=%lu/%lu\n",
        sound_latch_w, sound_latch, io_r, io_w, adc_r, adc_w);
    fprintf(stderr, "  rotate_control_r=%lu\n", rotate_control_r);
    fprintf(stderr, "\nnon-zero words:\n");
    fprintf(stderr, "  shared=%d ysprites=%d rotate=%d bsprites=%d palette=%d\n",
        count_words(shared_ram, sizeof shared_ram),
        count_words(ysprite_ram, sizeof ysprite_ram),
        count_words(rotate_ram, sizeof rotate_ram),
        count_words(bsprite_ram, sizeof bsprite_ram),
        count_words(palette_ram, sizeof palette_ram));
    dump_words("  shared @0c0000:", shared_ram, 16);
    dump_words("  yspr   @180000:", ysprite_ram, 16);
    dump_words("  rot    @180000:", rotate_ram, 16);
    dump_words("  bspr   @188000:", bsprite_ram, 16);
    dump_words("  pal    @190000:", palette_ram, 16);
}

static void write_snapshot_blob(const char *dir, const char *name, const uint8_t *data, size_t bytes)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) die_errno("open", path);
    if (fwrite(data, 1, bytes, f) != bytes) die_errno("write", path);
    fclose(f);
    fprintf(stderr, "  wrote %s (%zu bytes)\n", path, bytes);
}

static void write_snapshot(const char *dir)
{
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) die_errno("mkdir", dir);
    fprintf(stderr, "\nsnapshot:\n");
    write_snapshot_blob(dir, "shared_ram.bin", shared_ram, sizeof shared_ram);
    write_snapshot_blob(dir, "ysprite_ram.bin", ysprite_ram, sizeof ysprite_ram);
    write_snapshot_blob(dir, "rotate_ram.bin", rotate_ram, sizeof rotate_ram);
    write_snapshot_blob(dir, "rotate_buffer.bin", rotate_buffer, sizeof rotate_buffer);
    write_snapshot_blob(dir, "bsprite_ram.bin", bsprite_ram, sizeof bsprite_ram);
    write_snapshot_blob(dir, "palette_ram.bin", palette_ram, sizeof palette_ram);
}

static uint8_t env_hex8(const char *name, uint8_t fallback)
{
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s) return fallback;
    return (uint8_t)v;
}

static int env_int(const char *name, int fallback)
{
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s) return fallback;
    return (int)v;
}

static void update_frame_inputs(int frame)
{
    port_general = port_general_base;
    if (coin_frame >= 0 && frame >= coin_frame && frame < coin_frame + coin_hold)
        port_general &= (uint8_t)~0x40;
    if (start_frame >= 0 && frame >= start_frame && frame < start_frame + start_hold)
        port_general &= (uint8_t)~0x08;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/pdrift";
    int frames = argc > 2 ? atoi(argv[2]) : 120;
    const char *snapshot_dir = argc > 3 ? argv[3] : NULL;
    if (frames < 1) frames = 1;

    port_p1 = env_hex8("PDRIFT_P1", port_p1);
    port_general = env_hex8("PDRIFT_GENERAL", port_general);
    port_general_base = port_general;
    port_limitsw = env_hex8("PDRIFT_LIMITSW", port_limitsw);
    port_dsw = env_hex8("PDRIFT_DSW", port_dsw);
    port_coinage = env_hex8("PDRIFT_COINAGE", port_coinage);
    coin_frame = env_int("PDRIFT_COIN_FRAME", coin_frame);
    coin_hold = env_int("PDRIFT_COIN_HOLD", coin_hold);
    start_frame = env_int("PDRIFT_START_FRAME", start_frame);
    start_hold = env_int("PDRIFT_START_HOLD", start_hold);
    wait_hook_enabled = env_int("PDRIFT_WAIT_HOOK", wait_hook_enabled);
    max_slice_cycles = env_int("PDRIFT_SLICE_CYCLES", max_slice_cycles);
    if (max_slice_cycles < 1) max_slice_cycles = 1;
    coverage_path = getenv("PDRIFT_COVERAGE");
    fprintf(stderr, "inputs: P1=%02x GENERAL=%02x LIMITSW=%02x DSW=%02x COINAGE=%02x coin=%d+%d start=%d+%d wait_hook=%d slice=%d\n",
        port_p1, port_general, port_limitsw, port_dsw, port_coinage,
        coin_frame, coin_hold, start_frame, start_hold, wait_hook_enabled, max_slice_cycles);

    cpus[CPU_MAIN].name = "main";
    cpus[CPU_SUBX].name = "subx";
    cpus[CPU_SUBY].name = "suby";
    cpus[CPU_MAIN].rom = load_blob(dir, "maincpu.bin", 0x080000); cpus[CPU_MAIN].rom_size = 0x080000;
    cpus[CPU_SUBX].rom = load_blob(dir, "subx.bin", 0x040000);    cpus[CPU_SUBX].rom_size = 0x040000;
    cpus[CPU_SUBY].rom = load_blob(dir, "suby.bin", 0x040000);    cpus[CPU_SUBY].rom_size = 0x040000;

    m68k_init();
    init_coverage();
    if ((coverage_path && *coverage_path) || wait_hook_enabled)
        m68k_set_instr_hook_callback(instr_hook);
    unsigned ctx_size = m68k_context_size();
    for (int i = 0; i < CPU_COUNT; i++) {
        cpus[i].ctx = calloc(1, ctx_size);
        if (!cpus[i].ctx) { fprintf(stderr, "context allocation failed\n"); return 1; }
        init_cpu(i);
    }

    const int cyc_per_frame = 12500000 / 60;
    for (int fr = 0; fr < frames; fr++) {
        (void)cyc_per_frame;
        update_frame_inputs(fr);
        execute_frame_all();
    }

    dump_summary(frames);
    if (snapshot_dir) write_snapshot(snapshot_dir);
    write_coverage();
    return 0;
}
