#!/usr/bin/env python3
"""Fake OpenTrack: send its "UDP over network" packet (6 little-endian doubles: x y z in cm,
yaw pitch roll in degrees) so headlook.dll can be tested without a webcam.

  tools/opentrack_sim.py                       # yaw sweeps +-40 deg at 0.25 Hz, 60 packets/s, to 127.0.0.1:4242
  tools/opentrack_sim.py --yaw 30              # hold yaw 30 deg
  tools/opentrack_sim.py --pitch -20 --seconds 10
  tools/opentrack_sim.py --mode both           # yaw and pitch sweep together
  tools/opentrack_sim.py --tcp                 # send fixed 48-byte records over TCP (port 4243) instead
"""
import argparse, math, socket, struct, time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=None, help="default 4242 (UDP) or 4243 (--tcp)")
ap.add_argument("--tcp", action="store_true")
ap.add_argument("--mode", choices=["sweep", "pitch", "both"], default="sweep")
ap.add_argument("--yaw", type=float, help="hold this yaw (degrees) instead of sweeping")
ap.add_argument("--pitch", type=float, help="hold this pitch (degrees) instead of sweeping")
ap.add_argument("--amp", type=float, default=40.0, help="sweep amplitude in degrees")
ap.add_argument("--hz", type=float, default=0.25, help="sweep frequency")
ap.add_argument("--rate", type=float, default=60.0, help="packets per second")
ap.add_argument("--seconds", type=float, default=0.0, help="0 = run until interrupted")
a = ap.parse_args()

if a.tcp:
    s = socket.create_connection((a.host, a.port or 4243))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    send = s.sendall
else:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (a.host, a.port or 4242)
    send = lambda b: s.sendto(b, dest)

t0 = time.time()
n = 0
try:
    while a.seconds <= 0 or time.time() - t0 < a.seconds:
        t = time.time() - t0
        sw = a.amp * math.sin(2 * math.pi * a.hz * t)
        yaw = a.yaw if a.yaw is not None else (sw if a.mode in ("sweep", "both") else 0.0)
        pitch = a.pitch if a.pitch is not None else (sw if a.mode in ("pitch", "both") else 0.0)
        send(struct.pack("<6d", 0.0, 0.0, 0.0, yaw, pitch, 0.0))
        n += 1
        time.sleep(1.0 / a.rate)
except KeyboardInterrupt:
    pass
print(f"sent {n} packets")
