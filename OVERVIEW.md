# Power Drift — Amiga RTG Port: Status & Overview

**Game:** Sega Power Drift (1988), MAME set `pdrift`
**Hardware emulated:** Sega Y-board — 3× MC68000 (main / sub-X / sub-Y) + Z80 sound
+ YM2151 (FM) + SegaPCM (samples), rotate/zoom sprite scaler + System-16B sprites,
320×224 @ 60 Hz.
**Target:** Amiga A1200 / 68030, RTG (Picasso96 / Amiberry uaegfx), 8-bit chunky.
**State:** **Shipping** as a self-contained standalone RTG port. Hardware-confirmed
playable with sound, pad, and gears. A few known polish items remain (below).

---

## How it's built (architecture)

| Layer | File(s) | Notes |
|-------|---------|-------|
| 3× 68000 machine | `pdrift_machine.c/.h` | **Musashi** interpreter (MAME's core). Byte-identical host↔Amiga. Each CPU gets the correct 12.5 MHz × 262 scanlines = 208,333 cycles/frame. |
| Renderer / compositor | `pdrift_render.c/.h` | Y-board rotate/zoom sprite scaler → `rotate_draw` → 16B sprite overlay. Float-free integer DAC. Byte-identical to the MAME-validated reference. |
| Sound board | `pdrift_audio*.c`, `pdrift_z80.c`, `pdrift_segapcm.c`, `pdrift_opm.c` + `opm/ym2151.c` | Z80 + **lean Jarek YM2151** + SegaPCM → Paula ring (wall-clock/EClock paced). |
| Amiga driver | `amiga/pdrift_live_main.c` | 864×486 8-bit RTG screen, fixed RGB332 CLUT + per-frame pen8 LUT, 2× centred present, wall-clock frameskip, input, HUD. |
| Embedded ROMs | `pdrift_romdata.S` | ROMs `.incbin`'d → single self-contained 8.4 MB exe (ysprites expanded to 8 MB at load). |
| Build/package | `build_rtg_live.sh` | Builds exe + clones `RTG_boot_template.hdf` → `PowerDrift_RTG.hdf` + `PowerDrift-RTG.uae`, deploys to Amiberry. |

**Launch:** Amiberry config **`PowerDrift-RTG`** → boots `PowerDrift_RTG.hdf` straight
into the game (minimal Workbench-less boot) → ESC exits via `UAEquit`.

**Controls:** steer = D-pad/←→ · gas = up/Red/fire · brake = down/Blue ·
**gears R1=high / L1=low** (SPACE toggles on keyboard) · start = Play/1 · coin =
Green-Yellow/5 · **F** = toggle FPS overlay · **S** = cycle frameskip (1/2/3).

---

## What we did this session

### Packaging → canonical standalone
- Moved **off the shared WhittyArcade launcher** to the collection's canonical
  per-game pattern: own bootable HDF + own Amiberry config, like `SideArms_RTG`.
- Base HDF switched to the clean **`RTG_boot_template.hdf`** (no PicassoIV, no
  tainted lineage); renamed to `PowerDrift_RTG.hdf` / `PowerDrift-RTG.uae`; config
  fixed to **864×486**; icon on the HDF; `UAEquit` clean exit.
- Removed all WhittyArcade/AGS coupling (launcher entries, the Chase-HQ `stage_game`
  line, AGS exes, stale `PowerDrift-Live*` configs).

### Renderer optimizations (all byte-identical to MAME reference)
- **Y-board sprite scaler**: hoisted the invariant transparency test, precomputed
  pen + scanline pointer, dropped a per-pixel bounds check → **racing render −32%**
  (0.83→0.57 ms host). (Caught & reverted a DIVU idea — a divide is a trap on 030.)
- **Palette dirty-gate**: only rebuild the 8192-pen DAC cache + 16384-pen pen8 LUT
  when palette RAM actually changes (~9 % of frames in racing).
- **rotate_draw** row-pointer hoist; **overlay** skips empty road scanlines;
  **present** uses 32-bit stores; **HUD** redraws only on change.

### Sound
- Confirmed the board is Z80 + YM2151 + SegaPCM only — **all music & SFX covered**.
- Retired the old ymfm C++ core (validated the lean Jarek YM2151 spectrally
  identical); the lean core is the only path now.

### Input & HUD
- Added **CD32 pad + joystick** reading (raw registers, no lowlevel.library);
  **gears on L1/R1**; keyboard still works in parallel.
- HUD cleaned to a **gear indicator** (always) + an FPS overlay that's **off by
  default** and toggled with **F**; frameskip cycles on **S**.

### Frameskip / smoothness
- Frameskip cap made live-tunable (S): 1 = smoothest display, 3 = fastest game.
  The three 68000s and the renderer cost about the same per frame, so the cap trades
  game-speed for display-smoothness.

### Investigations (with conclusions)
- **Hill "map disappears"**: proved it's **NOT** a priority/layering bug (compositor
  is byte-exact vs MAME; every HUD sprite uses min-priority so it's always on top).
  Fixed a real **bank-wrap bug** in `render_bsprites` (ROM address now wraps within
  its 0x10000-word bank) — but that didn't resolve it, so the cause is **machine-side
  & gameplay-only**. Validated hill frames vs headless MAME for the first time
  (attract hills render faithfully). Added `tools/pdrift_hillscan.c`.
- **68k core**: deep-research survey → **Musashi is the definitive core**; nothing
  off-the-shelf beats it (Rust cores are interpreters; Emu68/ARAnyM are foreign-host
  JITs; no 68k→C static recompiler exists). Recorded as a standing decision.

### Boot / exit
- Disassembled the crt0 → confirmed it already handles the WBStartup message
  correctly. The startup glitch was the **HDF booting Workbench** (IPrefs/native
  monitors) before the game's own Intuition RTG screen. Replaced with a **minimal
  Workbench-less boot** (SetPatch + uaegfx + game + UAEquit) → clean start & close.

---

## What's left (open items)

### Bugs / correctness
1. **Map (+ a few sprites) disappear on hills** — machine-side & intermittent. The
   HUD sprite rendering is hill-independent, so the emulated 68000s must be
   mis-updating the sprite list on the busiest tilt frames (leading hypothesis: an
   IRQ/sprite-latch timing divergence, *not* cycle starvation — the budget is
   correct). **Blocker:** can't reproduce gameplay on host — the playtest doesn't
   perform Power Drift's course-select, so it never starts a race. **To fix:** a
   `grim` screenshot of the exact dropout (which element, climb/crest/descend,
   flicker vs gone), OR script a full race in MAME (coin→start→select→drive) and diff
   the 16B sprite RAM against ours. MAME oracle is set up and working.
2. **Stray blocks at road edges** (older report) — unconfirmed on host; likely the
   same machine-side family.
3. **Coin/start → gameplay path on host** — related enabling issue: solving host
   gameplay entry would unlock reproducing #1 and #2 for MAME diffing.

### Smoothness
4. **Course-preview jitter** — the fast flythrough stresses the display rate; **S →
   SKIP 1** is the smoothest. A deeper fix (present pacing from `WaitTOF` → the
   family-standard EClock cadence, to avoid the 50/25 alternation when a frame just
   misses vblank) is real but needs hardware validation.

### Bigger speed levers (not started / deliberately deferred)
5. **Direct-to-VRAM present** — write the doubled pixels straight into the locked RTG
   bitmap under Amiberry instead of building an intermediate buffer + WriteChunkyPixels.
   Biggest remaining present win, but RTG-specific and **unvalidatable without your
   hardware** (a wrong pitch garbles/crashes) — needs a careful, testable pass.
6. **Native 68k transcode** — the only thing faster than the Musashi interpreter for
   full 60/60: a bespoke same-ISA static translation of the 3× 68000 (guest ⊆ host
   68030 identity mapping). Large project (I-cache flush, prefetch/SMC, 3-CPU sync);
   **measure the existing Rust transcode vs Musashi before ever committing to it.**

### Housekeeping
7. Orphaned **`dist/PowerDrift-RTG-Diag.hdf`** (24 MB, old snapshot-diag, no config
   points to it) can be deleted whenever — kept only as a reference for now.
