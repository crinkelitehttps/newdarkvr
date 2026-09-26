THIEF II VR - STANDALONE QUEST TEST BUILD
=========================================

Thank you for testing! This is an early, untested-on-hardware build that runs Thief II (NewDark) inside
WinlatorXR, a Windows emulator for Quest and Pico headsets, and turns it into a VR game: 3D stereo,
head tracking, and the Touch controllers. Nobody has run it on a real Quest yet, so the most useful thing
you can do is go through the steps below in order and tell us exactly where things stop working.

You need
--------
* A Quest 3 or 3S (a Quest 2 should also work, but slower).
* Thief II: The Metal Age with NewDark, as sold on Steam or GOG. The mod is written for one exact build of
  Thief2.exe (SHA-256 af56a109...). A different build still runs, just without VR, and headlook.log says so.
* A PC to copy files, and the headset in developer mode (needed to install apps from outside the store).

This zip contains no game files and no emulator: only the mod (three DLLs, settings and launchers).


1. Install WinlatorXR
---------------------
* Download WinlatorXR-cats-27.apk (or newer) from https://github.com/WinlatorXR/WinlatorXR/releases
* Install it with SideQuest (https://sidequestvr.com) or Meta Quest Developer Hub. It appears under
  "Unknown sources" in the headset's app library.


2. Check the plain game runs (no mod yet)
-----------------------------------------
* Copy your whole Thief II folder from the PC to the headset, for example into Download/thief_2
  (in Steam: right-click Thief II > Manage > Browse local files).
* In WinlatorXR, create a container. Suggested settings:
  - Graphics driver: Turnip (the default for Quest).
  - DX wrapper: DXVK.
  - Box64 preset: the default. If the game crashes, try "Stability"; if it's slow, "Performance".
* Start Thief2.exe from the container's file browser, get to the main menu, then start a new game and
  walk around for a minute.

  >> Please tell us: does it start, does a mission load, and roughly how smooth is it?
     If it doesn't work at this step, the mod can't work either, so please stop and report here.


3. Install the mod
------------------
Copy everything from this zip into the Thief II folder on the headset, next to Thief2.exe:
    d3d9.dll  headlook.dll  dinput.dll  headlook.ini  xinput_joy.ini
    thief2vr_quest.bnd  run_quest.bat  run_quest_1080.bat
(Starting Thief2.exe directly still plays the normal flat game, just with the mod loaded.)

Then, in the container's settings, add this environment variable, so the game uses the mod's controller
support (dinput.dll) instead of Wine's built-in one:
    WINEDLLOVERRIDES=dinput=n,b


4. Controller settings in WinlatorXR
------------------------------------
WinlatorXR has its own controller-to-mouse/keyboard mapping. Our mod reads the controllers itself, so
the two would double up. In WinlatorXR's controller settings ("Controller manager" / motion controls):
* Turn OFF the motion (hand) mouse. Otherwise moving your right hand turns your character.
* Clear the button and thumbstick key mappings, if you can. The left menu button always stays Esc,
  which is fine.
If you can't find these, test anyway and tell us what happens.


5. Play
-------
Start run_quest.bat (1280x720, the safe choice). If that works well, try run_quest_1080.bat (sharper
but slower).
* Menus, books and loading screens appear on WinlatorXR's normal flat screen.
* When a mission starts it should switch to full 3D VR. Look around: the world should stay still
  while your head moves.
* The first run adds the controller binds to the game's user.bnd (your old one is kept as
  user.bnd.before-quest). To undo that: delete thief2vr_binds.done and put user.bnd.before-quest back as
  user.bnd.

Controls (rebind in Options > Controls > Customize Controls, using the controllers):
    Left stick         move (analog)
    Right stick        turn left/right (up/down is left to your head)
    Right trigger      attack / draw bow / click in menus
    Right grip         use / pick up / open (frob)
    A                  jump / mantle
    B                  crouch
    X                  next weapon          (left grip + X: previous weapon)
    Left trigger       next inventory item  (left grip + left trigger: previous item)
    Left grip + A      drop item
    Left grip + B      put weapon away
    Left grip + stick  lean left / right / forward
    Y                  show / hide the HUD (the light gem always stays)
    Both stick clicks  recentre the view
    Left menu button   game menu (Esc)


6. What to send back
--------------------
After a test, please send these files from the Thief II folder (they're plain text):
    headlook.log   d3d9proxy.log   dinput.log   Thief2.log
And tell us, in your own words:
  a) Did the plain game run (step 2)? How smooth?
  b) With run_quest.bat: did the mission appear in 3D in the headset, or did something else happen
     (flat, black, doubled, split screen, crash)?
  c) Head tracking: does looking around feel right, or does the world swim, lag, or turn the wrong way?
     Tilt your head sideways: does the world stay level?
  d) Does the depth look right? (If near objects look inside-out, see below.)
  e) Smoothness, and did it get worse in big areas?
  f) Controllers: which of the controls above work, which don't, which do something else?
  g) HUD: can you see the light gem? Is the HUD readable?
  h) Brightness: too dark or too bright?

Quick fixes you can try (edit headlook.ini with any text editor; most take effect within 2 seconds):
    World tilts the wrong way when you tilt your head    roll_sign=-1   (or wxr_roll=0 to turn tilt off)
    Turning your head turns the view the wrong way        yaw_sign=1    (up/down: pitch_sign=1)
    Depth looks inside-out / eyes crossed                  stereo_swap=1
    Depth too strong / too flat                            wxr_ipd_scale=0.7 / 1.5
    Too dark / too bright                                  vr_gamma=1.5 / 1.0
    HUD too small / too big                                vr_hud_deg=60 / 35
    Field of view looks stretched or squashed              wxr_fov=100 (try 90..120; 0 = automatic)

If run_quest.bat shows the plain flat game and d3d9proxy.log is missing, WinlatorXR loaded its own
d3d9.dll instead of ours. Change the environment variable from step 3 to this and try again:
    WINEDLLOVERRIDES=d3d9,dinput=n,b
If the controllers do nothing and dinput.log is missing, the dinput part of that variable isn't taking
effect.
