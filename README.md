# Thief II VR (NewDark)

Play **Thief II: The Metal Age** in a SteamVR headset. The game is drawn separately for each eye and follows your
head, the HUD and menus float on a panel in front of you, and an Xbox-style controller works throughout. Nothing in
the game install is modified: the mod is a few files dropped into the game folder.

> **Status: early.** Tested on one PC (Windows 10, RTX 3050) with one Windows Mixed Reality headset. Expect
> rough edges; reports are welcome.

## What you get

- **Stereo 3D with head tracking:** the view turns with your head, 1:1, and SteamVR keeps the world steady
  between frames. Your body's aim (movement, arrows) stays where you point it with the mouse or right stick.
- **Full field of view:** the game's view is widened to fill the headset.
- **HUD panel:** the light gem, health, inventory and item text on a panel in front of you. Show or hide it any
  time; the light gem can stay visible on its own.
- **Menus and books in the headset,** on a panel fixed in the room.
- **Controller support** (optional, `dinput.dll`): analog movement, right-stick look, attack on the right trigger,
  the menu on Back, and every other button bindable in the game's own controls menu.

## Requirements

- Windows 10 or 11 and **SteamVR** with a working headset.
- **Thief II with NewDark**, as sold on Steam/GOG. The mod is written for one exact build of `Thief2.exe`
  (SHA-256 `af56a109a51ac9100a15da72cd403170a266736313679029fae1c222ebfba684`). With a different build it
  refuses to hook the game and says so in `headlook.log`, rather than crash.
- A reasonably modern graphics card. Every frame is copied through the CPU on its way to SteamVR (see Known issues).

## Install

Download the release zip, and copy everything inside its `thief2-vr-…` folder into the Thief II folder (where
`Thief2.exe` is):

| File | What it is |
|---|---|
| `d3d9.dll` | Loads the mod when the game starts Direct3D; passes everything else to Windows' own `d3d9.dll` |
| `headlook.dll`, `headlook.ini` | The VR mod and its settings |
| `openvr32\openvr_api.dll` | Valve's OpenVR library (32-bit), in an `openvr32` subfolder |
| `run_openvr_1440.bat`, `run_openvr.bat` | Launchers |
| `dinput.dll`, `xinput_joy.ini` | Optional: controller support |

If you already use another `d3d9.dll` (ReShade, for example), rename that one to `d3d9_chain.dll` first; the mod's
`d3d9.dll` passes everything through to it.

**Uninstall:** delete those files (and rename `d3d9_chain.dll` back if you had one). The game itself is never changed.

## Play

1. Start SteamVR with the headset on.
2. Run **`run_openvr_1440.bat`** (1440x1080; the smoother choice), or `run_openvr.bat` (1920x1440: sharper, slower).
3. Load a mission, then recentre (below) while looking straight ahead.

| Action | Keyboard | Controller |
|---|---|---|
| Recentre the view | Pause/Break or Scroll Lock | Click both sticks (L3 + R3) |
| Show/hide the HUD panel | H | Y |
| Game menu | Esc | Back |
| Attack / click in menus | Mouse button 1 | Right trigger |
| Look / turn | Mouse | Right stick |
| Move | as bound | Left stick |

With the controller, holding **LB** gives every other button a second identity, so you can bind twice as many
actions in *Options > Controls > Customize Controls*.

## Settings

`headlook.ini`, in the game folder. Changes take effect within about two seconds, with no need to restart:

| Setting | Default | What it does |
|---|---|---|
| `vr_gamma`, `vr_black` | 1.3, 0 | Brightness of the headset picture. Raise `vr_gamma` (1.5-2.0) if shadows are too dark; `vr_black` 0.02-0.05 helps headsets that crush dark greys |
| `vr_hud_deg`, `vr_hud_down`, `vr_hud_dist` | 45, 0, 1.5 | HUD panel width (degrees), tilt below straight ahead (degrees), distance (metres) |
| `vr_menu_deg`, `vr_menu_down` | 50, 5 | The same for menus and books |
| `vr_gem_always` | 1 | Keep the light gem visible when the HUD is hidden (`vr_gem_x0`..`vr_gem_y1` frame it) |
| `vr_hud_key`, `vr_hud_pad` | 72 (H), 32768 (Y) | The show/hide key (a Windows key code) and controller button mask |
| `stereo_ipd` | 0.21 | Eye separation in game units: raise for stronger depth |
| `vr_view_scale` | 0 (auto) | How wide the game draws; 0 fills your headset |

The controller's settings (dead zones, look sensitivity, button behaviour) are in `xinput_joy.ini`.

## Known issues

- **Frame rate:** about 40-45 fps at 1440x1080 on an RTX 3050, with SteamVR filling in the rest. Each frame is
  copied from the game's Direct3D 9 to SteamVR through the CPU; a direct GPU-to-GPU path is planned.
- **Rotation only:** leaning or moving your head sideways doesn't move the view (use the game's lean keys).
- **Looking up/down with the mouse or stick tilts the world** in the headset. Leave the body level and look with your head.
- **HUD text is small.** It is drawn for a monitor; hide the HUD when you don't need it.
- **The monitor** shows only the HUD over black while you play (the 3D view goes to the headset).
- Microsoft is ending Windows Mixed Reality support in SteamVR (reportedly November 2026).

## Also in this repository

- **Head-tracking without a headset** (`headlook.dll` on its own): turn the view with your head using a webcam
  tracker such as OpenTrack, on an ordinary monitor.
- **Development notes:** [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) is a technical overview for modders.
  [docs/DEVLOG.md](docs/DEVLOG.md) is the full development log.

## Building from source

Cross-compiled with mingw-w64 (32-bit, `i686-w64-mingw32-gcc`) on Linux or WSL:

```
headlook/build.sh      # headlook.dll (also downloads the 32-bit openvr_api.dll into headlook/openvr32/)
d3d9proxy/build.sh     # d3d9.dll
xinput_joy/build.sh    # dinput.dll (controller)
```

The launchers and the default `headlook.ini` for VR are in `tools/hmd_bridge/vr/`. `tools/make_release.sh [VERSION]`
builds all three and packages the player zip in `dist/`.

## Credits

Thief II: The Metal Age by Looking Glass Studios. NewDark by its community developers. The OpenVR SDK by Valve
(BSD-3-Clause licence, `tools/hmd_bridge/LICENSE.openvr`). This project is not affiliated with or endorsed by any of
them, and ships none of the game's files.

This project's own code is under the [MIT licence](LICENSE).
