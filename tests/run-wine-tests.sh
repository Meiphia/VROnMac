#!/usr/bin/env bash
# End-to-end test of the Wine side against the mock bridge, using plain Wine (Linux or macOS).
#   tests/run-wine-tests.sh [path/to/opencomposite/openvr_api.dll] [path/to/valve/openvr_api.dll]
# Needs: wine (64-bit), mingw-w64, cmake. Uses a throwaway prefix in /tmp/vrconmac-wine-test.
# With the OpenComposite dll it also runs vrapp.exe through OpenComposite; with Valve's
# openvr_api.dll too, it tests the "system-wide" install (game files untouched).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OC="${1:-}" VALVE="${2:-}"
export WINEPREFIX=/tmp/vrconmac-wine-test WINEDEBUG=-all
OUT=/tmp/vrconmac-test-out; mkdir -p "$OUT/dump"

runtime/build-mingw.sh >/dev/null
cmake -S bridge -B bridge/build >/dev/null && cmake --build bridge/build -j >/dev/null
x86_64-w64-mingw32-g++ -std=c++17 -O1 -Ithird_party tests/harness.cpp -o "$OUT/harness.exe" -ld3d11 -ldxgi -static
[ -d "$WINEPREFIX/drive_c" ] || wineboot -i >/dev/null 2>&1

bridge/build/vrconmac-bridge --backend mock --render-scale 0.5 --dump-dir "$OUT/dump" --dump-every 2 > "$OUT/bridge.log" 2>&1 &
BRIDGE=$!
trap 'kill $BRIDGE 2>/dev/null || true' EXIT
sleep 0.5

cp runtime/out/vrconmac_xr.dll "$OUT/"
echo "== harness (direct runtime calls) =="
wine "$OUT/harness.exe" "$(winepath -w "$OUT/vrconmac_xr.dll")" 300 | grep -E "^(ok|FAIL|PASS)"

if [ -n "$OC" ]; then
  [ -n "${OPENVR_HEADERS:-}" ] || { echo "set OPENVR_HEADERS to OpenComposite's OpenVRHeaders dir to build vrapp.exe"; exit 0; }
  x86_64-w64-mingw32-g++ -std=c++17 -O1 -I"$OPENVR_HEADERS" tests/vrapp.cpp -o "$OUT/vrapp.exe" -ld3d11 -static
  scripts/install-into-prefix.sh --prefix "$WINEPREFIX" --opencomposite "$OC" >/dev/null
  echo "== OpenVR app -> OpenComposite (as openvr_api.dll) -> runtime =="
  wine "$OUT/vrapp.exe" "$(winepath -w "$OC")" 300 | grep -E "^(ok|FAIL|PASS)"
  if [ -n "$VALVE" ]; then
    echo "== OpenVR app -> Valve openvr_api.dll -> openvrpaths.vrpath -> OpenComposite -> runtime =="
    wine "$OUT/vrapp.exe" "$(winepath -w "$VALVE")" 300 | grep -E "^(ok|FAIL|PASS)"
  fi
fi
echo "bridge log: $OUT/bridge.log   eye dumps: $OUT/dump"
