@echo off
rem Thief II VR on a Quest (or Pico) headset, under WinlatorXR. See README_QUEST.txt.
rem First run only: adds the Touch controller binds (thief2vr_quest.bnd) to user.bnd, keeping a backup.
rem Safe start: 1280x720 (each eye 640x720 in the headset).
set QUEST_SIZE=1280 720
cd /d "%~dp0"
if exist thief2vr_binds.done goto play
if not exist user.bnd copy /y Default.bnd user.bnd >nul
copy /y user.bnd user.bnd.before-quest >nul
echo.>> user.bnd
type thief2vr_quest.bnd >> user.bnd
echo done> thief2vr_binds.done
:play
Thief2.exe "-game_screen_size=%QUEST_SIZE%" -multisampletype=0 -postprocess=0 %*
