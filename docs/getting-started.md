# Getting started

The first app release is a draft. These instructions describe the current developer preview.

## Before launching

Use an Apple Silicon Mac with the current bundle's macOS 27 minimum, Steam, a Quest headset, and a compatible NotProton setup. Install Windows VRChat in Steam. The pinned compatibility engine is Voile v26.3.0-24. Steam updates can affect compatibility.

Keep your headset and Mac on the same local network. Enable developer mode and USB debugging on the headset when installing its client. Accept the headset's USB authorization prompt.

## Prepare the game

1. Open VRonMac and use **Setup** to check Steam and compatibility components.
2. Install missing Windows support and configure Steam compatibility when prompted. Restart Steam after changing its compatibility integration.
3. Use **Install Quest Client** with the headset attached over USB.
4. In **Session**, select VRChat and prepare the game.
5. In Steam's VRChat properties, select **VRConMac Linux engine** under Compatibility.

The launcher can adopt an existing setup. End-to-end setup on a clean Mac is pending verification.

## Start a session

1. Open the VRonMac Quest client and start the headset connection in the Mac launcher.
2. Allow audio capture and microphone permissions when prompted. Install the bundled microphone driver through the macOS installer if needed.
3. For full-body tracking, start the Lighthouse service, select powered-on trackers, choose your reference, and calibrate.
4. Start face tracking on Quest Pro when desired. Enable OSC in VRChat and use an avatar with compatible face parameters.
5. Launch VRChat through Steam.

For manual tracker alignment, hold the tracker rigidly against the selected controller or headset and move them together through slow rotations and figure eights. A head-mounted reference must remain rigidly attached during continuous alignment. Recalibrate after a tracking-space reset.

Exit VRChat before stopping the headset connection. Setup includes runtime restoration controls for recovery after an interrupted session.

## Diagnostics

Use **Session → Export Launch Diagnostics** before relaunching after a failure. Include the approximate local time and describe whether the game froze, exited, or only lost headset video. Review reports for account names, device identifiers, and other personal information before uploading them.

The launcher also exposes read-only checks.

```sh
"VRonMac.app/Contents/MacOS/VRonMac" --doctor
"VRonMac.app/Contents/MacOS/VRonMac" --verify-payload
```
