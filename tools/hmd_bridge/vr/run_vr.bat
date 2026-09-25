@echo off
rem Thief II in side-by-side stereo with head tracking from a SteamVR headset.
rem Order: 1) start SteamVR (headset on, Home showing)  2) double-click this file
rem        3) put the game window into the headset with Desktop+ (see tools/hmd_bridge/README.md).
rem Recentre the view: Pause/Break or Scroll Lock (works while the game has focus).
rem Change the resolution below if you like: per-eye picture is half the width, squeezed. It must be a mode the
rem engine knows (1280 720, 1600 900, 1920 1080 ...). Extra arguments to this file go to the game.
cd /d "%~dp0"
set HEADLOOK_STEREO=sbs
start "hmd_bridge - Pause/Scroll Lock recentres, Q quits" hmd_bridge.exe
Thief2.exe "-game_screen_size=1920 1080" -multisampletype=0 -postprocess=0 %*
taskkill /im hmd_bridge.exe >nul 2>&1
