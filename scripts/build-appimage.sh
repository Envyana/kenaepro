#!/usr/bin/env bash
# Builds, tests and packages KenaePRO as an AppImage, inside the container from
# Containerfile.appimage. The result is build-appimage/KenaePRO-<version>-x86_64.AppImage.
set -euo pipefail

BUILD=build-appimage
VERSION=$(sed -n 's/^project(KenaePRO VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QTDIR" \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD"

# The same tests as the plain build, against this Qt and FFmpeg.
xvfb-run -a ctest --test-dir "$BUILD" --output-on-failure
xvfb-run -a "$BUILD/tests/test_ui" | tail -1
python3 -I blender/test_sync_protocol.py
python3 -I python/test_client.py

rm -rf "$BUILD/AppDir" "$BUILD"/KenaePRO-*.AppImage
DESTDIR="$PWD/$BUILD/AppDir" cmake --install "$BUILD"

# Our LGPL FFmpeg comes first so it is the copy that gets bundled (Qt Multimedia ships its own).
export LD_LIBRARY_PATH=/opt/ffmpeg/lib:$QTDIR/lib
export QMAKE=$QTDIR/bin/qmake
export LINUXDEPLOY_OUTPUT_VERSION=$VERSION
cd "$BUILD"
linuxdeploy-x86_64.AppImage --appdir AppDir \
    --desktop-file AppDir/usr/share/applications/kenaepro.desktop \
    --icon-file AppDir/usr/share/icons/hicolor/scalable/apps/kenaepro.svg \
    --plugin qt --output appimage
ls -la KenaePRO-*.AppImage
