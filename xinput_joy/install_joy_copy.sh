#!/bin/bash
# Deploy dinput.dll (+ its ini, on first install) into a game copy. Modeled on
# tools/hmd_bridge/install_vr_copy.sh: existence checks, atomic rename so it's safe even if the game is
# currently running, and never clobbers a live user-tuned ini unless asked.
# Usage: xinput_joy/install_joy_copy.sh [DEST_DIR] [--ini]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/games/thief_2"
FORCE_INI=0
for a in "$@"; do
  case "$a" in
    --ini) FORCE_INI=1 ;;
    *) DEST="$a" ;;
  esac
done

[ -f "$HERE/dinput.dll" ] || { echo "missing $HERE/dinput.dll -- run xinput_joy/build.sh first" >&2; exit 1; }
[ -d "$DEST" ] || { echo "no such folder: $DEST" >&2; exit 1; }

# Atomic rename: safe to run while the game is already loaded (never plain `cp` over a loaded DLL).
cp "$HERE/dinput.dll" "$DEST/dinput.dll.new" && mv "$DEST/dinput.dll.new" "$DEST/dinput.dll"
echo "deployed dinput.dll -> $DEST/dinput.dll"

if [ ! -f "$DEST/xinput_joy.ini" ] || [ "$FORCE_INI" = 1 ]; then
  cp "$HERE/xinput_joy.ini" "$DEST/xinput_joy.ini"
  echo "wrote $DEST/xinput_joy.ini"
else
  echo "kept existing $DEST/xinput_joy.ini (pass --ini to overwrite with the template)"
fi

ls -l "$DEST/dinput.dll" "$DEST/xinput_joy.ini"
