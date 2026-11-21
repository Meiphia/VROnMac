#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[[ ! -f config/build.env ]] || source config/build.env
BUILD="$ROOT/build/audio-tracking"
mkdir -p "$BUILD" "$ROOT/dist/audio-tracking"
export DOTNET_CLI_HOME="${DOTNET_CLI_HOME:-$BUILD/dotnet-home}"
export NUGET_PACKAGES="${NUGET_PACKAGES:-$BUILD/nuget}"
export ANDROID_USER_HOME="${ANDROID_USER_HOME:-$BUILD/android-user}"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$BUILD/gradle}"
DOTNET="${DOTNET:-dotnet}"
: "${JAVA_HOME:?Set JAVA_HOME to JDK 17}"
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK (platform 35, NDK 27.2.12479018)}"
cmake -S vendor/OXRSys -B "$BUILD/runtime" -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_MODULE_PATH="$ROOT/build-support" -DVulkan_INCLUDE_DIR="$ROOT/vendor/Vulkan-Headers/include"
cmake --build "$BUILD/runtime" -j6
ctest --test-dir "$BUILD/runtime" -E oxrsys_runtime_api_tests --output-on-failure
export CARGO_HOME="${CARGO_HOME:-$BUILD/cargo}"
export CARGO_TARGET_DIR="$BUILD/osc"
cargo build --manifest-path vendor/SimpleRustOSC/Cargo.toml --release --locked --target aarch64-apple-darwin
cp "$BUILD/osc/aarch64-apple-darwin/release/libfti_osc.dylib" vendor/VRCFaceTracking/VRCFaceTracking/runtimes/osxarm64/fti_osc.dylib
"$DOTNET" build tracking/quest-pro/VRConMac.Tracking.csproj -c Release --artifacts-path "$BUILD/dotnet" -o "$BUILD/module"
"$DOTNET" publish vendor/VRCFaceTracking/VRCFaceTracking/VRCFaceTracking.csproj -c Release -r osx-arm64 --self-contained true --artifacts-path "$BUILD/dotnet" -o "$BUILD/vrcft"
[[ -f config/vrconmac.keystore ]] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore config/vrconmac.keystore -storepass android -alias androiddebugkey -keypass android -dname 'CN=VRConMac Local Build' -keyalg RSA -keysize 2048 -validity 10000
(cd vendor/OXRSys/clients/Android/android-vr && ./gradlew assembleRelease -PvrconmacKeystore="$ROOT/config/vrconmac.keystore")
cp vendor/OXRSys/clients/Android/android-vr/app/build/outputs/apk/release/app-release.apk dist/audio-tracking/VRConMac-Quest.apk
swiftc -parse-as-library -swift-version 5 -module-cache-path "$BUILD/swift-cache" audio/AudioControl.swift -o "$BUILD/VRConMacAudio" -framework SwiftUI -framework ScreenCaptureKit -framework AVFoundation -framework CoreAudio -framework AudioToolbox
clang -arch arm64 -O2 build-support/launcher.c -o "$BUILD/VRCFaceTrackingLauncher"
mkdir -p "$BUILD/VRConMac Microphone.driver/Contents/MacOS"
clang -arch arm64 -O2 -fblocks -bundle -include audio/MicrophoneConfig.h vendor/BlackHole/BlackHole/BlackHole.c -o "$BUILD/VRConMac Microphone.driver/Contents/MacOS/VRConMacMicrophone" -framework CoreAudio -framework CoreFoundation -framework Accelerate
python3 scripts/package-audio-tracking.py
bash scripts/test-audio-tracking.sh
