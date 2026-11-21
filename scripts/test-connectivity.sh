#!/bin/bash
# Probe the replacement Wine DLL without starting VRChat or EAC.
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
OBELISK="$HOME/Library/Application Support/com.zagrios.obelisk"
RUNTIME="${VRCM_RUNTIME:-}"
if [ -z "$RUNTIME" ]; then
    for d in "$OBELISK"/Voile/v*/linux; do
        [ ! -x "$d/engine/fexdarwin-run" ] || RUNTIME="$d"
    done
fi
[ -n "$RUNTIME" ] || { echo "Voile Linux engine missing"; exit 1; }
[ -f "$RUNTIME/guest/steam-hostpid.so" ] || { echo "Runtime host PID library missing"; exit 1; }
mkdir -p "$PROJECT/work"
SCRATCH=$(mktemp -d "$PROJECT/work/connectivity-test.XXXXXX")
mkdir "$SCRATCH/home" "$SCRATCH/game"
cp -cR "$OBELISK/Prefixes/steam-438100-windows-linux/prefix" "$SCRATCH/home/prefix"
cp "$PROJECT/compatibility/wine-connectivity/"{probe.exe,windows.networking.connectivity.dll} "$SCRATCH/game/"
echo "Testing GetConnectionCost in a disposable Wine environment..."
echo "Test log: $SCRATCH/engine.log"
set +e
OBELISK_APP_NAME="VRConMac Networking Test" /bin/bash "$PROJECT/compat-tool/vrconmac-lkd/launch.sh" \
    "$RUNTIME" "$SCRATCH/home" "$SCRATCH/game" /opt/game \
    --graphics dxmt --bind "/opt/obelisk=$RUNTIME/guest:ro" \
    --env HOME=/home/user --env USER=user \
    --env WINEPREFIX=/home/user/prefix --env WINEDEBUG=err+all \
    --env WINEDLLOVERRIDES=windows.networking.connectivity=n \
    --env PATH=/opt/wine/bin:/usr/bin:/bin --env LANG=en_US.UTF-8 \
    -- /opt/game/probe.exe 2>&1 | tee "$SCRATCH/engine.log"
status=${PIPESTATUS[0]}
set -e
result="$SCRATCH/game/probe-result.txt"
if [ ! -f "$result" ]; then
    echo "FAIL: probe did not write a result (engine exit $status). Logs kept in $SCRATCH"
    exit 2
fi
cat "$result"
if [ "$status" -ne 0 ] || ! /usr/bin/grep -q '^PASS:' "$result"; then
    echo "FAIL: networking test did not pass (engine exit $status). Logs kept in $SCRATCH"
    exit 1
fi
# launch.sh waited for this disposable prefix's wineserver to stop.
rm -rf "$SCRATCH/home"
echo "Networking API test passed. Result and logs kept in $SCRATCH"
