/*
 * pdrift_render.h -- shared Sega Y-board compositor (MAME-validated).
 *
 * Reads the live machine RAM (pdrift_machine.h) plus the sprite ROM blobs and
 * produces a 320x224 buffer of arcade pen indices. The Amiga runtime maps those
 * pens to a fixed RGB332 CLUT via build_pen8(); the host validator resolves them
 * to RGB for PPM comparison against the MAME oracle.
 */
#ifndef PDRIFT_RENDER_H
#define PDRIFT_RENDER_H

#include <stdint.h>

#define PDR_W 320
#define PDR_H 224
#define PDR_SRC_W 512
#define PDR_SRC_H 512

/* Sprite ROM blobs: the caller points these at loaded data before rendering. */
#define PDR_YPIX_BYTES 0x800000
#define PDR_BROM_BYTES 0x80000
extern const uint8_t *pdr_ypixrom;   /* PDR_YPIX_BYTES: pre-expanded Y-board nibbles */
extern const uint8_t *pdr_brom;      /* PDR_BROM_BYTES: 16B sprite ROM */

/* Arcade pen output: 0x0000-0x1fff normal, +0x2000 = shadow bank. */
extern uint16_t pdr_outpen[PDR_W * PDR_H];

/* Composite one frame from the current machine RAM into pdr_outpen. */
void pdrift_render_frame(void);

/* Resolve pdr_outpen to 0x00RRGGBB (host PPM path). */
void pdrift_render_resolve_rgb(uint32_t *out);

/*
 * Fill pen8[0x4000] with an arcade-pen -> RGB332 mapping from the current
 * palette. Returns a checksum of palette RAM so the caller can skip the rebuild
 * when it has not changed since last frame.
 */
uint32_t pdrift_render_build_pen8(uint8_t pen8[0x4000]);

#endif
