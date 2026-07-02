/*
 * pdrift_host_live.c -- host driver for the shared machine + render modules.
 *
 * Runs the live pipeline (pdrift_machine + pdrift_render, the same C the Amiga
 * runtime uses) for N frames and writes a PPM. This is the regression check that
 * the refactor stayed byte-faithful: its output must match the MAME-validated
 * reference frames produced by the old snapshot-then-frameview flow.
 *
 * Build: see build_host_live.sh
 * Run:   pdrift_host_live build/pdrift <frames> out.ppm
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdrift_machine.h"
#include "pdrift_render.h"

static void die_errno(const char *what, const char *path)
{
    fprintf(stderr, "%s %s: %s\n", what, path, strerror(errno));
    exit(1);
}

static void load_into(const char *dir, const char *name, uint8_t *dst, size_t want)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) die_errno("open", path);
    size_t got = fread(dst, 1, want, f);
    if (ferror(f)) die_errno("read", path);
    fclose(f);
    if (got != want) {
        fprintf(stderr, "%s: got %zu bytes, expected %zu\n", path, got, want);
        exit(1);
    }
}

static uint8_t *load_alloc(const char *dir, const char *name, size_t want)
{
    uint8_t *p = malloc(want);
    if (!p) { fprintf(stderr, "out of memory for %s\n", name); exit(1); }
    load_into(dir, name, p, want);
    return p;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/pdrift";
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    const char *out = argc > 3 ? argv[3] : "build/pdrift_host_live.ppm";
    if (frames < 1) frames = 1;

    load_into(dir, "maincpu.bin", pdm_maincpu, PDM_MAINCPU_SIZE);
    load_into(dir, "subx.bin", pdm_subx, PDM_SUBX_SIZE);
    load_into(dir, "suby.bin", pdm_suby, PDM_SUBY_SIZE);
    pdr_ypixrom = load_alloc(dir, "ysprites_pix.bin", PDR_YPIX_BYTES);
    pdr_brom = load_alloc(dir, "bsprites.bin", PDR_BROM_BYTES);

    pdrift_machine_default_inputs();
    pdrift_machine_init();
    for (int fr = 0; fr < frames; fr++)
        pdrift_machine_run_frame();

    pdrift_render_frame();
    static uint32_t rgb[PDR_W * PDR_H];
    pdrift_render_resolve_rgb(rgb);

    FILE *f = fopen(out, "wb");
    if (!f) die_errno("open", out);
    fprintf(f, "P6\n%d %d\n255\n", PDR_W, PDR_H);
    for (unsigned i = 0; i < PDR_W * PDR_H; i++) {
        uint8_t px[3] = { (uint8_t)(rgb[i] >> 16), (uint8_t)(rgb[i] >> 8), (uint8_t)rgb[i] };
        if (fwrite(px, 1, 3, f) != 3) die_errno("write", out);
    }
    fclose(f);
    fprintf(stderr, "pdrift_host_live: %d frames -> %s\n", frames, out);
    return 0;
}
