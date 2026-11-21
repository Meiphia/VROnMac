#!/usr/bin/env bash
# Builds vrconmac-bridge on macOS. Uses cmake if installed, otherwise plain clang++
# (Xcode command line tools: `xcode-select --install`).
set -euo pipefail
cd "$(dirname "$0")"
if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
else
  mkdir -p build
  clang++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -I../third_party \
    src/main.cpp src/server.cpp src/backend_mock.cpp src/backend_openxr.cpp src/xr_loader.cpp \
    -x objective-c++ -fobjc-arc src/gfx_metal.mm \
    -framework Metal -framework Foundation \
    -o build/vrconmac-bridge
fi
echo "Built $(pwd)/build/vrconmac-bridge"
