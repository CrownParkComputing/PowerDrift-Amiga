/*
 * pdrift_render.c -- shared Sega Y-board compositor.
 *
 * Extracted from tools/pdrift_frameview.c (the MAME-validated version) and
 * rewired to read the live machine RAM instead of snapshot files. The pixel
 * pipeline is byte-identical to the frameview: rotate-clipped Y-board sprites ->
 * rotate_draw into pen space -> 16B sprite overlay with pen-0xe shadow.
 */
#include <stdint.h>
#include "pdrift_machine.h"
#include "pdrift_render.h"

#define SRC_W PDR_SRC_W
#define SRC_H PDR_SRC_H
#define OUT_W PDR_W
#define OUT_H PDR_H
#define PAL_BYTES 0x4000
#define YROM_BYTES 0x400000
#define BROM_BYTES PDR_BROM_BYTES

const uint8_t *pdr_ypixrom;
const uint8_t *pdr_brom;
uint16_t pdr_outpen[OUT_W * OUT_H];

static uint16_t ypix[SRC_W * SRC_H];
static uint16_t bpix[OUT_W * OUT_H];
/* per-scanline flag: 1 if any 16B sprite pixel was written to that row this frame,
 * so the overlay pass skips the (majority) empty road rows instead of scanning all
 * 224. Reset each frame in render_bsprites, set in put_bpx. */
static uint8_t brow_used[OUT_H];
static uint8_t outpri[OUT_W * OUT_H];
/* 0x0000-0x1fff normal pens, 0x2000-0x3fff shadow/hilight variants */
static uint32_t palrgb[0x4000];

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static uint16_t yword(unsigned index)
{
    index &= (0x10000 / 2 - 1);
    return be16(pdm_ysprite_ram + index * 2);
}

static uint16_t rword(unsigned index)
{
    index &= (0x0800 / 2 - 1);
    return be16(pdm_rotate_buffer + index * 2);
}

static uint16_t bword(unsigned index)
{
    index &= (0x1000 / 2 - 1);
    return be16(pdm_bsprite_ram + index * 2);
}

static uint16_t brom_word(unsigned index)
{
    index &= (BROM_BYTES / 2 - 1);
    return be16(pdr_brom + index * 2);
}

static const uint8_t *yrom_pixels(unsigned bank, unsigned offs)
{
    unsigned banks = YROM_BYTES / 0x80000;
    unsigned byte = (bank % banks) * 0x100000u + (offs & 0xffffu) * 16u;
    return pdr_ypixrom + byte;
}

/*
 * Sega 16-bit palette DAC: five bits per gun through a 3.9K/2K/1K/500/250 ohm
 * resistor ladder, plus a 470 ohm tristate leg driven by palette RAM bit 15
 * (pull-down = shadow, pull-up = hilight). Matches MAME
 * sega_16bit_common_base::palette_init/paletteram_w.
 *
 * Float-free: conductances are held as integer nano-siemens so the whole port
 * stays soft-float free (no mathieeedoubbas / libgcc __divdf3 on the Amiga).
 */
static void build_palette_cache(void)
{
    /* 1/R in nano-siemens for R = 3900,2000,1000,500,250 ohm */
    static const int32_t gbit[5] = { 256410, 500000, 1000000, 2000000, 4000000 };
    static const int32_t g470 = 2127659;   /* 1/470 ohm */
    int32_t g5 = 0;
    for (int i = 0; i < 5; i++) g5 += gbit[i];
    int32_t gtot = g5 + g470;
    uint8_t normtab[32], shadtab[32], hitab[32];
    for (unsigned v = 0; v < 32; v++) {
        int32_t gv = 0;
        for (int bit = 0; bit < 5; bit++)
            if (v & (1u << bit)) gv += gbit[bit];
        normtab[v] = (uint8_t)(((int64_t)255 * gv + g5 / 2) / g5);
        shadtab[v] = (uint8_t)(((int64_t)255 * gv + gtot / 2) / gtot);
        hitab[v]   = (uint8_t)(((int64_t)255 * (gv + g470) + gtot / 2) / gtot);
    }
    for (unsigned pen = 0; pen < 0x2000; pen++) {
        unsigned idx = pen % (PAL_BYTES / 2);
        uint16_t w = be16(pdm_palette_ram + idx * 2);
        unsigned r5 = ((w >> 12) & 0x01) | ((w << 1) & 0x1e);
        unsigned g5v = ((w >> 13) & 0x01) | ((w >> 3) & 0x1e);
        unsigned b5 = ((w >> 14) & 0x01) | ((w >> 7) & 0x1e);
        palrgb[pen] = ((uint32_t)normtab[r5] << 16) | ((uint32_t)normtab[g5v] << 8) | normtab[b5];
        if (w & 0x8000)
            palrgb[0x2000 + pen] = ((uint32_t)hitab[r5] << 16) | ((uint32_t)hitab[g5v] << 8) | hitab[b5];
        else
            palrgb[0x2000 + pen] = ((uint32_t)shadtab[r5] << 16) | ((uint32_t)shadtab[g5v] << 8) | shadtab[b5];
    }
}

/* Emit one source pixel's zoom-expanded run -- hot loop of the Y-board scaler.
 * `ind`/`colpri`/`y` are invariant per call, so hoist the transparency test,
 * precompute the pen + scanline pointer, and (x is pre-clamped to [minx,maxx] ⊆
 * [0,SRC_W-1] and y to [0,SRC_H) by the caller) write straight into the row with
 * no per-pixel bounds check. Transparent pixels (ind>=0x1fe) just advance (no
 * DIVU -- most runs are 1-2 pixels, where a divide is far slower on the 030). */
static void draw_pix_run(int *x, int y, int xdelta, int minx, int maxx, int *xacc, int zoom, unsigned ind, unsigned colpri)
{
    int xx = *x, acc = *xacc;
    if (ind < 0x1fe) {
        uint16_t pen = (uint16_t)(colpri | ind);
        uint16_t *row = ypix + (unsigned)y * SRC_W;
        while (acc < 0x200) {
            if (xx >= minx && xx <= maxx) row[xx] = pen;
            xx += xdelta;
            acc += zoom;
        }
    } else {
        while (acc < 0x200) { xx += xdelta; acc += zoom; }
    }
    *x = xx;
    *xacc = acc - 0x200;
}

static void render_sprite(unsigned entry)
{
    unsigned base = entry * 8;
    uint16_t d0 = yword(base + 0);
    uint16_t d1 = yword(base + 1);
    uint16_t d2 = yword(base + 2);
    uint16_t d3 = yword(base + 3);
    uint16_t d4 = yword(base + 4);
    uint16_t d5 = yword(base + 5);
    uint16_t d6 = yword(base + 6);
    int hide = d0 & 0x5000;
    unsigned indirect = (d0 & 0x07ff) << 4;
    unsigned bank = ((d1 >> 8) & 0x10) | ((d2 >> 12) & 0x0f);
    int xpos = (d1 & 0x0fff) - 0x600;
    int top = (d2 & 0x0fff) - 0x600;
    uint16_t addr = d3;
    int height = d4;
    int ydelta = (d5 & 0x4000) ? 1 : -1;
    int flip = (~d5 >> 13) & 1;
    int xdelta = (d5 & 0x1000) ? 1 : -1;
    int zoom = d5 & 0x07ff;
    unsigned colpri = (d6 << 1) & 0xfe00;
    int pitch = (int8_t)d6;
    uint16_t indtab[16];

    if (hide || height == 0) return;
    if (zoom == 0) zoom = 1;
    for (int i = 0; i < 16; i++) indtab[i] = yword(indirect + (unsigned)i);

    int ytarget = top + ydelta * height;
    int yacc = 0;
    for (int y = top; y != ytarget; y += ydelta) {
        if ((unsigned)y < SRC_H) {
            /*
             * The hardware clips every sprite scanline against per-line-pair
             * extents in rotation RAM (words 0x000-0x1ff, coordinates in the
             * same 0x600-based space as sprite X). Bit 0x8000 marks a line
             * above the screen, bit 0x4000 below it.
             */
            unsigned exlo = rword((unsigned)y & ~1u);
            unsigned exhi = rword((unsigned)y | 1u);
            if ((exlo & 0x8000) && ydelta < 0) break;
            if ((exlo & 0x4000) && ydelta > 0) break;
            if (exlo & 0xc000) goto next_line;
            int x = xpos;
            int xacc = 0;
            int minx = (int)exlo - 0x600;
            int maxx = (int)exhi - 0x600;
            if (minx < 0) minx = 0;
            if (maxx > SRC_W - 1) maxx = SRC_W - 1;
            if (!flip) {
                uint16_t offs = addr - 1;
                while ((xdelta > 0 && x <= maxx) || (xdelta < 0 && x >= minx)) {
                    const uint8_t *pixels = yrom_pixels(bank, ++offs);
                    int last_pix = 0;
                    for (int i = 0; i < 16; i++) {
                        unsigned pix = pixels[i];
                        last_pix = pix;
                        draw_pix_run(&x, y, xdelta, minx, maxx, &xacc, zoom, indtab[pix], colpri);
                    }
                    if (last_pix == 0x0f) break;
                }
            } else {
                uint16_t offs = addr + 1;
                while ((xdelta > 0 && x <= maxx) || (xdelta < 0 && x >= minx)) {
                    const uint8_t *pixels = yrom_pixels(bank, --offs);
                    int last_pix = 0;
                    for (int i = 15; i >= 0; i--) {
                        unsigned pix = pixels[i];
                        last_pix = pix;
                        draw_pix_run(&x, y, xdelta, minx, maxx, &xacc, zoom, indtab[pix], colpri);
                    }
                    if (last_pix == 0x0f) break;
                }
            }
        }
next_line:
        yacc += zoom;
        addr += pitch * (yacc >> 9);
        yacc &= 0x1ff;
    }
}

static void render_yboard_sprites(unsigned start)
{
    uint8_t visited[0x1000];
    for (unsigned i = 0; i < sizeof visited; i++) visited[i] = 0;
    for (unsigned i = 0; i < SRC_W * SRC_H; i++) ypix[i] = 0xffff;
    for (unsigned next = start & 0x0fff; next < 0x1000 && !visited[next]; ) {
        uint16_t d0 = yword(next * 8);
        if (d0 & 0x8000) break;
        visited[next] = 1;
        render_sprite(next);
        next = yword(next * 8 + 7) & 0x0fff;
    }
}

static int32_t rot_s32(unsigned word_index)
{
    uint32_t raw = ((uint32_t)rword(word_index) << 16) | rword(word_index + 1);
    return (int32_t)raw;
}

static void rotate_draw(void)
{
    int32_t currx = rot_s32(0x3f0);
    int32_t curry = rot_s32(0x3f2);
    int32_t dyy = rot_s32(0x3f4);
    int32_t dxx = rot_s32(0x3f6);
    int32_t dxy = rot_s32(0x3f8);
    int32_t dyx = rot_s32(0x3fa);

    currx += dxx * 27;
    curry += dyx * 27;

    for (int y = 0; y < OUT_H; y++) {
        int32_t tx = currx;
        int32_t ty = curry;
        uint16_t *orow = pdr_outpen + (unsigned)y * OUT_W;   /* hoist row base out of the */
        uint8_t  *prow = outpri + (unsigned)y * OUT_W;       /* per-pixel address math    */
        for (int x = 0; x < OUT_W; x++) {
            int sy = (ty >> 14) & 0x1ff;
            uint16_t pix = ypix[sy * SRC_W + ((tx >> 14) & 0x1ff)];
            if (pix != 0xffff) {
                orow[x] = (pix & 0x1ff) | ((pix >> 6) & 0x200) | ((pix >> 3) & 0xc00) | 0x1000;
                prow[x] = (uint8_t)((pix >> 8) | 1);
            } else {
                orow[x] = (uint16_t)sy;
                prow[x] = 0xff;
            }
            tx += dxx;
            ty += dyx;
        }
        currx += dxy;
        curry += dyy;
    }
}

static void put_bpx(int x, int y, unsigned pen)
{
    unsigned pix = pen & 0xf;
    if (pix == 0 || pix == 15) return;
    if ((unsigned)x >= OUT_W || (unsigned)y >= OUT_H) return;
    bpix[y * OUT_W + x] = (uint16_t)pen;
    brow_used[y] = 1;
}

static void render_bsprites(void)
{
    const int origin_x = 184;
    for (unsigned i = 0; i < OUT_W * OUT_H; i++) bpix[i] = 0xffff;
    for (int y = 0; y < OUT_H; y++) brow_used[y] = 0;

    for (unsigned e = 0; e < 0x1000 / 16; e++) {
        uint16_t d[8];
        for (int i = 0; i < 8; i++) d[i] = bword(e * 8 + i);
        if (d[2] & 0x8000) break;

        int bottom = d[0] >> 8;
        int top = d[0] & 0xff;
        int xpos = d[1] & 0x1ff;
        int hide = d[2] & 0x4000;
        int flip = d[2] & 0x100;
        int pitch = (int8_t)(d[2] & 0xff);
        unsigned addr = d[3];
        int bank = (d[4] >> 8) & 0xf;
        int colpri = ((d[4] & 0xff) << 4) | (((d[1] >> 9) & 0xf) << 12);
        int vzoom = (d[5] >> 5) & 0x1f;
        int hzoom = d[5] & 0x1f;

        if (hide || top >= bottom) continue;
        bank %= BROM_BYTES / 0x20000;

        int sx0 = xpos - origin_x;
        unsigned yacc = d[5] & 0x03ff;
        for (int y = top; y < bottom; y++) {
            addr += pitch;
            yacc += (unsigned)vzoom << 10;
            if (yacc & 0x8000) {
                addr += pitch;
                yacc &= ~0x8000u;
            }
            if ((unsigned)y >= OUT_H) continue;

            int xacc = 4 * hzoom;
            /* The ROM address wraps WITHIN the 0x10000-word bank (MAME advances a
             * uint16_t index), so keep base+offset separate and mask the offset --
             * a flat `a++` on (bank<<16 | addr) would spill a long/bank-edge sprite
             * into the next bank and corrupt/blank it. */
            unsigned base = 0x10000u * bank;
            unsigned off = addr & 0xffffu;
            int x = sx0;
            if (!flip) {
                for (;;) {
                    unsigned pixels = brom_word(base + (off++ & 0xffffu));
                    int pix = 0;
                    pix = (pixels >> 12) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  8) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  4) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  0) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    if (pix == 15 || x > sx0 + 0x200) break;
                }
            } else {
                for (;;) {
                    unsigned pixels = brom_word(base + (off-- & 0xffffu));
                    int pix = 0;
                    pix = (pixels >>  0) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  4) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  8) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >> 12) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    if (pix == 15 || x > sx0 + 0x200) break;
                }
            }
        }
    }
}

static void overlay_bsprites(void)
{
    for (int y = 0; y < OUT_H; y++) {
        if (!brow_used[y]) continue;                 /* no 16B sprite on this row */
        unsigned base = (unsigned)y * OUT_W;
        for (unsigned i = base; i < base + OUT_W; i++) {
            uint16_t pix = bpix[i];
            if (pix == 0xffff) continue;
            int priority = (pix >> 11) & 0x1e;
            if (priority < (outpri[i] & 0x1f)) {
                if ((pix & 0xf) == 0xe) pdr_outpen[i] += 0x2000;   /* shadow */
                else pdr_outpen[i] = 0x800 | (pix & 0x7ff);
            }
        }
    }
}

void pdrift_render_frame(void)
{
    /* The palette RGB cache only changes when a CPU writes palette RAM
     * (pdm_palette_dirty). In steady-state gameplay the palette is static, so this
     * skips the 8192-pen DAC recompute most frames. build_pen8() clears the flag. */
    if (pdm_palette_dirty) build_palette_cache();
    render_yboard_sprites(0);
    rotate_draw();
    render_bsprites();
    overlay_bsprites();
}

void pdrift_render_resolve_rgb(uint32_t *out)
{
    for (unsigned i = 0; i < OUT_W * OUT_H; i++)
        out[i] = palrgb[pdr_outpen[i] & 0x3fff];
}

uint32_t pdrift_render_build_pen8(uint8_t pen8[0x4000])
{
    /* Skip the 16384-pen RGB332 remap when the palette hasn't changed (pen8 stays
     * valid), and clear pdm_palette_dirty so the next frame's palette cache is
     * skipped too. The host validator never calls this, so the flag stays set and
     * the palette cache rebuilds every frame -> output stays byte-identical. */
    if (!pdm_palette_dirty) return 0;
    for (unsigned pen = 0; pen < 0x4000; pen++) {
        uint32_t rgb = palrgb[pen];
        unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
        pen8[pen] = (uint8_t)((r & 0xe0) | ((g & 0xe0) >> 3) | (b >> 6));
    }
    pdm_palette_dirty = 0;
    return 0;
}
