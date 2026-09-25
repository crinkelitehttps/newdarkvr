#!/bin/bash
# Build dinput.dll (32-bit) with mingw-w64. Output: xinput_joy/dinput.dll
# The output filename MUST be exactly dinput.dll -- that's the literal string Thief2.exe passes to
# LoadLibraryA (see docs/DEVLOG.md). No -lws2_32 needed (no networking, unlike headlook.dll).
# -luser32 is for SendInput (right-stick-as-mouselook, see xinput_joy.c).
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -shared -o dinput.dll xinput_joy.c \
  -luser32 -static-libgcc -Wl,--kill-at
i686-w64-mingw32-strip dinput.dll 2>/dev/null || true
ls -l dinput.dll
