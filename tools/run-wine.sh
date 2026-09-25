#!/bin/bash
# Launch Thief2.exe under Wine on the TigerVNC display, inside a Wine virtual desktop so it is an
# ordinary window that i3 can tile.
# Usage: tools/run-wine.sh [extra game args...]
# env:   SIZE="1280 720"  GAME_DIR  DISPLAY  WS (i3 workspace to move the window to, e.g. WS=3)
#        STEREO=sbs   side-by-side stereo experiment (headlook.dll draws the scene twice, once per eye,
#                     each squeezed into half the screen). Unset or STEREO=off = the normal game.
#                     Passed to the DLL as HEADLOOK_STEREO, which overrides the ini's `stereo` key.
#                     Eye separation and eye swap are in headlook.ini (stereo_ipd, stereo_swap; live).
#
# Wine dinput override: xinput_joy/dinput.dll must win over Wine's own builtin dinput.dll for the
# XInput controller feature to work (see xinput_joy/, DEVLOG "Buttons + shift/chording"). Scoped to
# this launch's own environment only -- never touches the prefix's global winecfg DLL overrides.
#
# Why a virtual desktop: the engine always asks D3D for exclusive fullscreen (game_full_screen=0 is
# ignored). Without one, Wine switches the *real* X display mode, which under Xvnc shrinks the whole
# VNC screen and is never restored. Inside `explorer /desktop=` the mode switch is virtual.
# Wine also flags the desktop window _NET_WM_STATE_FULLSCREEN, so the script clears that in i3.
# The virtual desktop is a fixed WxH: in a smaller tile it is cropped, in a bigger one the rest is
# black. Give it room with WS=<n> (own workspace) or i3's `layout tabbed`.
#
# Requirements/quirks:
#  - CWD must be the game dir: the engine loads cam.cfg/dark.cfg and .\RES etc. relative to it.
#  - Thief2.exe must be given as a full Windows path; explorer silently never starts a relative name.
#  - SIZE must be in the engine's mode table (800 600, 1024 768, 1280 720, 1920 1080, ...); 960x540
#    is not. Don't add -force_windowed: it pins the start mode to 640x480.
#  - The X screen must be at least SIZE, or Wine logs "Failed to set primary display settings".
# Renderer: default is Mesa's software llvmpipe. Mesa's OpenGL-on-D3D12 driver (GALLIUM_DRIVER=d3d12,
# host GPU via /dev/dxg) gave only 2-13 fps in a mission vs ~80-99 for llvmpipe (DEVLOG, 2026-09-20).
# Game dir: default is a native-ext4 copy. The Steam install on /mnt/d (9p) makes a New Game take
# minutes because of per-file open latency; set GAME_DIR to it only to compare.
# Wine tracing: uses T2_WINEDEBUG (default -all), NOT an ambient WINEDEBUG, because a stale
# WINEDEBUG=+d3d9,+d3d in the shell once slowed every launch. e.g. T2_WINEDEBUG=+fps for frame rate.
export GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe}
GAME_DIR=${GAME_DIR:-$HOME/games/thief_2}
[ -d "$GAME_DIR" ] || GAME_DIR=/mnt/d/SteamLibrary/steamapps/common/thief_2
SIZE=${SIZE:-1280 720}
#SIZE=${SIZE:-640 480}
export DISPLAY=${DISPLAY:-:1}
# Stereo toggle for headlook.dll (see the header). Always set or unset explicitly so a stale value in the
# calling shell cannot switch it on.
if [ -n "$STEREO" ] && [ "$STEREO" != off ]; then export HEADLOOK_STEREO="$STEREO"; else unset HEADLOOK_STEREO; fi
cd "$GAME_DIR" || exit 1

# Wine's Z: drive is the Unix root, so the Windows path is a plain slash swap.
EXE="Z:$(printf '%s' "$GAME_DIR/Thief2.exe" | tr / '\\')"

# Once the desktop window shows up in i3, clear the fullscreen flag (and optionally move it).
if command -v i3-msg >/dev/null 2>&1; then
  (
    sel='[class="explorer.exe" title="^Thief2 - Wine Desktop$"]'
    for _ in $(seq 60); do
      sleep 0.5
      i3-msg -t get_tree 2>/dev/null | grep -q '"title":"Thief2 - Wine Desktop"' || continue
      i3-msg -q "$sel fullscreen disable"
      [ -n "$WS" ] && i3-msg -q "$sel move container to workspace $WS"
      break
    done
  ) &
fi

# Force Wine to prefer our native dinput.dll (game-dir copy) over its builtin one; additive to
# anything the caller already set in WINEDLLOVERRIDES.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES,}dinput=n"

WINEDEBUG=${T2_WINEDEBUG:--all} exec wine explorer "/desktop=Thief2,${SIZE// /x}" "$EXE" \
  "-game_screen_size=$SIZE" -multisampletype=0 -postprocess=0 "$@"
