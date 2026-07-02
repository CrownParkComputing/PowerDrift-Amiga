#!/bin/bash
# Build the first Power Drift RTG diagnostic HDF/UAE package.
set -euo pipefail

export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_DIR/source"
GAME_DIR="$REPO_DIR"
BUILD_DIR="$GAME_DIR/build/rtg_diag"
DIST_DIR="$GAME_DIR/dist"
BOOT_DIR="$BUILD_DIR/boot"
HDF="$DIST_DIR/PowerDrift-RTG-Diag.hdf"
UAE="$DIST_DIR/PowerDrift-RTG-Diag.uae"
EXE="$BUILD_DIR/PowerDriftDiag"
FRAME_COUNTS=(300 420 540 660 780 900 1020 1140 1260 1380 1500 1620 4200 4320 4440)
FRAME_PPMS=()
RTG_BASE="/home/jon/Amiberry/HardDrives/GreenBeret_RTG.hdf"

mkdir -p "$BUILD_DIR" "$DIST_DIR" "$BOOT_DIR/s"

need_frames=0
for frame in "${FRAME_COUNTS[@]}"; do
  ppm="$SRC_DIR/build/pdrift_frame_rtg_diag_attract_${frame}.ppm"
  FRAME_PPMS+=("$ppm")
  if [ ! -f "$ppm" ]; then
    need_frames=1
  fi
done

if [ "$need_frames" -ne 0 ] || [ ! -x /tmp/pdrift_host_probe ] || [ ! -x /tmp/pdrift_frameview ]; then
  echo "== build host probe tools =="
  bash "$SRC_DIR/build_host_probe.sh"
fi

for frame in "${FRAME_COUNTS[@]}"; do
  snap="$SRC_DIR/build/snap_rtg_diag_attract_${frame}"
  ppm="$SRC_DIR/build/pdrift_frame_rtg_diag_attract_${frame}.ppm"
  if [ ! -f "$ppm" ]; then
    echo "== generate host attract composite frame $frame =="
    /tmp/pdrift_host_probe "$SRC_DIR/build/pdrift" "$frame" "$snap"
    /tmp/pdrift_frameview "$SRC_DIR/build/pdrift" "$snap" "$ppm"
  fi
done

echo "== generate embedded diagnostic frames =="
python3 "$SRC_DIR/tools/make_pdrift_diag_frame.py" "$BUILD_DIR/pdrift_diag_frame.h" "${FRAME_PPMS[@]}"

echo "== compile Power Drift RTG diagnostic =="
m68k-amigaos-gcc -m68020 -noixemul -O2 -fomit-frame-pointer \
  -I "$BUILD_DIR" \
  "$SRC_DIR/amiga/pdrift_rtg_diag.c" \
  -o "$EXE"
m68k-amigaos-strip "$EXE" || true
cp -f "$EXE" "$GAME_DIR/PowerDriftDiag"

echo "== build bootable diagnostic HDF =="
cat > "$BOOT_DIR/s/startup-sequence" <<'EOF'
; Power Drift RTG diagnostic startup.
C:SetPatch QUIET
C:Version >NIL:
FailAt 21

C:MakeDir RAM:T RAM:Clipboards RAM:ENV RAM:ENV/Sys
C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ

Resident >NIL: C:Assign PURE
Resident >NIL: C:Execute PURE

Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Classes ADD

BindDrivers
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)

IF EXISTS DEVS:Monitors
  IF EXISTS DEVS:Monitors/VGAOnly
    DEVS:Monitors/VGAOnly
  EndIF
  IF EXISTS DEVS:Monitors/more/uaegfx
    DEVS:Monitors/more/uaegfx
  EndIF
  C:List >NIL: DEVS:Monitors/~(#?.info|VGAOnly) TO T:M LFORMAT "DEVS:Monitors/%s"
  Execute T:M
  C:Delete >NIL: T:M
EndIF

C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip
C:Wait 2 SECS

Path >NIL: RAM: C: SYS:Utilities SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools
Echo "Power Drift RTG Diagnostic"
Stack 65536
SYS:PowerDriftDiag
EndCLI >NIL:
EOF
rm -f "$HDF"
if [ ! -f "$RTG_BASE" ]; then
  echo "missing RTG boot base: $RTG_BASE" >&2
  exit 1
fi
cp -f "$RTG_BASE" "$HDF"
xdftool "$HDF" delete PowerDriftDiag >/dev/null 2>&1 || true
xdftool "$HDF" delete S/startup-sequence >/dev/null 2>&1 || true
xdftool "$HDF" delete pdrift_diag.log >/dev/null 2>&1 || true
xdftool -f "$HDF" \
  write "$EXE" PowerDriftDiag \
  + write "$BOOT_DIR/s/startup-sequence" S/startup-sequence

echo "== write UAE config =="
cat > "$UAE" <<EOF
; Power Drift RTG diagnostic config.
; Boots a first RTG test that displays the host-validated composite frame.

[config]
config_description=Power Drift RTG Diagnostic
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

kickstart_rom_file=/home/jon/Amiberry/ROMs/kick40068.A1200.rom

nr_floppies=0
floppy0type=-1

hardfile2=rw,DH0:$HDF,32,1,2,512,0,,uae0,0
uaehf0=hdf,rw,DH0:$HDF,32,1,2,512,0,,uae0,0

gfx_display=0
gfx_display_rtg=0
gfx_width=640
gfx_height=480
gfx_x_windowed=64
gfx_y_windowed=48
gfx_width_windowed=640
gfx_height_windowed=480
gfx_width_fullscreen=640
gfx_height_fullscreen=480
gfx_fullscreen=0
gfx_fullscreen_amiga=false
gfx_fullscreen_picasso=false
gfx_lores=false
gfx_resolution=hires
gfx_lores_mode=normal
gfx_linemode=none
gfx_center_horizontal=smart
gfx_center_vertical=smart
gfx_keep_aspect=true
gfx_colour_mode=32bit
gfx_api=sdl3
gfx_api_options=hardware
gfx_backbuffers=2
gfx_backbuffers_rtg=1
gfx_vsync=false
gfx_vsync_picasso=false
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
input.joymouse_speed_analog=100
input.joymouse_speed_digital=10
input.joystick_deadzone=33

log_file=/tmp/amiberry-powerdrift-rtg-diag.log
log_console=1
fpu_model=68882
EOF

echo "DONE -> $EXE"
echo "DONE -> $HDF"
echo "DONE -> $UAE"
