# Building VRonMac

## Launcher

Use an Apple Silicon Mac with Xcode command-line tools and Python 3. The current component bundle targets macOS 27.

```sh
bash scripts/build-launcher.sh
```

This compiles launcher/ sources, the C++ microphone ring, and the Swift setup tests. Outputs are build/launcher/VRonMac and build/launcher/setup-tests. Tests use temporary fixture directories.

The active launcher sources were promoted from the development work directory for this first source publication. The older scripts/package-launcher.py assembles the earlier 0.1 developer preview. Use scripts/package-preview.py for the current launcher.

## Component source layout

| Path | Component |
| --- | --- |
| launcher/ | Current SwiftUI launcher, installer, session integration, microphone ring |
| runtime/ | Standard Wine-side OpenXR runtime and frame watchdog |
| bridge/ | Standard native Metal bridge |
| tracking/lighthouse/fullbody/ | Full-body bridge, runtime and tracker service |
| tracking/lighthouse/calibration/ | GPL-3.0-only Motoc-derived calibration and synthetic tests |
| tracking/lighthouse/libsurvive/ | Modified upstream solver and macOS registration/semaphore integration |
| tracking/lighthouse/eigen/ | Eigen source snapshot used by the tracker service |
| tracking/quest-pro/ | Quest Pro face tracking module |
| vendor/OXRSys/ | Modified native headset runtime and Android client |
| vendor/VRCFaceTracking/ | Modified face helper source |
| vendor/BlackHole/ | Microphone driver source |
| compatibility/wine-connectivity/ | Wine networking replacement source |
| patches/media/ | Media build recipe and local Wine patch |
| patches/graphics/ | Large resource upload patch for Neptune |

## OpenXR and tracking

Build the standard runtime with MinGW-w64 and the bridge with CMake.

```sh
bash runtime/build-mingw.sh
bash bridge/build.sh
bash tracking/lighthouse/fullbody/runtime/build-mingw.sh
bash tracking/lighthouse/fullbody/bridge/build.sh
```

The Lighthouse service is a target of the modified libsurvive CMake project. Install HIDAPI, pkg-config, and CMake, and configure a static build with HIDAPI enabled.

```sh
cmake -S tracking/lighthouse/libsurvive -B build/lighthouse \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC=ON -DUSE_HIDAPI=ON
cmake --build build/lighthouse --target vrconmac-tracker-service
```

The input archive hashes and upstream Eigen/OpenComposite commits are retained in tracking/lighthouse/fullbody/source-versions.json. Calibration carries its GPL license in tracking/lighthouse/calibration/COPYING.

## Headset, audio and face helper

scripts/build-audio-tracking.sh records the earlier component build flow. It requires JDK 17, Android SDK platform 35, NDK 27.2.12479018, .NET, Rust, CMake, and the configured OpenXR dependencies. Read docs/audio-tracking.md for its toolchain configuration.

The public source snapshot omits the unlicensed SimpleRustOSC input and generated native runtime binaries from VRCFaceTracking. As a result, the full audio/face build currently requires separately supplied OSC/runtime inputs. Resolve that dependency before producing a public binary distribution.

## Package the current preview

Supply a previously built and verified VRonMac component app explicitly. This command compiles the launcher from the repository and reuses the component payload without rebuilding it.

```sh
bash scripts/build-launcher.sh
python3 scripts/package-preview.py \
  --components-app "/path/to/VRonMac 0.2.15.app" \
  --output dist/VRonMac.app
```

Packaging verifies the input signature and payload, updates source notices and payload hashes, signs the result, and runs the app's payload verification. Use --identity with a signing identity when one is configured. The default is ad-hoc signing.

This packaging step establishes launcher buildability and bundle integrity. It does not establish complete component reproducibility or clean-Mac compatibility. See docs/release-status.md before publishing app assets.
