# Third-party source snapshots and licenses

Local snapshots were downloaded on 2026-11-05. The archived download hashes below identify the inputs; local patches are retained as complete source under `vendor` and `tracking`.

| Component | Source | License |
| --- | --- |
| OXRSys | https://github.com/demonixis/oxrsys (upstream snapshot 1.2.0, commit d6705f1a3609442049d87cd771ea5cd2ff560083) | MPL-2.0; see vendor source license |
| VRCFaceTracking | https://github.com/benaclejames/VRCFaceTracking (development master snapshot, v6/Avalonia) | Apache-2.0 |
| BlackHole | https://github.com/ExistentialAudio/BlackHole (master snapshot) | GPL-3.0; complete source retained |
| FB face mapping | https://github.com/korejan/VRCFT-ALXR-Modules (master snapshot, ModuleUtils.cs) | MIT; tracking/quest-pro/ALXR-LICENSE |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers (v1.4.304) | Apache-2.0 / MIT as marked upstream |

CMake fetches the pinned upstream OpenXR SDK, GLM, spdlog and Catch2 versions declared in OXRSys. The .NET publish output contains its runtime and NuGet dependency notices. These components retain their own licenses. The BlackHole-derived driver is a separate binary; it is not linked into the bridge or helper.

## Input archive SHA-256

- `oxrsys-1.2.0.tar.gz`: `7da386c41143576672e2e96b4ff7d9f66ff3feaf8f40cd5914e41a5a3a002783`
- `vrcft-src.tar.gz`: `6e8c770ea62c1b34c8617e88650bd7858148bb3aa1a58a1df507c791326d5eae`
- `blackhole-src.tar.gz`: `eb27880464fe2aaf581f62959f386e7f6d30a0b690af93851ae8422bb62c0114`
- `alxr-modules.tar.gz`: `76a0ccd9abd5df354e2d43e2f1f90f7b950a5c849d034b0f08188052b26c4a01`
- `vulkan-headers.tar.gz`: `5e1a06b3f27e7581b55d1dea176fd97ee0a2f299406db2f437c1d2f297ceba5b`

The original upstream ARM64 OSC dylib was rejected by macOS 27 (mis-aligned LINKEDIT string pool). It was rebuilt locally from https://github.com/benaclejames/SimpleRustOSC (master snapshot, source retained locally; excluded from the public repository). That upstream snapshot contains no separate license file.

## First source publication

The native microphone ring and audio integration files in launcher/ carry MPL-2.0 headers. The full license is retained in vendor/OXRSys/LICENSE.

The Lighthouse sources under tracking/lighthouse include the modified MIT-licensed libsurvive solver, its redist dependency notices, an Eigen snapshot with its upstream COPYING files, and Motoc-derived GPL-3.0-only calibration code. The tracker service is GPL-3.0-only. Archive identities and source commits are recorded in tracking/lighthouse/fullbody/source-versions.json.

The public repository omits generated VRCFaceTracking native libraries and SimpleRustOSC. Existing app payloads still include the locally built OSC codec. See release-status.md for the pending binary distribution inventory.

The Neptune large-upload patch targets Zagrios/neptune commit 1b949fd6f6466f966fc37b05ec9b300c9d78d280, matching the tested Voile release. The media patches record modifications used with Voile v26.3.0-24. These patches do not replace the complete upstream source or dependency notices.
