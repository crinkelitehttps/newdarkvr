# How it works

A technical overview for modders. The day-by-day evidence behind each point is in [DEVLOG.md](DEVLOG.md).

All addresses are for the NewDark `Thief2.exe` with SHA-256 `af56a109…fba684`, at its preferred image base
0x400000. The DLLs rebase them at run time, and check the bytes at every patch site before touching anything.

## 1. Getting loaded without touching the exe

The game `LoadLibrary`s `d3d9.dll` by bare name, and d3d9 isn't a KnownDLL, so a `d3d9.dll` in the game folder
wins. **`d3d9proxy/`** is that DLL:

- It forwards all 17 d3d9 exports, using asm jump thunks resolved on first use. The target is the system
  `d3d9.dll`, or `d3d9_chain.dll` if present, so it can sit in front of ReShade and similar wrappers.
- `Direct3DCreate9`/`Ex` call the real function, then load `headlook.dll` and call its export
  `headlook_d3d_created(IDirect3D9*)`, on the game's own thread, before the game can create a device.

(The project started with a 33-byte loader stub patched into the exe's entry point: `patches/install_loader.py`.
It still works, but it can't be distributed and antivirus flags it.)

## 2. Turning the view: a render-only offset

`FUN_005cee30` is the per-frame scene render. At `0x5CEF38` it copies the camera's position and angles into a
*local* pose on its stack. `headlook.dll` patches that one instruction into a call that adds the head's yaw and
pitch to the local copy only, and only while the camera is on the player (camera mode 0).

The camera struct itself is never touched: movement, mouse look and projectiles keep using the body's heading.
Angles are 16-bit (65536 = 360°). A positive yaw offset turns the view left; a positive pitch offset tilts it down.

## 3. Two eyes

The frame handler calls the scene render at `0x5CF2E2` (the hardware path). That call is redirected to a wrapper
which calls the render twice. Each pass shifts the local position by ±IPD/2 along the view's right vector, through
the same stub as the head offset.

The engine sends pre-transformed vertices (FVF 0x1C4) and never sets a D3D matrix or viewport, so there's no
projection to edit. The eyes are made purely by drawing twice. The engine draws into its own offscreen target and
sets it inside the render call, so each eye is fetched with `GetRenderTarget(0)` *after* its pass. The D3D9 device
pointer lives at `0xA36040`, but only once a mission's 3D view is running.

## 4. Filling the headset

The float at **`0x7E62D8`** is the engine's view scale: 1/tan of half the 4:3 horizontal field of view (default
1.0, i.e. 90°). `FUN_005cee30` reads it every frame, multiplied by the camera's zoom.

The int at **`0x7DF800`** is 1 in widescreen modes (Hor+: extra width, same height) unless `widescreen_lock_hfov`
is set. So the vertical view is tan 0.75 at any resolution, far narrower than a headset.

For headset frames the wrapper writes a wider scale, chosen from the eyes' `GetProjectionRaw` tangents, and
restores the engine's own value afterwards. Bow and spyglass zoom are divided out, so the headset scale stays fixed.

Each eye goes to SteamVR on a canvas sized for both the game picture and that eye's frustum. The picture is pasted
at its true angular position, and the texture bounds select the eye's field of view, so the scale is exact.

## 5. Frames to SteamVR

`headlook.dll` runs as an OpenVR **scene application** (`vr_openvr.c`). Each frame:

1. `WaitGetPoses` gives the head pose, and paces the game. Yaw and pitch go into the offsets from section 2.
2. After each eye pass, the render target is copied on the GPU (`StretchRect`) into its own target. The engine's
   16-bit float HDR target is converted to 8-bit on the way.
3. After both passes, both are read back (`GetRenderTargetData`), run through a gamma lookup table, uploaded into
   D3D11 textures (OpenVR takes no D3D9 textures), and `Submit`ted **with the pose they were drawn at**: yaw and
   pitch, roll 0. The compositor's reprojection makes up the rest: head roll, and the time since `WaitGetPoses`.
4. While the game shows menus or is paused, the `Present` hook keeps submitting the last 3D frame with its
   original pose, so the world stays put in the room.

That CPU round trip costs roughly 7-10 ms of a ~23 ms frame. The planned fix is to share surfaces GPU-to-GPU,
which needs a D3D9Ex device; the stand-in `d3d9.dll` is where that switch would go.

## 6. HUD and menus

Three separate things, found by tracing D3D calls:

- **Screen overlays** (health shields, item frame, light gem, inventory models): `FUN_0058c080`, called at
  **`0x5CF06F`** near the end of the scene render, draws a queue of overlays and then empties it. So only the
  first render of a frame draws them, and they landed in the left eye only.
  The call is redirected. In headset frames both eye passes skip it, and after the eyes are captured it runs
  once, at the engine's own view scale, onto the cleared scene target.
- **Text** is drawn later, onto the back buffer.
- **The HUD panel:** at every `Present` (device vtable slot 17), the back buffer is read back and shown as an
  OpenVR overlay: head-locked in a mission, black made transparent; world-locked and opaque for menus. When the HUD
  is toggled off, the panel is cropped to the light gem.
  The `Present` slot gets replaced by something else from time to time, so the hook checks and re-installs itself.
  The device is caught at creation by hooking `IDirect3D9::CreateDevice` (slot 16), so menus reach the headset
  before `0xA36040` is set.

## 7. The controller (`xinput_joy/`)

The engine still has its original DirectInput joystick support and loads `dinput.dll` by name. `xinput_joy`
provides a synthetic DirectInput joystick backed by XInput:

- **Left stick** → the engine's analog movement binds.
- **Buttons** → joystick buttons, plus a second bank while a chord modifier (default LB) is held.
- **Right stick** → relative mouse movement via `SendInput`, since the joystick code has no analog pitch.
- **Back** and **right trigger** → Esc and mouse button 1, also via `SendInput`, from a thread that works in menus.

The engine's data format is classic `DIJOYSTATE` (80 bytes), polled through `GetDeviceState`.

## 8. Diagnostics

- **Logs**, next to the DLLs: `headlook.log` records the hooks, OpenVR state, and per-stage timing every 5 s.
  `d3d9proxy.log` and `dinput.log` cover the other two DLLs.
- **Dumps:** the **Insert** key writes both eye passes, the composite, the presented frame and the HUD panel as
  BMPs, plus a run-length log of that frame's D3D calls.
- **Static analysis** used Ghidra headless with the two scripts in `tools/ghidra/`: string cross-references, and
  decompiling at an address.
