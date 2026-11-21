# VRConMac duplex media experiment

This fork retains the submitted-pose and absolute-frame-pacing fixes. It adds a separate,
versioned UDP side channel on port 47900 for 48 kHz PCM16 stereo Mac audio, mono Quest
microphone, and Quest Pro eye/face samples. It does not change the existing video or bridge
wire protocols. This custom Quest package is WiFi-only (USB is for sideloading/charging).
The original upstream client and the known-working runtime remain usable independently.

A worker thread relays packets outside XR frame submission. Registration is restricted to
the active video-client IP and a local Mac producer; per-endpoint random tokens validate
subsequent packets. Packets stay below 1056 bytes. Audio callbacks use bounded PCM buffers;
network I/O never runs in an audio callback. Startup buffering is 20 ms; excessive backlog
is discarded instead of growing latency. This is an initial uncompressed LAN transport,
with no packet-loss concealment or clock-drift resampler; real headset tuning is pending.

The Mac audio app uses ScreenCaptureKit with current-process audio excluded. It sends Mac
system audio, not an individual Windows process. Headset mic packets feed a separate
CoreAudio loopback device derived from GPL-3.0 BlackHole. Selecting this device as the Mac
input lets the existing Wine audio backend capture it. The Mac helper restores the previous
input on Stop or normal Quit. Abrupt process termination may require restoring it manually.

The Quest app requests microphone, eye and face permissions. Eye/face tracking uses optional
XR_FB_eye_tracking_social and XR_FB_face_tracking2 extensions, with head-relative gaze
orientations. Unsupported or unpermitted tracking produces inactive flags; face values are
not inferred. Tracking samples are forwarded only to loopback UDP 47901. The native
VRCFaceTracking module maps the 70 FB face weights to Unified Expressions and neutralizes
stale data after one second. Gaze conversion follows the ALXR module convention. Pupil size
is not measured or synthesized.

The legacy headset_audio config/feature bit remains reserved for the upstream audio protocol;
this experimental protocol has its own handshake and the Mac helper's explicit Start button.
No legacy audio support is advertised. Audio transmission stops when the Mac helper stops;
the Quest microphone opens only after Use headset microphone is selected in the Mac helper
and the Android microphone permission is allowed. Capture stops when that registration expires
(approximately three seconds after Stop), or when the Quest client exits.

Validation: native runtime build and three non-GPU test groups; release Quest APK build;
concurrent PCM-ring tests; loopback relay tests (both directions, endpoint tokens, malformed
packets, NaN); .NET face/blink/gaze mapping and UDP stale/order tests; native ARM64 app and
HAL-driver signature checks; extracted installer payload verification. Tests are kept in the
parent project's tests/media directory. Interactive tests confirmed native Mac GUI operation, speaker test tones, microphone return,
valid eye/face samples, and finally working VRChat headset audio, microphone and native VRCFaceTracking OSC.
The Mac helper queries the exact CoreMedia audio-buffer-list size (macOS 27 rejects larger lists);
its virtual-mic source connects directly to the selected output node.
