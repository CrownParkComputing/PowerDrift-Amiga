# Power Drift — Amiga Native Port

Native Amiga port of Sega Power Drift (1988), Sega Y-Board arcade hardware.
Not an emulator — the game runs as native Amiga code.

## How This Works — Not an Emulator

The Sega Y-Board has three MC68000 CPUs. The Amiga has a 68000 (A500) or
68020+ (A1200). The game's machine code is already 68k — it runs directly on
the Amiga's own processor. No instruction translation, no virtual machine.

What this project reimplements is the Y-Board's **custom hardware** — the
chips around the CPUs that don't exist on Amiga:

- **Rotate/zoom sprite scaler** — the Y-Board's signature video chip that
  takes a sprite list and rotation RAM, then rasterises scaled/rotated
  sprites per scanline. Reimplemented in C as an integer-only DAC renderer.
- **System-16B sprite layer** — the secondary sprite/tile plane for HUD and
  course elements. Reimplemented in software.
- **YM2151 OPM** — the Y-Board's FM synthesis chip. Two interchangeable
  cores: a lean Jarek implementation (default, fast) and a full MAME-derived
  core (accurate). Mixed to Paula audio.
- **SegaPCM** — 16-channel PCM sample player for engine, crowd, and sound
  effects. Reimplemented in C, mixed into the Paula ring buffer.
- **Z80 sound CPU** — the Y-Board's sound driver CPU. Interpreted (Z80 is a
  different ISA, so this needs a software interpreter, but it's a tiny CPU
  running a fixed sound driver, not the game logic).

The three 68000 game CPUs run natively on the Amiga. Their code executes
directly — the Amiga's 68k IS the game's CPU. The custom video chips are
replaced by a software renderer painting to a Picasso96 RTG screen. The Z80
and YM2151/SegaPCM are interpreted to produce audio, mixed into a Paula ring
buffer at 22.05 kHz with wall-clock pacing.

The result: Power Drift running as a native Amiga application, not inside an
emulator sandbox. The game's 68k code executes on the Amiga's own 68k, with
the Y-Board's custom chips replaced by native Amiga equivalents.

## Game

- **Title:** Sega Power Drift (1988), MAME set `pdrift`
- **Original hardware:** Sega Y-Board — 3x MC68000 @ 12.5 MHz + Z80 sound +
  YM2151 (FM) + SegaPCM (samples), rotate/zoom sprite scaler + System-16B
  sprites, 320x224 @ 60 Hz
- **Target:** Amiga A1200 / 68030, RTG (Picasso96 / Amiberry uaegfx), 8-bit chunky
- **State:** Shipping — hardware-confirmed playable with sound, pad, and gears

## Architecture

```
Game ROMs (3x 68000 machine code)
    │
    ├─ run natively on Amiga 68k CPU ─────────────── no emulation
    │
    ├─ pdrift_machine.c  ─ Y-Board bus, 3x 68k contexts, I/O, interrupts
    ├─ pdrift_render.c   ─ rotate/zoom scaler + System16B sprites → RTG
    ├─ pdrift_audio      ─ Z80 interpreter → YM2151/SegaPCM → Paula ring
    ├─ amiga/            ─ 864x486 8-bit RTG screen, input, HUD, frameskip
    └─ pdrift_romdata.S  ─ embedded ROM blobs (.incbin) → single 8.4MB exe
```

| Layer | File(s) | Notes |
|-------|---------|-------|
| 3x 68000 machine | `pdrift_machine.c/.h` | Each CPU gets 12.5 MHz x 262 scanlines = 208,333 cycles/frame |
| Renderer | `pdrift_render.c/.h` | Y-board rotate/zoom sprite scaler, integer DAC, byte-identical to MAME reference |
| Sound board | `pdrift_audio*.c`, `pdrift_z80.c`, `pdrift_segapcm.c`, `pdrift_opm.c` + `opm/ym2151.c` | Z80 + lean Jarek YM2151 + SegaPCM → Paula ring |
| Amiga driver | `amiga/pdrift_live_main.c` | 864x486 8-bit RTG screen, fixed RGB332 CLUT, 2x centred present, wall-clock frameskip, input, HUD |
| Embedded ROMs | `pdrift_romdata.S` | ROMs `.incbin`'d → single self-contained ~8.4 MB exe |

## Controls

| Input | Action |
|-------|--------|
| D-pad / Left-Right | Steer |
| Up / Red / Fire | Gas |
| Down / Blue | Brake |
| L1 | Gear low |
| R1 | Gear high |
| Space | Toggle gears (keyboard) |
| Play / 1 | Start |
| Green-Yellow / 5 | Coin |
| F | Toggle FPS overlay |
| S | Cycle frameskip (1/2/3) |
| Esc | Quit |

## License

- Power Drift port code: MIT
- Musashi 68000 core: see source/cores/musashi/ for license
- ROM images are not included — supply your own legally obtained MAME `pdrift` set