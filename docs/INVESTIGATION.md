# Steam Linux-engine investigation — 2026-11-05

## Confirmed

- Custom tool must live under `compatibilitytools.d/notproton...` for this NotProton launch hook. Renaming to `notproton-vrconmac-lkd` eliminated Steam OS Error 260.
- Ordinary Steam launch then crashed in `steam.exe`: `steamclient_init_registry` access violation. `GetSteamEnvironmentForApp` and `GetSteamUILanguage` IPC returned failure 12. The 0xBEEF fault is Proton reporting the underlying native Steam-client fault. EAC and VRChat did not start.
- Obelisk launches VRChat and genuine Linux EAC, but passes `--no-vr` from Steam metadata even when `--vr` is appended. The experimental rebuilt helper did NOT remove it in the actual launch; the literal `-=--no-vr` also reached VRChat. Do not regard that workaround as working.
- Bridge `--test-pattern` only draws while no game connects; it does not prevent game frames. OXRSys manifest is `$HOME/Downloads/OXRSys-macOS/runtime/oxrsys-runtime.json`.

## Prepared next test

Voile v26.3.0-24 bundles Linux SDK `bins_sdk_ubuntu12.zip.481edab41be304d924937ae3ffd747a87d3687a0`. Obelisk pins its own Mac host build 1788652215. Installed ordinary Mac Steam and Valve stable Linux manifest both report build 1792618285.

Downloaded matching Linux SDK `bins_sdk_ubuntu12.zip.a90d50238a2b218b247ce1a8e9e314cb4eb652b1`. Its SHA-256 matches Valve manifest: `ef12d97073d33a7dcb4c67db7e17fbddd54bd22ac79b4e49d38758112308f938`. This mismatch is a plausible cause, not yet a confirmed fix.

`compat-tool/vrconmac-lkd/run` now binds its own `steam-client` folder to `/opt/steam`. `scripts/prepare-steam-client.py` downloads/verifies the matching SDK and retains Voile Windows client DLLs in this separate folder. The installer copies it with the launcher.

Installer tested in an isolated folder; all copied libraries match; dry run resolves the matched client and `start_protected_game.exe` without `--no-vr`. An actual helper probe in a cloned prefix could not run here: Metal setup and guest socket sendmsg were denied by the execution environment. Therefore a real Steam test remains necessary.

Installation into the live custom-tool folder was NOT permitted this turn. User must run:

```sh
bash /path/to/VRonMac/scripts/install-compat-tool.sh
```

Then select VRConMac Linux engine in Steam and use the VR launch entry. Keep diagnostic launch options `VRCM_WINEDEBUG=err+all,+process,+seh %command%`. Review Steam compatdata/438100/vrconmac-lkd.log after launch. No changes made to installed Steam binaries, Voile runtime or original game executables.

## End-to-end success and next issues

User confirmed visuals and movement through Steam after installing the matching Linux Steam client.

Crash at 01:01:52: NativeProcess.dll GetNetworkCostType (export RVA 0x27b0), return address 0x28b1, throws HRESULT 0x80004001 after IConnectionProfile::GetConnectionCost (vtable slot 9) returns E_NOTIMPL. Wine log explicitly says connection_profile_GetConnectionCost stub immediately before exception. Stack: kernelbase -> ucrtbase -> nativeprocess -> gameassembly -> unityplayer.

Prepared standalone Wine DLL replacement in compatibility/wine-connectivity using original Voile Wine commit and a proper IConnectionCost implementation. It maps NLM cost flags, using NetworkCostType_Unknown if unsupported. It is NOT installed yet. Test DLL and probe.exe compile successfully. User should run scripts/test-connectivity.sh in Terminal; it runs only the probe in a disposable prefix copy. Source license and build inputs retained. Runtime testing here is blocked by socket/Metal restrictions.

Poor stream: despite 100 Mbps configured, OXRSys logs during VR run showed 1 Mbps, then 10 Mbps on the next run, abr=bitrate/recovery. Next controlled test: abr_mode=off, bitrate=60 Mbps, encoder=balanced, foveated_encoding=off, keep resolution_scale=.90. Prepared outputs/oxrsys-fixed-bitrate.toml. Permission to change active OXRSys config was not granted, so it is NOT applied. User must restart bridge after config changes. Disable VRCM_WINEDEBUG process/seh tracing for performance test. Reprojection approx 19-21 frames per 1s interval, encoder total approx12.6ms and decode approx12ms at72Hz in quality mode. Do not infer all periodic stutter is network or ABR until retest.

## Network-cost probe passed

User test `work/connectivity-test.gRM5EV/game/probe-result.txt` confirms replacement identity version 1, explicit native load, and all network-cost getters succeed (type 1 from NLM). Prior tests inadvertently loaded original builtin; test script now forces native loading and verifies custom export identity.

Prepared scripts/install-network-cost-fix.py. It requires an exact hash match against a passing probe, backs up user.reg and the original system32 DLL, atomically installs the replacement, and adds `Software\Wine\AppDefaults\VRChat.exe\DllOverrides` override `windows.networking.connectivity=native,builtin`. Tested installer twice on a fixture copied from live prefix; verified backup preservation and single scoped registry override. Live installation is still pending; user must close VRChat and run the installer, then test Steam launch.

## Head movement judder investigation

User reports regular ~0.5s game/particle hitch on Mac and headset, plus much worse head-motion judder only in headset. Reducing bridge scale .7 and OXRSys encoded scale .75/speed cut encoding from14-15ms to7-8ms but did not resolve head judder. Quest reports20-35 reused frames/s. Decoder metric includes acquisition wait, not pure hardware decode duration.

Found OXRSys1.2 source ValidateProjectionLayer ignores view poses and StreamingServer::SendFrame samples current predicted head pose when labeling video, including when bridge resubmits old pixels. Built separate local runtime preserving submitted head pose derived from stereo projection views in fixed reference spaces; no protocol/client changes. New manifest: runtime-host/oxrsys-submitted-pose/oxrsys-runtime.json. Original installed runtime untouched. Source in archived modified tree, 3 non-GPU CTest groups pass. Full Metal API tests fail because test environment yields no Metal command queue. Actual headset comparison pending.

## Pose fix confirmed; EAC and pacing follow-up

User confirmed head-turn smoothness completely fixed with pose-only OXRSys runtime. Occasional shared desktop/headset particle hitch remains. EAC latest game log at01:30:35 reports Type15 Forbidden system configuration (Debugger detected.). Prior7runs show no such detection. Read-only user `ps` command found no winedbg/windbg/lldb/gdb process. Older launch logs show automatic winedbg launches during the pre-fix NativeProcess crashes, but that does not establish a current debugger. Do not claim that EAC cause is known. No anti-debugging checks, EAC settings or flags were modified. Clean restart recommended for next normal test.

Clock measurements from xrEndFrame/frameId records show actual host clock62.75-63.16Hz while nominal72Hz. OXRSys1.2 WaitFrame relative sleep anchors each deadline to the late wake-up, accumulating macOS timer oversleep. Added separate `runtime-host/oxrsys-paced-pose` build combining known-working pose change with an absolute-deadline scheduler (skipmissed slots, no additional wait for late apps, reset on refresh/session changes). Pure timer benchmark reproduced62.11Hz original versus71.94Hz new at72target. All3non-GPU CTest groups pass; Metal API tests unavailable in agent environment. Pose-only runtime retained. User VR validation of pacing build pending.
