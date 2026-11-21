#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD="$ROOT/build/launcher"
mkdir -p "$BUILD"
clang++ -std=c++17 -O2 -c launcher/MicRing.cpp -o "$BUILD/MicRing.o"
swiftc -parse-as-library -swift-version 5 -module-cache-path "$BUILD/cache" \
  -import-objc-header launcher/MicRing.h \
  launcher/SetupCore.swift launcher/SetupInstaller.swift launcher/SteamVRMetadata.swift \
  launcher/SessionCore.swift launcher/AudioControl.swift launcher/Launcher.swift \
  "$BUILD/MicRing.o" -lc++ -o "$BUILD/VRonMac" \
  -framework SwiftUI -framework ScreenCaptureKit -framework AVFoundation \
  -framework CoreAudio -framework AudioToolbox -framework CryptoKit
swiftc -swift-version 5 -module-cache-path "$BUILD/cache" \
  launcher/SetupCore.swift launcher/SetupInstaller.swift launcher/SteamVRMetadata.swift \
  launcher/SessionCore.swift tests/launcher/SetupTests.swift -o "$BUILD/setup-tests"
"$BUILD/setup-tests"
echo "Built $BUILD/VRonMac"
