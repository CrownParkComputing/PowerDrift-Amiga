/*
 * pdrift_yview.c -- raw Sega Y-board sprite-layer debug renderer.
 *
 * Reads a pdrift_host_probe snapshot plus ysprites.bin and writes a 512x512 PPM.
 * This intentionally renders the raw Y-board intermediate sprite bitmap. It does
 * not yet apply the segaic16 rotate_draw pass that maps this layer to 320x224.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 512
#define H 512
#define YRAM_BYTES 0x10000
#define PAL_BYTES 0x4000
#define YROM_BYTES 0x400000

static uint8_t yram[YRAM_BYTES];
static uint8_t palram[PAL_BYTES];
static uint8_t *yrom;
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

static uint16_t yword(unsigned index)
{
    index &= (YRAM_BYTES / 2 - 1);
    return be16(yram + index * 2);
}

static uint64_t yrom_word(unsigned bank, unsigned offs)
{
    unsigned banks = YROM_BYTES / 0x80000;
    unsigned byte = (bank % banks) * 0x80000u + (offs & 0xffffu) * 8u;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | yrom[byte + i];
    return v;
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
        r = 40 + ((pen * 37) & 0xbf);
        g = 40 + ((pen * 73) & 0xbf);
        b = 40 + ((pen * 17) & 0xbf);
    }
    return (r << 16) | (g << 8) | b;
}

static void put_px(int x, int y, unsigned pen)
{
    if ((unsigned)x >= W || (unsigned)y >= H) return;
    fb[y * W + x] = pal_rgb(pen);
    pixels_drawn++;
}

static void draw_pix_run(int *x, int y, int xdelta, int minx, int maxx, int *xacc, int zoom, unsigned pen)
{
    while (*xacc < 0x200) {
        if (*x >= minx && *x <= maxx) put_px(*x, y, pen);
        *x += xdelta;
        *xacc += zoom;
    }
    *xacc -= 0x200;
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

    sprites_seen++;
    if (hide || height == 0) return;
    if (zoom == 0) zoom = 1;
    sprites_drawn++;

    int ytarget = top + ydelta * height;
    int yacc = 0;
    for (int y = top; y != ytarget; y += ydelta) {
        if ((unsigned)y < H) {
            int x = xpos;
            int xacc = 0;
            int minx = 0;
            int maxx = W - 1;
            if (!flip) {
                uint16_t offs = addr - 1;
                while ((xdelta > 0 && x <= maxx) || (xdelta < 0 && x >= minx)) {
                    uint64_t pixels = yrom_word(bank, ++offs);
                    int last_pix = 0;
                    for (int sh = 60; sh >= 0; sh -= 4) {
                        unsigned pix = (pixels >> sh) & 0x0f;
                        unsigned ind = yword(indirect + pix);
                        last_pix = pix;
                        if (ind < 0x1fe) draw_pix_run(&x, y, xdelta, minx, maxx, &xacc, zoom, colpri | ind);
                    }
                    if (last_pix == 0x0f) break;
                }
            } else {
                uint16_t offs = addr + 1;
                while ((xdelta > 0 && x <= maxx) || (xdelta < 0 && x >= minx)) {
                    uint64_t pixels = yrom_word(bank, --offs);
                    int last_pix = 0;
                    for (int sh = 0; sh <= 60; sh += 4) {
                        unsigned pix = (pixels >> sh) & 0x0f;
                        unsigned ind = yword(indirect + pix);
                        last_pix = pix;
                        if (ind < 0x1fe) draw_pix_run(&x, y, xdelta, minx, maxx, &xacc, zoom, colpri | ind);
                    }
                    if (last_pix == 0x0f) break;
                }
            }
        }
        yacc += zoom;
        addr += pitch * (yacc >> 9);
        yacc &= 0x1ff;
    }
}

static void render_all(unsigned start)
{
    uint8_t visited[0x1000];
    memset(visited, 0, sizeof visited);
    for (unsigned next = start & 0x0fff; next < 0x1000 && !visited[next]; ) {
        uint16_t d0 = yword(next * 8);
        if (d0 & 0x8000) break;
        visited[next] = 1;
        render_sprite(next);
        next = yword(next * 8 + 7) & 0x0fff;
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
        fprintf(stderr, "usage: %s build/pdrift snapshot-dir out.ppm [start-entry]\n", argv[0]);
        return 1;
    }
    unsigned start = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 0;

    char path[1024];
    snprintf(path, sizeof path, "%s/ysprites.bin", argv[1]);
    yrom = read_alloc(path, YROM_BYTES);
    snprintf(path, sizeof path, "%s/ysprite_ram.bin", argv[2]);
    read_file(path, yram, sizeof yram);
    snprintf(path, sizeof path, "%s/palette_ram.bin", argv[2]);
    read_file(path, palram, sizeof palram);

    for (unsigned i = 0; i < W * H; i++) fb[i] = 0x101018;
    render_all(start);
    write_ppm(argv[3]);
    fprintf(stderr, "pdrift_yview: start=%03x sprites seen=%lu drawn=%lu pixels=%lu -> %s\n",
        start & 0x0fff, sprites_seen, sprites_drawn, pixels_drawn, argv[3]);
    return 0;
}
