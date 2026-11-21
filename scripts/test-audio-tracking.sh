#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[[ ! -f config/build.env ]] || source config/build.env
BUILD="$ROOT/build/audio-tracking"
mkdir -p "$BUILD"
INCLUDES=(-Ivendor/OXRSys/runtime/src -Ivendor/OXRSys/common/protocol/include -I"$BUILD/runtime/_deps/spdlog-src/include")
clang++ -std=c++20 -pthread -DVRCONMAC_MEDIA_PORT=47910 -DVRCONMAC_TRACKING_PORT=47911 "${INCLUDES[@]}" tests/media/relay_harness.cpp vendor/OXRSys/runtime/src/DuplexMediaRelay.cpp -o "$BUILD/test-relay"
clang++ -std=c++20 -pthread -Ivendor/OXRSys/common/protocol/include tests/media/TestPcmRing.cpp -o "$BUILD/test-ring"
"$BUILD/test-ring"
python3 tests/media/test_relay.py
export DOTNET_CLI_HOME="${DOTNET_CLI_HOME:-$BUILD/dotnet-home}"
export NUGET_PACKAGES="${NUGET_PACKAGES:-$BUILD/nuget}"
"${DOTNET:-dotnet}" run --project tests/media/tracking/Tests.csproj -c Release --artifacts-path "$BUILD/dotnet"

swiftc -module-cache-path "$BUILD/swift-cache" tests/media/TestAudioCapture.swift -o "$BUILD/test-audio-capture" -framework CoreMedia -framework AudioToolbox
"$BUILD/test-audio-capture"
