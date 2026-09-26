#!/usr/bin/env python3
"""Stand-in for WinlatorXR's side of the XrAPI, to test headlook.dll's stereo=wxr on the desktop (Wine).

Mimics WinlatorXR (app/src/main/java/com/winlator/xr/api/XrAPI.java + XrVersion05.java):
  * clears Z:\\tmp\\xr (= /tmp/xr under Wine, or --dir) and writes a "system" file,
  * waits for the game to write "version" there, then listens on UDP 7278 for "L_HAP R_HAP MODE_VR MODE_3D FOVX FOVY",
  * once MODE_VR > 0, sends a v0.5 packet at --hz to UDP 7872 (and 7873): controllers, head quaternion swaying in yaw
    (and a little pitch/roll), IPD, FOVX/FOVY (the headset's own until the game sets them, then an echo), HMD_SYNC
    stepping 0, 12, ... 252, 19 T/F buttons, 9 more floats, "TF" flags.
Usage: tools/wxr_fake.py [--hz 72] [--yaw 30] [--period 8] [--still] [--dir /tmp/xr]
Ctrl-C to stop. Prints what the game sends and a line of counters every 5 s.
"""
import argparse, math, os, socket, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--hz", type=float, default=72.0)
ap.add_argument("--yaw", type=float, default=30.0, help="yaw sway amplitude, degrees")
ap.add_argument("--pitch", type=float, default=10.0, help="pitch sway amplitude, degrees")
ap.add_argument("--roll", type=float, default=0.0, help="roll sway amplitude, degrees")
ap.add_argument("--period", type=float, default=8.0, help="sway period, seconds")
ap.add_argument("--still", action="store_true", help="head still, straight ahead")
ap.add_argument("--fov", type=float, nargs=2, default=[104.0, 96.0], help="the fake headset's own FOVX FOVY")
ap.add_argument("--dir", default="/tmp/xr")
ap.add_argument("--input", action="store_true",
                help="controller demo: left stick pushes forward 2 s of every 6, right trigger pulled at 3 s, right grip at 4.5 s")
a = ap.parse_args()

os.makedirs(a.dir, exist_ok=True)
for f in os.listdir(a.dir):
    os.remove(os.path.join(a.dir, f))
with open(os.path.join(a.dir, "system"), "w") as f:
    f.write("FAKE\nWXR_FAKE_PY\n14\n2026-09-01\n4128x2208\n")
print(f"wrote {a.dir}/system; waiting for the game to write {a.dir}/version ...", flush=True)
while not os.path.exists(os.path.join(a.dir, "version")):
    time.sleep(0.2)
print("version:", open(os.path.join(a.dir, "version")).read().strip(), flush=True)

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("0.0.0.0", 7278))
rx.setblocking(False)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

inp = [0.0] * 6      # L_HAPTICS R_HAPTICS MODE_VR MODE_3D HMD_FOVX HMD_FOVY, as XrVersion02 stores them
last_msg = None
sync = 0
sent = 0
t0 = time.time()
next_stat = t0 + 5


def quat_yaw_pitch_roll(yaw_right, pitch_up, roll_right):
    """Head orientation for OpenXR (+Y up, looking along -Z): yaw about +Y (right = negative angle), then pitch about
    +X (up = positive), then roll about -Z... composed as q = qyaw * qpitch * qroll."""
    def q_axis(ax, ay, az, ang):
        s = math.sin(ang / 2)
        return (ax * s, ay * s, az * s, math.cos(ang / 2))

    def mul(p, q):
        px, py, pz, pw = p
        qx, qy, qz, qw = q
        return (pw * qx + px * qw + py * qz - pz * qy,
                pw * qy - px * qz + py * qw + pz * qx,
                pw * qz + px * qy - py * qx + pz * qw,
                pw * qw - px * qx - py * qy - pz * qz)
    r = math.radians
    return mul(mul(q_axis(0, 1, 0, -r(yaw_right)), q_axis(1, 0, 0, r(pitch_up))), q_axis(0, 0, 1, -r(roll_right)))


while True:
    # what the game says
    try:
        while True:
            data, _ = rx.recvfrom(1024)
            msg = data.decode("ascii", "replace").strip()
            parts = msg.split()
            for i, p in enumerate(parts[:6]):
                try:
                    v = float(p)
                except ValueError:
                    continue
                if v > 0 or i >= 2:
                    inp[i] = v
            if msg != last_msg:
                print(f"{time.time() - t0:7.1f}s game -> {msg!r}", flush=True)
                last_msg = msg
    except BlockingIOError:
        pass

    now = time.time()
    if inp[2] > 0:       # MODE_VR > 0: WinlatorXR sends the HMD state every frame
        ph = 2 * math.pi * (now - t0) / a.period
        yaw = 0 if a.still else a.yaw * math.sin(ph)
        pitch = 0 if a.still else a.pitch * math.sin(ph * 0.5)
        roll = 0 if a.still else a.roll * math.sin(ph * 0.7)
        q = quat_yaw_pitch_roll(yaw, pitch, roll)
        fovx = inp[4] if inp[4] > 1 else a.fov[0] * 1.1
        fovy = inp[5] if inp[5] > 1 else a.fov[1] * 1.1
        ls_y, btn = 0.0, ["F"] * 19
        if a.input:
            c = (now - t0) % 6.0
            if c < 2.0: ls_y = 0.8
            if 3.0 <= c < 3.3: btn[18] = "T"     # R_TRIGGER
            if 4.5 <= c < 4.8: btn[12] = "T"     # R_GRIP
        sync = sync + 12
        if sync >= 256:
            sync = 0
        f3 = lambda v: f"{v:.3f}"
        fields = (["client0"] +
                  [f3(0), f3(0), f3(0), f3(1), "0.0", f"{ls_y:.1f}", f3(-0.2), f3(1.2), f3(-0.3)] +
                  [f3(0), f3(0), f3(0), f3(1), "0.0", "0.0", f3(0.2), f3(1.2), f3(-0.3)] +
                  [f3(q[0]), f3(q[1]), f3(q[2]), f3(q[3]), f3(0), f3(1.6), f3(0), "0.0640",
                   f"{fovx:.2f}", f"{fovy:.2f}", str(sync)] +
                  ["".join(btn)] +
                  [f3(1.6)] + [f3(0), f3(0), f3(0), f3(1)] * 2)
        pkt = (" ".join(fields) + " FT").encode("ascii")
        for port in (7872, 7873):
            tx.sendto(pkt, ("127.0.0.1", port))
        sent += 1
    if now >= next_stat:
        print(f"{now - t0:7.1f}s sent {sent} packets; game's MODE_VR {inp[2]:.0f} MODE_3D {inp[3]:.0f} FOV {inp[4]:.1f} x {inp[5]:.1f}",
              flush=True)
        next_stat = now + 5
    time.sleep(max(0.0, 1.0 / a.hz - (time.time() - now)))
