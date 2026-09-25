# Phase 0 Spike: setup and procedure (updated 2026-09-20)

Read `README.md` first. This file is the working procedure for the spike; `HANDOFF.md`
has the session state, and the DEVLOG's "Phase 0 spike" log entries have the results.

## What the spike is

A throwaway experiment (a "spike") answering one question: if the render camera is
`DynamicAttach()`'d to a different object, do the player's *movement* and *interaction
targeting* still follow the body, or the camera?

Answer so far (2026-09-20): **movement follows the body, frob targeting follows the
camera.** So a script-only camera offset cannot give head-look independent of aim
(Stage A is out for that goal); work moved to a binary hook (Stage B, `patches/`).

Two files make up the spike:

- `miss_all.dml`: a DML dbmod (`doc/dbmod-sample.dml` in the game) applied to *every*
  mission when it loads fresh. It creates two invisible Marker objects,
  `HeadMarker` (the camera gets attached to it) and `HeadTrackController` (carries the
  script).
- `HeadTrackSpikeAuto.nut`: the Squirrel script on `HeadTrackController`. It cycles
  three 10-second phases forever, announced on screen and logged:
  0. **NORMAL**: camera on the player. Aim at a frobbable item until it highlights.
     The camera's exact pose is captured at the end of this phase.
  1. **CONTROL**: camera on the marker at that exact pose, yaw +0.
  2. **TEST**: same pose, yaw +30 degrees.

  It also logs, at every phase end, who owns the camera and the camera/player poses.

No DromEd and no mission building are needed: the game's `OSM/` folder is its
`uber_mod_path`, so files dropped there are loaded like any other mod.

## Install / remove

The game is run from the native ext4 copy, **not** the Steam install (which is slow to
load over 9p and is kept pristine for native play; see DEVLOG "Performance"). Install
into the copy:

```
G=~/games/thief_2
mkdir -p $G/OSM/sq_scripts
cp phase0-spike/HeadTrackSpikeAuto.nut $G/OSM/sq_scripts/
cp phase0-spike/miss_all.dml           $G/OSM/
```

Remove (fully undoes the experiment; edit the repo copies, then re-`cp`):

```
rm $G/OSM/miss_all.dml $G/OSM/sq_scripts/HeadTrackSpikeAuto.nut && rmdir $G/OSM/sq_scripts
```

Every New Game in a copy that has these installed runs the 10-second cycle, so remove
them before testing anything else (for example a binary patch). As of 2026-09-20 they
are **not installed** (moved out for the Stage B patch experiment); reinstall with the
commands above.

## Run

```
WS=3 tools/run-wine.sh      # game on i3 workspace 3; omit WS to tile it on your workspace
```

Wine on the VNC display, in a virtual desktop, software `llvmpipe` renderer, ~30 s to
the main menu. Details, flags and quirks: DEVLOG "Environment quirks". Stop it with
`wineserver -k` (never `pkill -f Thief2.exe`, which kills your own shell). The game must
be started with its own directory as the working directory; the launcher does this.

Then **Start a New Game, never Continue.** Mission dbmods apply only when a mission
loads fresh, not when a savegame loads (the save's object data overrides the mission).

## What to look at

1. Phase 0: aim at an item (for example in the first cell) until it is highlighted, and
   stand still.
2. Phase 1: is it still highlighted? Does the view change at all?
3. Phase 2: is it still highlighted? (Observed: it is not.)
4. Press W in any phase to see which way Garrett walks (observed: along his body
   heading, not the screen).

Note: a use/frob input makes the engine return the camera to the player by itself, so a
frob attempt during phases 1-2 resets the camera; check the highlight, not the click.

## Checking the log (agent side)

`Debug.Log` output does land in `Thief2.log` next to the exe (the ext4 copy's, not the
Steam install's). It is overwritten on every launch, so copy it out first if needed.

```
grep -n "HeadTrackSpike" ~/games/thief_2/Thief2.log
```

Expected: a `phase 0 NORMAL` line at mission start, then `end of phase N: camera
parent=... (player=..., marker=...)` lines and `captured eye pose`. "HeadMarker not
found" means the DML did not apply (not a New Game, or the file is missing). Squirrel
compile errors appear in the log at mission load; runtime errors are caught and logged
as `ERROR in Advance()`.

## Lessons learned (do not re-derive)

- **Squirrel `SetOneShotTimer` periods are in SECONDS**, not milliseconds. The first
  version passed `5000` and did nothing for 18 minutes.
- Yaw is the third component of a facing vector (`z`, "heading") in degrees;
  `Camera.GetFacing()` uses the same convention as `Object.Facing()`. The *player
  object's* facing has heading only (pitch 0); pitch lives in the camera.
- `Camera.LockMovement` made no difference to the result. Its parameter is documented
  as `move_allowed` in one file and `movement_locked` in another, so avoid it.
- `Camera.DynamicAttach()` to a marker at the exact camera pose leaves the view and the
  frob highlight unchanged; only changing the *direction* moves the frob target.
- The engine resets the camera to the player on use actions, so any real head-look has
  to re-assert itself every tick (or the code path has to be patched).
- On-screen text: `DarkUI.TextMessage(text, color, timeout)` works (the user saw the
  phase announcements). The timeout unit is *assumed* to be milliseconds; the display
  duration was not checked.

## Still open (see DEVLOG, "Phase 0 spike" and "Stage B map")

Weapon-aim direction, mouse-look while detached, per-tick marker updates, pitch,
first-person body visibility, and a native-Windows cross-check.
