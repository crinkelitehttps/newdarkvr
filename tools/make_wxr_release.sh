#!/bin/bash
# Build everything from source and package the standalone-Quest (WinlatorXR) test zip.
# Usage: tools/make_wxr_release.sh [VERSION]      (default: `git describe --tags --always --dirty`)
# Output: dist/thief2-vr-winlatorxr-VERSION.zip (+ its SHA-256). Only this project's files: no game files, no
# emulator (the tester installs WinlatorXR's own APK; see tools/quest/README_QUEST.txt).
# The Quest settings are derived from the PC templates (tools/hmd_bridge/vr/headlook.ini, xinput_joy/xinput_joy.ini)
# with a few keys changed, so they can't drift apart.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="${1:-$(git describe --tags --always --dirty)}"
NAME="thief2-vr-winlatorxr-$VERSION"

headlook/build.sh >/dev/null
d3d9proxy/build.sh >/dev/null
xinput_joy/build.sh >/dev/null 2>&1

mkdir -p dist
rm -f "dist/$NAME.zip"
python3 - "$ROOT" "dist/$NAME.zip" "$NAME" <<'EOF'
import re, sys, zipfile
root, out, name = sys.argv[1:4]
read = lambda p: open(f"{root}/{p}", encoding="utf-8").read().replace("\r\n", "\n")
crlf = lambda s: s.replace("\n", "\r\n")

def setkey(text, key, value):
    new, n = re.subn(rf"(?m)^{key}=.*$", f"{key}={value}", text)
    assert n == 1, key
    return new

hl = read("tools/hmd_bridge/vr/headlook.ini")
hl = hl.replace("; Stereo/VR is switched on by the launchers (run_openvr*.bat set HEADLOOK_STEREO=openvr), so starting Thief2.exe\n"
                "; directly plays normally.\n",
                "; wxr = standalone Quest/Pico under WinlatorXR (the game becomes the VR app through WinlatorXR's XrAPI).\n")
hl = setkey(hl, "stereo", "wxr")
hl = setkey(hl, "udp_port", "0")            # no OpenTrack on the headset
hl = setkey(hl, "vr_gamma", "1.2")          # the Quest's panels don't crush dark greys like the old WMR headset did
hl += """
; ---- stereo=wxr (WinlatorXR, standalone Quest/Pico) only ----
; Field of view drawn, degrees: 0 = the headset's own (as WinlatorXR reports it). Live.
wxr_fov=0
; Draw head roll (tilt). WinlatorXR places each picture at the full head pose it was drawn for, so this should
; stay on; roll_sign=-1 above flips it if the world tilts the wrong way. Live.
wxr_roll=1
; Eye separation = the headset's IPD times this (1 = true scale; smaller = flatter, bigger = stronger depth). Live.
wxr_ipd_scale=1
"""
xj = read("xinput_joy/xinput_joy.ini")
xj = setkey(xj, "mouselook_pitch", "0") if re.search(r"(?m)^mouselook_pitch=", xj) else xj.replace(
    "mouselook_invert_y=0\n",
    "mouselook_invert_y=0\n; 0 = the right stick only turns; looking up and down is left to the headset. Live.\nmouselook_pitch=0\n")
assert "mouselook_pitch=0" in xj

binary = {"d3d9.dll": "d3d9proxy/d3d9.dll", "headlook.dll": "headlook/headlook.dll", "dinput.dll": "xinput_joy/dinput.dll"}
text = {
    "headlook.ini": hl,
    "xinput_joy.ini": xj,
    "thief2vr_quest.bnd": read("tools/quest/thief2vr_quest.bnd"),
    "run_quest.bat": read("tools/quest/run_quest.bat"),
    "run_quest_1080.bat": read("tools/quest/run_quest_1080.bat"),
    "README_QUEST.txt": read("tools/quest/README_QUEST.txt"),
    "LICENSE.txt": read("LICENSE"),
}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dst, src in binary.items():
        z.write(f"{root}/{src}", f"{name}/{dst}")
    for dst, s in text.items():
        z.writestr(f"{name}/{dst}", crlf(s))
EOF
( cd dist && python3 -m zipfile -l "$NAME.zip" && sha256sum "$NAME.zip" )
