# VRonMac

Run Windows VRChat in VR on Apple Silicon, with Quest streaming, headset audio and microphone, Lighthouse full-body tracking, and Quest Pro face and eye tracking over OSC.

VRonMac combines a native SwiftUI launcher, a Wine-side OpenXR runtime, a Metal bridge, and an OXRSys headset client. Windows compatibility uses Steam, NotProton, and Voile.

**Experimental preview.** VRChat has been tested in home and public worlds with VR, full-body tracking, and face tracking. Intermittent freezes and crashes remain under investigation. The combined installer still needs testing on a clean Mac.

## Downloads

The first app build is being prepared as **0.2.15-preview.1**. Its GitHub release remains a draft while bundled component redistribution is checked. See [release status](docs/release-status.md) for the exact outstanding work.

## Requirements

- Apple Silicon Mac. The current bundle declares **macOS 27 or later**, matching its existing native component deployment targets. Earlier macOS versions require a rebuild and hardware verification.
- Steam, Windows VRChat, and the supported NotProton/Voile compatibility setup.
- A Quest headset on the same local network. USB debugging is used for headset installation.
- Quest Pro for the tested eye/face tracking path.
- Supported Lighthouse trackers and USB receivers for full-body tracking.

Headset video and media use Wi-Fi. Hardware verification so far comes from the development setup.

## Setup and use

Follow [getting started](docs/getting-started.md). The launcher provides setup checks, headset installation, game preparation, tracker alignment, audio controls, and launch diagnostics.

If VRChat freezes or exits unexpectedly, record the approximate time and export launch diagnostics before relaunching. Review exported reports before sharing them because game and device logs can contain personal information.

## Development

Build the current launcher and run its isolated setup tests on a Mac with Xcode command-line tools.

```sh
bash scripts/build-launcher.sh
```

See [building](docs/building.md) for the component source layout, toolchains, and preview packaging. The launcher can be built from this repository. Assembling the full app currently also requires an existing component bundle.

## Credits and licenses

VRonMac includes code from OXRSys, VRCFaceTracking, BlackHole, libsurvive, Motoc, Eigen, and other projects. Project code uses the license in each file. Third-party code retains its upstream license. The MIT license at the root applies to the project's MIT-licensed portions.

See [third-party sources](docs/third-party-sources.md) and [release status](docs/release-status.md). VRonMac is an independent community project.
