#!/usr/bin/env python3
"""UDP -> TCP relay for OpenTrack's "UDP over network" output.

Why: the game runs in WSL2 in NAT mode, which cannot receive UDP sent to the Windows host's LAN
address (Windows can forward TCP into WSL, e.g. ssh, but not UDP). headlook.dll also accepts the same
data as fixed 48-byte records over TCP (default port 4243), and TCP can ride the existing ssh path:

  laptop:   OpenTrack --UDP--> 127.0.0.1:4242 --> this relay --TCP--> 127.0.0.1:4243
  ssh:      the laptop's 127.0.0.1:4243 is forwarded to the WSL instance's 127.0.0.1:4243
            (PuTTY: Connection > SSH > Tunnels, source port 4243, destination localhost:4243, Local;
             or `ssh -L 4243:127.0.0.1:4243 user@<desktop>`)
  game:     headlook.dll (TCP listener) -> view offset

Run this on the laptop (needs only Python 3):  python opentrack_relay.py
Options:  --listen 4242   UDP port OpenTrack sends to (set OpenTrack's target to 127.0.0.1 : this port)
          --host 127.0.0.1 --port 4243   where the TCP connection goes (the forwarded end)
It reconnects by itself if the tunnel or the game restarts, and keeps only the newest data.
"""
import argparse
import socket
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--listen", type=int, default=4242, help="UDP port to listen on (default 4242)")
ap.add_argument("--host", default="127.0.0.1", help="TCP target host (default 127.0.0.1)")
ap.add_argument("--port", type=int, default=4243, help="TCP target port (default 4243)")
a = ap.parse_args()

udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(("0.0.0.0", a.listen))
udp.settimeout(1.0)
print(f"listening for OpenTrack on UDP {a.listen}; forwarding to TCP {a.host}:{a.port}", flush=True)

sent = 0
last_report = time.time()
while True:
    try:
        tcp = socket.create_connection((a.host, a.port), timeout=3)
        tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("connected to the game side", flush=True)
    except OSError as e:
        print(f"cannot connect to {a.host}:{a.port} ({e}); retrying in 2 s", flush=True)
        time.sleep(2)
        continue
    try:
        while True:
            try:
                data = udp.recv(2048)
            except socket.timeout:
                continue
            if len(data) >= 48:
                tcp.sendall(data[:48])
                sent += 1
            if time.time() - last_report > 5:
                print(f"forwarded {sent} packets", flush=True)
                last_report = time.time()
    except OSError as e:
        print(f"connection lost ({e}); reconnecting", flush=True)
        try:
            tcp.close()
        except OSError:
            pass
        time.sleep(1)
