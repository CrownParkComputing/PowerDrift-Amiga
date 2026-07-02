/* pdrift_romdata.S -- embedded Power Drift ROM blobs for the self-contained
 * Amiga build (GNU as .incbin). Paths are relative to the build cwd (source/).
 * Leading-underscore symbols for the bebbo m68k-amigaos ABI. Mirrors Saint
 * Dragon st_romdata.S. This lets the runtime drop into AGS SHARED as a single
 * binary like every other port, with no companion ROM files. */
	.section .rodata
	.balign 4
	.global _pdr_rom_maincpu
_pdr_rom_maincpu:      .incbin "build/pdrift/maincpu.bin"       /* main 68000 (512KB) */
	.balign 4
	.global _pdr_rom_subx
_pdr_rom_subx:         .incbin "build/pdrift/subx.bin"          /* sub-X 68000 (256KB) */
	.balign 4
	.global _pdr_rom_suby
_pdr_rom_suby:         .incbin "build/pdrift/suby.bin"          /* sub-Y 68000 (256KB) */
	.balign 4
	.global _pdr_rom_bsprites
_pdr_rom_bsprites:     .incbin "build/pdrift/bsprites.bin"      /* 16B sprite ROM (512KB) */
	.balign 4
	/* RAW Y-board sprite ROM (4MB); expanded to 8MB nibbles at load time. Embedding
	 * the raw form (not the 8MB pre-expanded ysprites_pix) keeps the exe small enough
	 * to fit the boot HDF alongside the sound ROMs. */
	.global _pdr_rom_ysprites_raw
_pdr_rom_ysprites_raw: .incbin "build/pdrift/ysprites.bin"      /* raw Y-board sprite ROM (4MB) */
	.balign 4
	.global _pdr_rom_soundcpu
_pdr_rom_soundcpu:     .incbin "build/pdrift/soundcpu.bin"      /* Z80 sound program (64KB) */
	.balign 4
	.global _pdr_rom_pcm
_pdr_rom_pcm:          .incbin "build/pdrift/pcm.bin"           /* SegaPCM samples (2MB) */
	.balign 4
