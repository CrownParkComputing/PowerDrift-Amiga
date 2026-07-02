#!/bin/bash
# Build the Power Drift Amiga RTG game and package it as a standalone port.
# Self-contained exe (embedded ROMs) on its own bootable RTG HDF + Amiberry config.
set -euo pipefail

export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_DIR/source"
M68K="$SRC_DIR/cores/musashi"
BUILD_DIR="$REPO_DIR/build/rtg_live"
DIST_DIR="$REPO_DIR/dist"
BOOT_DIR="$BUILD_DIR/boot"
ROM_SRC="$SRC_DIR/build/pdrift"
HDF="$DIST_DIR/PowerDrift_RTG.hdf"
UAE="$DIST_DIR/PowerDrift-RTG.uae"
EXE="$BUILD_DIR/PowerDriftLive"
ICON="$REPO_DIR/assets/PowerDrift.info"
RTG_BASE="${RTG_BASE:-/home/jon/Amiberry/HardDrives/RTG_boot_template.hdf}"
KICK="${KICK:-/home/jon/Amiberry/ROMs/kick40068.A1200.rom}"
AMIBERRY_HDD="${AMIBERRY_HDD:-/home/jon/Amiberry/HardDrives}"
AMIBERRY_CFG="${AMIBERRY_CFG:-/home/jon/Amiberry/Configurations}"

mkdir -p "$BUILD_DIR/obj" "$DIST_DIR" "$BOOT_DIR/s"

for b in maincpu.bin subx.bin suby.bin ysprites_pix.bin bsprites.bin; do
  if [ ! -f "$ROM_SRC/$b" ]; then
    echo "missing ROM blob $ROM_SRC/$b -- run tools/pdrift_extract_roms.py first" >&2
    exit 1
  fi
done

OBJ="$BUILD_DIR/obj"
GCC="m68k-amigaos-gcc -m68020 -noixemul -DNDEBUG -DPDR_LEAN_YM -I $SRC_DIR -I $SRC_DIR/opm -I $M68K -I $M68K/softfloat"
GCC_APP="$GCC -O2 -fomit-frame-pointer"
GCC_CORE="$GCC -O1 -fno-strict-aliasing"

echo "== Musashi core (cross) =="
$GCC_CORE -c "$M68K/m68kcpu.c"          -o "$OBJ/m68kcpu.o"
$GCC_CORE -c "$M68K/m68kops.c"          -o "$OBJ/m68kops.o"
$GCC_CORE -c "$M68K/softfloat/softfloat.c" -o "$OBJ/softfloat.o"

echo "== shared machine + renderer + math stubs =="
$GCC_APP -c "$SRC_DIR/pdrift_machine.c"   -o "$OBJ/pdrift_machine.o"
$GCC_APP -c "$SRC_DIR/pdrift_render.c"    -o "$OBJ/pdrift_render.o"
$GCC_APP -c "$SRC_DIR/pdrift_mathstubs.c" -o "$OBJ/pdrift_mathstubs.o"

echo "== sound board: Z80 + lean YM2151 (Jarek) + SegaPCM + Paula =="
$GCC_APP -c "$SRC_DIR/pdrift_segapcm.c"      -o "$OBJ/pdrift_segapcm.o"
$GCC_APP -c "$SRC_DIR/pdrift_z80.c"          -o "$OBJ/pdrift_z80.o"
$GCC_APP -c "$SRC_DIR/pdrift_audio.c"        -o "$OBJ/pdrift_audio.o"
$GCC_APP -c "$SRC_DIR/pdrift_audio_amiga.c"  -o "$OBJ/pdrift_audio_amiga.o"
$GCC_APP -c "$SRC_DIR/pdrift_opm.c"          -o "$OBJ/pdrift_opm.o"
$GCC_CORE -c "$SRC_DIR/opm/ym2151.c"         -o "$OBJ/ym2151.o"

echo "== Amiga live driver =="
$GCC_APP -c "$SRC_DIR/amiga/pdrift_live_main.c" -o "$OBJ/pdrift_live_main.o"

echo "== embedded ROM blobs (.incbin, ~7.5MB) =="
( cd "$SRC_DIR" && m68k-amigaos-gcc -c pdrift_romdata.S -o "$OBJ/pdrift_romdata.o" )

echo "== link (all C now -- no C++/ymfm; -lm for the YM2151 table init) =="
m68k-amigaos-gcc -m68020 -noixemul -o "$EXE" \
  "$OBJ/pdrift_live_main.o" "$OBJ/pdrift_machine.o" "$OBJ/pdrift_render.o" \
  "$OBJ/pdrift_mathstubs.o" \
  "$OBJ/pdrift_segapcm.o" "$OBJ/pdrift_z80.o" "$OBJ/pdrift_audio.o" \
  "$OBJ/pdrift_audio_amiga.o" "$OBJ/pdrift_opm.o" "$OBJ/ym2151.o" \
  "$OBJ/m68kcpu.o" "$OBJ/m68kops.o" "$OBJ/softfloat.o" \
  "$OBJ/pdrift_romdata.o" -lm
m68k-amigaos-strip "$EXE" || true
cp -f "$EXE" "$REPO_DIR/PowerDriftLive"
ls -l "$EXE" | awk '{print "exe:",$5,"bytes"}'

echo "== boot HDF (DH0) =="
cat > "$BOOT_DIR/s/startup-sequence" <<'EOF'
; Power Drift standalone RTG boot -- MINIMAL on purpose. The game opens its own
; exclusive Intuition RTG screen, so we deliberately do NOT load Workbench:
; no IPrefs / native monitors / datatypes / ConClip. That avoids the Intuition
; screenmode requester + Workbench flash at start, and there is nothing to unwind
; on the way out, so C:UAEquit closes cleanly. Just SetPatch + the uaegfx RTG
; monitor + the game.
C:SetPatch QUIET
FailAt 21

C:MakeDir >NIL: RAM:T RAM:ENV
C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ
Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: LIBS: SYS:Classes ADD

BindDrivers
IF EXISTS DEVS:Monitors/uaegfx
  DEVS:Monitors/uaegfx
EndIF
IF EXISTS DEVS:Monitors/more/uaegfx
  DEVS:Monitors/more/uaegfx
EndIF

Path >NIL: RAM: C: SYS:System S:
Stack 200000
SYS:PowerDriftLive
C:UAEquit
EndCLI >NIL:
EOF

rm -f "$HDF"
[ -f "$RTG_BASE" ] || { echo "missing RTG boot base: $RTG_BASE" >&2; exit 1; }
cp -f "$RTG_BASE" "$HDF"
xdftool "$HDF" delete PowerDriftLive >/dev/null 2>&1 || true
xdftool "$HDF" delete PowerDriftLive.info >/dev/null 2>&1 || true
xdftool "$HDF" delete PowerDriftDiag >/dev/null 2>&1 || true
xdftool "$HDF" delete S/startup-sequence >/dev/null 2>&1 || true
xdftool "$HDF" delete pdrift_live.log >/dev/null 2>&1 || true
xdftool -f "$HDF" \
  write "$EXE" PowerDriftLive \
  + write "$BOOT_DIR/s/startup-sequence" S/startup-sequence
[ -f "$ICON" ] && xdftool -f "$HDF" write "$ICON" PowerDriftLive.info || echo "  (no icon $ICON)"
echo "boot HDF free after exe:"; xdftool "$HDF" info 2>/dev/null | grep -E "free|used" | sed 's/^/  /'

echo "== config =="
cat > "$UAE" <<EOF
; Power Drift RTG config for Amiberry.
; Boots the Picasso96-derived HDF directly into the game.
[config]
config_description=Power Drift (Sega Y-Board) RTG
config_version=8.2.0
config_hardware=1
config_host=1
use_gui=no

amiga_model=A1200
chipset=aga
chipset_compatible=A1200
cpu_type=68030
cpu_model=68030
cpu_compatible=false
cpu_cycle_exact=false
cpu_speed=max
address_space_24=false
cpu_24bit_addressing=false
cachesize=16384
comp_trustbyte=direct
comp_trustword=direct
comp_trustlong=direct
comp_trustnaddr=direct
comp_flushmode=hard
comp_constjump=true

chipmem_size=4
fastmem_size=8
z3mem_size=512
z3mem_start=0x40000000
bogomem_size=0

gfxcard_size=16
gfxcard_type=ZorroIII
gfxcard_hardware_vblank=false
gfxcard_hardware_sprite=false
gfxcard_multithread=false
gfxcard_zerocopy=true
rtg_nocustom=false
rtg_modes=0x3ffe
rtg_noautomodes=false

kickstart_rom_file=$KICK

nr_floppies=0
floppy0type=-1

hardfile2=rw,DH0:$HDF,32,1,2,512,0,,uae0,0
uaehf0=hdf,rw,DH0:$HDF,32,1,2,512,0,,uae0,0

gfx_display=0
gfx_display_rtg=0
gfx_width=864
gfx_height=486
gfx_width_windowed=864
gfx_height_windowed=486
gfx_width_fullscreen=864
gfx_height_fullscreen=486
gfx_fullscreen_amiga=false
gfx_fullscreen_picasso=false
gfx_lores=false
gfx_resolution=hires
gfx_center_horizontal=smart
gfx_center_vertical=smart
gfx_keep_aspect=true
gfx_colour_mode=32bit
gfx_api=sdl3
gfx_api_options=hardware
gfx_backbuffers=2
gfx_backbuffers_rtg=1
gfx_vsync=false
immediate_blits=true
screenshot_dir=/home/jon/Amiberry/Screenshots/

sound=1
sound_output=exact
sound_channels=stereo
sound_frequency=44100
sound_interpol=anti
sound_volume=80

joyport0=mouse
joyport1=joy0
joyport1mode=cd32joy
input.autoswitch=false
input.joystick_deadzone=33

fpu_model=68882
EOF

echo "== canonical dist layout (exe + icon in dist/) =="
cp -f "$EXE" "$DIST_DIR/Power Drift"
[ -f "$ICON" ] && cp -f "$ICON" "$DIST_DIR/Power Drift.info"

echo "== deploy standalone HDF + config to Amiberry =="
if [ -d "$AMIBERRY_HDD" ]; then
  cp -f "$HDF" "$AMIBERRY_HDD/PowerDrift_RTG.hdf"
  sed "s|DH0:$HDF|DH0:$AMIBERRY_HDD/PowerDrift_RTG.hdf|g" "$UAE" > "$AMIBERRY_CFG/PowerDrift-RTG.uae"
  echo "deployed -> $AMIBERRY_HDD/PowerDrift_RTG.hdf"
  echo "deployed -> $AMIBERRY_CFG/PowerDrift-RTG.uae"
fi

echo "DONE -> $DIST_DIR/Power Drift (exe)"
echo "DONE -> $HDF"
echo "DONE -> $UAE"