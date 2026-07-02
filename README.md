# Power Drift - Amiga RTG Port

Native Amiga RTG port of Sega Power Drift (1988), Sega Y-Board arcade hardware.

## Overview

- **Game:** Sega Power Drift (1988), MAME set `pdrift`
- **Hardware emulated:** Sega Y-board — 3x MC68000 (main / sub-X / sub-Y) + Z80 sound + YM2151 (FM) + SegaPCM (samples), rotate/zoom sprite scaler + System-16B sprites, 320x224 @ 60 Hz
- **Target:** Amiga A1200 / 68030, RTG (Picasso96 / Amiberry uaegfx), 8-bit chunky
- **State:** Shipping as a self-contained standalone RTG port. Hardware-confirmed playable with sound, pad, and gears.

## Architecture

| Layer | File(s) | Notes |
|-------|---------|-------|
| 3x 68000 machine | `pdrift_machine.c/.h` | Musashi interpreter. Each CPU gets 12.5 MHz x 262 scanlines = 208,333 cycles/frame |
| Renderer | `pdrift_render.c/.h` | Y-board rotate/zoom sprite scaler, integer DAC, byte-identical to MAME reference |
| Sound board | `pdrift_audio*.c`, `pdrift_z80.c`, `pdrift_segapcm.c`, `pdrift_opm.c` + `opm/ym2151.c` | Z80 + lean Jarek YM2151 + SegaPCM -> Paula ring |
| Amiga driver | `amiga/pdrift_live_main.c` | 864x486 8-bit RTG screen, fixed RGB332 CLUT, 2x centred present, wall-clock frameskip, input, HUD |
| Embedded ROMs | `pdrift_romdata.S` | ROMs `.incbin`'d -> single self-contained ~8.4 MB exe |
| Build/package | `build_rtg_live.sh` | Builds exe + clones RTG boot template -> PowerDrift_RTG.hdf + PowerDrift-RTG.uae |

## Repository Structure

```
source/
├── build_rtg_live.sh          # Amiga RTG build + package script
├── build_host_live.sh          # Host-side build (for testing)
├── build_host_probe.sh         # Host probe build
├── build_rtg_diag.sh           # RTG diagnostic build
├── build_audiotest.sh          # Audio test build
├── pdrift_machine.c/.h         # 3x 68000 bus, I/O, interrupts, frame timing
├── pdrift_render.c/.h          # Y-board rotate/zoom renderer
├── pdrift_audio.c/.h           # Sound board logic
├── pdrift_audio_amiga.c        # Paula ring-buffer playback
├── pdrift_opm.c                # OPM (YM2151) interface
├── pdrift_mathstubs.c          # Math stubs
├── pdrift_segapcm.c/.h         # SegaPCM sample player
├── pdrift_z80.c                # Z80 sound CPU
├── z80.c / z80emu.h            # Z80 emulator
├── pdrift_romdata.S            # Embedded ROM blobs (.incbin)
├── tables.h                    # Z80 lookup tables
├── amiga/
│   ├── pdrift_live_main.c      # Amiga RTG live driver
│   └── pdrift_rtg_diag.c       # RTG diagnostic driver
├── cores/musashi/              # Musashi 68000 CPU emulator (vendored)
├── opm/                        # YM2151 OPM cores (Jarek + full)
└── tools/                      # Host-side dev/diagnostic tools
assets/
├── PowerDrift.info             # Amiga icon
└── powerdrift_loader.png        # Loader image
roms/                           # Place MAME pdrift.zip here for extraction
```

## Building

### Prerequisites

- `m68k-amigaos-gcc` (amiga-amigaos toolchain)
- `xdftool` (for HDF packaging)
- Python 3 (for ROM extraction)

### 1. Extract ROMs

Place MAME `pdrift.zip` in `roms/` and run:

```bash
cd source
python3 tools/pdrift_extract_roms.py ../roms/pdrift.zip build/pdrift
```

### 2. Build RTG port

```bash
cd source
./build_rtg_live.sh
```

Outputs in `dist/`:
- `Power Drift` — standalone exe
- `PowerDrift_RTG.hdf` — bootable RTG HDF
- `PowerDrift-RTG.uae` — Amiberry config

Also deploys to Amiberry if `~/Amiberry/HardDrives/` exists.

## Running

```bash
amiberry -f dist/PowerDrift-RTG.uae
```

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

## References

- Sega Y-Board hardware manual
- MAME `segaybd.cpp` / `sega16sp.cpp` / `segapcm.cpp`
- Musashi 68000 CPU emulator
- Jarek YM2151 emulator

## License

- Power Drift port code: MIT
- Musashi 68000 core: see source/cores/musashi/ for license
- ROM images are not included — you must supply your own legally obtained MAME `pdrift` set