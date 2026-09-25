#!/bin/bash
# Sync the VR test pieces into the Windows-side game copy (default /mnt/d/games/thief_2_vr = D:\games\thief_2_vr).
# Usage: tools/hmd_bridge/install_vr_copy.sh [DEST] [--ini]
#   Always refreshed: d3d9.dll (the stand-in that loads headlook.dll, from d3d9proxy/build.sh),
#   headlook.dll (from headlook/build.sh), openvr32\openvr_api.dll (32-bit, for stereo=openvr),
#   hmd_bridge.exe, openvr_api.dll (64-bit, for the bridge), run_vr.bat, run_openvr.bat.
#   headlook.ini is only written if missing, or overwritten with --ini (it may hold your tuning).
# The game files themselves (Thief2.exe with the loader, RES, ...) are copied once with tar from the ext4 copy;
# see docs/DEVLOG.md, "2026-09-21 (VR test on a Windows Mixed Reality headset)".
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEST=/mnt/d/games/thief_2_vr
FORCE_INI=0
for a in "$@"; do
  case "$a" in --ini) FORCE_INI=1 ;; *) DEST="$a" ;; esac
done
[ -d "$DEST" ] || { echo "no such folder: $DEST" >&2; exit 1; }
for f in "$ROOT/d3d9proxy/d3d9.dll" "$ROOT/headlook/headlook.dll" "$ROOT/headlook/openvr32/openvr_api.dll" "$HERE/hmd_bridge.exe" "$HERE/openvr_api.dll"; do
  [ -f "$f" ] || { echo "missing $f (run headlook/build.sh and tools/hmd_bridge/build.sh)" >&2; exit 1; }
done

# With the d3d9.dll stand-in the stock Thief2.exe works (the old loader-patched exe also still works).
if [ -f "$DEST/Thief2.exe" ] && [ -f "$ROOT/bin/Thief2.orig.exe" ]; then
  cmp -s "$DEST/Thief2.exe" "$ROOT/bin/Thief2.orig.exe" || cmp -s "$DEST/Thief2.exe" "$ROOT/bin/Thief2.patched.exe" ||
    echo "WARNING: $DEST/Thief2.exe is neither the stock exe nor the loader build: headlook's hooks will refuse to install" >&2
fi

cp "$ROOT/headlook/headlook.dll" "$DEST/headlook.dll"
cp "$ROOT/d3d9proxy/d3d9.dll" "$DEST/d3d9.dll"
mkdir -p "$DEST/openvr32"
cp "$ROOT/headlook/openvr32/openvr_api.dll" "$DEST/openvr32/openvr_api.dll"
cp "$HERE/hmd_bridge.exe" "$HERE/openvr_api.dll" "$DEST/"
sed 's/$/\r/' "$HERE/vr/run_vr.bat" > "$DEST/run_vr.bat"          # Windows line endings
sed 's/$/\r/' "$HERE/vr/run_openvr.bat" > "$DEST/run_openvr.bat"
sed 's/$/\r/' "$HERE/vr/run_openvr_1440.bat" > "$DEST/run_openvr_1440.bat"
if [ ! -f "$DEST/headlook.ini" ] || [ "$FORCE_INI" = 1 ]; then
  sed 's/$/\r/' "$HERE/vr/headlook.ini" > "$DEST/headlook.ini"
  echo "wrote headlook.ini"
else
  echo "kept existing headlook.ini (use --ini to overwrite)"
fi
ls -l "$DEST/headlook.dll" "$DEST/openvr32/openvr_api.dll" "$DEST/run_openvr.bat" "$DEST/hmd_bridge.exe" "$DEST/openvr_api.dll" "$DEST/run_vr.bat" "$DEST/headlook.ini"
