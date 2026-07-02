/*
 * pdrift_frameview.c -- rotated Sega Y-board frame debug renderer.
 *
 * Reads a pdrift_host_probe snapshot plus ysprites.bin and writes a 320x224 PPM.
 * This applies the same rotate_draw transform shape MAME uses for Y-board.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SRC_W 512
#define SRC_H 512
#define OUT_W 320
#define OUT_H 224
#define YRAM_BYTES 0x10000
#define ROT_BYTES 0x0800
#define BRAM_BYTES 0x1000
#define PAL_BYTES 0x4000
#define YROM_BYTES 0x400000
#define YPIX_BYTES 0x800000
#define BROM_BYTES 0x80000

static uint8_t yram[YRAM_BYTES];
static uint8_t rotbuf[ROT_BYTES];
static uint8_t bram[BRAM_BYTES];
static uint8_t palram[PAL_BYTES];
static uint8_t *ypixrom;
static uint8_t *brom;
static uint16_t ypix[SRC_W * SRC_H];
static uint16_t bpix[OUT_W * OUT_H];
static uint8_t outpri[OUT_W * OUT_H];
static uint16_t outpen[OUT_W * OUT_H];
static uint32_t outfb[OUT_W * OUT_H];
/* 0x0000-0x1fff normal pens, 0x2000-0x3fff shadow/hilight variants */
static uint32_t palrgb[0x4000];
static unsigned long sprites_seen, sprites_drawn, sprite_pixels, rotate_pixels;
static unsigned long bsprites_seen, bsprites_drawn, bsprite_pixels, bsprite_overlay_pixels;
static unsigned long bsprite_shadow_pixels;

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

static uint16_t rword(unsigned index)
{
    index &= (ROT_BYTES / 2 - 1);
    return be16(rotbuf + index * 2);
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

static const uint8_t *yrom_pixels(unsigned bank, unsigned offs)
{
    unsigned banks = YROM_BYTES / 0x80000;
    unsigned byte = (bank % banks) * 0x100000u + (offs & 0xffffu) * 16u;
    return ypixrom + byte;
}

/*
 * Sega 16-bit palette DAC: five bits per gun through a 3.9K/2K/1K/500/250 ohm
 * resistor ladder, plus a 470 ohm tristate leg driven by palette RAM bit 15
 * (pull-down = shadow, pull-up = hilight). Matches MAME
 * sega_16bit_common_base::palette_init/paletteram_w.
 */
static void build_palette_cache(void)
{
    static const double res[5] = { 3900.0, 2000.0, 1000.0, 500.0, 250.0 };
    double g5 = 0.0;
    for (int i = 0; i < 5; i++) g5 += 1.0 / res[i];
    const double g470 = 1.0 / 470.0;
    const double gtot = g5 + g470;
    uint8_t normtab[32], shadtab[32], hitab[32];
    for (unsigned v = 0; v < 32; v++) {
        double gv = 0.0;
        for (int bit = 0; bit < 5; bit++)
            if (v & (1u << bit)) gv += 1.0 / res[bit];
        normtab[v] = (uint8_t)(255.0 * gv / g5 + 0.5);
        shadtab[v] = (uint8_t)(255.0 * gv / gtot + 0.5);
        hitab[v] = (uint8_t)(255.0 * (gv + g470) / gtot + 0.5);
    }
    for (unsigned pen = 0; pen < 0x2000; pen++) {
        unsigned idx = pen % (PAL_BYTES / 2);
        uint16_t w = be16(palram + idx * 2);
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

static void put_ypx(int x, int y, unsigned pen)
{
    if ((unsigned)x >= SRC_W || (unsigned)y >= SRC_H) return;
    ypix[y * SRC_W + x] = (uint16_t)pen;
    sprite_pixels++;
}

/* x/xacc advance for every source pixel; only ind < 0x1fe pixels are written */
static void draw_pix_run(int *x, int y, int xdelta, int minx, int maxx, int *xacc, int zoom, unsigned ind, unsigned colpri)
{
    while (*xacc < 0x200) {
        if (*x >= minx && *x <= maxx && ind < 0x1fe) put_ypx(*x, y, colpri | ind);
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
    uint16_t indtab[16];

    sprites_seen++;
    if (hide || height == 0) return;
    if (zoom == 0) zoom = 1;
    for (int i = 0; i < 16; i++) indtab[i] = yword(indirect + (unsigned)i);
    sprites_drawn++;

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
    memset(visited, 0, sizeof visited);
    for (unsigned i = 0; i < SRC_W * SRC_H; i++) {
        ypix[i] = 0xffff;
    }
    for (unsigned next = start & 0x0fff; next < 0x1000 && !visited[next]; ) {
        uint16_t d0 = yword(next * 8);
        if (d0 & 0x8000) break;
        visited[next] = 1;
        render_sprite(next);
        next = yword(next * 8 + 7) & 0x0fff;
    }
}

static void clear_stats(void)
{
    sprites_seen = 0;
    sprites_drawn = 0;
    sprite_pixels = 0;
    rotate_pixels = 0;
    bsprites_seen = 0;
    bsprites_drawn = 0;
    bsprite_pixels = 0;
    bsprite_overlay_pixels = 0;
    bsprite_shadow_pixels = 0;
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
        for (int x = 0; x < OUT_W; x++) {
            int sx = (tx >> 14) & 0x1ff;
            int sy = (ty >> 14) & 0x1ff;
            uint16_t pix = ypix[sy * SRC_W + sx];
            if (pix != 0xffff) {
                outpen[y * OUT_W + x] = (pix & 0x1ff) | ((pix >> 6) & 0x200) | ((pix >> 3) & 0xc00) | 0x1000;
                outpri[y * OUT_W + x] = (uint8_t)((pix >> 8) | 1);
                rotate_pixels++;
            } else {
                outpen[y * OUT_W + x] = (uint16_t)sy;
                outpri[y * OUT_W + x] = 0xff;
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
    bsprite_pixels++;
}

static void render_bsprites(void)
{
    const int origin_x = 184;
    for (unsigned i = 0; i < OUT_W * OUT_H; i++) bpix[i] = 0xffff;

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

        bsprites_seen++;
        if (hide || top >= bottom) continue;
        bank %= BROM_BYTES / 0x20000;
        bsprites_drawn++;

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
            unsigned a = 0x10000u * bank + (addr & 0xffffu);
            int x = sx0;
            if (!flip) {
                for (;;) {
                    unsigned pixels = brom_word(a++);
                    int pix = 0;
                    pix = (pixels >> 12) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  8) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  4) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    pix = (pixels >>  0) & 0xf; xacc = (xacc & 0x3f) + hzoom; if (xacc < 0x40) { put_bpx(x, y, colpri | pix); x++; }
                    if (pix == 15 || x > sx0 + 0x200) break;
                }
            } else {
                for (;;) {
                    unsigned pixels = brom_word(a--);
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
    for (unsigned i = 0; i < OUT_W * OUT_H; i++) {
        uint16_t pix = bpix[i];
        if (pix == 0xffff) continue;
        int priority = (pix >> 11) & 0x1e;
        if (priority < (outpri[i] & 0x1f)) {
            if ((pix & 0xf) == 0xe) {
                /* pen 0xe shadows the pixel underneath (MAME: dest += palette_entries) */
                outpen[i] += 0x2000;
                bsprite_shadow_pixels++;
            } else {
                outpen[i] = 0x800 | (pix & 0x7ff);
                bsprite_overlay_pixels++;
            }
        }
    }
}

static void resolve_pens(void)
{
    for (unsigned i = 0; i < OUT_W * OUT_H; i++)
        outfb[i] = palrgb[outpen[i] & 0x3fff];
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) die_errno("open", path);
    fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);
    for (unsigned i = 0; i < OUT_W * OUT_H; i++) {
        uint8_t rgb[3] = { (uint8_t)(outfb[i] >> 16), (uint8_t)(outfb[i] >> 8), (uint8_t)outfb[i] };
        if (fwrite(rgb, 1, 3, f) != 3) die_errno("write", path);
    }
    fclose(f);
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

static double elapsed_ms(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) * 1000.0 + (double)(b.tv_nsec - a.tv_nsec) / 1000000.0;
}

static void render_frame(unsigned start)
{
    clear_stats();
    render_yboard_sprites(start);
    rotate_draw();
    render_bsprites();
    overlay_bsprites();
    resolve_pens();
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s build/pdrift snapshot-dir out.ppm [start-entry]\n", argv[0]);
        return 1;
    }
    unsigned start = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 0;

    char path[1024];
    snprintf(path, sizeof path, "%s/ysprites_pix.bin", argv[1]);
    ypixrom = read_alloc(path, YPIX_BYTES);
    snprintf(path, sizeof path, "%s/bsprites.bin", argv[1]);
    brom = read_alloc(path, BROM_BYTES);
    snprintf(path, sizeof path, "%s/ysprite_ram.bin", argv[2]);
    read_file(path, yram, sizeof yram);
    snprintf(path, sizeof path, "%s/bsprite_ram.bin", argv[2]);
    read_file(path, bram, sizeof bram);
    snprintf(path, sizeof path, "%s/rotate_buffer.bin", argv[2]);
    read_file(path, rotbuf, sizeof rotbuf);
    snprintf(path, sizeof path, "%s/palette_ram.bin", argv[2]);
    read_file(path, palram, sizeof palram);
    build_palette_cache();

    int loops = env_int("PDRIFT_FRAMEVIEW_BENCH", 0);
    if (loops > 0) {
        struct timespec t0, t1;
        double y_ms = 0.0, rot_ms = 0.0, b_ms = 0.0, ov_ms = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < loops; i++) {
            struct timespec a, b;
            clear_stats();
            clock_gettime(CLOCK_MONOTONIC, &a);
            render_yboard_sprites(start);
            clock_gettime(CLOCK_MONOTONIC, &b);
            y_ms += elapsed_ms(a, b);
            clock_gettime(CLOCK_MONOTONIC, &a);
            rotate_draw();
            clock_gettime(CLOCK_MONOTONIC, &b);
            rot_ms += elapsed_ms(a, b);
            clock_gettime(CLOCK_MONOTONIC, &a);
            render_bsprites();
            clock_gettime(CLOCK_MONOTONIC, &b);
            b_ms += elapsed_ms(a, b);
            clock_gettime(CLOCK_MONOTONIC, &a);
            overlay_bsprites();
            resolve_pens();
            clock_gettime(CLOCK_MONOTONIC, &b);
            ov_ms += elapsed_ms(a, b);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = elapsed_ms(t0, t1);
        fprintf(stderr,
            "pdrift_frameview_bench: loops=%d total_ms=%.3f ms_per_frame=%.3f y seen=%lu drawn=%lu ypix=%lu rotate=%lu b seen=%lu drawn=%lu bpix=%lu overlay=%lu\n",
            loops, ms, ms / loops, sprites_seen, sprites_drawn, sprite_pixels, rotate_pixels,
            bsprites_seen, bsprites_drawn, bsprite_pixels, bsprite_overlay_pixels);
        fprintf(stderr,
            "  phase_ms_per_frame: ysprites=%.3f rotate=%.3f bsprites=%.3f overlay=%.3f\n",
            y_ms / loops, rot_ms / loops, b_ms / loops, ov_ms / loops);
        return 0;
    }

    render_frame(start);
    write_ppm(argv[3]);
    fprintf(stderr,
        "pdrift_frameview: start=%03x y seen=%lu drawn=%lu ypix=%lu rotate=%lu b seen=%lu drawn=%lu bpix=%lu overlay=%lu shadow=%lu -> %s\n",
        start & 0x0fff, sprites_seen, sprites_drawn, sprite_pixels, rotate_pixels,
        bsprites_seen, bsprites_drawn, bsprite_pixels, bsprite_overlay_pixels, bsprite_shadow_pixels, argv[3]);
    return 0;
}
