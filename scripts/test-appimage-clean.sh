#!/usr/bin/env bash
# Runs the AppImage in a clean container (no Qt, no FFmpeg): opens a clip and saves a screenshot.
# Usage, from the project folder:
#   podman run --rm -v "$PWD":/src:Z -w /src ubuntu:22.04 scripts/test-appimage-clean.sh <name>
set -euo pipefail
NAME=${1:-clean}
OUT=build-appimage/smoke
mkdir -p "$OUT"

export DEBIAN_FRONTEND=noninteractive
# A virtual display plus what every desktop has and the AppImage leaves to the system by design
# (AppImage excludelist): the OpenGL/EGL driver, fontconfig and a font, com_err, gpg-error.
apt-get update -qq && apt-get install -y -qq --no-install-recommends xvfb xauth libgl1 libegl1 \
    libfontconfig1 fonts-dejavu-core libcom-err2 libgpg-error0 >/dev/null
grep PRETTY_NAME /etc/os-release
ldd --version | head -1

# No FUSE inside a container: let the AppImage unpack itself and run.
export APPIMAGE_EXTRACT_AND_RUN=1
APP=$(ls build-appimage/KenaePRO-*-x86_64.AppImage)

# Anything the bundle still expects from the system that is missing here.
DIR=$(mktemp -d)
(cd "$DIR" && "$OLDPWD/$APP" --appimage-extract >/dev/null)
MISSING=$(find "$DIR/squashfs-root/usr" -name '*.so*' -o -path '*/usr/bin/*' -type f \
    | xargs ldd 2>/dev/null | grep 'not found' | sort -u || true)
echo "${MISSING:-no missing libraries}"
rm -rf "$DIR"
xvfb-run -a -s "-screen 0 1600x1000x24" "$APP" --version
xvfb-run -a -s "-screen 0 1600x1000x24" "$APP" --frame 30 --screenshot "$OUT/$NAME.png" \
    build-podman/smoke/test.mp4
echo "APP_EXIT=$?"
