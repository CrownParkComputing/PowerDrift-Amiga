/*
 * pdrift_bview.c -- raw System16B secondary sprite debug renderer for Power Drift.
 *
 * Reads bsprites.bin plus a pdrift_host_probe snapshot and writes a 320x224 PPM.
 * This is a diagnostic view of the secondary sprite layer, not final Y-board
 * composition.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 224
#define BRAM_BYTES 0x1000
#define PAL_BYTES 0x4000
#define BROM_BYTES 0x80000

static uint8_t bram[BRAM_BYTES];
static uint8_t palram[PAL_BYTES];
static uint8_t *brom;
static uint32_t fb[W * H];
static unsigned long sprites_seen, sprites_drawn, pixels_drawn;

static void die_errno(const char *what, const char *path)
{
    fprintf(stderr, "%s %s: %s\n", what, path, strerror(errno));
    exit(1);
}

static void read_file(const char *path, void *dst, size_t bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) die_errno("open", path);
    size_t got = fread(dst, 1, bytes, f);
    if (ferror(f)) die_errno("read", path);
    fclose(f);
    if (got != bytes) {
        fprintf(stderr, "%s: got %zu bytes, expected %zu\n", path, got, bytes);
        exit(1);
    }
}

static uint8_t *read_alloc(const char *path, size_t bytes)
{
    uint8_t *p = malloc(bytes);
    if (!p) {
        fprintf(stderr, "out of memory for %s\n", path);
        exit(1);
    }
    read_file(path, p, bytes);
    return p;
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint16_t bword(unsigned index)
{
    index &= (BRAM_BYTES / 2 - 1);
    return be16(bram + index * 2);
}

static uint16_t brom_word(unsigned index)
{
    index &= (BROM_BYTES / 2 - 1);
    return be16(brom + index * 2);
}

static uint32_t pal_rgb(unsigned pen)
{
    unsigned idx = (pen & 0x1fff) % (PAL_BYTES / 2);
    uint16_t w = be16(palram + idx * 2);
    unsigned r5 = ((w >> 12) & 0x01) | ((w << 1) & 0x1e);
    unsigned g5 = ((w >> 13) & 0x01) | ((w >> 3) & 0x1e);
    unsigned b5 = ((w >> 14) & 0x01) | ((w >> 7) & 0x1e);
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g5 << 3) | (g5 >> 2);
    unsigned b = (b5 << 3) | (b5 >> 2);
    if ((r | g | b) == 0) {
        r = 32 + ((pen * 29) & 0xbf);
        g = 32 + ((pen * 61) & 0xbf);
        b = 32 + ((pen * 97) & 0xbf);
    }
    return (r << 16) | (g << 8) | b;
}

static void put_px(int x, int y, unsigned pen)
{
    unsigned pix = pen & 0xf;
    if (pix == 0 || pix == 15) return;
    if ((unsigned)x >= W || (unsigned)y >= H) return;
    fb[y * W + x] = pal_rgb(pen);
    pixels_drawn++;
}

static void render_all(void)
{
    const int origin_x = 184;
    for (unsigned e = 0; e < BRAM_BYTES / 16; e++) {
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

        sprites_seen++;
        if (hide || top >= bottom) continue;
        bank %= BROM_BYTES / 0x20000;
        sprites_drawn++;

        int sx0 = xpos - origin_x;
        unsigned yacc = d[5] & 0x03ff;
        for (int y = top; y < bottom; y++) {
            addr += pitch;
            yacc += (unsigned)vzoom << 10;
            if (yacc & 0x8000) {
                addr += pitch;
                yacc &= ~0x8000u;
            }
            if ((unsigned)y >= H) continue;

            int xacc = 4 * hzoom;
            unsigned a = 0x10000u * bank + (addr & 0xffffu);
            int x = sx0;
            if (!flip) {
                for (;;) {
                    unsigned pixels = brom_word(a++);
                    int pix = 0;
                    pix = (pixels >> 12) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >>  8) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >>  4) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >>  0) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    if (pix == 15 || x > sx0 + 0x200) break;
                }
            } else {
                for (;;) {
                    unsigned pixels = brom_word(a--);
                    int pix = 0;
                    pix = (pixels >>  0) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >>  4) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >>  8) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    pix = (pixels >> 12) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_px(x, y, colpri | pix); x++; }
                    if (pix == 15 || x > sx0 + 0x200) break;
                }
            }
        }
    }
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) die_errno("open", path);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (unsigned i = 0; i < W * H; i++) {
        uint8_t rgb[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        if (fwrite(rgb, 1, 3, f) != 3) die_errno("write", path);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s build/pdrift snapshot-dir out.ppm\n", argv[0]);
        return 1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/bsprites.bin", argv[1]);
    brom = read_alloc(path, BROM_BYTES);
    snprintf(path, sizeof path, "%s/bsprite_ram.bin", argv[2]);
    read_file(path, bram, sizeof bram);
    snprintf(path, sizeof path, "%s/palette_ram.bin", argv[2]);
    read_file(path, palram, sizeof palram);

    for (unsigned i = 0; i < W * H; i++) fb[i] = 0x101018;
    render_all();
    write_ppm(argv[3]);
    fprintf(stderr, "pdrift_bview: sprites seen=%lu drawn=%lu pixels=%lu -> %s\n",
        sprites_seen, sprites_drawn, pixels_drawn, argv[3]);
    return 0;
}
