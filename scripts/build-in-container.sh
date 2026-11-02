#!/usr/bin/env bash
# Configures, builds and tests KenaePRO inside the container from Containerfile.debian13.
# Uses its own build folder so it never mixes with a build made on the host.
set -euo pipefail

BUILD=build-podman
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD"

# test_ui needs OpenGL; xvfb gives a virtual display (Mesa software rendering).
xvfb-run -a ctest --test-dir "$BUILD" --output-on-failure
echo
echo "test_ui (look for 0 skipped):"
xvfb-run -a "$BUILD/tests/test_ui" | tail -3
echo
python3 -I blender/test_sync_protocol.py
python3 -I python/test_client.py
