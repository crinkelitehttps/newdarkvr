#!/bin/bash
# Sync the VR test pieces into a Windows-side game copy (default /mnt/d/games/thief_2_vr = D:\games\thief_2_vr;
# Thief Gold: pass /mnt/d/games/thief_gold_vr).
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

# Thief 2 or Thief Gold (the run_openvr*.bat launchers pick Thief2.exe or Thief.exe themselves; run_vr.bat is
# renamed here). With the d3d9.dll stand-in the stock exes work (the old 1.28 loader-patched Thief2.exe too).
if [ -f "$DEST/Thief2.exe" ]; then GAME=Thief2; elif [ -f "$DEST/Thief.exe" ]; then GAME=Thief; else
  echo "no Thief2.exe or Thief.exe in $DEST" >&2; exit 1; fi
KNOWN="af56a109a51ac9100a15da72cd403170a266736313679029fae1c222ebfba684 1.28
eb5e9abe0e8ed182c7946705f168f8ce6cdea8d0046f061c48e070cde8b1a640 1.28-loader
d26342c34624a08a0e5fc8d8c9daf14c676642c328f41c01f548170774aafe8f T2-1.29
4f1f98ed9cde1a2b1d8e2c6780ad4aebe74c0ea036efe0653b6fbae16bbc9f15 T2-1.29-hwtl
1a4bec9ef2d30ae948154bf74d31808d5942bbbcd9cbd6d40a0296622b44b5bb Gold-1.29
e34cb1e64a15d51695710e2550b4b2f847e11867c2c6fcff697f6d324ba24222 Gold-1.29-hwtl"
for f in "$GAME.exe" "${GAME}_hwtl.exe"; do
  [ -f "$DEST/$f" ] || continue
  h=$(sha256sum "$DEST/$f" | cut -d' ' -f1)
  echo "$KNOWN" | grep -q "^$h " ||
    echo "WARNING: $DEST/$f is not a known build: headlook's hooks will refuse to install (see headlook.log)" >&2
done
bat() { sed -e 's/$/\r/' "$HERE/vr/$1" > "$DEST/$1"; }                                     # Windows line endings
bat_renamed() { sed -e "s/Thief2\.exe/$GAME.exe/g" -e 's/$/\r/' "$HERE/vr/$1" > "$DEST/$1"; }  # no exe detection inside

cp "$ROOT/headlook/headlook.dll" "$DEST/headlook.dll"
cp "$ROOT/d3d9proxy/d3d9.dll" "$DEST/d3d9.dll"
mkdir -p "$DEST/openvr32"
cp "$ROOT/headlook/openvr32/openvr_api.dll" "$DEST/openvr32/openvr_api.dll"
cp "$HERE/hmd_bridge.exe" "$HERE/openvr_api.dll" "$DEST/"
bat_renamed run_vr.bat
bat run_openvr.bat
bat run_openvr_1440.bat
bat run_openvr_hwtl.bat
if [ ! -f "$DEST/headlook.ini" ] || [ "$FORCE_INI" = 1 ]; then
  sed 's/$/\r/' "$HERE/vr/headlook.ini" > "$DEST/headlook.ini"
  echo "wrote headlook.ini"
else
  echo "kept existing headlook.ini (use --ini to overwrite)"
fi
ls -l "$DEST/headlook.dll" "$DEST/openvr32/openvr_api.dll" "$DEST/run_openvr.bat" "$DEST/hmd_bridge.exe" "$DEST/openvr_api.dll" "$DEST/run_vr.bat" "$DEST/headlook.ini"
