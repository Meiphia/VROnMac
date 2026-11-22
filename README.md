# Human-written Intro

First off: This was vibecoded as hell. I made nothing, I told a machine to make it. Anyone who uses AI and claims they "made" an app is lying to themself and people around them. That said, generally all I had was a concept of how VR works on Linux, after doing that for a long time, and while there are other VR methods out there for MacOS, none tackle face-tracking or Full-Body Tracking via Quest Pro. So, I decided to give that a go with all the Linux lessons in mind.

This is a simple app that relies on NotProton and Crossover at its foundation. It will not work without these things. You'll also need to sideload the Quest APK (see in Releases along with the app itself) by using something like SideQuest (which requires Developer mode turned on in your headset).

What has been tested so far is VRChat, which works decently well on an M4 Pro 24GB Macbook Pro. Performance is pretty varied but I think most people who play VRC are more or less used to 30-50fps even in optimal conditions. The main known issue right now is that it crashes randomly. Haven't been able to track down what's causing it, but my theory is it has something to do with video streaming or encoding from video players. Other than that it runs quite smoothly, temps seem to sit around 85-90 celsius once the fans kick in which is alright.

# VRonMac

Run Windows VRChat in VR on Apple Silicon, with Quest streaming, headset audio and microphone, Lighthouse full-body tracking, and Quest Pro face and eye tracking over OSC.

VRonMac combines a native SwiftUI launcher, a Wine-side OpenXR runtime, a Metal bridge, and an OXRSys headset client. Windows compatibility uses Steam, NotProton, and Voile.

**Experimental preview.** VRChat has been tested in home and public worlds with VR, full-body tracking, and face tracking. Intermittent freezes and crashes remain under investigation. The combined installer still needs testing on a clean Mac.

## Downloads

Download **[0.2.15-preview.1 for Apple Silicon](https://github.com/Meiphia/VROnMac/releases/download/v0.2.15-preview.1/VRonMac-0.2.15-preview.1-macOS-arm64.zip)** from [GitHub Releases](https://github.com/Meiphia/VROnMac/releases/tag/v0.2.15-preview.1). The release includes a source archive and SHA-256 checksums. See [release status](docs/release-status.md) for known limitations and outstanding dependency checks.

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
