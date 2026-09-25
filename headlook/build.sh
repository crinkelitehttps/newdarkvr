#!/bin/bash
# Build headlook.dll (32-bit) with mingw-w64. Output: headlook/headlook.dll
# stereo=openvr also needs the 32-bit openvr_api.dll in openvr32/ next to the DLL: fetched here (gitignored),
# same pinned OpenVR SDK as tools/hmd_bridge (whose openvr_capi.h is used).
set -e
cd "$(dirname "$0")"
TAG=v2.15.6
mkdir -p openvr32
[ -f openvr32/openvr_api.dll ] || curl -sSfL -o openvr32/openvr_api.dll \
  "https://raw.githubusercontent.com/ValveSoftware/openvr/$TAG/bin/win32/openvr_api.dll"
i686-w64-mingw32-gcc -O2 -std=gnu11 -Wall -Wextra -isystem ../tools/hmd_bridge -shared -o headlook.dll headlook.c vr_openvr.c \
  -lws2_32 -static-libgcc -Wl,--kill-at
i686-w64-mingw32-strip headlook.dll 2>/dev/null || true
ls -l headlook.dll openvr32/openvr_api.dll
