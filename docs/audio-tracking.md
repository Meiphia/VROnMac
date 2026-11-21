# Audio and native face tracking

## Current verified state

- Original working runtime and launch configuration retain their saved SHA-256 values.
- VRConMac Quest release APK installs alongside the original OXRSys client; it is not debuggable.
- The Quest Pro granted microphone, eye and facial tracking permissions.
- A timed LAN check sent 5,000 stereo audio packets; the user heard the test beeps. The Mac
  received 4,268 microphone packets and 1,684 samples with valid facial and eye tracking.
- Native Apple Silicon VRCFaceTracking opens and loads the VRConMac Quest Pro module with
  eye and expression support. Its application settings live in `~/Documents/VRCFaceTracking`.
- The new bridge/runtime connects to the headset. The Mac audio app opens and registers with it.
- The installed virtual microphone initially needed CoreAudio to reload. After `sudo killall
  coreaudiod`, the user successfully selected Use headset microphone (button became disabled).
- After rebuilding the malformed upstream ARM64 OSC library, native OSC serialization/UDP tests pass. The app loaded the current avatar configuration (77 parameters), sent OSC messages, and the user confirmed face tracking works in VRChat.
- The user confirmed both headset game audio and the headset microphone now work in VRChat. The speaker capture failure was reproduced: macOS 27 rejects an oversized AudioBufferList (-12737) but accepts its queried exact size. A standalone CoreMedia regression test reproduces the oversized-buffer rejection and verifies the exact-size extraction. The updated helper uses that exact size. Its virtual microphone source now connects directly to the selected output device and marks rendered audio as non-silent.

These observations establish working audio and tracking end to end in VRChat. Avatar calibration
and behavior under every world/avatar load were not exhaustively tested.

## Starting a session

1. Open **VRConMac Quest** on the Quest. This experimental build uses WiFi; USB may remain
   connected for charging or sideloading. Keep Mac and Quest on the same LAN.
2. Run `bash scripts/start-audio-vr.sh` from Terminal. It opens both Mac apps and starts the
   bridge with the experimental runtime. The checkerboard gives way when the game connects.
3. In **VRConMac Audio**, click **Start headset audio**. Allow the Mac's Screen & System
   Audio Recording permission if requested. Capture is Mac system audio, excluding this app.
4. Click **Use headset microphone**. The status must confirm selection and that button becomes
   disabled. Start VRChat **after this step**: the Wine audio host chooses the Mac input at startup.
5. Launch VRChat normally through Steam with the existing NotProton VRConMac compatibility
   tool and DXMT backend. Confirm game sound in the Quest and its microphone meter responding.
6. Enable VRChat OSC in the radial menu, **Options → OSC → Enabled**. Use an avatar configured
   for VRCFaceTracking/Unified Expressions; tracking software cannot add facial blendshapes or
   avatar parameters that the avatar does not contain.
7. VRCFaceTracking should discover VRChat's OSC configuration and show outgoing messages.
   If OSC was enabled after VRCFaceTracking started, restart VRCFaceTracking or reload its OSC
   configuration. Native SteamVR/OpenVR initialization may warn on Mac; this tracking module
   receives Quest data over the LAN relay and sends OSC without a native SteamVR runtime.

**Stop** in the audio app, or normal Quit, closes capture and restores the previous Mac input.
The Quest microphone closes when the Mac microphone registration expires (about three seconds).
If the app is forcibly killed, select the previous input manually in Mac Sound settings.
Face/eye data flows only to the local VRCFaceTracking module; it is not uploaded.

## Microphone device installation and recovery

`dist/audio-tracking/VRConMac Microphone.pkg` installs a separate two-channel CoreAudio HAL
loopback device. It is built from BlackHole with its own name, bundle identifier and device UID;
existing audio drivers are not replaced. The helper writes returning mono headset audio to this
virtual device, and Wine captures its input through the existing audio path.

If the helper cannot find the device after installation, stop audio, run `sudo killall coreaudiod`
in Terminal, then quit/reopen the helper. This briefly interrupts Mac sound. Restarting the Mac is
an alternative. The installer does not terminate the audio service automatically.

To disable/remove the device, choose another input and quit the audio app, then follow
`scripts/uninstall-microphone.sh`. It moves the driver aside rather than deleting it; restart
macOS to unload it. This installer/driver is a local build with an ad-hoc signature.

## Implementation

- `vendor/OXRSys/common/protocol/include/oxrsys/protocol/DuplexMedia.h`: versioned PCM16/face packet layout.
- Native runtime `DuplexMediaRelay`: independent worker, active video-client IP, endpoint tokens,
  local audio producer, local-only face forwarding. Live UDP port 47900; tracking loopback 47901.
- Quest `DuplexClient`: AAudio stereo playback and mono capture, bounded PCM buffers, no network
  work in audio callbacks. Microphone opens only after the Mac requests it.
- Quest `XrApp::UpdateFace`: optional FB eye-social and face-tracking2 extensions, head-relative
  eye orientations and 70 facial weights. No artificial pupil-diameter measurements.
- Mac SwiftUI app: ScreenCaptureKit audio capture and CoreAudio/AVAudioEngine virtual-input routing.
- `tracking/quest-pro`: .NET module, FB-to-Unified-Expressions mapping adapted from ALXR (MIT),
  gaze conversion, packet checks, out-of-order rejection and one-second stale-data neutralization.
- Native VRCFaceTracking: upstream v6 development snapshot with macOS/Apple Silicon support;
  local changes bundle this module, find the Voile VRChat OSC cache, keep data under Documents,
  locate its module process relative to the app, and disable external crash reporting for this build.

This is an initial uncompressed LAN audio transport, with bounded startup buffering but no
packet-loss concealment or adaptive clock resampler. The old upstream `headset_audio` option
belongs to a different reserved protocol; this experiment uses the helper's explicit controls.

## Building and checks

On this Mac, `config/build.env` points to the already downloaded .NET 10 ARM64 SDK, Rust toolchain, JDK 17,
Android SDK platform 35 / build-tools 35 / NDK 27.2.12479018 / CMake 3.22.1, and shared caches.
These local paths and the local sideload key are excluded from the source archive.

Run `bash scripts/build-audio-tracking.sh`. It builds the runtime, native tracking app/module,
Quest release APK, Mac audio helper and HAL driver. Packaging signs local binaries and creates
an installer. The packaging script accepts `VRCONMAC_DIST_DIR` for a staging destination; the
current verification build used `build/audio-tracking/rebuild-check` so running apps were not
replaced. To publish a rebuild into the usual folder, set `VRCONMAC_DIST_DIR="$PWD/dist/audio-tracking"`
after quitting the apps. The matching rebuilt runtime manifest lives inside that package's
`runtime` folder; copy both files into a new runtime-host folder before selecting it.

Run `bash scripts/test-audio-tracking.sh` for PCM-ring concurrency, relay behavior, packet/token
validation, face/blink/gaze mappings, stale data reset, and native OSC encoding/UDP delivery.
These tests use separate ports 47910/47911 and do not replace a live headset connection.

The runtime's three non-GPU test groups pass. The full Metal API suite cannot run inside the
agent's execution environment; real GPU/headset checks run through the user's interactive bridge.
The driver bundle and both Mac apps pass deep/strict signature verification, and the extracted
installer payload was checked. No debugger was attached to VRChat or EAC; no EAC checks were modified.

## Rollback

Quit the two helper apps, close the new bridge and select the original OXRSys Quest client.
Start the bridge with `--runtime-json runtime-host/oxrsys-paced-pose/oxrsys-runtime.json` and the
previous render scale. The Steam compatibility configuration, Wine networking fix and game files
were preserved. `docs/working-baseline-sha256.json` records the original working binaries.
