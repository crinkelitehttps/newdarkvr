# hmd_bridge: testing Thief II side-by-side stereo in a SteamVR headset

Prototype for the Windows host (the game runs **natively on Windows**, not under Wine; WSL cannot reach the
headset). Nothing here is a finished VR mode: it is a fixed floating "screen" with the stereo image split
per eye, and the game's camera follows your head rotation through the existing head-look hook.

```
headset --OpenVR--> hmd_bridge.exe --UDP 127.0.0.1:4242--> headlook.dll (in Thief2.exe)
                                                              |  draws the scene twice (left/right eye, side by side)
Thief2.exe window  --Desktop+ (3D mode Side-by-Side, HMD origin)-->  headset
```

## Newer route: `run_openvr.bat` (direct submit, no Desktop+, no bridge)

`headlook.dll` can talk to SteamVR itself (`stereo=openvr`, see `headlook/vr_openvr.c`): it takes the head pose
from SteamVR and submits each eye image straight to the headset, at the right scale, with SteamVR's own
reprojection. Start SteamVR, double-click `run_openvr.bat`, load a mission. Menus stay on the desktop; black
borders in the headset are where it sees wider than the game draws. Needs `openvr32\openvr_api.dll` (32-bit;
the installer copies it). Rough cut, not yet run on a headset: after a try, read the `openvr:` lines in
`headlook.log`. The rest of this file is the older bridge + Desktop+ route.

## Pieces

| File | What |
|---|---|
| `hmd_bridge.c` / `build.sh` | Reads the headset pose from OpenVR and sends OpenTrack-format UDP packets. `--selftest` checks the maths with no headset. |
| `vr/headlook.ini` | Settings for the VR copy: wide angle clamps, little smoothing, loopback-only listening. |
| `vr/run_vr.bat` | Starts the bridge and the game with `HEADLOOK_STEREO=sbs`. |
| `install_vr_copy.sh` | Syncs DLL, bridge, `openvr_api.dll`, bat (and the ini if missing) into the Windows-side copy. |
| `openvr_capi.h`, `LICENSE.openvr` | Valve's OpenVR SDK header, v2.15.6 (BSD-3). `openvr_api.dll` is fetched by `build.sh` and not committed. |

## Where things are

The Windows-side copy is `D:\games\thief_2_vr` (`/mnt/d/games/thief_2_vr` from WSL), copied from the ext4 game
copy with logs and the backup exe left out. The Steam install is untouched. Saves in the VR copy are separate.

## Running it

1. Put the headset on and start **SteamVR** (Windows Mixed Reality needs the "Windows Mixed Reality for SteamVR"
   plugin and Windows 10 or Windows 11 before 24H2; Microsoft is ending that support, reportedly November 2026).
2. Install **Desktop+** from Steam if you have not.
3. Double-click `D:\games\thief_2_vr\run_vr.bat`. Two windows appear: the bridge console ("Connected to OpenVR"
   once SteamVR is up) and the game (1920x1080). Start or load a mission.
4. In VR, open the SteamVR dashboard, then Desktop+, and add an overlay capturing the **Thief2 window** (Graphics
   Capture) or the desktop (Desktop Duplication mirrors exclusive fullscreen too). In the overlay's properties:
   * Advanced -> **3D Mode**: Side-by-Side. If the picture looks squashed or doubled in width, try the other
     Side-by-Side variant (half vs full).
   * Position -> **Origin: HMD** (glued to your head, 1:1) and adjust distance/width so it fills your view.
5. Turn your head: the game view should follow. Press **Pause/Break** or **Scroll Lock** to recentre (works
   while the game has focus). Close the game to close the bridge (the bat kills it); `Q` in its window quits it.

## Tuning (all live except where noted; edit `headlook.ini` in the VR folder)

* `stereo_ipd`: eye separation in game units, default 0.21 (a guess). Depth flat -> raise it; eyes strained -> lower it.
* `stereo_swap=1`: swap the halves if depth looks inside-out.
* `smoothing_ms`: more smoothing = steadier but laggier. `hmd_bridge.exe --predict-ms 25` predicts the pose
  further ahead to offset latency (default 15).
* Resolution: edit `-game_screen_size` in `run_vr.bat`; it must be a mode the engine knows.

## Limits to expect

* Latency: pose -> UDP -> game frame -> capture -> overlay, with nothing reprojecting in between. The image is
  head-locked, so any lag shows as the picture trailing your head. Yaw and pitch only; no positional tracking.
* The overlay shows the game's own field of view on a flat plane: no lens correction, no per-eye FOV.
* HUD and menus are drawn once at full width over the split image, not per eye.
* Roll is off by default (`use_roll=0`); its sign has never been checked.
* Native Windows rendering was not tested from here (only under Wine). If the picture is wrong natively, the
  suspects are the engine's post-process chain and MSAA (the launcher already passes `-postprocess=0
  -multisampletype=0`), and the exclusive-fullscreen window capture.
* If the game does not react to your head: look in `headlook.log` next to `Thief2.exe` (it shows the first
  packet, packet counts, and whether stereo was installed).
