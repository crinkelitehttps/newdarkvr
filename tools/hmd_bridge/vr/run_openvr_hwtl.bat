@echo off
rem EXPERIMENTAL: run_openvr_1440.bat for NewDark 1.29's Thief2_hwtl.exe / Thief_hwtl.exe (the hardware T&L renderer: dynamic
rem shadows, normal maps, SSAO...). Same controls and settings as run_openvr_1440.bat; much heavier on the GPU,
rem and in the headset each frame draws the scene twice, shadow and reflection passes included.
rem Known gap: the HUD overlays (light gem, held-item models) are not moved onto the HUD panel for this exe yet.
rem Log: headlook.log (first lines name the exe build found) and Thief2_hwtl.log / Thief_hwtl.log.
cd /d "%~dp0"
set HEADLOOK_STEREO=openvr
rem Thief II (Thief2.exe) or Thief Gold (Thief.exe), whichever is in this folder.
set GAME=Thief2
if not exist Thief2.exe set GAME=Thief
%GAME%_hwtl.exe "-game_screen_size=1440 1080" -multisampletype=0 %*
