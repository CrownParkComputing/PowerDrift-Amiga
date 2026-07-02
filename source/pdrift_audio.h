/* pdrift_audio.h -- Power Drift Y-board sound subsystem API.
 *
 * Sound board: Z80 @ SOUND_CLOCK/8 + YM2151 (ymfm) + SegaPCM (315-5218).
 * The soundlatch (main CPU writes 0x082001) raises the Z80 NMI; the YM2151 timer
 * IRQ drives the Z80 INT. Audio-clocked: pdr_audio_run_cycles() is pulled from
 * the Paula refill so tempo is independent of video fps (family standard). */
#ifndef PDRIFT_AUDIO_H
#define PDRIFT_AUDIO_H

#include <stdint.h>

/* Sega Y-board sound clocks (segaybd.cpp: SOUND_CLOCK = 32'215'900). */
#define PDR_SOUND_CLOCK   32215900
#define PDR_Z80_CLOCK     (PDR_SOUND_CLOCK / 8)   /* 4,026,987 Hz */
#define PDR_YM2151_CLOCK  (PDR_SOUND_CLOCK / 8)   /* 4,026,987 Hz */
#define PDR_SEGAPCM_RATE  (PDR_SOUND_CLOCK / 8 / 128)  /* 31,461 Hz native */
#define PDR_OUT_RATE      22050                    /* Paula/host mix rate */

/* ROM blobs supplied by the caller before pdr_audio_init(). */
void pdr_audio_init(const uint8_t *soundcpu /*64K*/, const uint8_t *pcm, uint32_t pcm_size);
void pdr_audio_reset(void);

/* Main CPU wrote a sound command (soundlatch, 0x082001) -> data-pending NMI. */
void pdr_audio_command(uint8_t cmd);

/* Advance the sound board by N Z80 cycles (pull from the audio refill). */
void pdr_audio_run_cycles(int cycles);

/* Render n mono signed-8 samples at PDR_OUT_RATE (FM + SegaPCM mix). */
void pdr_audio_render(signed char *out, int n);

/* Amiga Paula lifecycle (pdrift_audio_amiga.c) */
void pdr_audio_amiga_open(void);
void pdr_audio_amiga_frame(void);
void pdr_audio_amiga_close(void);
/* Current EClock tick count for wall-clock pacing; *rate := ticks/sec (0 if the
 * timer isn't available, in which case the caller should fall back to 1x). */
unsigned long pdr_audio_eclock(unsigned long *rate);

/* diagnostics */
int  pdr_audio_dbg_booted(void);
unsigned long pdr_audio_dbg_nmis(void);
unsigned long pdr_audio_dbg_ym_writes(void);
unsigned long pdr_audio_dbg_pcm_writes(void);
int  pdr_audio_dbg_out_peak(void);

#endif
