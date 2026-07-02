/* pdrift_segapcm.h -- integer SegaPCM (315-5218) for the Y-board sound board.
 * 16 PCM channels, generated at the chip's native rate (clock/128). Float-free.
 * Modeled byte-for-byte on MAME segapcm_device::sound_stream_update. */
#ifndef PDRIFT_SEGAPCM_H
#define PDRIFT_SEGAPCM_H

#include <stdint.h>

/* Power Drift: SEGAPCM(config,...BANK_12M | BANK_MASKF8) -> shift 13, mask 0xf8 */
#define PDR_SEGAPCM_BANKSHIFT 13
#define PDR_SEGAPCM_BANKMASK  0xf8

void pdr_segapcm_init(const uint8_t *rom, uint32_t romsize);
void pdr_segapcm_reset(void);
void pdr_segapcm_write(uint32_t addr, uint8_t v);   /* 0x00-0xff */
uint8_t pdr_segapcm_read(uint32_t addr);
/* Advance one native (clock/128) sample; adds the 16-channel mix into *l,*r. */
void pdr_segapcm_render(int32_t *l, int32_t *r);

#endif
