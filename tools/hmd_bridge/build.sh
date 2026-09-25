#!/bin/bash
# Build hmd_bridge.exe (64-bit) with mingw-w64. Fetches the pinned OpenVR SDK pieces if missing:
# openvr_capi.h and LICENSE.openvr are committed, openvr_api.dll (the runtime client library) is fetched
# and gitignored. Output: tools/hmd_bridge/hmd_bridge.exe (+ openvr_api.dll beside it).
set -e
cd "$(dirname "$0")"
TAG=v2.15.6
BASE=https://raw.githubusercontent.com/ValveSoftware/openvr/$TAG
[ -f openvr_capi.h ]   || curl -sSfL -o openvr_capi.h   "$BASE/headers/openvr_capi.h"
[ -f LICENSE.openvr ]  || curl -sSfL -o LICENSE.openvr  "$BASE/LICENSE"
[ -f openvr_api.dll ]  || curl -sSfL -o openvr_api.dll  "$BASE/bin/win64/openvr_api.dll"
x86_64-w64-mingw32-gcc -O2 -std=gnu11 -Wall -Wextra -isystem . -o hmd_bridge.exe hmd_bridge.c \
  -lws2_32 -lwinmm -static-libgcc
x86_64-w64-mingw32-strip hmd_bridge.exe 2>/dev/null || true
ls -l hmd_bridge.exe openvr_api.dll
