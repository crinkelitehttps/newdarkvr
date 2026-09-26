# Thief 2 / NewDark Engine Hacking Notes

> **Development log** (this file was the project README.md until 2026-09-25). References to "README" in the
> entries below mean this file. The player-facing overview is now [../README.md](../README.md), the technical
> summary [HOW_IT_WORKS.md](HOW_IT_WORKS.md).

## Start here (state as of 2026-09-22)

**Read `HANDOFF.md` next: the current task, what to check first, next steps, commands, and hard-won facts.**

**Headline:**
1. **Head-tracking works** (yaw/pitch, OpenTrack over the network, user-verified with a real webcam): only the
   drawn view is offset; movement and the held weapon stay on the body, frob follows the view, arrow/aim
   direction is untested (static reading says the body). Plain desktop use (no headset, no stereo) is what
   the user is actually using day to day, and calls it excellent: "The frame rate is very smooth and the
   tracking works well."
2. **Side-by-side stereo works in Wine** (user: "beautifully"): `STEREO=sbs tools/run-wine.sh`. `headlook.dll`
   draws the scene twice per frame, camera shifted +-`stereo_ipd`/2, each eye squeezed into half the screen.
   HUD/menus are still drawn once at full width. Details: the "stereo SBS" session-log entry.
3. **Reference marks (2026-09-22): built and user-confirmed working AND accurate** ("works like a charm";
   then, separately, "The marks are accurate"). Amber ticks at the screen edges mark the body's true
   heading/pitch while head-look has the view turned away from it -- extend the ticks inward and they cross
   at the point an arrow would fly. On by default (`show_heading_marks=1` in `headlook.ini`). `fov_deg`
   (the screen-position constant, default 90, a guess) was NOT changed from its default and the user still
   called the marks accurate, so the default appears correct or close to it for this game/resolution setup
   -- not proven exact, not re-derived from the binary, but no longer flagged as a known-uncalibrated guess.
   Works in both plain head-look and stereo. See "reference marks" in the session log.
4. **A headset test was tried and did not work** (untriaged); this is not the user's current focus.
   The WMR/SteamVR/Desktop+ path is unresolved (see the headset session-log entry's open questions); the
   `tools/hmd_bridge/` pose bridge itself was not reported as the problem.
5. **OpenVR direct submit (2026-09-23): built, NOT yet run on the headset.** `stereo=openvr` /
   `run_openvr.bat`: `headlook.dll` itself sends each eye image to SteamVR (`IVRCompositor::Submit`, via a CPU
   copy into D3D11) and takes the head pose from it; replaces Desktop+ and `hmd_bridge.exe`. See the
   2026-09-23 session-log entry.
6. **XInput controller support (2026-09-22): built, NOT yet deployed or tested in-game.** A new,
   separate feature/directory (`xinput_joy/`, produces `dinput.dll`) satisfies the engine's own
   original-2000 legacy DirectInput joystick support (`LoadLibraryA("dinput.dll")`, dead code until now
   -- see "Active: XInput controller support" under Goals) with an XInput-backed synthetic joystick:
   left stick drives the engine's existing analog `+joyforward`/`+joyxaxis` binds, right stick drives
   `rudderturn`/free, and a configurable chord button (default LB) gives every other button (and the
   left stick's 4 directions) a second, distinct bindable identity in the game's own "Customize
   Controls..." menu. No exe patching at all -- unlike `headlook.dll`, the engine loads this DLL
   itself. Compiles clean; not yet copied into the game folder or run.

**How it runs**
- Game (Wine): `tools/run-wine.sh` (Wine, virtual desktop, VNC display `:1`, from the ext4 copy
  `~/games/thief_2`). That copy holds the loader-patched `Thief2.exe` (stock one is
  `Thief2.exe.orig-backup`), `headlook.dll` and `headlook.ini`. The Steam install is pristine.
- Laptop chain: OpenTrack (UDP over network -> `127.0.0.1:4242`) -> `tools/opentrack_relay.py`
  -> PuTTY local forward 4243 (session to `<desktop-ip>:2222`) -> the DLL's TCP listener.
- Rebuild/redeploy: `headlook/build.sh`; exe via `python3 patches/install_loader.py --dll
  headlook.dll`; copy exe, DLL, ini into the game copy; the DLL loads at process start (restart
  the game), the ini reloads live every 2 s. Log: `~/games/thief_2/headlook.log`.
  Deploy a rebuilt DLL into a running game's folder with `cp x.new && mv x.new headlook.dll` (atomic rename).
- Headset copy: `tools/hmd_bridge/build.sh`, then `tools/hmd_bridge/install_vr_copy.sh [--ini]`; on Windows
  `run_openvr.bat` (direct submit) or the older `run_vr.bat` (bridge + Desktop+).

**Uncommitted:** only the user's own edit in `tools/run-wine.sh` (`SIZE` default now `1280 720`, the old
`640 480` line commented out); ask before committing it. Everything else is committed. The yaw/pitch sign
defaults (-1, so OpenTrack's "Pre-invert" boxes stay unticked) are committed but UNTESTED in the un-inverted
state; the user's live `~/games/thief_2/headlook.ini` still uses the old setup (Pre-invert ticked, +1).

**Open items:** why the headset test didn't work (untriaged: SteamVR/WMR itself, Desktop+ setup, or native
Windows rendering); `fov_deg` calibration for the reference marks (user hasn't tried them yet); sound
direction; an arrow/aim test (the marks may help with this one); roll (built, off, untested) and position
(received, unused); save/load and cutscene regression; HUD/menus not split per eye; latency (plan in an
earlier entry, deferred); Quest/WinlatorXR route (researched only).

**Habits that matter here:** `pkill -f` can match your own shell (use `[x]pattern` in its own call);
never `pkill -f Thief2.exe`; `wineserver -k` (then wait) before relaunching, or an old instance causes an X
`BadWindow` error; the game must start with its own directory as CWD; reading the game's memory from
outside is blocked (use the DLL's log); ask before stopping or restarting the user's running game. More in
"Environment quirks".

## Target

- Game install: `/mnt/d/SteamLibrary/steamapps/common/thief_2`
- Main binary: `Thief2.exe` — this is the **NewDark** engine executable (a
  community-maintained fork/patch of the original 2000 Dark Engine binary,
  historically developed by Le Corbeau / Team TTLG and still updated today).
- Editor binary: `DromEd.exe` (same engine family, level editor front-end).

### Verification (2026-09-18)

- `Thief2.exe`: PE32, Intel i386, 7 sections, linker version 9.0, timestamped
  **2025-05-16** (compile time) — confirms this is a modern patched build, not
  the original 2000 executable (which would predate this by 25 years).
  - `DllCharacteristics`: DYNAMIC_BASE, NX_COMPAT, TERMINAL_SERVICE_AWARE
  - Binary contains strings `dark_version`, `version_failed`, and mission
    version-check error text, consistent with the known NewDark version-gating
    behavior.
- `DromEd.exe`: same folder, dated 2025-05-15, same engine family.
- Install directory contains NewDark-specific config files: `cam.cfg`,
  `cam_ext.cfg`, `cam_mod.ini`, `dark.cfg`, `DARKINST.CFG`.
- `Tools/` directory contains folders explicitly named `OldDark Installs`,
  `Thief1 Gold Newdark Dromed`, and `Thief2 Newdark Dromed` — direct
  confirmation of NewDark branding in the installed toolset.
- No literal `"NewDark"` string found inside `Thief2.exe` itself (checked via
  `strings`), so the executable doesn't self-report the name, but circumstantial
  evidence (timestamp, surrounding tooling, config files) is conclusive.

## Binary Handling

We alter the executable locally, never in the Steam install directory, and
never commit the raw `.exe` bytes to git (see `.gitignore`) — they're
copyrighted game software, diff meaninglessly in git, and would bloat the
repo forever. Instead:

- `bin/Thief2.orig.exe` — pristine reference copy, chmod 444 (read-only).
  Never edit this. Restore from here if a patch attempt goes wrong.
- `bin/Thief2.patched.exe` — working copy we actually modify.
- `patches/` — the actual versioned artifacts: patch scripts, byte-offset
  notes, assembly diffs, anything needed to reproduce a patch from the
  original. This is what goes in git, not the binary.
- To test a patch in-game, copy `bin/Thief2.patched.exe` over the real
  `Thief2.exe` in the Steam install directory (back up the install's copy
  first if it ever diverges from `bin/Thief2.orig.exe`).

### Reference hashes

| File | SHA256 |
|---|---|
| `bin/Thief2.orig.exe` (== install copy as of 2026-09-18) | `af56a109a51ac9100a15da72cd403170a266736313679029fae1c222ebfba684` |

## Environment

- Game files live on a Windows-formatted drive mounted at `/mnt/d/...` under
  WSL2 (Linux 6.18 kernel, `microsoft-standard-WSL2`).
- This project directory (`~/projects/Thief2-newdark`) is separate
  from the game install and is git-tracked; the game install itself is not
  under version control here.

## Goals

### Active: OpenTrack head-tracking (pitch/yaw)

Make Garrett's rendered view pitch/yaw follow head-tracking data from
OpenTrack (sent over UDP), independent of aim/body direction. Full design
doc: `(a local planning note, not in the repository)`.

**Status (2026-09-21): working with a real OpenTrack over the network.** Laptop webcam ->
OpenTrack -> `tools/opentrack_relay.py` -> ssh tunnel -> `headlook.dll` in the game. The user's
verdict: natural, smooth and intuitive head tracking that fits the existing mechanics;
frobbing is natural, the arms and weapons are simple but accurate, no graphical anomalies.
Yaw and pitch only; roll (built, off by default) and position (received, unused) are
untested or unused. Still to do: sound direction, an arrow/aim test, save/load and
cutscene regression checks, a hotkey if wanted. See "headlook" in the session log.

Key research findings (2026-09-18):
- No existing head-tracking/TrackIR/FreeTrack support anywhere in the binary
  or docs; `disable_headtracking` is unrelated (NPC AI look-at-player only).
- No existing DLL-injection/proxy mechanism in this install. `dmm.exe` is
  just a fan-mission manager GUI (no process injection). `cam.cfg`/
  `cam_ext.cfg`/`cam_mod.ini` are mundane engine-config files — "cam" is the
  Dark Engine's internal historical codename, unrelated to camera hardware.
- `Thief2.exe` dynamically loads legacy `dinput.dll` (not `dinput8.dll`) via
  `LoadLibraryA`/`GetProcAddress` for joystick support, and no such file
  ships in the install — a viable classic proxy-DLL hijack target if we end
  up needing a runtime/memory-patch approach.
- The engine has one official extension point: native **Object Script
  Module** (`.osm`) DLLs, e.g. `archer.osm`/`convict.osm` already ship with
  the game, with documented script services (`Camera`, `Object`, `Physics`,
  etc. — see `doc/script/*.txt` in the game install). A sample OSM project
  ships at `doc/script/t2sample-osm.7z` (not yet extracted — needs a `.7z`
  tool, not installed in this WSL/Arch environment).
- `Camera` service has `GetPosition`/`GetFacing`/`DynamicAttach`/
  `IsRemote()`/`LockMovement` (the last is new in exactly this v1.28 build)
  but no `SetFacing`. `Object.Teleport(obj, pos, facing, ref_frame)` can set
  an object's facing from script every tick.
- **Open question the whole project hinges on:** does the engine's
  weapon-fire/aim logic read the player object's own facing, or the active
  camera's facing? If the latter, decoupling head-look from aim via
  `Camera.DynamicAttach` to a separate object won't work, and a real
  binary/memory hook (touching only the render transform) is needed instead.
  This is being tested empirically — see `phase0-spike/` in this repo.
  **First result (2026-09-20, movement only):** with the camera
  `DynamicAttach`ed to an object yawed +30 deg, the player's *movement* stayed on
  the body's heading, not the screen's. Encouraging for Stage A (scripted), but
  movement is only a proxy for aim and the caveats below are unresolved.
  **Second result (2026-09-20, frob test) points the other way:** with the
  camera offset, a frob-highlighted object lost its highlight although still in
  frame and still in front of the body, i.e. interaction targeting appears to
  follow the *camera*. So movement is decoupled but frob targeting looks
  coupled; weapon aim is untested and may well follow the camera too.
  **Control run (2026-09-20) confirms the frob reading:** marker at the *exact*
  captured camera pose with 0 deg offset keeps the highlight; the same pose with
  +30 deg loses it. So attaching the camera elsewhere is harmless to frob, and
  frob targeting follows the camera's *direction*. A scripted camera offset
  therefore also moves interaction targeting: it decouples movement only.
  Weapon aim is still untested, but frob is the strongest evidence so far that
  this route (Stage A) will not give head-look independent of aim.
  **Next step:** decide between a weapon-aim check and starting Stage B
  (binary: find where the frob/aim ray reads its facing vs where the renderer
  does); caveats in the 2026-09-20 spike log entry; `phase0-spike/HANDOFF.md`
  has the launch steps.

#### Stage B map (static RE in Ghidra, 2026-09-20; addresses in `bin/Thief2.orig.exe`, image base 0x400000)

Names are mine (the binary has no symbols, only MSVC RTTI class names); decompiler
output for these is approximate (custom calling conventions), so treat details as
leads to verify, not facts, until a patch confirms them.

- **The camera state is one struct** pointed to by global `DAT_00a2a988`, 0x28
  bytes, saved/loaded with the game (`FUN_0054c180` writes, `FUN_0054c0e0` reads).
  Layout as used by the code: `+0x00` mode (0 = on the player), `+0x08/0x0C/0x10`
  position x,y,z (floats), `+0x14`/`+0x16`/`+0x18` three 16-bit angles (0..65535 =
  360 deg; matches the script's (bank, pitch, heading) order), `+0x20` attached
  object id. The player object id is `DAT_00a2a984`.
- **Camera attach** (what `Camera.DynamicAttach`/`StaticAttach` do): `FUN_0059ee50`
  (attach to the player), `FUN_0059eed0` (sets mode 4) and `FUN_0059ef50` (sets
  mode 3) store the object id at `+0x20`, call `FUN_0059ea90` (refresh) and
  `FUN_005a42a0` (sends the `CameraDetach`/`CameraAttach` script messages).
  Script wrappers: `FUN_004029d0`, `FUN_00402a20`. Which of modes 3/4 is Static
  and which is Dynamic is not pinned down.
- **Frob targeting reads that struct directly.** `FUN_0058eae0` (called from
  `FUN_0044d420`) copies `+8..+0x18` into globals `0x92c8d4..0x92c8e4`, turns the
  angles into a forward vector with `FUN_00678b30` (generic angles-to-matrix,
  >100 callers, so not a useful landmark by itself), and `FUN_0058e850` /
  `FUN_0058ea90` score candidate objects against that ray using the `PickBias` /
  `PickDist` config vars (created in `FUN_0058ec20`). This is the code-level
  reason the frob highlight followed the offset camera.
- **"camSynch" (`FUN_0054e710`) and "bowFlex" (`FUN_0054e930`) never run in normal
  play.** Registered in `FUN_0054ece0`, they look like view/frustum setup but a
  debugger run (below) never saw them execute (no read of the heading from
  `0x54e732`, and a breakpoint on the experiment-1 stub never fired). Ignore them.
- **The real per-frame scene render is `FUN_005cee30`** (called twice from
  `FUN_005cf290`; the observed heading read `0x5cef1f` is inside it). EBP = the
  camera struct. It copies position, attached object id and the three angles into a
  **local location on its stack**, stores a pointer to it in the global
  `DAT_0092DC44` ("current view"), builds the view matrix with `0x678B30`
  (EDI = the local angles, ESI = global matrix `0x92D8F4`) and draws the frame
  (`FUN_005cea90`, zoom = camera struct `+4`, forced to a constant in attach modes
  3/4). Site details:
  `5cef34: mov [esp+0x24],eax` (bank | pitch<<16), `5cef38: mov [esp+0x28],cx`
  (heading), `5cef3d: mov [0x92dc44],edx`, `5cef43: call 0x678b30`. **That local
  copy is the render-only copy the plan wants.**
- **Who touches the struct's heading (`+0x18`) per frame (debugger read/write
  watchpoint, ~38 frames of a mission):** `FUN_0059f170` (`add [ebx+0x14/16/18]`:
  the mouse-look update, a writer), `FUN_00536690` (sets an angle field), the
  matrix routine `0x678B30` and `FUN_00678C00` (matrix to angles) from some caller,
  `FUN_0051e4a0`, `FUN_005cee30` (render), `FUN_0058eae0` (frob ray), a reader at
  `0x57d407` (function unidentified), `FUN_0059efd0` (a writer, from `FUN_0054c6c0`)
  and `FUN_0054c0e0/180` (save/load copy).
- **An engine-side camera hook already exists:** both callbacks call
  `DAT_00a2a9c8(&pose, &matrix)` if it is non-null. It is *written* by seven
  functions in the `0x469xxx-0x46bxxx` block (`FUN_0046b490`, `FUN_00469340`,
  `FUN_004695d0`, `FUN_00469a90`, `FUN_004696d0`, `FUN_0046a970`, `FUN_0046aa00`),
  i.e. other camera modifiers (possibly leaning or head effects; not yet read).
  Prefer patching the read site over taking this pointer, or we would fight them.
- **Projectile/weapon launch also reads the camera struct.** `FUN_0054f000`
  (log tag "Event Launch"/"LaunchVel"; args roughly launcher, projectile, speed,
  flags, offset) gets its origin and direction from `FUN_0054eec0`, which returns
  the camera struct's position and angles when the object is the player *and the
  camera mode is 0*, and the object's own position/angles otherwise. So aim
  (arrows and the like, by static reading only, not yet observed in game) is
  camera-struct-driven like frob. Any head-look patch must leave that struct alone
  and change only what is drawn.
- **Where the picture's view comes from: answered 2026-09-21.** The engine sends
  *pre-transformed* vertices (`SetFVF(0x1c4)` = XYZRHW|DIFFUSE|SPECULAR|TEX1, then
  `DrawPrimitiveUP`), so there is no D3D view/projection matrix to edit; it projects in its own code.
  **Correction:** the earlier "no `SetTransform` call sites" search was flawed. MSVC loads the vtable slot
  into a register and does `call reg`, so `call [reg+0xB0]` never matches; search for
  `mov reg,[reg2+0xB0]` followed by `call reg`. Real D3D9 device calls exist (driver code around
  `0x5fe000-0x61a000`); the raw `IDirect3DDevice9*` is in the global `0xA36040`.
- **Experiment 1 (2026-09-20): +30 deg yaw in camSynch = no visible effect,
  because camSynch never runs.** (`render_yaw_offset.py` first targeted `0x54E7D4`.)
  The user saw the original behaviour; the debugger showed why.
- **Experiment 2: the same offset in `FUN_005cee30`'s local copy.**
  `patches/render_yaw_offset.py --yaw 30` now replaces the 5-byte `mov [esp+0x28],cx`
  at `0x5CEF38` with a `call` to an 11-byte stub in the padding at the end of `.text`
  (`0x71C220`, `.text` VirtualSize raised): `add cx,0x1555 / mov [esp+0x2c],cx / ret`.
  **Result (user, 2026-09-21): it works.** The view is offset by 30 deg. The item
  frobbed is the one at the *centre of the view*. The held weapon model is drawn at
  the *body's* heading, not the view's. Sound direction not yet checked (quiet
  environment). Movement stays on the body (Phase 0). Projectile/aim direction not
  yet tested in game.
  **Correction to an earlier prediction:** I expected frob to stay on the body,
  because `FUN_0058eae0` builds the frob ray from the camera *struct*. It follows the
  *view* instead. The script test could not tell the two apart (struct pose and
  render pose were both the marker); this one can. So frob targeting is driven by
  something derived from the render pose (`DAT_0092DC44` / matrix `0x92D8F4`, or
  the pick candidates come from the renderer), not the struct. Code path not traced.
  Net effect of the current hook: view = head, frob = view (natural look-to-interact),
  movement and the weapon model = body, launch direction = body by static reading.
- **Debugger recipe that works (Wine 11.17, new-WoW64):** run the game under
  `winedbg` *inside* the virtual desktop and send the debugger's console output to a
  file through `cmd`:
  `wine explorer /desktop=Thief2,640x480 cmd /c "winedbg --file Z:\...\cmds.txt Z:\...\Thief2.exe \"-game_screen_size=640 480\" -multisampletype=0 -postprocess=0 > Z:\...\out.txt 2>&1"`
  (winedbg started straight from `explorer` loses its output; game args with spaces
  need the inner quotes). The `cmds.txt` file is executed line by line and `cont`
  blocks until a stop. Useful commands: `break *0xADDR` (module `thief2` loads at
  0x400000, unrelocated), `print *(int*)0xADDR` (casts and derefs work),
  `rwatch *(int*)(*(int*)0xa2a988+0x18)` (a hardware watchpoint that fires on reads
  AND writes; the reported EIP is the instruction *after* the access), `info reg`,
  `delete N`, `detach`. `x/xw ADDR` does not parse. `quit` hangs, so end runs with
  `wineserver -k`. The hardware watchpoint fired reliably (300 stops in seconds).
  Not to be confused with the earlier dead end (attaching to a *running* game).
  To learn a caller inside `0x678B30`, print `*(int*)($ebp+4)` at the stop.

### Active: XInput controller support

Separate from the OpenTrack/VR work above. Give Thief II Xbox/XInput controller support by satisfying
the engine's own **original ~2000 Dark Engine** legacy joystick support (not a NewDark addition --
absent from `doc/new_config_vars.txt`/the NewDark changelog): `Thief2.exe` calls
`LoadLibraryA("dinput.dll")` + `GetProcAddress("DirectInputCreateA")` at startup, and no such DLL ships,
so this whole subsystem has been dead code. The engine already has default binds
(`bind joy_axisy +joyforward` etc. in `dark.bnd`/`user.bnd`) and a real in-game Options -> Controls UI
("Joystick: On/Off", "Joystick Left/Right: Rotate/Strafe", a generic "Customize Controls..." bind-capture
screen that already accepts joystick buttons/axes/hat) -- so the plan is a `dinput.dll` implementing
just enough legacy DirectInput, backed by XInput, rather than a custom binding UI of our own. Full design
doc: `(a local planning note, not in the repository)`.

**Status (2026-09-22): working, user-verified on native Windows** ("Excellent work", "That works extremely
well"). New directory `xinput_joy/` (`xinput_joy.c`, `didefs.h`, `build.sh`, `xinput_joy.ini`,
`install_joy_copy.sh`), builds `dinput.dll` with the same mingw32 toolchain as `headlook/build.sh`
(now also linking `-luser32` for `SendInput`, see mouselook below). Needs no exe patching at all
(`bin/Thief2.orig.exe` and `patches/install_loader.py` untouched) since the engine loads this DLL
itself, by its own filename. Tested by deploying straight into the pristine Steam install
(`/mnt/d/SteamLibrary/steamapps/common/thief_2`) and running natively on the Windows host -- see
"XInput controller support" in the session log for the two real crashes found and fixed along the way
(both instructive, not guesses) and the tuning passes since. **Not yet tested under Wine at all** --
`dinput.dll`+`xinput_joy.ini` were also just deployed into the ext4 Wine sandbox copy
(`~/games/thief_2`), alongside the already-working `headlook.dll`/patched exe, to try running
head-tracking and controller support together; the one real unknown there is whether Wine/WSL2 can see
the physical Xbox controller as an XInput device at all (untested), independent of whether
`SendInput`-based mouselook works under Wine (likely fine, no real-hardware dependency).

Key design points (full detail in the plan doc, `(a local planning note, not in the repository)`):
- Left stick -> `lX`/`lY` (engine's existing `joy_axisx`/`joy_axisy` binds -> `+joyxaxis`/`+joyforward`,
  i.e. analog movement speed, the headline ask) -- **confirmed working**, analog forward/back speed
  control in a mission. A separate, smaller deadzone (`stick_deadzone`, default 0.08) than the right
  stick's is applied by us directly (radial, with rescale so full deflection still reaches 100%) --
  the engine's own `joystick_deadzone` config var alone wasn't enough to stop centering drift.
- Right stick: **not** fed into the classic joystick R/Z axes -- this engine's joystick support (original
  ~2000 code) has no analog look/pitch bind at all, only mouselook ever drove pitch, so getting a
  twin-stick feel needed a different mechanism. `mouselook_enabled=1` (default) makes the right stick
  drive synthetic relative mouse movement (`SendInput`, yaw + pitch together, only while the game's own
  window has focus) instead. Needed its own, larger deadzone (`mouselook_deadzone`, default 0.15,
  separate from the left stick's) -- a little leftover movement-stick creep is unnoticeable, but the
  same amount of residual camera drift was very noticeable and had to be tuned out separately.
- Button/chording ("shift") scheme: one configurable physical button (`shift_button` in the ini, default
  Left Shoulder) gives every other tracked button, plus the left stick's 4 directions, a second, distinct
  virtual button identity while held -- so e.g. "A" and "LB+A" bind to different actions in the existing
  "Customize Controls..." screen with zero engine-side changes. 27 of the 32 classic-`DIJOYSTATE`
  `rgbButtons` slots used. D-pad -> the POV hat, not the button bank (`joy_hat*` binds, free).
  **Chord-vs-solo timing fix:** reporting "modifier held alone" the instant the modifier went down made
  the bind-capture screen grab it before a second button could be pressed to form a chord. Fixed by
  deferring: while the modifier is held, only chord targets report (their shifted id); "modifier alone"
  is reported as a one-tick press-pulse only at the moment the modifier is *released*, and only if
  nothing was chorded during that hold. Tradeoff, flagged deliberately: an action bound to
  modifier-alone can only ever be a tap/toggle, never a sustained hold.
- Wine wrinkle (Wine only, not real Windows): Wine's own builtin `dinput.dll` can shadow a native-named
  one depending on the prefix's per-DLL override list -- `tools/run-wine.sh` now sets
  `WINEDLLOVERRIDES=dinput=n` scoped to its own launch, so the deployed copy wins. Not yet exercised
  (no Wine test run of this feature yet).

**Not yet done:** a Wine test of either the controller alone or combined with head-tracking; the exact
button-count ceiling above `joy10` (whether the engine really accepts values up to `joy27`, used by the
shift bank, was never separately confirmed against the "manually add `bind joy15 ...`" test the plan
proposed -- it's working in practice via the menu's own bind-capture, which is stronger evidence than
that test would have been, so this is no longer flagged as blocking, just as not independently verified);
`mouselook_sensitivity`'s default (18.0) is an untuned guess, not yet dialed in; regression check with
`joystick_enable 0`/pad unplugged not explicitly redone since the crash fixes.

### Backlog / general

_(other potential directions: static analysis, format research, etc., to be
filled in as they come up)_

## Tools

- **Wine 11.17** (with an existing `~/.wine` prefix) + **winedbg** — for
  running/testing the patched exe from this WSL/Arch environment if needed.
- **GNU binutils 2.47** (`objdump`, `nm`) — confirmed working against the
  PE32/i386 target for static inspection (imports, sections, version info).
- **Mesa 26.2.3, vulkan-icd-loader, vulkan-tools, winetricks** — installed
  2026-09-18/19 (user ran the `sudo pacman` commands; no non-interactive sudo
  from the agent). Mesa provides `libGL`/`libEGL`, giving Wine software
  OpenGL. Mainline Arch mesa has **no** WSL "dzn" (D3D12) Vulkan driver, so
  `vulkaninfo` reports "Found no drivers" — no GPU acceleration.
- **TigerVNC** — `Xvnc :1` + `i3` already running on this Arch instance
  (localhost-only, port 5901, started by `vncsession`); the shell has
  `DISPLAY=:1`. The user views/controls it from their laptop. This is how
  the game window gets seen — launch Wine apps with `DISPLAY=:1`.
  **The user's VNC screen is 1280x720** (confirmed by the user 2026-09-20; live
  `xrandr` mode `1280x720`). Do not "restore" it to 1920x1080: `Xvnc` was started
  with `-geometry 1920x1080`, which is only its initial size (the source of the
  earlier wrong figure), and the viewer session is sized 1280x720. Consequences:
  `xrandr` recovery is `--mode 1280x720`; screenshots use `-video_size 1280x720`;
  a virtual desktop taller than 720 (e.g. `SIZE="1024 768"`) will not fit the X
  screen (untested inference from the "desktop bigger than the screen" error
  seen earlier), and `SIZE="1280 720"` is cropped slightly by the i3 bar unless
  the window is on its own workspace. `640 480` and `800 600` fit.
- **xorg-xrandr, ffmpeg** — `xrandr` restores the VNC screen mode after a Wine
  mode change; `ffmpeg -f x11grab` takes screenshots of `:1` (2026-09-20).
- The native-Windows-host route (Cheat Engine / x64dbg / Ghidra on Windows)
  is no longer the plan for running the game; see the 2026-09-19 log entry.
- Not yet installed in WSL/Arch: Python3/pip, radare2, Ghidra, IDA,
  mingw-w64, gdb, a `.7z` extractor. Install via `pacman`/AUR as they become
  needed for a specific task (see plan doc for when each is expected).

## Environment quirks (learned the hard way)

- Topology: user's Windows laptop (PuTTY / VNC viewer) -> LAN -> sshd inside
  this WSL2 Arch instance on a Windows desktop host. The Windows host's own
  desktop is not reachable from here.
- `binfmt_misc` entry `DOSWin` routes **every** `.exe` execution through
  `/usr/bin/wine`. There is no genuine `WSLInterop` entry, so running
  Windows binaries "natively" from this shell is not currently possible; an
  earlier test suggesting real interop worked was a false positive (it was
  Wine, visible as `fixme:heap:...` output). `/init` and `/run/WSL/*_interop`
  do exist, so real interop might be re-registerable, but that is unproven.
- Never use `pkill -f Thief2.exe` from a Bash tool call — the pattern
  matches the tool's own shell and kills it. Use `wineserver -k` instead.
- **Always launch the game with its own directory as the working directory**:
  `cd /mnt/d/SteamLibrary/steamapps/common/thief_2 && DISPLAY=:1 wine Thief2.exe`.
  The engine loads `cam.cfg`/`dark.cfg`/`DARKINST.CFG` and all `.\RES`, `.\OSM`,
  `.\MODS` paths relative to the CWD (not the exe's folder). From any other
  directory it finds no config, silently creates a blank `cam.cfg` there (that
  is the stray file that once landed in this repo), and crashes at startup.
  `Thief2.log`, by contrast, is written next to the exe regardless.
- **Launcher: `tools/run-wine.sh`** (2026-09-20: runs the game inside a Wine
  virtual desktop so i3 can tile it; `SIZE` default is now `640 480` (edited by
  the user; was `1280 720`), optional `WS=3` moves the window to i3 workspace 3.
  Defaults after the performance work: `GAME_DIR=~/games/thief_2`,
  `GALLIUM_DRIVER=llvmpipe`, tracing via `T2_WINEDEBUG` only, e.g.
  `T2_WINEDEBUG=+fps tools/run-wine.sh > fps.txt 2>&1`). Config vars can be overridden per
  launch as `-name=value` (multi-number values need a space inside one quoted
  arg, e.g. `"-game_screen_size=800 600"`; a comma form is not honoured).
  Verify what took effect in the `misc config` block of `Thief2.log`. Findings:
  `game_screen_size` must be in the engine's mode table (640x480, 800x600,
  1024x768, 1280x720, 1600x900, 1920x1080, ...; **960x540 is not** and falls
  back to 640x480); `-force_windowed` pins the start mode to 640x480, so
  don't use it; `game_full_screen=0` is accepted into the config but ignored
  (D3D CreateDevice still logs `fs=1`); `-multisampletype=0 -postprocess=0`
  cut the heaviest software rendering costs.
- **Windowed / i3 (2026-09-20):** see the log entry below for why. Launch
  `Thief2.exe` as `wine explorer /desktop=Thief2,WxH 'Z:\full\path\Thief2.exe'`
  — the full Windows path is mandatory (a relative `Thief2.exe` makes explorer
  sit there and never start the game, with no error). Never run a plain
  (non-virtual-desktop) game launch on the VNC display: it resizes the real
  screen and Wine never puts it back. Recover with
  `DISPLAY=:1 xrandr --output VNC-0 --mode 1280x720` (the screen size used here;
  see the TigerVNC bullet under Tools). Even unrelated Wine
  commands (`winepath`, `wine notepad`) appear to re-apply that stale saved
  mode, so run `xrandr` if the VNC view suddenly shrinks.
- **GPU rendering (2026-09-19):** software `llvmpipe` is *not* required. Arch
  mesa ships `/usr/lib/dri/d3d12_dri.so` (OpenGL over D3D12, the host RTX 3050
  via `/dev/dxg`); `GALLIUM_DRIVER=d3d12` selects it (now the launcher
  default). Proof: with `WINEDEBUG=+d3d_caps`, wined3d logs `GL_VENDOR
  "Microsoft Corporation"` for d3d12 vs `GL_RENDERER "llvmpipe (LLVM 22.1.8,
  256 bits)"` for the default; `MESA_LOADER_DRIVER_OVERRIDE=d3d12` did NOT
  switch drivers. The game's own `descr`/`GTX 470` adapter name is wined3d's
  generic stand-in and cannot tell the two apart. The removed PCI-ID registry
  spoof (`VideoPciVendorID/DeviceID`) is gone from `~/.wine`. Frames are
  read back and pushed to Xvnc (no DRI3), so expect some copy cost, but 3D is
  on the GPU. The Vulkan "dzn" driver is still absent, so DXVK is not an
  option; this OpenGL path (wined3d) is the one to use.
  **Superseded for speed (2026-09-20):** the d3d12 driver is correct but far
  too slow for this game (2-13 fps in a mission). `llvmpipe` is now the launcher
  default; see the performance entry below.
- **Performance (2026-09-20) — read before blaming the engine.** A New Game took
  ~5 min and play was 5-15 fps. Three independent causes, all environmental:
  1. **Game on `/mnt/d` (9p).** Thief II opens huge numbers of small files
     (`.crf` archives, `res/*.crf`, the `mods/NewT2SFX` loose `.wav`s, ...).
     Each `open()` on drvfs costs ~1.6 ms warm (~14 ms cold) vs ~0.01 ms on
     ext4. During a load `Thief2.exe` sat in state `D` at 0-3% CPU with the
     machine ~93% idle, issuing ~1,000 tiny reads/s. **Fix:** run from a native
     copy, `~/games/thief_2` (`cp -a` of the Steam install, 2.0 GB, ~10
     min the first time). New Game then loads near-instantly. The launcher now
     defaults to it (falls back to `/mnt/d` if the copy is missing). The Steam
     install stays the pristine reference; the copy is a snapshot (see below).
  2. **Leaked `WINEDEBUG=+d3d9,+d3d`** exported in the interactive shell (left
     over from the 2026-09-19 debugging; in no rc file, so it survived only in
     that session). The launcher did `${WINEDEBUG:--all}`, so it inherited it
     and traced every D3D call (~38,000 lines in the first 15 s). **Fix:** the
     launcher now reads `T2_WINEDEBUG` (default `-all`) and ignores ambient
     `WINEDEBUG`. Untraced, a `/mnt/d` load was ~2.5 min (20:58:10 -> 21:00:46);
     the traced-run figure is the user's ~5 min, not separately timed. Run
     `unset WINEDEBUG` in any shell that still has it.
  3. **GL driver for the 3D scene.** With `WINEDEBUG=+fps` (a cheap once-a-second
     channel; log lines `wglSwapBuffers @ approx N fps`): `GALLIUM_DRIVER=d3d12`
     ran ~100 fps in the menu but **2-13 fps in a mission**, one thread pinned at
     100% CPU. `llvmpipe` (12-core software) logged **~80-99 fps** in the
     mission (6-7 cores busy) and felt "much better" to the user. Menu fps was
     ~100 either way, so the frame copy to Xvnc is not what breaks d3d12; the
     scene's uploads/draws are. Not diagnosed further (likely per-frame
     texture/buffer uploads or state changes through Mesa's d3d12 layer).
  - **Open:** the user still *perceives* only ~20-30 fps with llvmpipe while Wine
    logs ~97, and suspects the VNC path. Plausible (Xvnc ~25-30% CPU, every frame
    is copied and encoded to a remote viewer; `+fps` counts `wglSwapBuffers`, not
    what reaches the viewer's screen) but **untested**. A test that would tell:
    compare against the game on a display without VNC, or lower the VNC
    quality/encoding, or measure Xvnc update rate.
  - **Method that worked:** `/proc/<pid>/io` (`syscr`, `rchar`), `/proc/stat`
    (user/sys/iowait) and per-process state sampled every 2 s while the user
    clicked New Game; `top -H` to see which thread is pinned; micro-benchmarks
    of `open`/`stat` on `/mnt/d` vs ext4. A load with low CPU, idle machine,
    state `D` means I/O latency, not compute.
  - **Dead end:** `winedbg` attach (`attach 0x<pid>` / `bt all`) on the running
    game killed it and gave only ntdll/wined3d frames (new-WoW64). Don't sample
    a live game that way.
  - The copy is a snapshot: config edits, saves (`SAVES/`) and `Thief2.log` are
    separate in each tree, and the Phase 0 spike files
    (`OSM/miss_all.dml`, `OSM/sq_scripts/HeadTrackSpikeAuto.nut`) live **only in
    the copy** (removed from the Steam install the same day). Re-sync deliberately if the Steam install
    changes (`rsync -a` of the changed files, not a blind overwrite).

## Session Log

### 2026-09-18
- Initialized project (`git init`, README.md, CLAUDE.md).
- Verified target binary and engine identity (see Verification above).
- Set up binary workflow: copied `Thief2.exe` into `bin/Thief2.orig.exe`
  (read-only reference) and `bin/Thief2.patched.exe` (working copy), added
  `.gitignore` to keep `.exe`/`.dll` files out of git, created `patches/`
  for versioned patch artifacts, recorded original SHA256 hash.
- Scoped first feature: OpenTrack head-tracking (pitch/yaw), decoupled from
  aim. Researched engine internals (script services, existing config/tools,
  import table) — see Goals above and the full plan doc referenced there.
- Tried launching `Thief2.exe` via Wine (11.17, WSLg) to test/interact with
  the game directly from this environment — **failed**: the game's own
  hardware-compatibility check rejects Wine's D3D9 device ("not compatible
  with my video card"), even though Wine successfully created a D3D9 device
  at 1920x1080 per Wine's own logs. Confirmed the game launches fine natively
  from the Windows host. **Decision: use the native Windows host to run/test
  the game; this environment is for editing files, git, and static
  analysis only.**
- Redesigned `phase0-spike/` to avoid DromEd entirely: discovered the
  install already has a configured `uber_mod_path` (`.\OSM`, containing
  community OSMs `squirrel.osm`, `NVScript.osm`, etc. — `NVScript.html` in
  there is likely the public OSM SDK docs Stage A will need) and a
  documented DML dbmod format (`doc/dbmod-sample.dml`) that can create new
  objects with scripts attached in *any* existing mission via a plain text
  patch file — no editor required. Wrote `HeadTrackSpikeAuto.nut` (attaches
  to a throwaway Marker object, auto-toggles a +30° camera-yaw offset via
  `Camera.DynamicAttach` every ~8s, logs status via `Debug.Log`) and
  `miss_all.dml` (creates the throwaway Marker objects and attaches the
  script — applies to any mission on a fresh load). Copied both into the
  install's existing `OSM/` mod folder (`OSM/sq_scripts/` and `OSM/`
  directly) — additive only, nothing existing was touched, fully removable
  by deleting those two files. **Note:** mission dbmods only apply on a
  fresh mission load (New Game / mission transition), not when continuing a
  savegame — see `phase0-spike/SETUP.md`.
- Not yet run — needs someone to start a New Game and report which way
  movement goes (real body facing vs. wherever the camera points) while the
  offset is engaged. This decides Stage A vs. Stage B.

### 2026-09-19
- **Root cause of the Wine "video card not compatible" error found:** with no
  `libGL`/`libEGL`/`libvulkan` installed, Wine's log showed `Failed to load
  libGL.so.1 ... OpenGL support is disabled`, so wined3d fell back to a
  `no3d` null adapter. `CreateDevice` still returned `D3D_OK` on it (that is
  why `Thief2.log` looked healthy), but the game rejected the adapter. Not a
  DirectX-redistributable problem, and the registry PCI vendor/device ID
  spoof (`HKCU\Software\Wine\Direct3D`, NVIDIA 0x10de / 0x1180) did nothing —
  that spoof is still set in `~/.wine` and is probably unnecessary.
- After the user installed mesa (+ vulkan-icd-loader, vulkan-tools,
  winetricks), Wine gets a GL context (software; `MESA-EGL: DRI3 error`
  warnings) on the VNC display `:1`. The "not compatible" dialog is gone from
  the Wine log, but the game now **crashes on startup**: `Unhandled page
  fault on write access to 00000000 at address 004F6DB2`. Disassembly of
  `bin/Thief2.orig.exe` there is `or BYTE PTR [eax], dl` (0x4F6DB2) —
  looks like setting a bit in a bitmap/mask buffer whose base pointer is
  NULL, preceded by bit-index math (`and ecx,0x80000007` / `shl dl,cl`).
  Cause not yet determined (candidates: a D3D surface lock or allocation
  returning NULL under wined3d software GL, or a Wine-specific behaviour).
  Next: get a backtrace via `winedbg`, check which caller reaches 0x4F6DB2,
  and try `WINEDEBUG=+d3d9,+d3d` for a failing call just before the crash.
- Mesa's WSL D3D12 ("dzn") Vulkan driver is not in Arch's mainline `mesa`;
  building it is a separate, uncertain effort and is not being pursued.
- Crash analysis (run under `winedbg --file <cmds> Thief2.exe`, cmds =
  `cont`/`bt`/`info reg`/`quit`; scratch disassembly via `objdump -d -M intel`
  on `bin/Thief2.orig.exe`): the fault is **deterministic** — identical
  registers/stack every run, `EIP=0x4F6DB2`, `EAX=EDX=ESI=EDI=0`,
  return address `0x54610A` (a `call [vtbl+0x4C]` dispatch of an event with
  code `0x80000003`, in the function at `0x5460F0`).
  - The faulting function (`~0x4F6C6E-0x4F6E1C`) sets bits in a per-column
    occupancy bitmap: base pointer in global `0xA2A79C`, active bit range in
    `0x7FFB70` (lo) / `0x7FFB6C` (hi), spans reserved via helper `0x4F7B90`
    (returns E_FAIL if a bit is already set).
  - `0xA2A79C` is only ever written in two places: `0x4F5BB4` (reset to 0) and
    `0x4F7CB9` (after `malloc`, `call 0x70157D`). `0x4F7B90` returns early
    *without allocating* when the requested range equals the current range
    (`0x4F7C4F`), so an empty/zero range at init leaves the bitmap NULL and
    the next set-bit dereferences it. Something feeds a zero-size range under
    Wine that real Windows does not.
  - **Ruled out:** fullscreen vs windowed (`-force_windowed` on the command
    line is not honoured — log still says `fs=1`; a Wine virtual desktop,
    `wine explorer /desktop=Thief2,1920x1080 ...`, crashes identically);
    missing DirectX DLLs; GPU vendor/device ID.
  - Open leads: identify what `0x80000003` events are and who calls
    `0x5460F0` (return addresses on the stack: `0x546194`, `0x5FCCA9`,
    `0x705C68`); Wine 11.17 runs this 32-bit exe in new-WoW64 mode (no
    32-bit host libs) — a possible factor.
- Quick Wine experiments (2026-09-19), each launched via a scratch helper and
  judged by whether `Unhandled page fault ... 004F6DB2` appears within 20s.
  **All crashed identically**: Windows version override Win7, WinXP (main
  prefix restored to Win10); `WINEDLLOVERRIDES=dinput,dinput8=` (DirectInput
  disabled); a brand-new Wine prefix (`WINEDLLOVERRIDES=mscoree,mshtml=`, so
  no leftover registry/GPU-ID spoof). So this is not a Wine config issue —
  the zero-size range comes from something in the environment Wine presents
  (display/monitor metrics, GL/D3D caps, window messages) or a genuine
  Wine-vs-Windows behaviour difference. Next step is tracing the source of
  the `0x80000003` event / zero range in the disassembly.
- **ROOT CAUSE FOUND (working directory).** Ghidra 12.1.2 headless
  (`tools/ghidra/DecompileAt.java`, project in gitignored `ghidra-proj/`) +
  winedbg breakpoints traced it:
  - `FUN_004f6be0` is the object-ID manager's database-message handler
    (`0x80000000..4`; `"ObjVec"` is its save chunk). `DAT_007ffb70`/`6c` are
    the object-ID range `lo`/`hi`, `DAT_00a2a79c` the allocation bitmap.
  - Startup order (breakpoints): component Init `FUN_004f5b90` zeroes
    lo/hi/bitmap -> DB msg 0 twice -> `FUN_0041c620` builds a range from the
    config vars **`obj_min` / `obj_max`** (via config parser `FUN_0067f9b0`,
    which leaves the value untouched if the var is undefined) and calls
    `SetRange` (`FUN_004f7b90`) -> DB msg 3 (`FUN_004f6be0`) writes into the
    bitmap.
  - Under Wine the vars were undefined, so `SetRange({0,0})` hit its "range
    unchanged, return 1" early exit without allocating; msg 3 then wrote
    through the NULL bitmap. On native Windows (reference log from a native
    run: `game dark` / `obj_min -8192` / `obj_max 2400` / `max_refs 32000`)
    the config is loaded, so it works.
  - Why undefined: my Wine launches used the wrong CWD (scratch dir / this
    repo), so no `cam.cfg`/`dark.cfg` were read. Launched with CWD = the game
    dir, Thief2.exe starts, loads config, and stays running. The earlier
    "video card not compatible" dialog was the separate libGL/no3d problem.
  - The other Wine experiments above (Win7/XP, dinput override, fresh
    prefix, virtual desktop) were red herrings; the GPU-ID spoof in `~/.wine`
    is likewise unnecessary and can be removed.
- Reference: native Windows startup log (RTX 3050, Win10 19045) for
  comparison lives in the game dir's `Thief2.log` whenever the game was last
  started natively; it is overwritten on every launch.

### 2026-09-20 (GPU check + windowed/i3 launcher)
- **Correction (added later the same day):** the user's VNC screen is
  **1280x720**, not 1920x1080. Screen-size figures below (1920x1080, "3 windows
  across 1920", "window 1920x1061") were written assuming 1920 and are stale;
  see the TigerVNC bullet under Tools for what to use instead.
- **GPU acceleration confirmed, not broken.** `WINEDEBUG=+d3d_caps` still logs
  `wined3d_guess_card_vendor Received unrecognized GL_VENDOR "Microsoft
  Corporation"` (=> `d3d12` Gallium driver; llvmpipe would say "Mesa"). The
  `MESA-EGL: DRI3 error: Could not get DRI3 device` warnings are only because
  Xvnc has no DRI3: rendering is on the GPU, finished frames are copied to the
  X server. Harmless; can't be removed without a different X server. Wine also
  logs `GL version 4.6 is not supported on wow64, using 4.3` (new-WoW64 cap).
- **Why the game is fullscreen/untileable:** Wine sets
  `_NET_WM_STATE_FULLSCREEN`, which i3 honours (`fullscreen_mode=1`), *and*
  changes the real X display mode via RandR. Under Xvnc that mode change
  resizes the whole VNC screen (CRTC 1920x1080 -> 800x600) and Wine does **not**
  restore it after `wineserver -k`; i3's `VNC-0` output stays 800x600 while the
  root window stays 1920x1080.
- Virtual desktop attempt 1 failed for two independent reasons: (a) the X
  screen was already stuck at 800x600 (`err:explorer:initialize_display_settings
  Failed to set primary display settings`, desktop bigger than the screen), and
  (b) `Thief2.exe` given as a relative name is never started by explorer (a
  `notepad` control launched fine). After `xrandr --output VNC-0 --mode
  1920x1080` (needs `xorg-xrandr`, now installed; hand-rolled libXrandr ctypes
  `XRRSetCrtcConfig` got `BadValue`) and the full `Z:\...` path, the game runs
  in the virtual desktop and the real screen stays 1920x1080 through the run
  and after `wineserver -k` (polled with `xrandr`).
- Wine still flags the *desktop* window `_NET_WM_STATE_FULLSCREEN` (+ maximized
  flags), so i3 makes it fullscreen and it covers everything. Clearing it with
  `i3-msg '[class="explorer.exe" title="^Thief2 - Wine Desktop$"] fullscreen
  disable'` once the window exists sticks for the whole run (Wine does not
  re-request it). `i3-msg` reports `success:true` even with no match, so the
  launcher polls `i3-msg -t get_tree` for the title first.
- Result: `tools/run-wine.sh` now launches into a normal tiled i3 window; the
  Thief II main menu renders in it on the GPU (d3d12). The virtual desktop is a
  **fixed** WxH: in a narrower tile (3 windows across 1920) it is cropped; in a
  bigger one the remainder is black. `WS=3 tools/run-wine.sh` puts it alone on
  workspace 3 (window 1920x1061) so the whole 1280x720 canvas shows. The game
  takes ~30 s to reach the menu, and right after a workspace move the window
  shows stale pixels until the first repaint.
- Not tested: mouse/keyboard input inside the virtual desktop (VNC absolute
  pointer vs the engine's DirectInput mouse), and a running mission.
- Screenshots for checking results: `ffmpeg -f x11grab -video_size 1280x720 -i
  :1 -frames:v 1 out.png` works on the VNC display (only shows the visible
  workspace). Beware: a fullscreen game window hides the terminal.

### 2026-09-20 (performance: slow load and low fps)
- Symptom: New Game ~5 min, in-game 5-15 fps. Diagnosed and mostly fixed; full
  detail, numbers and caveats are in "Performance (2026-09-20)" under
  Environment quirks.
- Found and fixed: game on 9p `/mnt/d` (now run from ext4 copy
  `~/games/thief_2`), leaked `WINEDEBUG=+d3d9,+d3d` (launcher now uses
  `T2_WINEDEBUG`), slow `d3d12` GL driver (launcher now defaults to `llvmpipe`).
- Result: level load near-instant; in-mission ~80-99 fps by Wine's `+fps`
  counter, though ~20-30 *perceived* over VNC (cause untested, suspected VNC).
- `tools/run-wine.sh` edited (defaults above); `SIZE` default 640x480 was the
  user's uncommitted change and is left as they set it.
- VNC screen size: the README's 1920x1080 was wrong for this setup; the user
  confirmed it is 1280x720. Docs updated (recovery command, screenshot size, the
  TigerVNC bullet). Nothing on the system was changed.
- Phase 0 spike cleanup: the spike's `miss_all.dml` and `HeadTrackSpikeAuto.nut`
  had ended up in two trees. Verified both were byte-identical to the repo
  copies (`cmp`), then **removed them from the Steam install** (`OSM/` is back
  to its original contents; the `sq_scripts/` dir we created was `rmdir`'d), so
  native play and the pristine reference are unaffected. They remain **only in
  the ext4 copy** (`~/games/thief_2`), where the spike is run. The DML
  and script do load and run under Wine: `Thief2.log` on New Game showed
  "mission started, first toggle in ~5s". The camera-offset result itself is
  still unobserved. To clear the last copy:
  `rm ~/games/thief_2/OSM/miss_all.dml ~/games/thief_2/OSM/sq_scripts/HeadTrackSpikeAuto.nut && rmdir ~/games/thief_2/OSM/sq_scripts`.

### 2026-09-20 (Phase 0 spike: first observed result)
- **Bug found and fixed first:** the spike did nothing for 18+ minutes because
  Squirrel's `SetOneShotTimer` period is in **seconds** (float), not
  milliseconds; the script passed `5000`/`8000`. Evidence: API-reference.txt
  declares `float fPeriod`, the ReadMe example is `SetOneShotTimer("TestTimer", 5)`,
  the shipped samples use `("HealTick", 30)`. Confirmed by the fix: after changing
  to `5`/`8` the log shows `timer fired` / `offset ENGAGED` / `RELEASED`
  alternating every ~8 s. Also added a `timer fired` log line. (Squirrel run-time
  errors go to mono, and to the log file in the game exe; none appeared, the timer
  simply had not elapsed.)
- **Observed by the user over VNC** (fresh New Game, Wine, 640x480 window): about
  8 s in, the camera turns ~30 deg to the left. Pressing W, Garrett's model moves
  forward along the direction he faced *before* the camera changed; on screen
  that reads as moving forward-and-right. So **movement is decoupled from the
  camera**. Heading (`facing.z`) is the correct yaw axis.
- **Also seen (user's description, not separately verified):** Garrett's own
  model is visible while the camera is on the marker, i.e. the view became
  third-person-ish. A real head-look feature needs a first-person view, so this
  is a likely Stage A problem (hide the player body, or place the marker at the
  eye position and check for model/head clipping).
- **Not established, do not call this Stage A yet:**
  1. Movement is a proxy; weapon/aim direction was not tested.
  2. ~~`Camera.LockMovement(1)` was on during the test and may affect the
     outcome~~ **Resolved:** run 2 (both `LockMovement` calls removed) gave the
     same result — user: "no change". Movement stays on the body heading; the
     camera stays frozen at the offset (marker is placed once), Garrett's model
     walks away from the camera position, and normal play resumes on release.
     Log: 16 ENGAGED / 15 RELEASED, no errors, no "HeadMarker not found".
  3. Mouse-look while engaged was not tried (VNC sends absolute pointer only).
  4. The marker is teleported once per engage; per-tick updating (needed for
     head tracking) is untested, as is pitch.
  5. Run under Wine (new-WoW64), not native Windows.
- Spike files remain installed in the ext4 copy only (fixed script included).
- **Frob test (user, later 2026-09-20, LockMovement removed):** looking straight
  at a frobbable item highlights it in normal mode. When the camera shifts +30
  deg the item is unhighlighted "because the camera is not looking directly at
  it", even if it is still in frame. Right-click during the offset makes the
  camera reset to normal, so whether frob *works* under the offset could not be
  checked (the user thinks it would not). **Reading:** frob targeting probably
  follows the camera facing, unlike movement. **Confounds:** the marker is placed
  at the player's position, not the camera's eye position, so a shifted ray
  origin could contribute; the yaw could also just move the object out of the
  frob cone. **Also learned:** a use/frob input makes the engine return the
  camera to the player itself, so a real feature would have to re-attach every
  tick (or that path needs patching).
  - **Control to run next:** engage with a 0-deg offset (marker at the player's
    position and facing). If the highlight survives, the position is not the
    cause and the yaw/camera-direction reading holds; if it drops, the position
    artifact is real. Better still, put the marker at the true eye position
    (`Camera.GetPosition()` before attaching) with the +30 yaw.
- **Control run (frob vs camera pose), same day.** `HeadTrackSpikeAuto.nut` was
  rewritten to cycle three 10 s phases with on-screen text
  (`DarkUI.TextMessage`): 0 NORMAL (camera on player; the exact pose is captured
  at the end), 1 CONTROL (camera on the marker at that pose, yaw +0), 2 TEST
  (same pose, yaw +30). It re-arms its timer before doing work and wraps the work
  in `try/catch`, and logs the camera's owner and pose at every phase end.
  - **User's observation:** item highlighted in phase 0, still highlighted in
    phase 1, **not** highlighted in phase 2.
  - **Conclusion:** frob targeting follows the camera direction, not the player
    object's facing and not a position artifact. (Subjective highlight reading,
    but the log shows the camera really was on the marker with the pose/yaw
    intended: e.g. cycle 1 cam facing `(0, 337.4, 307.29)` in CONTROL, z `337.29`
    in TEST, player facing z `307.29`.)
  - **Log coverage:** 11 cycles; 8 were clean (camera on the marker at the end of
    both marker phases). In cycles 2, 4 and 5 the camera was back on the player
    mid-phase, i.e. something reset it (consistent with the earlier right-click
    reset). Not tied to specific user actions.
  - **Facts learned from the log:** (a) `Camera.GetFacing()` uses the same degree
    convention as `Object.Facing()` (heading matched to <0.01 deg), (b) the
    **player object's facing has pitch 0 and only heading**; camera pitch lives
    in the camera (e.g. camera `(0, 337.4, 307.3)` vs player `(0, 0, 307.3)`), so
    the first two runs' marker (built from `Object.Facing`) had no pitch, and
    where mouse pitch goes while the camera is detached is untested, (c) camera
    z is eye height: `~5.6-11.6` on this map.
  - **Ideas noted, not done:** `Camera.LockMovement`'s parameter is named
    `move_allowed` in `doc/script/Camera.txt` but `movement_locked` in the
    Squirrel reference, so its meaning is ambiguous; run 1's use of it is moot
    since run 2 removed it with no change.

### 2026-09-20 (Stage B started: static RE of camera vs frob)
- User chose the binary route after the frob control run. New helper
  `tools/ghidra/StringXrefs.java` (regex over defined strings -> referencing
  functions; ~11 s per headless run with `-process -noanalysis`), used with the
  existing `DecompileAt.java`. Invocation:
  `/opt/ghidra/support/analyzeHeadless ghidra-proj Thief2 -process -noanalysis -scriptPath tools/ghidra -postScript DecompileAt.java <out> <hexaddr | x:hexaddr>...`
  (the output file is appended to, so delete it between runs).
- Method that worked: strings (`frob`, `camera`, config var names like
  `PickBias`, `user_camera_offset`, `player_camera_limit_%c`) -> functions ->
  cross-references of the globals they use. The MSVC RTTI names
  (`.?AVcCameraSrv@@` etc.) are present and help identify classes.
- Result: a map of the camera struct, the attach functions, the frob ray, and the
  two render callbacks; see "Stage B map" under Goals.
- **Stage B experiment 1 run (later 2026-09-20):** see "Experiment 1" in the Stage B
  map. Result: negative (no visible change). Deployed to the ext4 game copy only,
  with `Thief2.exe.orig-backup` beside it; the Phase 0 spike files were moved out of
  that copy (repo copies remain) so the test was clean. `phase0-spike/SETUP.md`
  rewritten to match reality (Wine launch, seconds-based timers, the 3-phase script,
  results, lessons). Side finding: `FUN_0054f000` (projectile launch) reads the camera
  struct, so aim is camera-driven by static reading.

### 2026-09-21 (Stage B: hook works; loader for the live DLL)
- **Experiment 2 succeeded** (see "Stage B map" > Experiment 2): a constant +30 deg in
  the render function's local angle copy turns the view only. Frob follows the view,
  the held weapon model and movement follow the body. Sound not yet checked.
- **Loader:** `patches/install_loader.py [--dll NAME]` builds `bin/Thief2.patched.exe`
  with the entry point's `call 0x7022A8` redirected to a 38-byte position-independent
  stub (in the `.text` padding at `0x71C220`) that calls `LoadLibraryA("<dll>")` through
  the exe's own import slot (`0x71F0D4`) and tail-jumps to the original target. Missing
  DLL = vanilla start. **Verified** with `T2_WINEDEBUG=+loaddll` and `wsock32.dll` as a
  stand-in: it loads after the CRT and before `d3d9`, i.e. at entry. The game dir's
  `Thief2.exe` currently holds this test build (loads `wsock32.dll`, harmless).
- **Why this loader and not OSM or a `dinput.dll` proxy:** OSMs must export
  `_ScriptModuleInit@20` and return a valid module object or the engine logs "init
  failed", and they load per mission; a `dinput.dll` proxy also fights Wine's DLL
  override order. A DLL loaded once at entry lives for the whole process.
- **Design for the live version (not built yet):** `headlook.dll` (32-bit) with
  (a) a thread receiving OpenTrack's UDP packet (6 little-endian doubles: x, y, z in
  cm; yaw, pitch, roll in degrees; port 4242 is the OpenTrack default; to be
  verified against real packets), smoothing/clamping into three 16-bit angle offsets;
  (b) at load, a runtime hook on `0x5CEF38` (checks the original 5 bytes first, works
  from the real image base so ASLR is fine) into a naked stub that adds the offsets
  to the local copy: heading, pitch and bank (roll), only while the camera struct mode
  is 0 (so cutscenes/remote cameras are untouched); (c) a small ini for axis
  inversion, limits and gain. **Blocked on a C compiler:** this box has only binutils
  (`as`, `ld`, both with pe-i386 support); no gcc/clang/mingw. Needs
  `sudo pacman -S mingw-w64-gcc` (user runs it).
- **Open questions for the live version:** where OpenTrack runs and how its UDP
  reaches the Wine process (laptop -> LAN -> this WSL instance); how the held weapon
  model should behave under head-look (currently stays on the body heading); whether
  projectiles (arrows) really fire along the body heading; sound listener direction.

### 2026-09-21 (headlook.dll: live head-tracking on loopback)
- **`headlook/`** (built with `headlook/build.sh`; `mingw-w64-gcc` was installed by the
  user 2026-09-21; output `headlook/headlook.dll` is gitignored):
  `headlook.c` (DLL), `headlook.ini` (settings), `build.sh`.
  - Loaded at process start by the `patches/install_loader.py` stub (build the exe with
    `python3 patches/install_loader.py --dll headlook.dll`).
  - **Hook installed at run time**, not baked into the exe: it checks the 9 original bytes
    around `0x5CEF38`, rebases from `GetModuleHandle(NULL)` (ASLR-safe), and redirects the
    store of the render function's local heading to a naked stub that adds yaw, pitch and
    roll (bank) offsets to the local copy, only while the camera struct mode is 0.
  - **Input:** OpenTrack "UDP over network" (6 little-endian doubles: x, y, z cm; yaw,
    pitch, roll deg) on UDP 4242, plus the same 48-byte records over TCP 4243 (for a
    relay; see networking). Smoothing (time-constant filter, `smoothing_ms`, default 25),
    per-axis sign/gain/limit, and `timeout_ms` (no data -> eases back to centre).
    Roll is off by default (`use_roll=0`); position is received but unused.
  - **`headlook.ini` is re-read every 2 s** (signs, gains, limits, smoothing, enabled,
    use_roll; ports need a restart). Verified live: flipping `yaw_sign` moved the view.
  - **`headlook.log`** next to the DLL: what loaded, hook status, listeners, first packet,
    a status line every 5 s while data flows (packet counts, applied offsets, and the
    game's own camera mode/angles as a diagnostic), data lost/resumed.
  - Deployed state of the game copy `~/games/thief_2`: patched `Thief2.exe`
    (loader), `headlook.dll`, `headlook.ini`; the stock exe is `Thief2.exe.orig-backup`;
    the Phase 0 spike files are not installed.
- **Verified by the user (fake tracker):** yaw and pitch sweeps look smooth; the held
  weapon is independent of the view "as desired"; movement is smooth; no artifacts or
  odd geometry. Convention observed: **+yaw turns the view LEFT, +pitch tilts it DOWN**
  (the values the fake tracker sent, applied with sign +1). Which sign OpenTrack's own
  numbers need is unknown until tested with a real head; the ini flips it live.
- **"Slightly down" report = false alarm.** With yaw swinging +-26 deg, the log showed
  the camera struct constant (mode 0, bank 0.0, pitch 0.0, heading 151.0) and applied
  pitch 0.0. The user then saw no pitching.
- **Networking (tested with the real laptop, 2026-09-21):** this WSL instance is in NAT mode
  (`wslinfo --networking-mode` = nat; `<wsl-ip>`; `.wslconfig` is empty), which cannot
  receive UDP sent to the desktop's LAN address, so the laptop uses a relay. Chain:
  OpenTrack "UDP over network" -> `127.0.0.1:4242` on the laptop -> `tools/opentrack_relay.py`
  (UDP 4242 -> TCP `127.0.0.1:4243`) -> a PuTTY local port forward (source 4243, destination
  `localhost:4243`, added to the existing session to `<desktop-ip>:2222`, the same session that
  already tunnels VNC; PuTTY's "Change Settings" adds it live) -> `headlook.dll`'s TCP
  listener on 4243. Needs Python 3 on the laptop (standard library only; winget or python.org).
  The DLL keeps one TCP client at a time and the newest wins, so do not leave a second relay
  connecting (e.g. a local test relay). WSL mirrored networking was not needed and not tried.
- **Test tools:** `tools/opentrack_sim.py` (fake OpenTrack: sweep or hold yaw/pitch, UDP or
  `--tcp`), `tools/opentrack_relay.py`.
- **Process notes:** `pkill -f` with a pattern that also appears in the same command line
  kills the calling shell (happened with `opentrack_sim`); use the `[o]pattern` trick and keep
  the pattern out of the rest of the command, or stop it in its own call. Reading the game's
  memory from outside (`/proc/<pid>/mem`) is blocked (`ptrace_scope=1`), so diagnostics go
  through the DLL's log. Alt+numpad codes do not work through the VNC session (typing `0176`
  in a terminal printed digits).
- **Open items:** real OpenTrack over the network and its signs; roll; position (x, y, z);
  sound listener direction; whether arrows fire along the body heading (static reading
  says yes, untested); a toggle/recentre hotkey (OpenTrack has its own); the held weapon
  stays at the body heading (currently liked); behaviour in menus/inventory/cutscenes
  (mode 0 only, so remote cameras are untouched); regression check across a save/load;
  native-Windows cross-check (the DLL uses only Win32 plus the UCRT).

### 2026-09-21 (later): real OpenTrack works; sign defaults changed
- **Real head-tracking test, user's words:** "natural smooth and intuitive head tracking
  seamlessly integrated into the existing game mechanics. Frobbing is natural, the arms and
  weapons are simply but accurately represented. There does not appear to be any graphical
  anomalies." (Sound still untested.)
- **The user's working setup:** the "Pre-invert" tickboxes for yaw and pitch on OpenTrack's
  Output tab (Options menu) ticked, the ini signs at +1, and limits of 50 deg yaw and 20 deg
  pitch.
- **Change made:** `headlook/headlook.ini` and the DLL's built-in fallbacks now default to
  `yaw_sign=-1` and `pitch_sign=-1`, so a new user does NOT have to tick the "Pre-invert"
  tickboxes for yaw and pitch on OpenTrack's Output tab (Options menu), which is what the user
  had to do before. `roll_sign` stays 1 (never tested with a real head). **Magnitudes
  deliberately unchanged** (`max_yaw=120`, `max_pitch=70`, gains 1): the useful range depends on
  the user's head, camera and taste. The ini comment names the OpenTrack setting. Only two
  combinations work per axis: Pre-invert ticked with ini +1 (the old setup), or unticked with ini -1
  (the new default). Ticked with -1, or unticked with +1, is backwards. Edits apply live.
- **Caveat:** the new defaults are inferred, not observed. Nobody has yet run OpenTrack with
  Pre-invert unticked and the ini at -1/-1. The user's live
  `~/games/thief_2/headlook.ini` was not touched and still matches their old setup
  (Pre-invert ticked, ini +1); to switch, untick Pre-invert in OpenTrack AND set the ini to -1.
- The deployed `headlook.dll` in the game copy is the earlier build (it differs only in the
  fallback defaults, which matter only when an ini key is missing). Rebuilt in the repo.

### 2026-09-21 (research only, no code): stereo VR and Android/Quest feasibility
- Desk-study of "stereo VR" and "run it on a Quest". Nothing was built or tested; the engine-side
  claims are from the notes above, the WinlatorXR facts are from web pages (search summaries and
  one fetch of its XrAPI page), so treat them as leads.
- **Stereo VR is mostly graphics plumbing, not more head-look RE.** Open engine questions: can
  `FUN_005cee30`/`FUN_005cea90` be drawn twice per frame without double-ticking state (why
  `FUN_005cf290` calls the render function twice is unknown); where the projection/FOV is built
  (unknown; symmetric wide FOV per eye is the workaround); HUD/2D overlay and weapon viewmodel
  placement. Suggested first milestone: side-by-side or anaglyph on the current Wine setup, no HMD.
  Optional next static-RE step: `FUN_005cf290`, `FUN_005cee30`, `FUN_005cea90`.
- **Android:** Winlator / GameHub / GameNative (Wine + Box64, DXVK or WineD3D, Turnip on Adreno)
  run 2000-era D3D9 games well by all accounts; no Thief II / NewDark report found.
- **WinlatorXR (Winlator fork for Quest/Pico) changes the plumbing picture.** Its "XrAPI" lets a
  Windows game or mod inside the container act as the VR app: head and controller pose arrive as
  space-separated floats over UDP `localhost:7872` (quaternions + positions, IPD, FOV, sync counter);
  the game draws stereo as side-by-side (left half = left eye) or alternate-eye, and paints a sync
  value as a shade of red in the top-left pixel; VR mode is switched on with a `MODE_VR` file under
  `Z:\tmp\xr\`. Per-game work is required (it is not an OpenXR runtime). Precedents exist
  (Halo CE, CoD4 ports, and an OpenTrack conversion). Docs give no eye resolution, FOV or D3D9
  statement. Headsets listed: Quest 3, 3S, Pico 4 Ultra; Quest 2 works with Turnip enabled.
- **Quest 1** (Snapdragon 835, Adreno 540, 4 GB, Android 10 after updates, support ended
  2024-08-31 but sideloading works): not listed anywhere found; my inference (not verified) is that
  Turnip does not target Adreno 5xx, so it is unlikely to work. Quest 2 is the realistic minimum.

Sources: https://winlatorxr.github.io/xrapi.html, https://github.com/WinlatorXR/WinlatorXR,
https://vr.org/articles/winlatorxr-half-life-2-quest-3-standalone-2026,
https://www.uploadvr.com/quest-1-can-no-longer-get-new-app-updates/

### 2026-09-21 (stereo SBS: built, diagnosed, working)
- **What it is:** `headlook.dll` gained a stereo mode. At start-up (only if `HEADLOOK_STEREO=sbs` or ini
  `stereo=sbs`) it patches the `call 0x5CEE30` at **`0x5CF2E2`** (the *hardware-render* branch of the frame
  handler `FUN_005cf290`, message `0x40`; `0x5CF335` is the software fallback, message `0x80`) to go to
  `hl_scene`. That draws the scene twice per frame: the render-copy stub now also adds a sideways shift
  (`g_eye_dx/dy`) to the local view position, then each eye is `StretchRect`ed into the left/right half of a
  temporary surface, which is blitted back. Half-SBS: each eye squeezed to half width. Only while camera mode
  is 0 (cutscenes/remote cameras draw normally). The right-vector convention was checked against the view
  matrix logged from `0x92D8F4` (its second row is the *left* vector `(-sin h, cos h)`; heading 0 = +X, CCW).
- **First attempt showed one mono view; the D3D-call trace explained it.** The engine draws the 3D scene into
  its **own offscreen render target** (it calls `SetRenderTarget` *inside* the scene call) and afterwards
  `StretchRect`s that surface over the back buffer at full size, then draws the 2D overlay and `Present`s.
  I had copied from the back buffer (empty) and written the composite there, and the engine's later full-size
  copy overwrote it with the last-drawn eye. Fix: after each pass take `GetRenderTarget(0)` (the scene
  surface), copy from it, and put the composite back into it. Then the engine's own copy carries the SBS
  image to the screen. Per pass the trace showed `SetRenderTarget, BeginScene, Clear, DrawPrimitiveUP x19,
  Clear, DrawPrimitiveUP x3, EndScene`. The whole scene is only ~22 draw calls (batched).
- **Gotchas:** the composite surface is default-pool and is created and released *every frame*, because
  one that outlives a frame would make the engine's later device Reset (menu <-> mission mode switches in
  `Thief2.log`) fail. The second eye needs `Clear(TARGET|ZBUFFER|STENCIL)` first. Run with
  `-postprocess=0 -multisampletype=0` (the launcher's defaults); the post-process chain and MSAA are untested.
- **Resolved:** in the trace the *right* pass lacked the `Clear + 3 draws` tail that the left pass had, so I
  suspected the held weapon was missing from the right eye. The user checked: **the weapon shows in both
  halves**, so the missing calls were something else (unidentified, harmless as seen). HUD and menus are still
  drawn once at full width after the composite. IPD scale (0.21 units) is unverified.
- **Diagnostic (kept, inert until used):** create a file `stereo_dump.now` next to the DLL while in a mission;
  the next stereo frame writes `stereo_1_left_pass.bmp`, `stereo_2_right_pass.bmp`, `stereo_3_composite.bmp`,
  `stereo_4_present.bmp` and logs the run-length-encoded D3D call order to `headlook.log` (it patches the
  device method table on first use). Convert the BMPs with `ffmpeg -i x.bmp -pix_fmt rgb24 x.png`.
- **Process notes:** a relaunch failed with an X `BadWindow` (`X_CreateWindow`) because an earlier
  `Thief2.exe` and its `wineserver` were still alive: `wineserver -k`, wait, relaunch. Deploy a rebuilt DLL
  into a running game's folder with `cp x.new && mv x.new headlook.dll` (atomic rename), never `cp` over it.
- **Next: test in a headset (user has a Windows Mixed Reality headset).** WSL/Wine cannot reach it, so the
  game would run natively on the Windows host. WMR is deprecated: removed in Windows 11 24H2; on Windows 10 /
  11 23H2 it works, and sources say SteamVR support for WMR ends **November 2026**. Zero-code display idea:
  SteamVR + Desktop+ (window overlay, head-locked, 3D mode; only Over-Under was clearly confirmed by search,
  check the Desktop+ user guide for Side-by-Side). Head pose needs a small bridge (HMD via OpenVR -> OpenTrack
  UDP packet to port 4242); OpenTrack itself was NOT found to have a SteamVR *input* (the OpenVR-OpenTrack
  project goes the other way, OpenTrack -> SteamVR). Not started.

Sources: https://www.uploadvr.com/windows-11-24h2-kills-windows-mr-support/,
https://roadtovr.com/unofficial-steamvr-windows-11-mixed-reality-driver/,
https://github.com/elvissteinjr/DesktopPlus/blob/master/docs/user_guide.md,
https://github.com/r57zone/OpenVR-OpenTrack

### 2026-09-21 (VR test on a Windows Mixed Reality headset: built, NOT yet run on the headset)
- **Goal:** try the SBS stereo in the user's WMR headset. WSL/Wine cannot reach it, so the game runs natively on
  the Windows host from a separate copy; display via SteamVR + Desktop+, head pose via a new bridge.
- **`tools/hmd_bridge/`** (see its `README.md` for the run steps): `hmd_bridge.c` reads the HMD pose through
  OpenVR (runtime-loaded `openvr_api.dll`, Valve SDK v2.15.6 C header, BSD-3, header committed, DLL fetched by
  `build.sh` and gitignored; app type Background = waits for SteamVR, `--launch` = Utility) and sends
  OpenTrack UDP packets to 127.0.0.1:4242, so `headlook.dll` needed no changes for it. OpenTrack-convention
  output (yaw + = right, pitch + = up, roll + = tilt right). Recentre on first pose and on Pause/Break or Scroll
  Lock (global; not F9, Thief's quick-load). Pose prediction `--predict-ms` (default 15).
  **Verified without a headset (under an isolated Wine prefix, no X):** `hmd_bridge.exe --selftest` passes:
  angle round trips, an independent textbook rotation (-30 deg about +Y gives yaw +30 = right; +20 deg about
  +X gives pitch +20 = up), and a 48-byte packet received intact (6 little-endian doubles). Not verified: real
  OpenVR init, the actual pose stream, Windows behaviour of anything.
- **`headlook.dll`:** new ini key `listen_loopback` (default 0) binds the UDP/TCP listeners to 127.0.0.1 only
  (no Windows Firewall prompt, not exposed to the LAN).
- **Logs to read after a headset run** (all readable from WSL at `/mnt/d/games/thief_2_vr/`): `hmd_bridge.log`
  (OpenVR connection state/errors, centring, a status line every 5 s), `headlook.log` (first packet, packet
  counts, whether stereo installed) and `Thief2.log` (native D3D startup, device creation lines).
- **Latency (discussed, not started):** estimated 80-150 ms head-to-photon now (unmeasured). Cheap: tune
  `--predict-ms`, lower `smoothing_ms`, keep the native frame rate high. Real fix: our own OpenVR overlay
  presenter that places a world-locked SBS overlay at the pose each frame was rendered with (rotation-only
  reprojection by the compositor); needs per-frame pose tagging + frame transfer, and a game FOV wide enough
  to fill the view. Same idea as WinlatorXR's sync-pixel handshake. Deferred by the user.
- **Windows-side game copy: `D:\games\thief_2_vr`** (`/mnt/d/games/thief_2_vr`), made with `tar` from the ext4
  copy (logs, dumps, backup exe excluded; 2.0 GB), Steam install untouched. Contains the loader-patched
  `Thief2.exe`, `headlook.dll`, `hmd_bridge.exe`, `openvr_api.dll`, `run_vr.bat` (sets `HEADLOOK_STEREO=sbs`,
  starts the bridge, runs the game at 1920x1080 with `-multisampletype=0 -postprocess=0`) and a VR-tuned
  `headlook.ini` (`tools/hmd_bridge/vr/headlook.ini`: clamps 180/89, `smoothing_ms=8`, `tcp_port=0`,
  `listen_loopback=1`, `stereo=off` so a plain launch stays mono). Refresh with
  `tools/hmd_bridge/install_vr_copy.sh [--ini]` (`--ini` overwrites the VR ini; the first install kept the copied
  *live* ini, which had the old sign/limit setup, and had to be overwritten). Saves there are separate.
- **Desktop+ facts (from its user guide):** Overlay Properties -> Advanced -> "3D Mode" splits the image between
  the eyes (Side-by-Side is its recommended mode; whether a separate *Half* variant exists was not shown);
  Position -> Origin "HMD" attaches an overlay 1:1 to the head; capture by Graphics Capture (windows) or Desktop
  Duplication, which supports exclusive-fullscreen apps.
- **Expect / unknown:** latency (nothing reprojects a head-locked overlay), flat game FOV, no positional
  tracking, HUD unsplit; native Windows rendering with the post-process/MSAA paths untested; WMR SteamVR support
  reportedly ends 2026-11.

### 2026-09-22 (headset test result; latency discussion; reference marks)
- **Headset test tried, did not work.** The user reports the desktop OpenTrack setup (no headset, no
  stereo -- head-look only, `tools/run-wine.sh` without `STEREO`) is "excellent": "The frame rate is very
  smooth and the tracking works well." Not triaged: which piece failed (SteamVR/WMR itself, Desktop+
  overlay setup, or something native-Windows-specific in the game/DLL); no logs were read for this attempt.
  `tools/hmd_bridge/` was not identified as the problem.
- **Latency discussed (not built): reprojection.** Explained the size of the head-to-photon delay (rough
  estimate only, ~80-150 ms, unmeasured) and that it is fixable the standard VR way: an OpenVR overlay
  presenter of our own, world-locked at the head pose each frame was actually rendered with, so the
  compositor's own rotation-only reprojection (same idea as WinlatorXR's sync-pixel handshake) hides it
  for yaw/pitch (not for positional tracking, which would need the depth buffer). User: worry about it
  "later" -- deferred, not started.
- **Reference marks, built** (`headlook/headlook.c`; see the file's top block comment and the ini for the
  detail): amber tick marks at the screen edges, one pair moving up/down with the body's true pitch, one
  pair moving left/right with its true heading -- extended inward they cross at the point body-forward
  projects to, which is also where a projectile launches (per the Stage B map, `FUN_0054f000`/`FUN_0054eec0`
  read the camera struct, which head-look never touches). The value is *free*: it's exactly the offset
  (g_yaw/g_pitch) already being computed and added to the render each frame, no new tracking of "true"
  angles needed.
  - Reuses the existing stereo call-site wrapper (`hl_scene` at `0x5CF2E2`) rather than adding a second
    hook, so it works standalone too now: the wrapper installs whenever stereo or marks (or both) are
    wanted, and `hl_scene` draws marks after the single scene call when stereo is off, or inside each eye
    pass (before the eye is squeezed into its half, so no separate stereo-side math is needed) when it is on.
  - Drawn with `DrawPrimitiveUP` (pre-transformed XYZRHW verts, matching how the engine itself draws),
    render state (Z, alpha blend, cull, texture stage 0) saved and restored around it so the engine's own
    later 2D/HUD drawing is unaffected.
  - **Screen position needs a field-of-view constant the engine's real value for is not confirmed
    statically**, so `fov_deg` (ini, default 90) is a guess like `stereo_ipd` was; the crosshair the ticks
    would form is itself a calibration tool (dial in a known offset, fire an arrow, adjust `fov_deg` until
    they land together) -- not yet done.
  - **A promising lead for an exact value, not pursued (would remove the guess):** `_DAT_007e62d8` (VA
    `0x7e62d8`) is set by `FUN_005cc900` as `1/tan(param_1)` -- the textbook perspective-scale formula --
    from what looks like a config-var read (`FUN_005cc980` -> `FUN_0067f9b0(2, ...)`, an indexed rather
    than named lookup, not traced further) guarded to only run for `0 < param_1 < threshold` (consistent
    with a validated FOV angle). It is read back in `FUN_005cee30` as `_DAT_007e62d8 * local_1c` (`local_1c`
    = the camera struct's own zoom, `+4`) and passed into `FUN_005cea90`, whose use of it was not traced
    (`FUN_005e1e10`/`FUN_004d9f10`, undecompiled) -- so this is a lead, not a confirmed fact. If it holds
    up, one focal-length-in-pixels constant serves both screen axes (same maths already used for the
    ticks: `(width/2)/tan(hfov/2) = (height/2)/tan(vfov/2)` under square pixels), so reading this global
    plus the zoom float would replace `fov_deg` outright and get bow-draw/spyglass zoom for free.
  - Deployed: `headlook/headlook.c`/`.ini` (main and `tools/hmd_bridge/vr/`); the ext4 game copy's DLL was
    replaced directly (game was not running); the user's live `headlook.ini` was left untouched (the new
    keys fall back to their code defaults, `show_heading_marks=1 fov_deg=90`, when absent from an ini).
- **User-confirmed working** ("works like a charm"), same session. `fov_deg` calibration (does the crosshair
  actually line up with where an arrow lands?) was not part of that check.
- **Follow-up, same day: "The marks are accurate."** No `fov_deg` change was made or requested, so this is
  the default (90) confirmed good enough by the user's own judgement, not a numeric calibration against an
  arrow's landing point -- the method used to reach "accurate" is not recorded, only the verdict. Treat the
  `_DAT_007e62d8` lead above as no longer urgent (a nice-to-have exactness check, not a bug fix) unless a
  more demanding test (e.g. sniping at range) later shows otherwise. Session ended here; the user is moving
  to unrelated work next.

### 2026-09-22 (XInput controller support: built, untested)

- New, separate feature (not related to the OpenTrack/VR work above): see "Active: XInput controller
  support" under Goals for the full design and status. Summary: the engine's own original-2000 legacy
  DirectInput joystick support has been dead code (missing `dinput.dll`) -- research this session found
  it's real and complete (default analog-movement binds, an in-game Controls-menu joystick toggle, a
  generic bind-capture screen that already accepts joystick input), so the plan is a `dinput.dll` proxy
  backed by XInput rather than a custom binding UI.
- Research: two parallel explorations confirmed (a) the engine's joystick/bind-menu internals (config
  vars, `.bnd` bind syntax, `OPTIONS.STR` UI strings, confirmed via `strings` cross-referenced against
  `dark.bnd`/`user.bnd`/`INTRFACE.CRF`) and (b) this project's own DLL/build/deploy conventions
  (`headlook/`, `patches/install_loader.py`, mingw toolchain) to reuse rather than reinvent.
- Design: full plan written to `(a local planning note, not in the repository)`
  (context, vtable/struct layout, axis mapping, the button/shift-chording scheme, config, build/deploy,
  verification). The shift/chording design changed from an initial single-modifier-only proposal after
  the user clarified the actual want: not one fixed "always L1" scheme, but freely deciding per
  individual bind (at bind-capture time) whether to hold the modifier or not -- e.g. push-stick-forward
  alone moves Garrett, but hold the modifier while doing it and it's a distinct bindable action (e.g.
  lean forward) instead. Landed on: one configurable modifier button (not per-combo, since that would
  need a separate virtual id for every possible input pair and blow past DirectInput's 32-button legacy
  cap) that shifts every other tracked input's identity, with the *choice of which action needs it*
  entirely free-form, decided live like any other bind.
- Implementation: `xinput_joy/` (`didefs.h`, `xinput_joy.c`, `build.sh`, `xinput_joy.ini`,
  `install_joy_copy.sh`). All DirectInput vtable slot orders, struct layouts, GUID values and error
  codes were cross-checked against mingw's own `/usr/i686-w64-mingw32/include/dinput.h` (the real
  Microsoft-published ABI) rather than written from memory -- notably, `IDirectInputDevice2A` does NOT
  simply append `Poll`/`SendDeviceData` right after `Initialize`; it inserts 7 force-feedback methods
  first, so `Poll` actually lives at vtable slot 25, not 18. Compiles clean (`xinput_joy/build.sh`,
  `i686-w64-mingw32-gcc`), exports a plain undecorated `DirectInputCreateA` (`-Wl,--kill-at`), verified
  via `objdump -p`.
- `tools/run-wine.sh`: added a scoped `WINEDLLOVERRIDES=dinput=n` (own environment only, not global
  `winecfg`) so Wine's builtin `dinput.dll` doesn't shadow the deployed one -- a real Wine-only wrinkle
  with no equivalent on native Windows.
- **Not yet done, deliberately (session ended before touching the user's live game/Wine session):**
  deploying `dinput.dll` into `~/games/thief_2/`, launching the game to generate a first
  `dinput.log`, and everything downstream of that (confirming the real `DIJOYSTATE`/`DIJOYSTATE2` call
  sequence, axis sign conventions, whether button ids above `joy10` are actually live -- all flagged as
  open/to-verify-empirically in the plan doc, not assumed). Code is a rough-cut-but-complete first pass
  covering axes, buttons, D-pad-as-hat and the shift bank in one go, not staged as separate incremental
  builds, since the discovery logging is baked in throughout regardless.
- Uncommitted, in addition to the pre-existing `tools/run-wine.sh` `SIZE` edit: the new
  `WINEDLLOVERRIDES` hunk in that same file, the `.gitignore` addition (`xinput_joy/*.dll`), and the
  entire new `xinput_joy/` directory. Nothing committed this session (commits happen when asked).

### 2026-09-22 (XInput controller support: tested, two real crashes fixed, tuned, working)

Direct continuation of the entry above, same day, same session. Deployed to
`/mnt/d/SteamLibrary/steamapps/common/thief_2` (the pristine Steam install -- the actual Windows host
this WSL2 instance runs on, not reachable for direct control from here, but its `D:` drive is; the exe
there stayed byte-identical to `bin/Thief2.orig.exe` throughout, confirmed by hash before deploying) and
run **natively on Windows**, not through Wine -- this project has no real Windows-execution path from
inside WSL2 (`binfmt_misc` routes every `.exe` through Wine), so testing meant asking the user to launch
it themselves on the host and reading `dinput.log`/`Thief2.log` back over the `/mnt/d` mount.

- **Crash 1 (native launch: black screen ~0.25s, then a silent crash, no dialog).** `dinput.log` showed
  the full sequence `DirectInputCreateA` -> `EnumDevices` -> `CreateDevice` -> `QueryInterface
  (IDirectInputDevice2A)` -> `SetCooperativeLevel` -> `SetDataFormat` -> `GetCapabilities` ->
  `GetObjectInfo`x4 -> `Acquire` completing cleanly every time, then silence -- no crash line, no detach
  line. Windows Event Viewer's Application log (user checked by hand) showed the faulting **application**
  was `dwm.exe`, not `Thief2.exe`, `ucrtbase.dll`+`0xc0000005` -- a strong hint this wasn't a bug inside
  our own process. **Isolation test (rename `dinput.dll` out of the way, relaunch): no crash without it**
  -- so it was us, just manifesting as a DWM-level fault. Root cause: `DllMain(DLL_PROCESS_ATTACH)` was
  calling `LoadLibraryA("xinput1_4.dll")` and `CreateThread(...)` synchronously while still holding the
  loader lock -- a well-known Windows hazard (MSDN "DllMain restrictions") -- happening right before the
  engine created its exclusive-fullscreen D3D9 device, a transition already sensitive to timing
  perturbation on some driver/DWM combos. **Fix:** moved both to a `static void ensure_lazy_init(void)`
  guarded by `InterlockedCompareExchange`, called once from the top of `DirectInputCreateA` instead --
  well after the loader lock is released. `DllMain` now only sets up the log file and reads the ini.
- **Crash 2 (after fixing #1): a real crash in `Thief2.exe`, faulting module `dinput.dll`, offset
  `0x1d88`** (per Event Viewer, user hand-copied). Mapped the offset to source by building a matching
  unstripped `-g` copy (`i686-w64-mingw32-objdump -p` for each build's own `ImageBase`, since mingw
  linker picks a different one per build when not pinned -- has to be added to the *same* build's own
  base, not assumed shared -- then `addr2line`): `guid_name()`, the line calling `IsEqualGUID(g, ...)`.
  Root cause: DirectInput "property GUIDs" (`DIPROP_RANGE` etc.) are tiny integers cast to a pointer via
  `MAKEDIPROP` (e.g. `DIPROP_RANGE` is literally `(const GUID*)4`), not real memory addresses --
  `IsEqualGUID` unconditionally dereferences its argument, so logging a `SetProperty`/`GetProperty` call
  (which the engine makes right after `Acquire`, explaining the exact silence point in crash #1's log)
  read from address `0x4` and died -- as a side effect of evaluating `xj_log(...)`'s arguments, before
  the log line itself was even written, which is why nothing appeared. **Fix:** `guid_name()` now checks
  `(UINT_PTR)g < 0x10000` first (plain pointer comparison, never dereferenced) and returns a name for
  any small property value -- including ones not individually enumerated -- before ever reaching
  `IsEqualGUID`.
- **Working after both fixes**, user-verified ("Excellent work", "That's great"): axes and buttons
  detected in "Customize Controls...", analog left-stick movement confirmed in a mission.
- **Empirically confirmed (from the working `dinput.log`), not just assumed:** the engine's declared
  `SetDataFormat` `dwDataSize` is exactly 80 bytes, matching classic `DIJOYSTATE` (`xj_djoystate_t`)
  precisely -- so the "is it `DIJOYSTATE` or `DIJOYSTATE2`" open question from the design phase is
  settled. Only polled access is used: 0 calls to `GetDeviceData` or `Poll` across a full session
  (~30,000 `GetDeviceState` calls logged), confirming the self-polling-inside-`GetDeviceState` design
  (rather than requiring `Poll()`) was the right call. `SetProperty(DIPROP_RANGE/DIPROP_DEADZONE, ...)`
  is called for all 4 axes (offsets 0/4/8/20 = X/Y/Z/Rz) every time the Controls menu is opened.
- **Two UX rounds after "working", both user-reported from actual play, both fixed same session:**
  1. *Both sticks drift slightly at rest.* We weren't applying any deadzone ourselves at all -- relying
     entirely on the engine's own `joystick_deadzone`/`rudder_deadzone`, which wasn't enough in practice.
     Added `apply_deadzone()` (radial, with rescale so full travel still reaches -1..1 beyond the dead
     zone) and a `stick_deadzone` ini key (default 0.08), applied to both sticks.
  2. *Right stick doesn't affect pitch at all.* Checked, and this is a genuine, original-engine
     limitation, not a bug: the classic joystick support only ever wires three axes to built-in analog
     actions (`+joyxaxis`, `+joyforward`, `rudderturn`) -- there's no fourth "analog look" action an axis
     can target; mouselook has always been the only thing that drives pitch. Asked the user how far to
     take this (pitch-only vs. full twin-stick); answer was full twin-stick. **Implemented as a
     different mechanism entirely**, not a DirectInput axis: `inject_mouselook()` sends synthetic
     relative mouse movement via `SendInput` (`-luser32` added to `build.sh`), gated to only fire while
     the game's own window has foreground focus, replacing the right stick's old `rudderturn`/`joy_axisz`
     role (those axes now always report centered). Needed its own separate, larger deadzone
     (`mouselook_deadzone`, default 0.15, vs. the left stick's 0.08) after the user reported the shared
     deadzone still left a small amount of yaw drift -- residual movement-stick creep is unnoticeable,
     the same amount of camera drift was not, so one shared value was wrong for both.
  3. *Bind-capture UX issue (separate report, chording specifically):* holding the modifier (L1) alone
     to start a chord made the "Customize Controls..." dialog immediately grab it as a solo bind, before
     a second button could be pressed. User's own diagnosis and requested fix: wait for release before
     deciding, treating a press of anything else *before* release as the chord signal instead. Implemented
     exactly that in `compute_buttons()` (see "Buttons + chording" above); confirmed working
     ("extremely well").
- **Now combining with head-tracking (same day):** deployed `dinput.dll`+`xinput_joy.ini` into the ext4
  Wine sandbox copy (`~/games/thief_2`), alongside the already-working, headlook-patched
  `Thief2.exe`/`headlook.dll`/`.ini` -- no conflict expected (dinput.dll needs no exe patching, headlook's
  hook site is unrelated code). Snapshotted the copy's `Thief2.exe` first
  (`Thief2.exe.before-xinput-joy-<timestamp>`) purely as extra insurance, though it isn't actually
  modified by this feature. **Not yet run under Wine at all** -- deliberately not launched this session
  (user: "don't worry about wine for now"); the one real open question for that test is whether
  WSL2/Wine can see the physical Xbox controller as an XInput device (USB passthrough, untested),
  separate from `SendInput`-based mouselook, which has no real-hardware dependency and should work
  regardless.
- Uncommitted this session, in addition to everything listed in the entry above: the `guid_name()`,
  `DllMain`/`ensure_lazy_init`, deadzone, mouselook, and chord-release-timing changes to `xinput_joy.c`;
  the `xinput_joy.ini` template additions (`stick_deadzone`, `mouselook_*`); the `-luser32` addition to
  `xinput_joy/build.sh`. Nothing committed (commits happen when asked, per this project's convention).

### 2026-09-23 (OpenVR direct submit: built, NOT yet run on the headset)
- **Why:** the user found the Desktop+ route unreliable and asked whether SteamVR can take the images
  directly. It can: a **scene app** hands each eye to `IVRCompositor::Submit` and gets the head pose from
  `WaitGetPoses`. That removes Desktop+, window capture and `hmd_bridge.exe` from the chain. `Submit` takes no
  D3D9 textures (D3D11/D3D12/GL/Vulkan/DXGI shared handle only), so the rough cut copies through the CPU.
- **What was built:** `headlook/vr_openvr.c` (+ `.h`), linked into `headlook.dll`. Switch: `stereo=openvr`
  (ini) or `HEADLOOK_STEREO=openvr` (what the new `tools/hmd_bridge/vr/run_openvr.bat` sets). It rides on the
  existing SBS code: the same wrapper at `0x5CF2E2`, the same two eye passes, and the desktop window still
  shows the SBS composite.
  * Start-up (worker thread): loads `openvr32\openvr_api.dll` next to `headlook.dll` (the **32-bit** SDK DLL,
    v2.15.6, fetched by `headlook/build.sh`; the folder's own `openvr_api.dll` is the bridge's 64-bit one),
    `VR_InitInternal(Scene)`, `IVRSystem_026` + `IVRCompositor_029`, seated tracking space, logs each eye's
    `GetProjectionRaw` and the headset's DXGI adapter. If SteamVR isn't reachable it falls back to plain SBS and
    retries every 10 s. The worker also drains `PollNextEvent` and stops submitting on `VREvent_Quit`.
  * Per frame (render thread, player view only): `WaitGetPoses`, which also paces the game to the headset,
    gives yaw/pitch in `hmd_bridge`'s convention (same maths). They go straight into `g_yaw`/`g_pitch` through
    the ini's `yaw_sign`/`pitch_sign` (same OpenTrack convention, so the existing -1/-1 apply). Gains, clamps
    (except pitch +-89), smoothing and roll are bypassed; `g_vr_pose` stops the UDP filter overwriting them.
    Pause/Scroll Lock recentres yaw.
  * Per eye (after the pass, after the marks): `GetRenderTargetData` into a persistent SYSTEMMEM surface ->
    lock -> force alpha 0xFF -> `UpdateSubresource` into a D3D11 texture on our own device (created lazily on
    the render thread, on the adapter `GetDXGIOutputInfo` names) -> `Submit` with
    `Submit_TextureWithPose`, **the pose actually drawn** (yaw + pitch, roll 0, real position). The compositor
    then reprojects the difference (head roll, time since `WaitGetPoses`) itself. `PostPresentHandoff` after
    the right eye. `stereo_swap` is honoured (pass 0 is the right eye when swapped).
  * **Scale/FOV:** each eye texture is a black canvas covering the union of the headset eye's frustum and the
    game picture's `+-tan(fov_deg/2)` (horizontal, `fov_deg` 90 = the user-confirmed marks value), with the
    game picture pasted at its true angular spot; the texture bounds select the eye frustum. So the scale is
    1:1 and the headset shows a black border wherever it sees wider than the game draws. Widening the game's
    FOV is the obvious follow-up.
- **Checked here:** builds clean (`-std=gnu11`, which the header's `typedef char bool` needs). In a throwaway
  Wine prefix a test exe calling `vr_init` loaded the 32-bit `openvr_api.dll`, found its exports and failed
  cleanly with "Installation path could not be located (110)", as expected with no SteamVR. **Nothing else
  is tested:** the compositor calls, D3D11 on the host GPU, `GetProjectionRaw`'s top/bottom sign (taken as
  min/max so a flip can't turn the picture upside down), and the yaw/pitch signs in a real headset.
- **Known limits of the rough cut:** two GPU->CPU readbacks per frame (the likely frame-rate cost; the D3D9Ex
  shared-surface route is the no-copy upgrade); menus/cutscenes aren't submitted (headset shows SteamVR's
  void; use the desktop window); HUD drawn once over the SBS desktop picture, not in the headset images
  (they're taken before the HUD); mouse pitch on the body tilts the world in the headset; no positional
  parallax; IPD is still the ini's `stereo_ipd`, not the headset's.
- **To run:** `headlook/build.sh`, `tools/hmd_bridge/install_vr_copy.sh`, start SteamVR, double-click
  `D:\games\thief_2_vr\run_openvr.bat`, load a mission. Read `headlook.log` (lines `openvr:`) afterwards.

### 2026-09-24 (OpenVR direct submit: first desktop run stops at the first frame; tracing added)
- Laptop try (bundle zip, `~/thief2_vr_bundle.zip`): stuck in the SteamVR idle room; the user says many VR
  titles failed to launch there too, so it may be the laptop's setup. No log came back.
- Desktop run (`D:\games\thief_2_vr`, 19:00): `headlook.log` shows OpenVR connected (WMR eyes about +-1.21..1.23
  tangents, 1860x1860 recommended, adapter 0 = RTX 3050), the first pose, the D3D11 device, both eye canvases
  (~2340x2340, game picture 1920x1080 at fov 90), and ONE finished stereo frame at 19:00:17.395. Then nothing,
  not even the worker's 10 s status line, so the process most likely died or froze in its first headset frame
  (in `Submit`, `PostPresentHandoff`, or the next `WaitGetPoses`). The user's description wasn't asked yet.
- Added and redeployed: step-by-step `openvr: frame N -> <step>` lines for the first 3 frames (return codes of
  `WaitGetPoses`/`Submit`), a worker-side "render thread STUCK in <step>" warning (status every 5 s; the worker
  no longer calls OpenVR at all -- `PollNextEvent` moved to the render thread), and a vectored-exception logger
  (`EXCEPTION code at addr (module+offset)`, first 10 fatal-type ones, first-chance so possibly handled).
- **Later that evening: `Thief2.exe` vanished from `D:\games\thief_2_vr`.** Copying `bin/Thief2.patched.exe` back
  succeeded (size right), but reading it back from WSL fails with `Invalid argument`, and so does the older
  `Thief2.exe.before-xinput-joy-*` copy (same patched exe); `headlook.dll` reads fine. That's the pattern of
  Windows antivirus (almost certainly Defender) blocking/quarantining the loader-patched exe: a
  `LoadLibraryA` stub at the entry point is a classic heuristic hit. Fix is on the Windows side: Windows
  Security -> Protection history (allow/restore), and a folder exclusion for `D:\games\thief_2_vr`. Possibly
  also what happened on the laptop.
- **First picture in the headset (user: could "see the thief game in stereo through the headset")**, once.
  Then Esc -> the menu (desktop only, headset back to the idle room, expected: menus aren't submitted), and on
  returning, and on every relaunch after, the headset stayed in the idle room. `headlook.log` of the next
  run: poses fine (`WaitGetPoses` 0, tracking result 200), but **`render target format 113 not handled`**:
  the engine's scene target was now `D3DFMT_A16B16G16R16F` (HDR; `d3d_disp_enable_hdr` is in the config
  dump in `Thief2.log`), where the first working run had 22 (X8R8G8B8). What flipped it isn't known (the
  menu visit rewrote `cam.cfg`; only gamma 0.75 and volumes differ from the ext4 copy).
- **Fix (deployed):** `vr_openvr.c` now has a format table: 22/21 -> `B8G8R8A8_UNORM`, 113 ->
  `R16G16B16A16_FLOAT` (same memory layout, alpha forced to half 1.0). The canvases rebuild when the format
  changes. The float picture is submitted as gamma colour space by default; the new ini key
  `vr_hdr_linear=1` (live) submits it as linear if it looks washed out or dark. Also fixed the tracing, which
  never stopped when no frame got as far as `Submit`: it now counts frames from `WaitGetPoses`.

### 2026-09-24, later (OpenVR: playable; letterbox and left-eye glitch)
- **User: "it's playable and hilariously fun."** Two issues reported: (1) the picture is letterboxed ("looking
  out through the slot of a letterbox"); (2) graphical glitches almost only in the **left** eye: at some
  places/head poses the left eye shows "a textureless lightmap of the scene in gradients of light to dark
  brown"; moving off restores it. Seemed random but common, not tied to walls.
- **Letterbox, cause found statically (Ghidra):** the engine's view scale is the float at **`0x7E62D8`**,
  `1/tan(x)` set once by `FUN_005cc900` from a config read, **default 1.0** (90 deg), read every frame in
  `FUN_005cee30` times the camera's zoom (+4). `FUN_00689aa0` (mode set) computes the pixel aspect and sets
  **`0x7DF800` = 1 for widescreen modes (Hor+)** unless the cfg var `widescreen_lock_hfov` is set. So the
  reference view is 90 deg horizontal at 4:3 (tan 1 x 0.75), and 16:9 widens it sideways (tan 1.333 x 0.75):
  the vertical view is **tan 0.75 at any resolution**, against the WMR headset's ~+-1.23. A squarer resolution
  alone can't fix it. (Also: the earlier `fov_deg=90`-as-horizontal model used for the VR canvas was wrong at
  16:9: the true width was tan 1.333, so the headset picture was ~25% too small. The reference marks use the
  same wrong model, even though the user called them accurate; not revisited.) The `fov` string in the exe
  is a sky/stars property, not the camera.
- **Fix (built, NOT yet deployed: the game was running and the DLL was locked):** for VR frames only,
  `hl_scene` writes a wider scale into `0x7E62D8` around the two eye passes and restores the engine's value
  after (zoom divided out, so bow/spyglass zoom doesn't change the headset scale). Auto value
  `vr_auto_scale()`: the smallest scale covering both eyes' `GetProjectionRaw` tangents +2% (WMR: ~0.598,
  about 118 deg at 4:3), override with ini `vr_view_scale` (live). The canvas maths now takes the picture's
  tangents from that scale and the Hor+ flag (`image_tangents()`). `run_openvr.bat` moves to 1920x1440 (4:3,
  in the mode table) so less of the picture's width is outside the headset's view. Untested: whether frustum
  culling follows the scale (if not, expect geometry missing near the edges of the wider view).
- **Left-eye glitch: not diagnosed.** Left eye = pass 0 (the first scene call of the frame), so it looks like
  per-frame engine state, not the eye position (the position shift is symmetric). Or the surface read back
  after pass 0 is sometimes an intermediate target (HDR path). Tools added: the **Insert** key (unbound in
  the game) triggers the existing stereo dump (now numbered, `dumpN_*.bmp`, and FP16 targets converted);
  the dump log line records the view scale and Hor+. Cheap live tests for the user: `stereo_swap=1` (with
  openvr the eye mapping follows the swap, so if the glitch moves to the right eye it follows pass order), and
  `show_heading_marks=0` (the marks' state save/restore runs between the passes; the D: ini has them on).

### 2026-09-24, evening (VR: widened view confirmed; glitch caught; HUD panel; recentre)
- **Widened view confirmed:** user: "much better, the view feels natural and fills the screen without any
  strangeness". So the `0x7E62D8` scale write works, including culling as far as seen (auto scale 0.598 at
  1920x1440; the log said "fixed width", i.e. `0x7DF800` = 0 for a 4:3 mode, as expected).
- **Left-eye glitch caught with Insert (`dump1_*` in the VR folder):** the left pass was flat untextured
  geometry, and it already had the HUD (health shields, item box) in it; the right pass was normal. The
  D3D trace showed the same call pattern for both passes (49+3 vs 51 `DrawPrimitiveUP`). **Prime suspect: the
  reference marks** (on in the user's D: ini): `draw_reference_marks` runs between the passes and set the FVF to
  its own `XYZRHW|DIFFUSE` without restoring it. If the engine caches its FVF and skips re-setting it, its
  next draws have no texture coordinates, which matches "textureless". Why only the left eye (and randomly) is
  not explained. **Fix (deployed, untested):** the marks now wrap their drawing in a `D3DSBT_ALL` state block
  (plus explicit FVF restore), and are skipped in VR frames anyway (they'd be placed for the wrong FOV there).
- **Recentre:** Pause/Break or Scroll Lock already recentred (in `vr_begin_frame`); now also **L3+R3** (both
  sticks clicked) on any XInput pad. A real entry in the game's bind menu would need an engine command hooked
  in (not done). The user's "biased" view: the log showed the first centring at tracking yaw 38.3, i.e. the
  headset wasn't facing forward when the first mission frame arrived.
- **HUD/menus in the headset (built, deployed, untested):** a head-locked OpenVR overlay ("HUD panel",
  `IVROverlay_028`) fed from the back buffer at every `Present` (device vtable slot 17, hooked by the worker
  once `0xA36040` holds the device). During VR frames, `stereo_frame` clears the scene target to black instead
  of blitting the SBS composite back, so the back buffer at Present holds only the HUD. It's shown
  premultiplied with alpha = brightest channel (black = transparent). Without a VR frame for 250 ms (menus,
  loading, cutscenes/remote cameras) the panel shows the back buffer opaque, so menus appear in the headset
  too. ini (live): `vr_hud` (1), `vr_hud_deg` (60), `vr_hud_dist` (1.5 m). Side effect: in a mission the
  desktop window shows only the HUD over black. Costs one more 8-bit readback per frame.
- **Controller: Back -> Esc, RT -> left mouse (2026-09-24, user request; deployed to the VR copy, untested).**
  `xinput_joy` got a `key_mouse_thread` polling XInput itself at ~100 Hz (the game may not poll the joystick
  in menus), sending `SendInput` Esc (scancode 0x01) while Back is held and LEFTDOWN/LEFTUP following RT
  (`trigger_threshold`), only while the game window is in front; both released when that stops. Those two no
  longer report as joystick buttons (joy8/joy7, alone or chorded) unless they are the `shift_button`. ini
  (live): `back_as_escape=1`, `rt_as_mouse1=1`. Parked by the user for later: a proper menu/"click" design.
- **Test result:** the left-eye glitch is **gone** (user: "you seem to have squashed the rendering bug"), so the
  reference marks' leaked FVF was the cause. No HUD visible: `headlook.log` shows "Present hooked" at
  21:19:56 but **0 HUD panel updates and no "overlay created" line**, so `vr_Present` never ran even once.
  Presumably something replaced vtable slot 17 after our one-shot patch (not confirmed). Esc: the headset holds
  the last frame, still head-tracked by the compositor's reprojection, and SteamVR's surroundings show past
  its edges; the menu was only on the monitor (the panel would have shown it). Fix built, **not yet deployed**
  (game running): `ensure_present_hook()` on every worker tick re-patches the slot whenever it isn't ours,
  logging who held it (`module+offset`, first 20 times), and a re-entry guard calls the first-seen original
  if a foreign hook chains back into us. The status line now also logs the Present call count.

### 2026-09-24, night (VR frame rate: diagnosis, timing build, quick wins; built, NOT yet deployed)
- User: headset frame rate low, "<30 fps", much lower than the monitor looked. The log's submit counts: a
  locked **30.0 fps** for minutes (earlier ~22 fps), i.e. 90/3 and ~90/4: WMR's reprojection throttling an
  app that can't make 45, so the real frame cost is ~22-33 ms. Suspects: the CPU round trip of both eyes
  (FP16 1920x1440 = ~22 MB per eye each way, a synchronous `GetRenderTargetData` between the passes, and a
  per-pixel alpha loop), two scene draws at 1920x1440 with the widened view, the monitor's vsync (`pres=1`)
  stacked on `WaitGetPoses`, and the HUD readback once it works.
- `vsync_mode` (a config var, read by index via `FUN_0067f9b0(1, &DAT_007dc84c)` in `FUN_006824e0`, default 3,
  masked to 4 bits) exists, but its meaning wasn't decoded, so it's left alone until the timing says Present is
  expensive. The game accepts config vars on the command line, so a test would be `-vsync_mode=0` in the bat.
  `d3d_disp_managed_textures` exists too: a possible way into a D3D9Ex (shared-surface, no-copy) route.
- **Built:** (1) per-stage timing (QPC), logged every 5 s as ms per frame: frame, wait (`WaitGetPoses`),
  drawL, drawR, capture, readback, alpha, upload, submit (incl. `PostPresentHandoff`), hud, present (the real
  Present), and "other" (engine work outside those). (2) Eyes are now captured on the GPU after each pass
  (`StretchRect` into a per-frame default-pool render target, **converted FP16 -> X8R8G8B8** when
  `CheckDeviceFormatConversion` allows; ini `vr_gpu_convert`, live) and read back **together after the second
  pass** (`vr_capture_eye` / `vr_end_frame`), so there's no CPU wait between the passes and half the bytes.
  (3) In VR+HUD mode the desktop SBS composite is no longer built (it was cleared anyway). (4)
  `run_openvr_1440.bat`: 1440x1080, ~44% fewer pixels per eye.

### 2026-09-24 (goal noted: public release on the TTLG forums; discussion only, nothing changed)
- The user wants the VR mod packageable for others (TTLG), without copyright problems and without the
  antivirus trouble. Constraints that follow:
  * **Never ship `Thief2.exe`**, patched or not (the laptop bundle `~/thief2_vr_bundle.zip` contains the
    patched exe: fine for the user's own machines, never for release). `bin/*.exe` is already gitignored.
    Don't publish `ghidra-proj/` or decompiler output; keep README pseudo-code excerpts minimal in anything public.
  * **Replace the exe patch with a proxy DLL.** The game `LoadLibrary`s `d3d9.dll`, `dinput.dll`, `ddraw.dll`,
    `dsound.dll`, `version.dll`, ... by bare name, and imports `WINMM.dll`, none of them KnownDLLs, so a DLL
    of that name in the game folder is loaded instead. `xinput_joy` already works this way as `dinput.dll`
    with no AV trouble. Best candidate: a `d3d9.dll` proxy (forward to the system one), which would also give
    control of `Direct3DCreate9`, the entry point for the D3D9Ex no-copy route. Needs chaining for users who
    already have a `d3d9.dll` (ReShade, dgVoodoo). Alternative: `dinput.dll` (ours) loads `headlook.dll`.
  * Our code: pick a licence (MIT/BSD fits the OpenVR SDK's BSD-3); ship `openvr_api.dll` with
    `LICENSE.openvr`; ship source so builds can be checked.
  * AV: removing the exe patch removes the main trigger. Beyond that: code signing (a certificate, or
    Microsoft's signing service where eligible), Microsoft false-positive submissions per release, VirusTotal
    links and source in the forum post.

### 2026-09-24 (LONG-TERM GOAL, low priority: motion-controller weapons; discussion only)
- User's idea: use the headset's motion controllers ("wands") so the sword, blackjack and bow follow the
  wand's pose. Swinging the wand swings the sword; **blocking = holding the sword up over your head on the
  right-hand side**. Could the game's own collision detection be used?
- Assessment (not investigated beyond a string scan of the exe):
  * **Tier 1, small:** wand buttons/stick as game input (trigger = attack, grip = block, stick = move), via
    OpenVR's input API, the same idea as `xinput_joy`.
  * **Tier 2, moderate, recommended target:** gestures driving the game's *existing* attacks. A wand swing
    (speed over a threshold) triggers a normal attack. The user's block pose (a zone relative to the HMD,
    up-right, with hysteresis) holds block for as long as the wand is there. The swing's direction can pick
    the attack: the exe has directional motion tags (`PlyrSword 1, PlyrSwordSwing 1, Direction 1/2`,
    `MeleeCombat 0, Block 0, Direction 1/2/5/6`, "Directed Block"). Bow: aim along the wand instead of the view.
    We already know projectiles launch from the camera struct, so one hook in the launch path; drawing = hold
    attack while pulling the wand back. Possible conflict: the block zone overlaps an overhead wind-up
    (tell them apart by the wand's orientation (flat = block, upright = wind-up) or by holding still ~150 ms).
  * **Tier 3, large/risky:** a true 1:1 weapon: the viewmodel ("PlayerArm") posed from the wand every frame,
    and hits from real collision. Hit detection in Dark is tied to the attack state machine (swing phases,
    `Event WeaponSwing` / `WeaponCharge`, swing-type damage, AI parry/notice reactions), so it needs either
    forcing that state while moving the weapon's physics to the wand, or our own wand-vs-AI test calling
    the engine's damage/stim path. Blackjack knockouts also depend on the hit location and whether the AI is
    alert. Significant RE (`PlayerArm`, `IWeaponScriptService`/`cWeaponSrv`, weapon physics).
- Suggested path when picked up: tier 1 -> tier 2 (block zone, swing gestures, wand-aimed bow) -> maybe the
  visual-only half of tier 3 (the viewmodel follows the wand, hits still from gestures) -> real collision last.
- **User's decision (same day):** Tier 1 yes (it helps adoption). **Tier 2 not wanted.** Tier 3 worth a
  real look. The user points out the swing is already collision-aware: swinging past a wall or door jamb
  stops the attack at a plausible moment. **Arms can simply be hidden: show only the weapon.**
- **First static RE pass on melee (string xrefs + Ghidra, 2026-09-24): the weapon IS a physical object:**
  * `FUN_0059b3e0` builds **8 physics sphere submodels** ("halos"): for an AI, 4 spheres around it from
    config `halo_ai_offset/radius/pushout`; for the player's weapon, spheres laid along the weapon's
    orientation from `halo_player_offset/radius/offset2/radius2/set2offset/pushout(2)`, positioned from an
    object's position (`[DAT_00a2a988]+8..+0x10`) and a rotation matrix (`FUN_00678b30`). Submodel setters:
    `FUN_00536430` (offset) and `FUN_00537030` (radius). Player/arm ids compared against:
    `DAT_00a2a984`, `DAT_00a2a97c+4`.
  * `FUN_0059c020` registers the weapon properties: `CurWeapon`, `BaseWpnDmg`, `CurWpnDmg` (damage for the
    current swing), `WpnExposure`, `SwingExpose`, and **`WpnTerrColl`: weapon-vs-terrain collision**, which is
    what stops a swing at a wall. `FUN_0059ccb0` registers act/react effects **`weapon_hit`** (`LAB_0059cd60`)
    and **`weapon_block`** (`FUN_0066f730`). `Event WeaponSwing` / `WeaponCharge` are fired by
    `FUN_0057d020` / `FUN_0057d0a0` (callers `FUN_0056dbf0` / `FUN_0056dc30`). `cPlayerSwordAbility` is the
    player-sword state machine class; motion tags `PlyrSword N, PlyrSwordSwing N, Direction N` and
    `PlyrBow N` drive the arm animation.
  * **Consequence:** hits and blocks come from the engine's physics collision of the weapon's spheres (with
    terrain and with the AI halos), not from a scripted animation timer. So a Tier 3 design can keep the
    engine authoritative: **drive the weapon object's pose from the wand** (instead of from the arm
    animation), let the physics do the collision, and **open the damage window ourselves** (the
    `CurWpnDmg`/swing state) when the wand is moving fast enough. Unknowns for the next pass: where the
    weapon's pose is written each frame from the arm animation (the hook point); whether `CurWpnDmg`/swing
    state can be set without the arm state machine; the bow (a separate `PlyrBow` path; aim = launch
    direction). Hiding the arms: the arm is its own object (`PlayerArm` archetype); making it not render
    (a property) should leave the weapon visible.
- **Deployed (2026-09-24, late):** the timing/quick-wins build + self-healing Present hook (`headlook.dll` ae068065...), `run_openvr_1440.bat`; `dinput.dll` (Back->Esc, RT->mouse1) was already there. Awaiting the user's test.

### 2026-09-24, late (VR: timing results; HUD panel reworked; menus from launch)
- **Timing (1440x1080 run; the 1920x1440 run's log was overwritten), ms per frame:** frame ~23-25 (40-44 fps),
  wait 0.0, drawL ~1 / drawR ~1 (CPU side only), capture ~0, **readback ~7-8** (includes waiting for the GPU
  to finish both passes), alpha ~1.4, upload ~1.1, **submit ~3.5** (Submit x2 + PostPresentHandoff),
  **hud ~7**, present 0.0 (so vsync isn't a factor), other ~2. The 8-bit GPU conversion was supported and
  used. The user: 1440 "runs much better".
- **HUD hook works now:** "Present hook installed" at mission start (slot held `d3d9.dll+0xe6120`),
  RE-installed a few times after (something re-patches the slot; the self-heal copes). ~1 panel update per
  frame.
- **User feedback:** the light gem and health bar aren't visible; items/weapons only "the very top", in the
  periphery. Menus/books are head-locked, "very slightly high", readable "with some effort", in a good
  position, maybe need downsizing. **Nothing shows in the headset until a map loads** (so no main menu).
- **Cause of the missing menus at launch:** `0xA36040` stays NULL until the 3D view starts. **Fix:**
  `hook_create_device()` makes an IDirect3D9 at start-up (its method table is d3d9.dll's, shared with the
  game's) and hooks `CreateDevice` (slot 16); the created device feeds `ensure_present_hook`.
- **Theory for the item/weapon icons:** they're drawn as 3D models in the corners of the *scene* picture
  (cf. `inv_model_zoom`), which the widened VR view pushes outside the headset's crop (u 0.138..0.866). Not
  verified. The light gem/health bar are 2D and should be on the panel; not explained yet. The alpha may have
  hidden dark parts (now alpha = 4 x brightest channel), and **Insert now also writes `hud_panel_N.bmp`**
  (the exact panel image with its alpha).
- **Panel reworked:** in a mission it's head-locked, `vr_hud_deg` 45, `vr_hud_down` 0, refreshed every
  `vr_hud_every`=3rd frame (to cut its ~7 ms); menus/books are **world-locked** (`SetOverlayTransformAbsolute`,
  seated, placed from the head's yaw when the menu opens), `vr_menu_deg` 50, `vr_menu_down` 5, refreshed
  every frame. Both at `vr_hud_dist` 1.5 m. Deployed.

### 2026-09-25 (VR: menus at launch, idle frames, brightness curve)
- Test with the previous build: the main menu still wasn't visible at launch (fine after a map loads); dark
  areas hard to see (worse in daylight; an old, cheap WMR headset). The log: the CreateDevice hook worked
  (device caught at 13:03:43, 1440x1080), and the menu panel was placed "fixed in the room" and updated
  ~60/s from launch, so it was drawn but not seen. Presumably because we'd never submitted a scene frame
  (SteamVR keeps its idle room up), and/or the panel was placed from a pose taken before the headset was
  worn. Timing this run: ~31-34 fps: readback ~10 ms, **submit ~12 ms (was ~3.5)**, hud ~3 (the every-3rd-frame
  refresh worked). Busier GPU than the night before, reason unknown (another session was on the display earlier).
- **Built + deployed:** (1) `vr_idle_frame()`: whenever no 3D frame was drawn in the last 250 ms (menus,
  loading, pause, cutscenes), the Present hook does `WaitGetPoses` and submits the last 3D frame with the pose
  it was drawn at (the paused world stays in place, no idle-room bleed at the edges), or black before the
  first map. (2) Menu panel: re-placed in front of the head on recentre (Pause/Scroll Lock/L3+R3, now also
  polled in menus), when it was placed without a valid pose, and when the head has been >50 deg away from
  it for 1.5 s. (3) **Brightness curve for the eye images** (`vr_set_picture`, LUT in the per-pixel loop,
  8-bit path): the eye images are captured *before* the engine's own final colour correction
  (`d3d_disp_sw_cc`, the game's gamma), so the headset got none of it. ini (live): `vr_gamma` (default 1.3),
  `vr_black` (0).
- **Follow-up (same day):** the user traced the launch problem to the game window not having focus, and the
  idle frames made SteamVR open its dashboard ("Resume game") at launch. Now idle frames only start once a
  3D frame exists; ini `vr_idle_frames` (1). **`yaw_gain` has no effect in openvr mode by design** (gains,
  smoothing, clamps are bypassed for 1:1); snap turn offered as the alternative. **Light gem visible only on
  the death screen** (FMV, a non-VR frame, i.e. the engine's own view scale) and absent from a VR frame's
  back buffer even when the health bar is there. Theory: the gem and the 3D inventory icons are positioned
  with the view scale we widen at `0x7E62D8`. Suggested live test: `vr_view_scale=1`. If confirmed, those
  elements need the engine's own scale while the world keeps the wide one.

### 2026-09-25, evening (HUD in VR: what is drawn where; plan pending the user's choice)
- Test run with `vr_view_scale=1` (the letterboxed engine view): **the light gem shows, in the right spot**,
  which confirms the widened view scale hides/misplaces it. Timing that run: **43-45 fps** (readback ~6.5,
  alpha+LUT 3.5, submit ~4, hud ~2). User's remaining HUD issues: health shields not visible; the weapon's 3D
  model (bottom left) and the item model + its text (bottom right) almost out of view; the item-name text
  (centre) and most HUD text is too small/low-res to read. The user suggests removing the text or a show/hide.
- **Where the HUD pieces are drawn** (dumps from 2026-09-24 + the D3D trace + Ghidra): the health shields and
  the item frame are in the **left eye pass image only** (the right pass lacks them; in the SBS composite they
  sit in the left half's bottom-left). The left pass also has an extra depth `Clear` + 3 `DrawPrimitiveUP`
  that the right pass doesn't: the 3D weapon/item models. So the engine draws its post-world overlays **once
  per frame, in the first scene call**, into the scene target, positioned with the (widened) projection, and
  they land in the left eye's bottom corners, outside the comfortable view. Text is drawn later onto the back
  buffer, so it reaches the panel. In `FUN_005cee30`, after the world render `FUN_005cea90(_DAT_007e62d8 *
  zoom)`: a loop over 3 overlay objects (`PTR_PTR_007f1550..58`, vtable+8; only `FUN_005a15c0` does
  anything, and it's state reset), then `FUN_005a36b0` (`0x5cf036`; per-object overlay render: sets up via
  `FUN_005eb1c0`/`FUN_005ea230`, draws, `FUN_005eb1f0`), `FUN_005a3520` (`0x5cf04c`, if (3|4) state:
  `d3d_disp_camoverlay_postpost`, then each overlay object's vtable+0x14/+0xc: very likely the inventory/weapon
  3D models), `FUN_0060b390`/`FUN_00621570` (post-processing), `FUN_0058c080` (`0x5cf06f`, not read yet).
  Where the shields/gem are drawn wasn't pinned down.
- **Plan options:** (a) quick: HUD panel on demand (a key/button, or glance down); (b) proper: patch the
  post-world overlay call sites in `FUN_005cee30` so in VR frames they're skipped in the eye passes and drawn
  once into a separate transparent target at the engine's own scale (no eye offset), which then goes on the
  panel with the text: shields, gem, weapon/item models all in one head-locked layer, both eyes the same.
- **User's choice: the proper HUD fix first, then a toggle button** (not glance-down). Further RE: at
  `0x5cf036` FUN_005a36b0 takes `ecx = [camera+0x20]` (an attached camera-overlay object) and FUN_005a3520 only
  runs for camera modes 3/4, so in player view the post-world overlay step is **`FUN_0058c080`** (call at
  `0x5cf06f`): it draws a queue of screen overlays (`DAT_0092c868[]`, count `DAT_00a2ad7c`, each via
  `FUN_0058b3a0`) and then **empties the queue**, which is why only the first scene call of a frame draws them.
  The scene is bracketed by provider (`[0xA36014]`) vtable[0](`[0xA33F44]`) before and vtable[1]() after.
- **Built + deployed:** `install_overlay_hook()` patches the `0x5cf06f` call to `hl_overlays`. In VR frames with the
  HUD panel on, both eye passes skip it (the queue survives). After both eyes are captured and the scene target
  cleared, `draw_overlays_once()` runs the original once at the engine's own view scale (mode 2: plain
  `BeginScene`/`EndScene`, the default; mode 1: the provider's brackets), so the overlays land on the target
  that becomes the HUD panel, in neither eye. ini `vr_hud_overlays` (0/1/2, live). **HUD toggle:** `vr_hud_key`
  (72 = H) / `vr_hud_pad` (0x8000 = Y) flip the in-game panel (menus always show); both unbound in the user's
  binds. `vr_pad_buttons()` now serves both this and the L3+R3 recentre. Untested: whether the light gem is in
  that queue too (it was affected by the view scale, which suggests it's drawn elsewhere, with the projection).
- **Test result (2026-09-25 evening):** the HUD overlay redirect works: "almost all the hud items" show on the
  panel, **including the light gem** (so it *is* in the `FUN_0058c080` queue; the earlier scale effect was about
  where it landed in the eye picture). The Y toggle works. User wants the light gem visible at all times.
  **Built (not yet deployed): `vr_gem_always`**: with the HUD toggled off, the panel isn't hidden but cropped
  (`SetOverlayTextureBounds`) to the gem's part of the screen (`vr_gem_x0/y0/x1/y1`, default 0.40,0.82 to
  0.60,1.00, a guess), shrunk and offset so the gem stays exactly where the full panel would show it.

### 2026-09-25, late (d3d9.dll stand-in: no more patched exe; built + tested in Wine, not yet on the game)
- For a public release (no game exe may be shipped, and the patched exe tripped Defender): **`d3d9proxy/`**
  builds a `d3d9.dll` for the game folder. The game `LoadLibrary`s `d3d9.dll` by bare name (not a KnownDLL),
  so ours is loaded. It forwards all 17 d3d9 exports (generic asm jump thunks, resolved lazily outside DllMain)
  to the system `d3d9.dll`, or to **`d3d9_chain.dll`** in the game folder if present (for ReShade etc., renamed).
  `Direct3DCreate9`/`Ex` are real wrappers: after the real call they `LoadLibrary` **`headlook.dll`** from the same
  folder and call its new export **`headlook_d3d_created(IDirect3D9*)`**, synchronously, so `CreateDevice` is
  hooked before the game can create its device. On the headlook side, `hook_d3d_object()` is shared with the
  start-up path, with a lock and an already-hooked check (a double patch would recurse). Log: `d3d9proxy.log`.
  The game only uses `Direct3DCreate9` (D3DX9_43 doesn't import d3d9). **Wine test** (throwaway prefix, a test exe
  loading `d3d9.dll` from its folder): stand-in loaded, system d3d9 loaded, headlook loaded, CreateDevice
  hooked "from the d3d9.dll stand-in". Next: the real game on the desktop with the **stock** `Thief2.exe`.
  `install_vr_copy.sh` now also installs `d3d9.dll` and accepts either the stock or the patched exe.

### 2026-09-25, late (docs restructure)
- At the user's request, since the old README was a verbose lab notebook: **README.md -> docs/DEVLOG.md** and
  **HANDOFF.md -> docs/HANDOFF.md** (`git mv`, content unchanged apart from a header note), references to
  "README" in code comments and scripts repointed to `docs/DEVLOG.md`, and CLAUDE.md now sends sessions to
  docs/DEVLOG.md then docs/HANDOFF.md. **New README.md:** a short player-facing page (what it is, requirements,
  install, controls, settings, known issues, building, credits). **New docs/HOW_IT_WORKS.md:** a technical overview
  for modders (loading, the render-only offset, the eye passes, the view widening, the SteamVR path, HUD/menus,
  the controller, diagnostics). CLAUDE.md asks future sessions to keep both current.
- Still open before going public: the personal details in this log and in HANDOFF (LAN IP, paths, "working with
  this user"), a licence, a release-zip script. The d3d9.dll stand-in is installed in the VR copy (with the stock
  exe) but not yet tested with the game.

### 2026-09-25, late (pre-release tidy-up, licence, release script)
- **Tidy-up for publishing:** home-directory paths (`/home/<user>/...` -> `~/...`), the desktop's LAN IP and the
  WSL IP (-> `<desktop-ip>`/`<wsl-ip>`), the SSH user name and links to local planning notes are scrubbed from
  the docs and scripts. `tools/run-wine.sh` and `xinput_joy/install_joy_copy.sh` now default to
  `$HOME/games/thief_2`. HANDOFF's "Working with this user" section became neutral "Working conventions" (the
  personal working preferences live in Claude's private memory, not the repo). **Git history still contains the
  old wording and the commit author email of the first commits;** rewriting it (e.g. one squashed public commit)
  is a separate decision before making the repository public.
- **Licence: MIT** (`LICENSE`), the one suggested earlier; the user asked for "the licence" without naming one.
  Valve's OpenVR header and DLL stay under their BSD-3 licence (noted in LICENSE).
- **`tools/make_release.sh [VERSION]`** builds headlook, d3d9proxy and xinput_joy from source and writes
  `dist/thief2-vr-VERSION.zip` (gitignored): `d3d9.dll`, `headlook.dll`, `openvr32/openvr_api.dll`, `dinput.dll`, the
  VR `headlook.ini`, `xinput_joy.ini`, both launchers, README.md, LICENSE.txt, LICENSE-openvr.txt (text files
  with CRLF), inside one top folder, plus its SHA-256. A test run produced the expected 11 files. The VR ini
  template's comments were rewritten for players (no more hmd_bridge-era wording).

### 2026-09-26 (Quest plan; VR speed-ups built + deployed, NOT yet run on the headset)
- **Plan agreed** (user: Quest 3, PC quick wins first, no D3D9Ex zero-copy):
  - Phase 0: the PC speed-ups below.
  - Phase 1: a `stereo=wxr` backend for **WinlatorXR** (Wine + Box64 + DXVK/Turnip on the Quest). Its XrAPI, per
    winlatorxr.github.io/xrapi.html:
    - Head and controller poses arrive as text floats on UDP `localhost:7872`; they end with IPD, FOVX, FOVY and
      HMD_SYNC, then T/F button states.
    - The game sends `L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY` to `:7278` (1 1 = VR, side-by-side).
    - Frames are side-by-side in the back buffer. HMD_SYNC is painted as red in the top-left pixel, and the
      renderer is held until it changes.

    Tested on the desktop against a fake sender; the HUD is baked into both halves because there's no overlay
    layer.
  - Phase 2: the Touch controllers via `xinput_joy`.
  - Phase 3: on the Quest 3.

  Biggest unknown: whether NewDark runs under WinlatorXR at all. A plain flat-mode smoke test on the Quest should
  come early.
- **Baseline** (last run's log, 1440x1080): 44-46 fps.

  | Stage | ms per frame |
  |---|---|
  | readback | ~7 |
  | alpha (curve loop) | 3.5 |
  | upload | 1.2 |
  | submit | ~4.8 |
  | hud | ~2.1 |
  | other | ~2.3 |
- **Built:**
  1. **Pipelined readback** (`vr_pipeline`, default 1, live).
     - The eye capture targets are now kept across frames.
     - Frame N is read back and submitted in `vr_begin_frame` of frame N+1, right after `WaitGetPoses`, with the
       pose it was drawn at (`g_pend_pose`). So the CPU no longer waits for the GPU to finish both passes. Cost:
       one frame more delay, left to SteamVR's reprojection.
     - Idle frames send a still-pending frame first, then resend the textures with the pose they hold
       (`g_tex_pose`).
     - Since the kept targets are default-pool, a **`Reset` hook** (device slot 16, installed and self-healed next
       to the Present hook) releases them before the engine's Reset. They're also dropped whenever menus show.
  2. **Brightness curve on the GPU** (`vr_gpu_gamma`, default 1, live).
     - Each eye is captured through a ps_2_0 shader instead of `StretchRect`: saturate, `pow(1/gamma)`,
       black-level mad, alpha 1, into an A8R8G8B8 target. So the CPU per-pixel loop (the 3.5 ms "alpha") is
       skipped for those eyes.
     - The shader is assembled at run time with `D3DXAssembleShader` from `d3dx9_43.dll` (which the game
       imports). It assembled fine with Wine's d3dx9 (176 bytes, throwaway prefix).
     - Device state is saved and restored with a `D3DSBT_ALL` state block, plus the render target and
       depth-stencil.
     - If the scene target isn't a texture, it's `StretchRect`ed into a kept copy texture first.
     - Any failure logs once and falls back to the old path.
     - `vr_set_picture` now runs before the eye passes.
  3. **HUD panel copy pipelined** (same `vr_pipeline` switch): the back buffer is `StretchRect`ed into a kept
     target at Present and read back at the next Present. That removes the GPU stall that happened every
     `vr_hud_every`-th frame.
- Not done: halving the HUD panel's resolution (its text is already hard to read); trimming the per-eye D3D11
  `Flush`. Revisit if `submit` stays ~5 ms.
- Deployed to `D:\games\thief_2_vr` (`headlook.dll`; the previous one kept as `headlook.dll.prev`; the user's
  `headlook.ini` kept, and the new keys default on in code). The template ini documents both keys.
- **To check in `headlook.log` after the next headset run:**
  - "brightness curve applied on the GPU", or "curve pass samples a copy of the scene target".
  - "Reset hook installed", and no "device Reset failed" when going between menu and mission.
  - The timing line: readback and alpha should be down to ~1-2 ms and ~0.
  - If anything looks wrong, set `vr_pipeline=0` / `vr_gpu_gamma=0` live to compare.
- **Evidence on the "does NewDark run there" risk (user, same day; the user can't test the headset or WinlatorXR
  for now):** the user found YouTube videos of Thief II running at full speed on a Steam Deck and on a **KTR1**.
  - The Steam Deck is x86 Linux + Proton: it confirms Wine + DXVK, but not ARM emulation.
  - The KTR1 is an **Android ARM handheld**, and videos exist of Thief (Winlator 2.0 Mr.J builds) on it, e.g.
    Thief Gold: https://www.youtube.com/watch?v=B0MMLmXkpb8. There is also a TTLG thread, "Winlator Emulator: run
    Thief on Android phone": https://www.ttlg.com/forums/showthread.php?t=152615.

  Winlator is the base WinlatorXR is forked from (Wine + Box64 + DXVK/Turnip on Android). So the engine running on
  that stack is **supported by others' reports, not tested by us**. Still unknown: our DLLs (d3d9 stand-in,
  headlook) under it, and the cost of drawing twice per frame at headset resolution.

### 2026-09-26, later (Phase 1: stereo=wxr, WinlatorXR backend; built, desk-tested in Wine with a fake headset)
- **Protocol, from WinlatorXR's own source**
  (github.com/WinlatorXR/WinlatorXR: `app/src/main/java/com/winlator/xr/api/XrAPI.java`, `XrVersion01-05.java`,
  `XrActivity.java`, `cpp/xr/renderer.c`, `main.c`):
  - **The game switches the API on**, by writing e.g. `0.5` to `Z:\tmp\xr\version` (= imagefs `/tmp/xr`).
    WinlatorXR then listens on UDP 7278 for `L_HAP R_HAP MODE_VR MODE_3D FOVX FOVY`, and **only sends poses once
    MODE_VR > 0**.
  - **The v0.5 packet to 7872/7873:**
    - `client0`, then 29 floats: per controller quat, stick, pos; head quat, pos, IPD, FOVX, FOVY, HMD_SYNC.
    - 19 T/F buttons, in the `ControllerButton` enum order.
    - 9 floats: altitude, then the grip quats.
    - The flags `TF`: immersive, SBS.
  - **HMD_SYNC** steps 0, 12, … 252. The runtime stores the pose sent with each value, and reads pixel (0,0) of the
    window: red = sync, green must be 0, alpha > 0. It auto-calibrates the colour mapping from the 22 distinct reds
    it sees. The projection layer then uses that stored pose (roll included), for both eyes.
  - **FOV:** the layer's FOV is symmetric, taken from what the app sends. With FOV 0 it uses its own (the packet
    then shows it, ×1.1). Its FOVX/FOVY naming is swapped in the per-frame computation.
  - **MODE_VR 2** is WinlatorXR's flat screen, which we use for menus.
- **Built:**
  - `headlook/vr_wxr.c`/`.h`: UDP listener, parser, mode sender; pose to yaw/pitch/**roll**. Roll is drawn,
    because the runtime places the picture at the full stored pose.
  - Frame pacing: wait up to 20 ms for a new sync.
  - The view scale covers the headset's own FOV, and the resulting FOV is reported back.
  - Eye capture through the brightness curve.
  - At Present (in mission): keep a copy of the back buffer (the HUD over black); eyes side by side; the HUD
    drawn into both halves with a premultiplied-alpha shader, at `vr_hud_dist` with the IPD disparity; the sync
    pixel via `ColorFill`.
  - Menus: MODE_VR 2, and no surfaces kept.
  - Shared GPU helpers moved to `headlook/vr_gpu.c`: shader assembly, a state-safe quad draw, the curve copy.
  - `stereo=wxr` wired into `headlook.c`, which also wires in the Reset/Present/CreateDevice hooks, the H/Y toggle
    and the light-gem crop.
  - New ini keys: `wxr_fov` (0 = the headset's own), `wxr_roll` (1), `wxr_ipd_scale` (1). The eye distance comes
    from the headset's IPD, converted at 1 unit = 1 foot.
  - `tools/wxr_fake.py`: WinlatorXR's side, for desktop tests.
- **Wine test** (ext4 copy, 1280x720, llvmpipe, `STEREO=wxr tools/run-wine.sh` + `tools/wxr_fake.py`; the user
  clicked into the first mission):
  - **Handshake:** version file written; MODE 2/0 sent at start; the fake then streamed; ~20k packets, 0
    unreadable; headset FOV learned (104 × 1.1 = 114.4).
  - **In mission:** MODE 1/1 and FOV 140.1 × 114.4 sent. The 140 is due to the 16:9 per-eye picture squeezed
    into half width (see below).
  - **Frames:** 1659 drawn, 1658 composed, 0 errors. User: "the view is panning left and right. The framerate
    looks good."
  - **Sync pixel:** read off the screen (x11grab of the window's top-left 2×2) as red 252 / 144 / 240 / 108, G=B=0.
    Correct.
  - **Timing:** 20-26 fps under llvmpipe, with ~30-37 ms of it in Present, i.e. the software GPU. Our own
    per-frame work: capture 0.1 ms, compose 0.1 ms.
  - **Not yet seen:** the right-eye half and the HUD in both halves; the i3 tile crops the window to its left
    half.
- **Known limitations / next:**
  - Each eye is drawn at the full back buffer shape and squeezed, which wastes horizontal pixels and FOV.
    Alternate-eye frames (MODE_3D 2) or a per-eye-shaped target would fix it.
  - The eye shift is along the level right vector, so it is wrong under roll.
  - The roll sign in the engine is unverified; `roll_sign` flips it.
  - The alpha byte of the X window as WinlatorXR reads it is unknown. If it's 0, the framesync is ignored
    (the picture is still shown, just without matching poses).
  - Phase 2: the controllers via `wxr_get_input()` → `xinput_joy`.
- **Full-window screenshot** (the user made the game fullscreen for it; the game was closed straight after at the
  user's request):
  - Correct side-by-side eyes with parallax.
  - **No HUD in either half**, not even the light gem. Expected: a ~96×108 px panel in each half's centre (45°
    of a 140° eye), but none visible when zoomed.
  - The log had no errors: both shaders were ready, and "HUD overlays drawn once per frame".
  - Unknown whether the HUD copy of the back buffer is empty, or the quad draw fails without saying so.
- **Diagnostic build deployed** to the ext4 copy:
  - `stereo_dump.now` / Insert also writes `dumpN_wxr_hud_src.bmp` (the HUD copy) and `dumpN_wxr_composed.bmp`
    (the final frame).
  - The HUD quad's rectangle is logged.
  - The HUD copy and quad draw failures, and `CreateStateBlock` failures, are now logged.
  - Next run: touch `~/games/thief_2/stereo_dump.now` while in a mission, then look at the two BMPs.

### 2026-09-26, evening (Phase 0 result on the headset; a WMR outage that wasn't ours)
- **Headset showed nothing (tracking fine):**
  - Our side was healthy: SteamVR was connected, Submit returned 0, and the old DLL behaved the same.
  - The SteamVR compositor log had "AcquireSync FAILED with WAIT_TIMEOUT" / "driver took the sync texture"
    warnings, but those also appear on working days.
  - Suggested isolating it via WMR's Cliff House, then SteamVR Home, and re-plugging the headset; the user said
    "that worked".
- **Phase 0 on the headset** (1440x1080, new DLL, `vr_pipeline=1`, `vr_gpu_gamma=1` by default):
  - **48-54 fps, was 44-46.**
  - frame 18.6-20.9 ms. alpha **0.0** (was 3.5; the curve shader is used: "brightness curve shader ready").
    readback **7-8.8, unchanged** (was ~7). upload 1.4. submit 4.0-5.2. hud 2.2-2.6.
  - The Reset hook fired once ("device Reset: capture surfaces released") with no failure.
  - So the pipelining didn't remove the readback cost. The NVIDIA D3D9 driver presumably syncs on everything
    queued, or the copy itself costs that much.
  - The remaining fix for the PC path is D3D9Ex shared surfaces (zero copy). Deferred: it doesn't help the Quest.

### 2026-09-26, night (Phase 2 controllers + the WinlatorXR test release)
- **HUD in the wxr mode:** the user saw the HUD in one run (not legible) and decided it isn't blocking for a Quest
  tester. HUD text was visible in the packaged run below. The diagnostic dump build stays in.
- **What WinlatorXR does with the controllers itself** (`XrController.java`):
  - Keyboard mapping, always on: the left menu button → Esc (hard-wired); A/B/X/Y, left grip/trigger and left
    stick directions → keys set in its controller settings.
  - Mouse emulation, on by default: right trigger/grip → left/right click, right stick up/down → scroll wheel,
    right stick left/right → snap turn. **The right hand's position also moves the mouse**, which would turn the
    body in Thief.

  So our own controller path is needed, with WinlatorXR's disabled.
- **Built (Phase 2):**
  - `headlook.dll` exports `headlook_xr_pad(XINPUT_STATE*)` (0 = not wxr, 1 = fresh, 2 = no data yet), via
    `wxr_xinput()`:
    - A/B/X/Y as labelled.
    - Left grip = LB (the chord button), right grip = RB.
    - Triggers = LT/RT (on/off only), stick clicks = L3/R3.
    - Sticks come in 0.1 steps. The left menu button is left out.
  - `vr_pad_buttons()` also reads it, so L3+R3 recentre and Y toggles the HUD.
  - `xinput_joy`: a `get_state()` wrapper (a real XInput pad first, else `headlook_xr_pad`).
  - `xr_mode` (from `HEADLOOK_STEREO` / `headlook.ini` `stereo`) reports the joystick present at enumeration,
    before `headlook.dll` exists.
  - New ini key `mouselook_pitch` (0 = the right stick only turns).
  - `tools/wxr_fake.py --input`: stick, trigger and grip demo.
- **Built (release):**
  - `tools/quest/`: `thief2vr_quest.bnd` (VR binds: grip = +use_item, A = jump, B = crouch, X/LT = next
    weapon/item, left-grip chords for drop/put away/previous/lean), `run_quest.bat` and `run_quest_1080.bat`
    (append the binds once, with a `user.bnd.before-quest` backup and a `thief2vr_binds.done` marker), and
    `README_QUEST.txt` (tester guide: WinlatorXR APK cats-27, a plain-game check first, container settings,
    `WINEDLLOVERRIDES=dinput=n,b`, controller settings, controls, what to report, ini quick fixes).
  - `tools/make_wxr_release.sh` → `dist/thief2-vr-winlatorxr-VERSION.zip`. The Quest `headlook.ini` and
    `xinput_joy.ini` are derived from the PC templates: `stereo=wxr`, `udp_port=0`, `vr_gamma=1.2`, the `wxr_*`
    keys, `mouselook_pitch=0`.
- **Packaged end-to-end test in Wine** (a clean copy `~/games/thief_2_questtest` with the STOCK exe, the zip
  unpacked, started via `run_quest.bat`, the fake headset; the user clicked into the mission):
  - The binds were appended and backed up.
  - The d3d9 stand-in loaded, then `headlook.dll` in wxr mode.
  - The handshake worked, and so did the mode switch to VR when the mission started.
  - `dinput.dll` enumerated the joystick 0.5 s before `headlook.dll` loaded, then picked up `headlook_xr_pad`.
  - User: "The game launched, there is visible hud text. The view was panning left and right."
  - **Not tested:** the controller demo inside a mission. It was stopped by the user after the fake's trigger
    clicks kept opening Credits in the main menu. Mouse1 from the trigger works in menus too; a tester's trigger
    will click the menu too, as intended.
- Learned: plain Wine needs `dinput=n` for our `dinput.dll` (run-wine.sh sets it), while `d3d9.dll` in the game
  folder loads without an override. Hence the README step.
- Habit: `pkill -f "[w]xr_fake.py"` killed its own shell, because the same command line contained
  `tools/wxr_fake.py` further on. Use a pattern that doesn't occur elsewhere in the command, or a PID.
