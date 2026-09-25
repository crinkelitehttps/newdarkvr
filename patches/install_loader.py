#!/usr/bin/env python3
"""Stage B loader: make Thief2.exe load a helper DLL at startup.

Builds bin/Thief2.patched.exe from the pristine bin/Thief2.orig.exe (never edits the original).
The DLL (default `headlook.dll`, looked up by LoadLibraryA in the game directory) is where the
OpenTrack listener thread and the render hook will live. This patch only gets it loaded.

How: the PE entry point (0x701AFB) is the usual CRT startup, `call 0x7022A8 / jmp 0x70183B`. The
`call` is redirected to a small stub in the zero padding at the end of .text (0x71C220) that runs
    pushad
    LoadLibraryA("<dll>")        ; through the exe's own import slot, 0x71F0D4
    popad
    jmp 0x7022A8                 ; the original call target, so it returns to 0x701B00 as before
The stub is position-independent (it finds its string and the import slot relative to its own
address), so it needs no relocation entries and is correct even if ASLR moves the image. If the DLL
is missing, LoadLibraryA returns NULL and the game starts exactly as before.

.text VirtualSize is raised so the loader maps the padding (it lies past the old VirtualSize).

Usage: patches/install_loader.py [--dll NAME]
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
TEXT_VA, TEXT_RAW = 0x401000, 0x400
STUB_VA = 0x71C220                 # inside the zero padding after .text's VirtualSize
ENTRY_CALL_VA = 0x701AFB           # `call 0x7022A8` at the PE entry point
ENTRY_CALL_TARGET = 0x7022A8
LOADLIBRARYA_IAT_VA = 0x71F0D4     # exe's import slot for KERNEL32!LoadLibraryA


def file_off(va):
    return TEXT_RAW + (va - TEXT_VA)


def make_stub(dll):
    name = dll.encode("ascii") + b"\0"
    head_len = 1 + 5 + 1 + 6 + 1 + 6 + 1 + 5       # pushad, call $+5, pop ebx, lea, push, call [], popad, jmp
    here = STUB_VA + 6                              # runtime value EBX holds after `pop ebx`
    str_va = STUB_VA + head_len
    code = bytearray()
    code += b"\x60"                                                     # pushad
    code += b"\xE8\x00\x00\x00\x00"                                     # call $+5
    code += b"\x5B"                                                     # pop ebx  (= STUB_VA+6)
    code += b"\x8D\x83" + struct.pack("<i", str_va - here)              # lea eax,[ebx+str]
    code += b"\x50"                                                     # push eax
    code += b"\xFF\x93" + struct.pack("<i", LOADLIBRARYA_IAT_VA - here)  # call [ebx+iat]  (stdcall pops arg)
    code += b"\x61"                                                     # popad
    jmp_va = STUB_VA + len(code)
    code += b"\xE9" + struct.pack("<i", ENTRY_CALL_TARGET - (jmp_va + 5))   # jmp original target
    assert len(code) == head_len
    return bytes(code) + name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dll", default="headlook.dll", help="DLL file name to load (default headlook.dll)")
    args = ap.parse_args()
    if len(args.dll) > 64 or not args.dll.isascii():
        sys.exit("dll name must be short ASCII")

    data = bytearray(SRC.read_bytes())
    if hashlib.sha256(data).hexdigest() != ORIG_SHA256:
        sys.exit("bin/Thief2.orig.exe does not match the reference SHA256; refusing to patch")

    stub = make_stub(args.dll)

    o = file_off(ENTRY_CALL_VA)
    expect = b"\xE8" + struct.pack("<i", ENTRY_CALL_TARGET - (ENTRY_CALL_VA + 5))
    if bytes(data[o:o + 5]) != expect:
        sys.exit(f"unexpected bytes at entry call {ENTRY_CALL_VA:#x}: {bytes(data[o:o+5]).hex()}")
    data[o:o + 5] = b"\xE8" + struct.pack("<i", STUB_VA - (ENTRY_CALL_VA + 5))

    so = file_off(STUB_VA)
    if any(data[so:so + len(stub)]):
        sys.exit("stub area is not zero padding")
    data[so:so + len(stub)] = stub

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sh = pe + 24 + opt_size
    name, vsize, vaddr, rawsize = struct.unpack_from("<8sIII", data, sh)
    assert name.rstrip(b"\0") == b".text"
    need = (STUB_VA + len(stub)) - IMAGE_BASE - vaddr
    assert need <= rawsize, "stub does not fit in the raw section data"
    if need > vsize:
        struct.pack_into("<I", data, sh + 8, need)

    DST.write_bytes(bytes(data))
    print(f"wrote {DST.relative_to(ROOT)}: loads {args.dll!r}, stub {len(stub)} bytes at {STUB_VA:#x}")
    print("sha256", hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
