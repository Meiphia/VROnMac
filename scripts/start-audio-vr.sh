#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/work"
open "$ROOT/dist/audio-tracking/VRConMac Audio.app"
open "$ROOT/dist/audio-tracking/VRCFaceTracking Mac.app"
echo "Keep this window open. Start headset audio in the audio app, then launch VRChat through Steam."
"$ROOT/bridge/build/vrconmac-bridge" --backend openxr --render-scale 1.0 --test-pattern --runtime-json "$ROOT/runtime-host/oxrsys-audio-tracking/oxrsys-runtime.json" 2>&1 | tee "$ROOT/work/audio-tracking-bridge.log"
