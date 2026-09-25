#!/usr/bin/env python3
"""Stage B: add a constant yaw/pitch offset to the RENDER view only.

Builds bin/Thief2.patched.exe from the pristine bin/Thief2.orig.exe (never edits the original).

Where (DEVLOG, "Stage B map"): FUN_005cee30 is the per-frame scene render. EBP = the camera struct
(global 0xA2A988, which frob, projectile launch, movement and mouse-look all use). The function copies
the struct's position and angles into a LOCAL location on its stack, publishes a pointer to it in
DAT_0092DC44 (the "current view"), builds the view matrix with 0x678B30 (EDI = the local angles,
ESI = matrix 0x92D8F4) and draws. So offsetting the LOCAL angle copy changes only what is drawn.

The site (0x5CEF38) is the store of the local heading word:
    5cef34: mov [esp+0x24], eax        ; local angles: bank | pitch<<16
    5cef38: mov [esp+0x28], cx         ; local angles: heading         <- replaced by `call stub`
    5cef3d: mov [0x92dc44], edx
    5cef43: call 0x678b30
Inside the stub the return address sits on the stack, so those [esp+..] offsets are 4 higher.

Edits made:
  1. 5-byte `mov [esp+0x28],cx` at 0x5CEF38 -> `call <stub>`
  2. stub written into the zero padding at the end of .text (VA 0x71C220)
  3. .text VirtualSize raised so the loader maps the padding (it lies past the old VirtualSize)
No absolute addresses are embedded, so no relocation entries are needed.

History: experiment 1 patched the "camSynch" callback (0x54E7D4) instead. A debugger run showed that
callback never executes, so it had no effect (DEVLOG).

Usage: patches/render_yaw_offset.py [--yaw DEGREES] [--pitch DEGREES]
Angle units: 65536 = 360 deg. Positive heading turned the view LEFT in the Phase 0 script test.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "bin" / "Thief2.orig.exe"
DST = ROOT / "bin" / "Thief2.patched.exe"
ORIG_SHA256 = "af56a109a51ac9100a15da72cd403170a266736313679029fae1c222ebfba684"

IMAGE_BASE = 0x400000
TEXT_VA, TEXT_RAW = 0x401000, 0x400          # .text: VA 0x401000, file offset 0x400
STUB_VA = 0x71C220                            # inside the zero padding after .text's VirtualSize
SITE_VA = 0x5CEF38                            # `mov WORD PTR [esp+0x28], cx` in FUN_005cee30
SITE_ORIG = bytes.fromhex("66894c2428")


def file_off(va):
    return TEXT_RAW + (va - TEXT_VA)


def units(deg):
    return round(deg / 360.0 * 65536) & 0xFFFF


def make_stub(yaw_units, pitch_units):
    """Entered by `call` from the site: CX = heading, [esp+0x28] (post-call) holds bank|pitch."""
    code = bytearray()
    code += bytes.fromhex("6681C1") + struct.pack("<H", yaw_units)       # add cx, yaw
    code += bytes.fromhex("66894C242C")                                  # mov [esp+0x2c], cx   (heading)
    if pitch_units:
        code += bytes.fromhex("668144242A") + struct.pack("<H", pitch_units)   # add word [esp+0x2a], pitch
    code += b"\xC3"                                                      # ret
    return bytes(code)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--yaw", type=float, default=30.0, help="degrees added to render heading (default 30)")
    ap.add_argument("--pitch", type=float, default=0.0, help="degrees added to render pitch (default 0)")
    args = ap.parse_args()

    data = bytearray(SRC.read_bytes())
    if hashlib.sha256(data).hexdigest() != ORIG_SHA256:
        sys.exit("bin/Thief2.orig.exe does not match the reference SHA256; refusing to patch")

    stub = make_stub(units(args.yaw), units(args.pitch))

    # 1. redirect the site
    o = file_off(SITE_VA)
    if bytes(data[o:o + 5]) != SITE_ORIG:
        sys.exit(f"unexpected bytes at {SITE_VA:#x}: {bytes(data[o:o+5]).hex()}")
    data[o:o + 5] = b"\xE8" + struct.pack("<i", STUB_VA - (SITE_VA + 5))

    # 2. write the stub into the padding (must be all zero)
    so = file_off(STUB_VA)
    if any(data[so:so + len(stub)]):
        sys.exit("stub area is not zero padding")
    data[so:so + len(stub)] = stub

    # 3. raise .text VirtualSize to cover the stub
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sh = pe + 24 + opt_size                      # first section header (.text)
    name, vsize, vaddr, rawsize = struct.unpack_from("<8sIII", data, sh)
    assert name.rstrip(b"\0") == b".text"
    need = (STUB_VA + len(stub)) - IMAGE_BASE - vaddr
    assert need <= rawsize, "stub does not fit in the raw section data"
    if need > vsize:
        struct.pack_into("<I", data, sh + 8, need)

    DST.write_bytes(bytes(data))
    print(f"wrote {DST.relative_to(ROOT)}: yaw {args.yaw} deg = {units(args.yaw):#06x}, "
          f"pitch {args.pitch} deg = {units(args.pitch):#06x}, stub {len(stub)} bytes at {STUB_VA:#x}")
    print("stub:", stub.hex())
    print("sha256", hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
