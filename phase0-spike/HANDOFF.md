# Handoff: run the Phase 0 spike (written 2026-09-20)

Read `README.md` first (CLAUDE.md requires it), then this. This file is the
"where we are and what to do next" for the spike; `SETUP.md` is the original
procedure and is **partly stale** (see "Stale bits" below).

> **UPDATE (later 2026-09-20): the spike has now run.** Two things changed vs the
> text below. (1) The original script never fired because Squirrel timer periods
> are **seconds**, not ms; fixed (`5`/`8`). (2) Observed result: movement follows
> the body, not the offset camera (details and caveats: DEVLOG, "Phase 0 spike:
> first observed result"). Wherever this file says the result is unobserved, or
> lists "input is the first real risk", read it as superseded: New Game, the
> mission, and W/keyboard all work over VNC. Still open: real aim test,
> **Control run also done (3-phase cycle script, now installed): frob targeting
> follows the camera direction** (DEVLOG, 2026-09-20 control run). That points
> away from a scripted Stage A and toward Stage B (binary). Still open: real weapon aim,
> mouse-look, per-tick marker updates, pitch, first-person
> body visibility, native Windows check.

## The one question

Does the head-tracking approach have a scripted route (Stage A, OSM) or does it
need a binary/memory hook (Stage B)? The spike answers it by attaching the
render camera to a dummy object facing +30 deg off the player and seeing
whether the player's *movement* follows the body or the screen. Full design:
`(a local planning note, not in the repository)`.

## State right now

- Repo `master` @ `5bafd21`, working tree clean apart from this file.
- **The spike is installed in exactly one place and its result has never been
  observed.** Both files are in the game's native ext4 copy, byte-identical to the
  repo copies (verified with `cmp`):
  - `~/games/thief_2/OSM/sq_scripts/HeadTrackSpikeAuto.nut`
  - `~/games/thief_2/OSM/miss_all.dml`
  `OSM/` is the install's `uber_mod_path`; `miss_all.dml` in a mod dir is the
  documented way to patch every mission (`doc/dbmod-sample.dml`, "Mission dbmods").
- **The Steam install is clean.** The game is run from that ext4 copy because the
  Steam install on `/mnt/d` is minutes slow to load (DEVLOG "Performance"). The
  spike files were originally installed in the Steam install too and were removed
  on 2026-09-20 (checked identical to the repo copies first), so native play is
  unaffected. Don't reinstall them there.
- **Side effect to remember:** every New Game / mission load *in the ext4 copy*
  gets the +30 deg camera toggle every 8 s. Delete both files when the spike is
  done (or if it is paused for long):
  `rm ~/games/thief_2/OSM/miss_all.dml ~/games/thief_2/OSM/sq_scripts/HeadTrackSpikeAuto.nut && rmdir ~/games/thief_2/OSM/sq_scripts`
  Edit the spike files in the repo, then `cp` them into the ext4 copy.
- **Confirmed under Wine:** a New Game logged `OSM: HeadTrackSpikeAuto: mission
  started, first toggle in ~5s.`, so the DML applies and the Squirrel script
  loads and runs. The camera behaviour (the actual test) has not been observed.
- The game now runs under Wine on the VNC display, in a tiled i3 window, on the
  GPU. That was the previous session's work; details in DEVLOG (2026-09-20).
  It has only been observed as far as the **main menu**. Nothing beyond that
  (New Game, a mission, input) has been tried.

## How to launch

```
WS=3 tools/run-wine.sh        # game alone on i3 workspace 3
```
Defaults (2026-09-20): runs from `~/games/thief_2` (ext4), software
`llvmpipe` renderer (~80-99 fps logged vs 2-13 with `d3d12`), `SIZE` 640x480 (the
user's edit; the screen is 1280x720, so 800x600 also fits, 1024x768 will not),
Wine tracing off. For
a frame rate: `T2_WINEDEBUG=+fps tools/run-wine.sh > fps.txt 2>&1`, then grep
`wglSwapBuffers`. An ambient `WINEDEBUG` is deliberately ignored.
Then `i3-msg workspace 3` to look at it (the user views via VNC; tell them
before switching their view, and switch back to workspace 1 after).
- Takes ~30 s to reach the menu. `intro` is already skipped (`skip_intro` in cam.cfg).
- Stop with `wineserver -k`. **Never** `pkill -f Thief2.exe` (kills your own shell).
- The user's VNC screen is **1280x720** (not 1920x1080; see DEVLOG, Tools). If it
  shrinks below that: `DISPLAY=:1 xrandr --output VNC-0 --mode 1280x720`.
- Screenshots: `ffmpeg -loglevel error -y -f x11grab -video_size 1280x720 -i :1 -frames:v 1 out.png`
  then Read the PNG. Only the visible i3 workspace is captured, and a
  fullscreen game window hides the terminal, so keep the game off the user's
  workspace unless you are capturing.
- `Thief2.log` (next to the exe) is **overwritten every launch**. Copy it out
  (scratchpad) before relaunching if you need it.

## Plan for this session

1. Launch, confirm the game still reaches the main menu.
2. **Input is the first real risk** (never tested): mouse and keyboard inside a
   Wine virtual desktop over VNC. New Game needs a menu click; the test itself
   needs W/A/S/D held and ideally mouse-look. Options, cheapest first:
   - Ask the user to click through New Game themselves over VNC and try WASD.
     The engine reads DirectInput/raw mouse, and VNC only sends absolute pointer
     positions, so mouse-*look* may not work; keyboard movement should.
   - Drive it yourself: `xdotool` is **not installed** (needs
     `sudo pacman -S xdotool`, user runs it via `!`); with it you can send keys
     and clicks and take screenshots between steps.
   - Fallback: the user runs the game natively on the Windows host (the original
     plan, no Wine) and reports what they see. The Windows desktop isn't
     reachable from here, so this needs them at that machine.
3. Start a **New Game** (never Continue: mission dbmods don't apply to savegames).
4. Confirm the mod actually loaded *before* judging behaviour: grep
   `Thief2.log` for `HeadTrackSpikeAuto`. Expected lines: "mission started,
   first toggle in ~5s", then "offset ENGAGED" / "RELEASED" every 8 s. If you
   see "'HeadMarker' not found", the DML didn't apply. If there is no such line
   at all, look for Squirrel compile/load errors in the log.
5. While "ENGAGED" is active, hold W and compare: where the screen points vs
   where Garrett actually walks. Screenshots before/after a second of walking
   work as evidence. Get the user to confirm what they see; the result decides
   Stage A vs B.
6. Record the result (DEVLOG session log), including which stage it points to.
   Remove the two installed files unless a follow-up run is planned.

## Caveats worth knowing before you trust the result

- **Movement is a proxy for aim.** The plan doc's original test fired a bow at
  a target; `SETUP.md` swapped it for movement so it needs no props. If movement
  is *coupled* to the camera, Stage B is settled. If movement is *decoupled*,
  that is encouraging but does not prove weapon aim is decoupled; a real aim
  test (a weapon in a mission that gives you one) is the stronger check. Say so
  in the log rather than declaring Stage A on movement alone.
- **Script and DML are untested.** Nothing has ever parsed them. API names
  (`Object.Position`, `Object.Facing`, `Object.Teleport`, `Object.Named`,
  `Camera.DynamicAttach`, `Camera.LockMovement`, `Debug.Log`) come from
  `doc/script/*.txt` and NVScript docs, not from a run.
- Yaw axis unconfirmed: script adds 30 to `facing.z` (heading). If the view
  doesn't visibly turn left/right, try `.y` or `.x` in `offset_facing`.
- `Camera.LockMovement(1)`'s effect on mouse-look is unknown, and it may itself
  change the outcome; consider repeating with it removed.
- `Debug.Log` landing in `Thief2.log` (not a separate file) is assumed, not seen.
- Untested in this Wine setup: whether the game pauses or stops presenting when
  its window is unfocused or on a hidden i3 workspace. Timings (5 s, then 8 s
  toggles) run on game time, so a paused game may miss its windows.
- Wine is 32-bit-on-new-WoW64 with GL capped at 4.3; the Wine engine is a
  different runtime from real Windows, so a surprising result (crash, odd
  camera) is worth re-checking natively before blaming the engine.

## Stale bits in SETUP.md

- Step 2 says Wine fails the video-card check and to launch natively. That was
  the missing-libGL/wrong-working-directory problem, since solved; Wine is now
  the default route (fallback above).
- It still says nothing about `tools/run-wine.sh`, the virtual desktop, or
  screenshots. Update it once the spike has actually run.

## Things not to redo

- GPU acceleration via the d3d12 driver is confirmed working but **too slow to
  use** for this game (2-13 fps in a mission); the default is now `llvmpipe`.
  The `DRI3` warnings are harmless either way.
- The slow load and low fps were diagnosed (9p filesystem, a leaked `WINEDEBUG`,
  the d3d12 driver). Don't re-investigate; DEVLOG "Performance (2026-09-20)".
- The windowed/i3 launcher works; don't re-investigate fullscreen or the
  virtual-desktop full-path requirement (DEVLOG quirks section).
