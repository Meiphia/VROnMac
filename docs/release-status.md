# 0.2.15-preview.1 release status

Prepared on 2026-11-21 from the local 0.2.15 development build and published as an experimental prerelease on 2026-11-22.

## Scope

The preview includes the native Mac launcher, Quest client, OpenXR bridge/runtime, audio and microphone support, Lighthouse full-body tracking, face/eye tracking helper, game preparation, and frame-stall diagnostics.

VRChat VR, audio, full-body tracking, face tracking over OSC, and successful EAC startup have been observed on the development setup. EAC and Steam compatibility depend on their current upstream behavior. This release makes no guarantee of future compatibility.

## Known limitations

- Intermittent Unity crashes and frame submission freezes remain unresolved.
- The complete setup and recovery flow needs a clean-Mac hardware test.
- Native components in the existing bundle require macOS 27. Lowering its Info.plist minimum alone does not provide compatibility.
- This preview is ad-hoc signed and has no Apple notarization.
- Wi-Fi carries headset video and media.
- Full component builds and packaging still depend on separately prepared assets.

## Binary publication

The compiled app is available as [0.2.15-preview.1](https://github.com/Meiphia/VROnMac/releases/tag/v0.2.15-preview.1), with a source archive and SHA-256 checksums. The dependency checks below remain outstanding.

The bundled native OSC codec was rebuilt from a SimpleRustOSC snapshot that has no license file or Cargo license declaration. Redistribution permission must be established, or the dependency replaced and the resulting face tracking path tested.

The complete media payload also needs a component-by-component license and corresponding-source inventory, including its Wine/GStreamer/FFmpeg libraries. Source patches are retained under patches/media and patches/graphics. Those patches alone do not form a complete corresponding-source distribution for every binary in the bundle.

The release owner's signing and notarization can be added later. Ad-hoc signing is disclosed here as a preview limitation.

## Source publication

The public repository contains the current launcher, tracking/calibration code, modified libsurvive source, Eigen snapshot, runtime source, and relevant media/graphics patches. It excludes game files, Steam binaries, user environments, caches, device captures, signing keys, generated binaries, and the unlicensed SimpleRustOSC snapshot.

Repository source is published under the license applicable to each file. Bundled upstream snapshots retain their notices. The build instructions identify the remaining externally supplied inputs.
