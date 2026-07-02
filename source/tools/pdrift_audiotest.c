/* pdrift_audiotest.c -- host harness for the Power Drift sound board.
 *
 * Boots the machine (which writes sound commands), runs the Z80 sound board +
 * YM2151 + SegaPCM audio-clocked, and writes an 8-bit PCM WAV plus diagnostics.
 * The point is to confirm the sound path is non-silent and reacts to the game's
 * commands before wiring Paula. Compare loudness/spectrum against
 *   mame pdrift -wavwrite ref.wav
 * per the family rule (generate chips at native rate, then resample).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdrift_machine.h"
#include "pdrift_audio.h"

static void die(const char *w, const char *p) { fprintf(stderr, "%s %s: %s\n", w, p, strerror(errno)); exit(1); }

static void load_into(const char *dir, const char *name, uint8_t *dst, size_t want)
{
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) die("open", path);
    size_t got = fread(dst, 1, want, f); fclose(f);
    if (got != want) { fprintf(stderr, "%s: got %zu want %zu\n", path, got, want); exit(1); }
}
static uint8_t *load_alloc(const char *dir, const char *name, size_t want)
{
    uint8_t *p = malloc(want); if (!p) { fprintf(stderr, "oom %s\n", name); exit(1); }
    load_into(dir, name, p, want); return p;
}

static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/pdrift";
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *out = argc > 3 ? argv[3] : "build/pdrift_audio.wav";

    load_into(dir, "maincpu.bin", pdm_maincpu, PDM_MAINCPU_SIZE);
    load_into(dir, "subx.bin", pdm_subx, PDM_SUBX_SIZE);
    load_into(dir, "suby.bin", pdm_suby, PDM_SUBY_SIZE);
    uint8_t *soundcpu = load_alloc(dir, "soundcpu.bin", 0x10000);
    uint8_t *pcm = load_alloc(dir, "pcm.bin", 0x200000);

    pdrift_machine_default_inputs();
    pdrift_machine_init();
    pdr_audio_init(soundcpu, pcm, 0x200000);

    const int cyc_per_frame = PDR_Z80_CLOCK / 60;
    const int smp_per_frame = PDR_OUT_RATE / 60;
    int total_smp = frames * smp_per_frame;
    signed char *buf = malloc(total_smp);
    if (!buf) { fprintf(stderr, "oom wav\n"); return 1; }

    unsigned long cmds = 0;
    for (int fr = 0; fr < frames; fr++) {
        pdrift_machine_run_frame();
        uint8_t cmd;
        while (pdrift_machine_sound_latch_take(&cmd)) { pdr_audio_command(cmd); cmds++; }
        pdr_audio_run_cycles(cyc_per_frame);
        pdr_audio_render(buf + fr * smp_per_frame, smp_per_frame);
    }

    /* stats */
    long sum = 0, nz = 0; int pk = 0;
    for (int i = 0; i < total_smp; i++) { int a = buf[i] < 0 ? -buf[i] : buf[i]; sum += a; if (a) nz++; if (a > pk) pk = a; }
    fprintf(stderr, "pdrift_audiotest: frames=%d cmds=%lu nmis=%lu ym_writes=%lu pcm_writes=%lu\n",
            frames, cmds, pdr_audio_dbg_nmis(), pdr_audio_dbg_ym_writes(), pdr_audio_dbg_pcm_writes());
    fprintf(stderr, "  samples=%d nonzero=%ld peak=%d mean|amp|=%.2f out_peak=%d\n",
            total_smp, nz, pk, total_smp ? (double)sum / total_smp : 0.0, pdr_audio_dbg_out_peak());

    FILE *f = fopen(out, "wb"); if (!f) die("open", out);
    fputs("RIFF", f); put32(f, 36 + total_smp); fputs("WAVE", f);
    fputs("fmt ", f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, PDR_OUT_RATE); put32(f, PDR_OUT_RATE); put16(f, 1); put16(f, 8);
    fputs("data", f); put32(f, total_smp);
    for (int i = 0; i < total_smp; i++) fputc((uint8_t)(buf[i] + 128), f);
    fclose(f);
    fprintf(stderr, "  wrote %s (%d samples @ %d Hz)\n", out, total_smp, PDR_OUT_RATE);
    return 0;
}
