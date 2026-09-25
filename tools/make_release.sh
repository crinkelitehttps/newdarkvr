#!/bin/bash
# Build everything from source and package the player release zip.
# Usage: tools/make_release.sh [VERSION]      (default: `git describe --tags --always --dirty`)
# Output: dist/thief2-vr-VERSION.zip (+ its SHA-256, printed; put it in the release notes).
# The zip holds only this project's files and Valve's redistributable openvr_api.dll: never any game file.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="${1:-$(git describe --tags --always --dirty)}"
NAME="thief2-vr-$VERSION"

headlook/build.sh >/dev/null
d3d9proxy/build.sh >/dev/null
xinput_joy/build.sh >/dev/null 2>&1

mkdir -p dist
rm -f "dist/$NAME.zip"
python3 - "$ROOT" "dist/$NAME.zip" "$NAME" <<'EOF'
import sys, zipfile
root, out, name = sys.argv[1:4]
crlf = lambda p: open(f"{root}/{p}", encoding="utf-8").read().replace("\r\n", "\n").replace("\n", "\r\n")
binary = {                                           # zip path -> source (copied as is)
    "d3d9.dll": "d3d9proxy/d3d9.dll",
    "headlook.dll": "headlook/headlook.dll",
    "openvr32/openvr_api.dll": "headlook/openvr32/openvr_api.dll",
    "dinput.dll": "xinput_joy/dinput.dll",
}
text = {                                             # zip path -> source (Windows line endings)
    "headlook.ini": "tools/hmd_bridge/vr/headlook.ini",
    "xinput_joy.ini": "xinput_joy/xinput_joy.ini",
    "run_openvr_1440.bat": "tools/hmd_bridge/vr/run_openvr_1440.bat",
    "run_openvr.bat": "tools/hmd_bridge/vr/run_openvr.bat",
    "README.md": "README.md",
    "LICENSE.txt": "LICENSE",
    "LICENSE-openvr.txt": "tools/hmd_bridge/LICENSE.openvr",
}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dst, src in binary.items():
        z.write(f"{root}/{src}", f"{name}/{dst}")
    for dst, src in text.items():
        z.writestr(f"{name}/{dst}", crlf(src))
EOF
( cd dist && python3 -m zipfile -l "$NAME.zip" && sha256sum "$NAME.zip" )
