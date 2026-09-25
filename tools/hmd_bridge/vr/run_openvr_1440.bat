@echo off
rem LIGHTER VARIANT (1440x1080, ~44% fewer pixels per eye than run_openvr.bat). Thief II straight into a SteamVR headset: headlook.dll submits each eye to SteamVR itself (stereo=openvr)
rem and takes the head pose from SteamVR. No Desktop+ and no hmd_bridge.exe.
rem Order: 1) start SteamVR (headset on)  2) double-click this file  3) start or load a mission.
rem Menus are not sent to the headset (use the desktop window for them). Recentre: Pause/Break or Scroll Lock.
rem Log: headlook.log next to Thief2.exe (lines starting "openvr:"). Insert = picture dump for debugging.
rem 1920x1440 is 4:3: the engine's view is widened to fill the headset, and 4:3 wastes the fewest pixels on width
rem the headset can't see. Any mode in the game's list works (Thief2.log lists them); fewer pixels = faster.
cd /d "%~dp0"
set HEADLOOK_STEREO=openvr
Thief2.exe "-game_screen_size=1440 1080" -multisampletype=0 -postprocess=0 %*
