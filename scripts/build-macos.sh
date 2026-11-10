#!/usr/bin/env bash
# Builds dale for macOS as one universal binary (arm64 + x86_64) with the Metal GPU backend.
# Usage: scripts/build-macos.sh [install-dir]   (default: ~/.local/bin)
set -euo pipefail
cd "$(dirname "$0")/.."

git submodule update --init --depth 1
cmake -B build-macos -DCMAKE_BUILD_TYPE=Release -DDALE_TEST_HOOKS=OFF -DDALE_NATIVE=OFF \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build-macos -j"$(sysctl -n hw.ncpu)" --target dale
lipo -info build-macos/dale

dest="${1:-$HOME/.local/bin}"
mkdir -p "$dest"
install -m755 build-macos/dale "$dest/dale"
echo "installed: $dest/dale"
