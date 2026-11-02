#!/usr/bin/env bash
# Builds KenaePRO on Debian/Ubuntu and runs the tests.
# Usage: scripts/build-linux.sh [--no-install]   (run from the project folder)
set -euo pipefail

if [ "${1:-}" != "--no-install" ]; then
    sudo apt-get update
    sudo apt-get install -y cmake ninja-build g++ pkg-config \
        qt6-base-dev libqt6opengl6-dev qt6-multimedia-dev libgl1-mesa-dev \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        ffmpeg xvfb
fi

cmake -S . -B build -G Ninja
cmake --build build

# test_ui needs OpenGL; xvfb gives a virtual display when the machine has none.
if [ -z "${DISPLAY:-}" ]; then
    xvfb-run -a ctest --test-dir build --output-on-failure
else
    ctest --test-dir build --output-on-failure
fi
echo
echo "Check that test_ui shows 0 skipped:"
if [ -z "${DISPLAY:-}" ]; then xvfb-run -a build/tests/test_ui | tail -3; else build/tests/test_ui | tail -3; fi
