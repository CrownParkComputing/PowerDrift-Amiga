# Power Drift Source Scaffold

This directory is the start of the Sega Y-board Power Drift port.

The first working target is not an Amiga executable. It is a host-side asset and
machine bring-up path:

1. Extract and CRC-check the exact MAME `pdrift` ROM set.
2. Build flat ROM regions matching MAME's `segaybd.cpp` layout.
3. Bring up three 68000 interpreters against the documented address maps.
4. Capture active Y-board sprite RAM and rotation RAM from attract mode.
5. Build a debug Y-board sprite/rotation viewer before RTG packaging.

Useful reference files in local MAME:

- `/home/jon/AndroidStudioProjects/AndroidMame/src/mame/sega/segaybd.cpp`
- `/home/jon/AndroidStudioProjects/AndroidMame/src/mame/sega/sega16sp.cpp`
- `/home/jon/AndroidStudioProjects/AndroidMame/src/devices/sound/segapcm.cpp`

## Address Map Summary

All three 68000s are clocked at 12.5 MHz and share `0x0c0000-0x0cffff`.

Main CPU:

- `0x000000-0x07ffff` main ROM
- `0x080000-0x080007` 315-5248 multiplier
- `0x082001` sound latch
- `0x084000-0x08401f` 315-5249 divider
- `0x0c0000-0x0cffff` shared RAM
- `0x100000-0x10001f` 315-5296 I/O
- `0x100040-0x100047` MSM6253 ADC
- `0x1f0000-0x1fffff` work RAM

Sub-X CPU:

- `0x000000-0x03ffff` sub-X ROM
- `0x080000-0x080007` multiplier
- `0x084000-0x08401f` divider
- `0x0c0000-0x0cffff` shared RAM
- `0x180000-0x18ffff` Y-board sprite RAM
- `0x1f8000-0x1fbfff` work RAM
- `0x1fc000-0x1fffff` backup RAM

Sub-Y CPU:

- `0x000000-0x03ffff` sub-Y ROM
- `0x080000-0x080007` multiplier
- `0x084000-0x08401f` divider
- `0x0c0000-0x0cffff` shared RAM
- `0x180000-0x1807ff` rotation RAM mirror
- `0x188000-0x188fff` System16B sprite RAM mirror
- `0x190000-0x193fff` palette RAM mirror
- `0x198000-0x19ffff` rotation control reads
- `0x1f0000-0x1fffff` work RAM

Sound CPU:

- `0x0000-0xefff` Z80 ROM
- `0xf000-0xf0ff` SegaPCM registers, mirrored through `0xf7ff`
- `0xf800-0xffff` Z80 RAM

## Current Host Status

Commands:

```sh
python3 tools/pdrift_extract_roms.py /home/jon/Downloads/pdrift.zip pdrift
python3 tools/pdrift_extract_roms.py /home/jon/Downloads/pdrift.zip pdrifte
python3 tools/pdrift_extract_roms.py /home/jon/Downloads/pdrift.zip pdriftj
bash build_host_probe.sh
/tmp/pdrift_host_probe build/pdrift 180
PDRIFT_COIN_FRAME=120 PDRIFT_START_FRAME=240 /tmp/pdrift_host_probe build/pdrift 1500 build/snap1500_lockstep_play_parent
/tmp/pdrift_bview build/pdrift build/snap1500_lockstep_play_parent build/pdrift_braw_1500_lockstep_play_parent.ppm
/tmp/pdrift_yview build/pdrift build/snap1500_lockstep_play_parent build/pdrift_yraw_1500_lockstep_0.ppm
/tmp/pdrift_frameview build/pdrift build/snap1500_lockstep_play_parent build/pdrift_frame_1500_composite.ppm
bash build_rtg_diag.sh
amiberry -f ../dist/PowerDrift-RTG-Diag.uae
```

Current result:

- `pdrift.zip` found at `/home/jon/Downloads/pdrift.zip`.
- All ROMs CRC-check against MAME `pdrift`; merged-zip clone extraction also
  works for `pdrifta`, `pdrifte`, `pdriftj`, and `pdriftjb`.
- Flat blobs are generated under `source/build/<set>/`.
- The three 68000s boot and run for 1500+ frames in the host probe.
- IRQ2 scanline timing and IRQ4 vblank timing are both modeled enough to run.
- The host scheduler now advances the main, sub-X, and sub-Y CPUs in matching
  scanline phases instead of full-frame CPU batches.
- The Sega 315-5248 multiplier and 315-5249 divider are modeled per CPU.
- No unmapped 68000 reads/writes are seen in the current stub map.
- Edge-based coin/start pulses are available with `PDRIFT_COIN_FRAME` and
  `PDRIFT_START_FRAME`.
- Sub-X writes Y-board sprite RAM.
- Sub-Y writes rotation RAM, secondary sprite RAM, and palette RAM.
- The host probe can dump live RAM snapshots.
- Rotation-control reads swap rotation RAM into `rotate_buffer.bin`, matching
  MAME's Y-board rotate buffering side effect.
- `pdrift_bview` renders the secondary System16B sprite layer from a snapshot.
- `pdrift_yview` traverses the Y-board linked list from entry 0 and renders the
  raw pre-rotation Y-board sprite bitmap.
- `pdrift_frameview` applies the Y-board rotate matrix and overlays secondary
  System16B sprites with a MAME-shaped priority test.
- `build_rtg_diag.sh` builds a bootable Picasso96/RTG HDF that animates four
  host-composited snapshots on an Amiga RTG screen. The app logs progress to
  `SYS:pdrift_diag.log`; the current validated boot reaches
  `enter animation loop`.

Observed 1500-frame probe summary with timed coin/start pulses:

```text
main  PC=036b18 IRQ2=1500/1495 IRQ4=1500/1497 unmapped r/w=0/0
subx  PC=00dd16 IRQ2=1495/1494 IRQ4=1495/1494 unmapped r/w=0/0
suby  PC=0129dc IRQ2=1495/1490 IRQ4=1495/1494 unmapped r/w=0/0
spin_skips main/subx/suby=2285/2267/2966
shared_w=2960105 ysprite_w=1652039 rotate_w=391428 bsprite_w=573184 palette_w=22560
```

Speed notes from the current host probe:

- Baseline 1500-frame run before wait yielding: `1.388s` host wall time.
- Phase-start semaphore skip only: `1.237s`.
- Instruction-hook wait yield plus the Sub-Y `011c64/011c6c` semaphore:
  `1.092s`, with byte-identical frame output at frame 1500.
- The host scheduler exposes `PDRIFT_WAIT_HOOK` and `PDRIFT_SLICE_CYCLES` for
  quick timing experiments; current best/default is hook enabled at 20000-cycle
  slices.
- The scheduler recognizes these no-IRQ wait loops:
  - main `033084/03308a`: waits on shared RAM `0cff0e` bit 7 clear.
  - sub-X `00db90`: waits on shared RAM `0cff0c` non-zero.
  - sub-Y `01188e`: waits on Sub-Y RAM `1ff9f0` non-zero.
  - sub-Y `011c64/011c6c`: waits on shared RAM `0cff12` bit 0 set.
- After those waits are removed from the hot profile, the main remaining cost is
  real object/sprite prep. Current top blocks are main `038842/038844`, Sub-X
  `00dd00-00e0e6`, and Sub-Y `011b28/011b30` plus palette/rotation prep spans.
  These are the first native/transcode candidates.

The secondary sprite debug render produces coherent HUD/course-select elements
and proves the `bsprites.bin` layout plus System16B sprite draw path are
basically sane. The raw Y-board debug render currently reports:

```text
pdrift_yview: start=000 sprites seen=188 drawn=187 pixels=244871
```

The composite debug renderer currently reports:

```text
pdrift_frameview: start=000 y seen=188 drawn=187 ypix=244871 rotate=59961 b seen=256 drawn=30 bpix=14965 overlay=11054
```

This produces a coherent 320x224 frame with rotated Y-board car/road/background
plus B-sprite HUD/course elements. The current RTG diagnostic proves the Amiga
boot, Picasso96 screen, palette, and chunky blit path. The next practical step
is a live RTG prototype that links the cross-compiled Musashi 68000 core, loads
the ROM blobs from the HDF, and renders frames in-process instead of embedding
host-generated snapshots.
