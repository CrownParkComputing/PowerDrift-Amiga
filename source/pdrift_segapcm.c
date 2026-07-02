/* pdrift_segapcm.c -- integer SegaPCM (315-5218), MAME-faithful. */
#include "pdrift_segapcm.h"

static const uint8_t *pcm_rom;
static uint32_t pcm_mask;
static uint8_t ram[0x100];
static uint32_t low[16];      /* internal sub-sample low byte per channel */

void pdr_segapcm_init(const uint8_t *rom, uint32_t romsize)
{
    pcm_rom = rom;
    /* round romsize down to a power-of-two mask (2MB -> 0x1fffff) */
    uint32_t m = 1;
    while (m < romsize) m <<= 1;
    if (m > romsize) m >>= 1;
    pcm_mask = m - 1;
    pdr_segapcm_reset();
}

void pdr_segapcm_reset(void)
{
    for (int i = 0; i < 0x100; i++) ram[i] = 0;
    for (int i = 0; i < 16; i++) low[i] = 0;
    /* channels start disabled (control bit0 set) */
    for (int ch = 0; ch < 16; ch++) ram[8 * ch + 0x86] |= 1;
}

void pdr_segapcm_write(uint32_t addr, uint8_t v) { ram[addr & 0xff] = v; }
uint8_t pdr_segapcm_read(uint32_t addr) { return ram[addr & 0xff]; }

/* One native-rate output sample. Mirrors MAME's per-sample inner loop. */
void pdr_segapcm_render(int32_t *l, int32_t *r)
{
    int32_t ml = 0, mr = 0;
    for (int ch = 0; ch < 16; ch++) {
        uint8_t *regs = ram + 8 * ch;
        if (regs[0x86] & 1) continue;   /* channel disabled */

        uint32_t offset = (uint32_t)(regs[0x86] & PDR_SEGAPCM_BANKMASK) << PDR_SEGAPCM_BANKSHIFT;
        uint32_t addr = ((uint32_t)regs[0x85] << 16) | ((uint32_t)regs[0x84] << 8) | low[ch];
        uint32_t loop = ((uint32_t)regs[0x05] << 16) | ((uint32_t)regs[0x04] << 8);
        uint8_t end = regs[6] + 1;

        if ((addr >> 16) == end) {
            if (regs[0x86] & 2) { regs[0x86] |= 1; continue; }  /* loop disabled -> stop */
            addr = loop;
        }

        int32_t v = (int32_t)pcm_rom[(offset + (addr >> 8)) & pcm_mask] - 0x80;
        ml += v * (regs[2] & 0x7f);
        mr += v * (regs[3] & 0x7f);
        addr = (addr + regs[7]) & 0xffffff;

        regs[0x84] = (addr >> 8) & 0xff;
        regs[0x85] = (addr >> 16) & 0xff;
        low[ch] = (regs[0x86] & 1) ? 0 : (addr & 0xff);
    }
    *l += ml;
    *r += mr;
}
