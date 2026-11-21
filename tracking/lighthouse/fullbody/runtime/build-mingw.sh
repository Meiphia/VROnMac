#!/usr/bin/env bash
# Cross-compiles the Wine-side runtime DLL with MinGW-w64 (works on macOS via `brew install mingw-w64`,
# or on Linux via your distro's mingw-w64 package). Output: runtime/out/vrconmac_xr.dll
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p out
$CXX -std=c++17 -O2 -g0 -shared -Wall -Wno-unknown-pragmas \
  -I../third_party -I../../../../runtime/src \
  src/runtime.cpp src/transport.cpp src/common.cpp src/pixels.cpp \
  -o out/vrconmac_xr.dll \
  -static -static-libgcc -static-libstdc++ \
  -ld3d11 -ldxgi -lws2_32 \
  -Wl,--exclude-all-symbols
cp vrconmac_xr.json out/
[ -f out/vrconmac.ini ] || cp vrconmac.ini.example out/vrconmac.ini
echo "Built $(pwd)/out/vrconmac_xr.dll"
