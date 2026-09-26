# Handoff (written 2026-09-21, updated 2026-09-22; see the 2026-09-25 update directly below)

Read `docs/DEVLOG.md` "Start here" first (CLAUDE.md), then this. This file is the short version for picking the
work up cold; the DEVLOG has the full history and evidence.

## Update 2026-09-25: the headset VR mode is the active work (supersedes "nothing pending" below)

`stereo=openvr` (`headlook/vr_openvr.c`, launched by `run_openvr.bat` / `run_openvr_1440.bat` in
`D:\games\thief_2_vr`) is **working and playable on the user's WMR headset** ("hilariously fun"): SteamVR
direct submit (per-eye CPU copy into D3D11), head pose from SteamVR, the engine's view widened to fill the headset
(`0x7E62D8`), a HUD/menu overlay panel (head-locked in game, world-locked for menus) carrying the engine's HUD
overlays (redirected from the first eye pass, `0x5cf06f`), an H/Y toggle, gamma curve, idle frames, and ~40-45 fps
at 1440x1080. The controller DLL (`xinput_joy/`, as `dinput.dll`) adds Back->Esc and RT->mouse1. The DEVLOG's
session log from 2026-09-23 onward has the details, the settings and the open issues. The user intends to
publish on the TTLG forums (DEVLOG: proxy-DLL instead of exe patch, licence, AV). Repo pushed privately to
GitHub (`crinkelitehttps/newdarkvr`) on 2026-09-25. The latest build (`vr_gem_always`, the light gem kept
visible with the HUD toggled off) was built but not yet deployed or tested at that point.

## Update 2026-09-26, night: WinlatorXR (standalone Quest) test release

- **Phase 0 (PC speed-ups):** on the headset, 48-54 fps (was 44-46).
- **Phase 1 (`stereo=wxr`, `headlook/vr_wxr.c`):** works in Wine against `tools/wxr_fake.py`.
- **Phase 2 (Touch controllers → `dinput.dll`):** built and packaged.
- **Release:** `tools/make_wxr_release.sh` makes `dist/thief2-vr-winlatorxr-VERSION.zip` for a Quest 3 owner who
  agreed to send feedback. The tester guide is `tools/quest/README_QUEST.txt`.
- **Next:** read the tester's logs (`headlook.log`, `d3d9proxy.log`, `dinput.log`, `Thief2.log`) and answers. Known
  open points: the per-eye squeeze (140° horizontal) → alternate-eye mode; HUD legibility; the roll sign; the
  controllers inside a mission (untested). DEVLOG's 2026-09-26 entries have the details.

## Update 2026-09-26: Quest 3 plan; Phase 0 speed-ups deployed, awaiting a headset run

The user wants the game standalone on a **Quest 3** via WinlatorXR, after quick PC speed-ups. The plan and the
speed-ups (pipelined readback + `Reset` hook, the GPU brightness curve, a pipelined HUD copy; ini `vr_pipeline`,
`vr_gpu_gamma`) are in DEVLOG's 2026-09-26 entry. Next: the user runs `run_openvr_1440.bat`, and we compare the
timing line with the baseline in that entry. Then Phase 1 (the `stereo=wxr` backend).

## The task

Give Thief II (NewDark, Windows exe, D3D9) head-tracked stereo VR by patching/hooking the binary at run time.
Done and working, in Wine, on the desktop (no headset): head-look (render-only view offset), side-by-side
stereo, and reference marks for true heading/pitch -- all three user-verified, the plain head-look one
called "excellent", the marks "works like a charm" and, separately, "accurate". A Windows Mixed Reality
headset test was tried and did not work; not triaged. That headset route is not the user's current focus.
**The user is now moving to unrelated work; there is no open thread on this project to pick back up without
being asked.**

**Unrelated new work also in this repo (2026-09-22): Xbox/XInput controller support, in `xinput_joy/`,
separate from everything else in this file.** Working and user-verified on native Windows (analog
movement, buttons, chording, right-stick mouselook, all tuned) -- see docs/DEVLOG.md "Active: XInput
controller support" under Goals and its two matching session-log entries (build, then two crash fixes +
tuning) for the full picture. Just deployed into the ext4 Wine sandbox copy alongside `headlook.dll` to
try combining with head-tracking; not yet actually run under Wine. Don't confuse the two projects when
picking work up cold.

## State

| Piece | State |
|---|---|
| Head-look via OpenTrack (`headlook/headlook.c`) | Works, user-verified; desktop (no headset) use called "excellent" |
| SBS stereo (`STEREO=sbs tools/run-wine.sh`) | Works in Wine; weapon shows in both halves; HUD/menus unsplit |
| Reference marks (`show_heading_marks`, 2026-09-22) | **User-confirmed working AND accurate** ("works like a charm", then "The marks are accurate"). `fov_deg` was left at its default (90) throughout; the user's method for judging "accurate" wasn't stated |
| Headset test (`tools/hmd_bridge/`, `D:\games\thief_2_vr`) | Tried, did not work; untriaged; not the current priority |
| OpenVR direct submit (`stereo=openvr`, `headlook/vr_openvr.c`, `run_openvr.bat`, 2026-09-23) | Built at the user's request as the replacement for Desktop+ ("not the most reliable"); only the DLL load/error path tested (Wine); **next: user runs it on the headset, then read `openvr:` lines in `headlook.log`** |
| Latency (reprojection presenter) | Largely covered by the direct submit (SteamVR reprojects submitted frames); nothing else started |

## First thing next session

Nothing is pending -- the last exchange ("The marks are accurate" / "we're going to work on something
else") closed this thread. Wait for the user to bring it back up rather than resuming it unprompted. If
they do:

* **If about the reference marks:** they've confirmed accuracy at the default `fov_deg=90`, so treat that as
  settled unless the user says otherwise -- don't re-open calibration unprompted. `_DAT_007e62d8` (an exact
  FOV value instead of the guess) is now a nice-to-have, not something blocking anything.
* **If about the headset:** nobody read `hmd_bridge.log`/`headlook.log`/`Thief2.log` from the failed
  attempt (likely gone/overwritten by now) -- start over: run `run_vr.bat`, then read those three logs on
  the `D:` mount for where it stopped working (DEVLOG has a table: "Logs to read after a headset run").
* **If about anything else:** this file's "Facts worth not re-deriving" and "Working conventions" below
  still apply to any further work on this DLL/binary.

## Next work, in the user's order of priority (as last known; may be stale, ask)

1. Whatever the user raises -- recent asks have been small, concrete "let's just try X" steps rather than
   working down a fixed list; don't assume any of the below is next without asking.
2. Deferred until asked: latency (DEVLOG has the plan: an OpenVR overlay presenter, world-locked at the
   render-time pose, for rotation-only reprojection), HUD/menus per eye, the headset test itself.
3. Loose ends: sign defaults untested un-inverted, sound direction, aim/arrow direction (the marks may help
   with this one directly), save/load and cutscene regression, the `_DAT_007e62d8` lead for an exact FOV
   (a nice-to-have now, not a fix for anything broken).

## Commands

```
headlook/build.sh                                  # headlook.dll (i686 mingw)
tools/hmd_bridge/build.sh                          # hmd_bridge.exe (x86_64 mingw; fetches openvr_api.dll)
tools/hmd_bridge/install_vr_copy.sh [--ini]        # sync DLL/bridge/bat (+ VR ini) into D:\games\thief_2_vr
STEREO=sbs tools/run-wine.sh                       # stereo in Wine (normal game if STEREO unset)
# deploy a DLL to the ext4 game copy while the game may be running (never plain cp over a loaded DLL):
cp headlook/headlook.dll ~/games/thief_2/headlook.dll.new && mv ~/games/thief_2/headlook.dll.new ~/games/thief_2/headlook.dll
# stereo diagnostic in a mission: touch ~/games/thief_2/stereo_dump.now   (writes stereo_*.bmp + D3D call trace to headlook.log; marks-only mode, stereo off, isn't covered by this trigger)
```

## Facts worth not re-deriving (addresses are for `bin/Thief2.orig.exe`, image base 0x400000)

* The engine sends **pre-transformed vertices** (FVF 0x1c4) and never sets a D3D matrix or viewport. The
  raw `IDirect3DDevice9*` is the global at `0xA36040`. Finding D3D calls needs `mov reg,[reg2+off]` then
  `call reg`; the old "no SetTransform" search used the wrong pattern.
* Render-copy hook `0x5CEF38` (local pose copy in `FUN_005cee30`) shifts view angles and, in stereo, position.
  The call-site wrapper (`hl_scene`, used for stereo AND/OR the reference marks) is at `0x5CF2E2` (the
  **hardware** branch of the frame handler); `0x5CF335` is the software fallback, not wrapped.
* The engine draws the scene into its **own offscreen render target** (`SetRenderTarget` inside the scene call)
  and later `StretchRect`s it over the back buffer. So take `GetRenderTarget(0)` *after* each pass/draw, not
  before. The stereo composite surface must be created and released every frame (a default-pool surface would
  break the engine's Reset).
* Heading 0..65535 = 360 deg CCW, forward = (cos h, sin h), right = (sin h, -cos h); the engine's view matrix
  rows are forward, left, up. OpenTrack convention: yaw + = head right, pitch + = up. In the game itself:
  +yaw offset turns the view LEFT, +pitch tilts it DOWN (this is the raw engine convention the hl_stub adds
  in, independent of ini signs -- used directly by the reference-marks math).
* `stereo_ipd` 0.21 (1 unit = 1 foot) and `fov_deg` 90 are both guesses, not read from the game -- `fov_deg`
  90 is now user-confirmed as accurate (or close enough), `stereo_ipd` is not separately confirmed. A
  possible exact source for FOV instead of the guess, not pursued: `_DAT_007e62d8` (VA `0x7e62d8`), see
  DEVLOG's 2026-09-22 entry.
* The reference-marks and stereo compositing share one call-site wrapper: it installs if `cfg.stereo ||
  cfg.show_marks` (both need `install_hook()` too, i.e. `cfg.enabled` or `cfg.stereo`); marks only actually
  draw when head-look's offset is really being applied (`g_hook_ok && cfg.enabled`), or they'd show an
  "error" that isn't real.

## Working conventions

* Test builds in the game (native Windows or the Wine copy) rather than reasoning only: a working rough version
  that can be tried beats a more precise one that can't. Verdicts come from playing.
* Never stop or restart a running game; never plain-`cp` over a loaded DLL (copy to `.new` and rename, or let the
  copy fail while it's locked); don't overwrite a tuned `headlook.ini` (new keys have code defaults).
* Keep checks against a live desktop/VNC/Wine session minimal and announced; prefer reading logs.
* No Windows execution from WSL (binfmt sends every `.exe` to Wine). Test Windows tools under a throwaway
  `WINEPREFIX` with `DISPLAY` unset.
* Commit and write handoff notes when asked, not proactively.
