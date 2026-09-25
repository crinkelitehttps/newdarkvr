#!/bin/bash
# Build the d3d9.dll stand-in (32-bit) with mingw-w64. Output: d3d9proxy/d3d9.dll
# It goes next to Thief2.exe (with headlook.dll); the game then loads headlook without a patched exe.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -std=gnu11 -Wall -Wextra -shared -o d3d9.dll d3d9proxy.c d3d9.def -static-libgcc
i686-w64-mingw32-strip d3d9.dll 2>/dev/null || true
ls -l d3d9.dll
