> Current publication status is documented in [release status](release-status.md). The plan below records the earlier installer design and its status at the time.

# VRConMac: from working setup to a GitHub app

## Intended experience

Download one signed Mac app, connect a Quest over USB and authorize debugging, then choose
**Set up**. The app checks Steam, prepares its own compatible Windows environment, installs
its headset client and starts VRChat with audio and optional face/eye tracking. Later sessions
use **Start VRChat**. No Homebrew, Xcode, Python, .NET installation, terminal commands or
separate launchers should be required on the end user's Mac.

Steam sign-in/game installation, VRChat login, macOS privacy/administrator approvals and
headset microphone/face/eye permission dialogs remain user actions. Developer Mode does not
replace them. The app should guide these steps rather than claim a silent installation.

## Developer preview built in this project

`dist/launcher/VRConMac.app` combines a native setup/status window and the existing audio
controller. It bundles the verified bridge, patched OXRSys runtime, Quest APK, native
VRCFaceTracking helper, microphone installer and Google's official ADB tools. The app has no
Jaz/project-directory paths in its executable and locates resources inside its own bundle.
The current .NET tracker retains its existing per-user data location.

Implemented:

- Detect the installed Windows VRChat game in Steam libraries, supported Steam/client pairing,
  the known Voile engine, configured game environment and microphone driver.
- Distinguish USB authorization, offline and connected states; require explicit device selection
  when multiple devices are connected. Confirm a selected device is a Quest before installation.
- Install/update the headset APK using bundled ADB; it retains the existing independent package ID.
- Offer the bundled microphone package through the standard macOS installer.
- Check all bundled payload hashes and reject path traversal/external symlinks before use.
- Start the bridge, headset client, native tracker and inline audio/microphone controller, then
  ask Steam to start VRChat. Stop only this launcher's bridge and restore the previous Mac input.
- Refuse a second bridge or parallel separate audio helper instead of disrupting an active session.
- A read-only `--doctor` command and isolated setup tests.

**This is a local developer preview, not a finished installer.** Fresh-Mac NotProton installation,
Voile downloading and environment initialization are not yet wired into this app. Its UI says so
and disables Start until a compatible configured installation exists. The launch path has been
compiled and checked, but its combined GUI/session flow has not yet replaced the separately
verified launcher in a hardware test. The existing working setup remains unchanged.

The binaries currently used by the bridge/runtime were built with a macOS 27 deployment target.
The preview declares that minimum honestly. Supporting earlier macOS releases requires rebuilding
and testing the complete payload against an explicit deployment baseline; changing Info.plist
alone cannot make those binaries compatible. The app is ad-hoc signed, not notarized.

## Remaining work before the first public release

1. **Fresh installation, owned by the app.** Use `~/Library/Application Support/VRConMac` for
   downloaded/versioned components, game environments, logs and state. Adopt existing installations
   only as an explicit migration option. Implement resumable, verified downloads and a journal of
   completed steps. Roll back interrupted installs without deleting an existing working environment.
2. **Steam integration without a CrossOver requirement.** NotProton currently targets CrossOver;
   our working Linux tool uses its Steam hook but also delegates helper verbs to a NotProton runner.
   A fresh install must replace that helper dependency and install/verify the necessary Steam hook,
   supported signature databases and game mapping. Use a narrow, versioned integration rather than
   silently editing a running Steam installation. Preserve/restorable originals and check supported
   Steam builds before changes.
3. **Bootstrap the Linux environment.** Voile publishes a downloadable release and Obelisk's source
   documents direct Linux wineboot. Initialize the prefix, fetch a matching Linux Steam SDK from
   Valve, install OpenComposite and the bridge DLL, apply the tested networking fix and configure
   registry/VR paths. Keep game/EAC files untouched. Do not copy a user's existing prefix into releases.
4. **Complete USB transport.** The current custom headset client uses WiFi for video and duplex
   media, even with USB attached. Upstream OXRSys has USB video/tracking support, but our audio/face
   side channel is UDP. A USB-only promise needs an ADB-reversed TCP media channel and tests for
   reconnection, unplugging and WiFi fallback. Do not advertise USB-only streaming yet.
5. **First-run permissions and recovery.** Consolidate capture controls in the launcher. Install
   microphone support with an explicit OS authorization and handle audio-service reload. A future
   direct Voile audio path could eliminate both virtual-microphone installation and system-audio
   capture permission; that would be a separate tested runtime change, not a packaging shortcut.
6. **Distribution provenance.** Ship notices and corresponding source for relevant GPL/MPL components.
   Download proprietary Steam components from their publisher rather than rehosting user-installed
   files. Review the exact Voile component licenses. Clarify permission for the locally rebuilt
   SimpleRustOSC snapshot (it has no separate license file), or replace the native codec with an
   explicitly licensed implementation before publishing that build. The local preview is not a
   representation that every bundled component has been cleared for public redistribution.
7. **Release pipeline.** Choose and enforce an actual minimum macOS version, rebuild all native
   artifacts for it, pin component versions/hashes, sign the app and nested components with a stable
   Developer ID, notarize/staple the release, and generate source/notices alongside binary assets.
   Apple signing/notarization needs the release owner's developer credentials. Avoid ad-hoc identity
   changes that repeatedly invalidate capture permissions.
8. **Clean-Mac verification.** Test first install, an interrupted download, unsupported Steam build,
   Steam update, permission denial, unplugged/unauthorized/multiple headsets, existing installations,
   reinstall/repair/uninstall and restoring defaults. Test Quest 2/3/Pro separately; face/eye tracking
   must gracefully remain unavailable on hardware that lacks it.

## First-release scope

Apple Silicon + a tested macOS baseline + VRChat + Quest WiFi streaming. Quest Pro adds face/eye
tracking when available. Keep arbitrary Windows VR games, automatic speculative Steam downgrades,
and unsupported runtime updates outside the initial release. USB-only can become a supported mode
after the complete media path is implemented and verified.

## Known pinned inputs

- Tested Steam stable: `1792618285`; the Linux client must match.
- Voile: `v26.3.0-24`, contract `1`.
- Official Voile archive: `https://github.com/Zagrios/Voile/releases/download/v26.3.0-24/voile-v26.3.0-24.tar.xz`
- Archive size: `439583920`; SHA-256: `6151007373b1b48dad8ef4908723d7e93ef3356ab2f903f864c053c6317f166c`.
- Source of pin: the upstream Voile release feed, read on 2026-11-05. A production updater must
  verify the upstream signed feed or use a release-owned signed manifest; downloading mutable
  JSON and trusting its hash alone is insufficient for updates.

## Sources

- [NotProton design and supported builds](https://github.com/NotProtonNot/NotProton)
- [Voile runtime layout and release mechanism](https://github.com/Zagrios/Voile)
- [Obelisk environment boot implementation](https://github.com/Zagrios/Obelisk/blob/main/crates/game-runtime/src/wine/linux_engine.rs)
- [Apple notarization documentation](https://developer.apple.com/documentation/security/notarizing_macos_software_before_distribution)
- [Current OSC native library source](https://github.com/benaclejames/SimpleRustOSC)

## Building the local preview

Run `bash scripts/build-launcher.sh` on the development Mac after building the verified audio/
tracking payload. The builder uses the existing local `config/build.env` for SDK paths; those paths
are build-time only. End users do not need these toolchains. Inspect without changing the setup:

```
dist/launcher/VRConMac.app/Contents/MacOS/VRConMac --doctor
```

Unit tests use isolated fixture folders and cover setup detection, incompatible Steam builds,
USB authorization/selection, payload integrity and path boundaries, literal command arguments
and command timeouts. Deep signature verification runs during packaging.
