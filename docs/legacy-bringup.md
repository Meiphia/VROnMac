# VRConMac (via NotProton)

VRChat in VR on Apple Silicon **without a VM**: the game runs in a Windows compatibility layer
(voile-lkd / Obelisk, NotProton, CrossOver...), its OpenVR calls go through OpenComposite into a small
OpenXR runtime that lives *inside Wine*, and that runtime streams frames and poses to a native Mac
"bridge" that talks to a real macOS OpenXR runtime (OXRSys → Quest Pro).

```
 Wine prefix (Windows side)                                   │  macOS (native)
                                                              │
 VRChat ── openvr_api.dll (Valve, untouched)                  │
             │ reads openvrpaths.vrpath                       │
             ▼                                                │
           OpenComposite (vrclient_x64.dll)  OpenVR → OpenXR  │
             │ OpenXR loader → HKLM\…\Khronos\OpenXR\1        │
             ▼                                                │
           vrconmac_xr.dll  ── TCP 127.0.0.1:47811 ──────────────▶ vrconmac-bridge ── OXRSys ── Quest Pro
             • D3D11 swapchains, GPU→CPU readback      TICKs ◀──    • OpenXR app (XR_KHR_metal_enable)
             • OpenXR action system (Touch layout)     FRAMEs ──▶   • uploads frames into Metal swapchains
             • paces xrWaitFrame off the bridge                     • sends head/eye/controller poses
```

Nothing in the game folder is modified: OpenComposite is loaded through `openvrpaths.vrpath`, the
same way SteamVR itself is found. EAC is not touched by any of this. (Server-side ban risk is still
unknown: nobody outside EAC knows what it checks.)

## Status

| Piece | State |
| --- | --- |
| `vrconmac_xr.dll` (Wine-side runtime) | **Tested** under Wine 9 (Linux): passes `tests/harness.exe`, and runs real **OpenComposite** (openxr branch) from an OpenVR D3D11 test app, both as a replaced `openvr_api.dll` and system-wide via Valve's own `openvr_api.dll` + vrpath. 72 fps at 900×960/eye into the mock bridge, colours verified (sRGB + RGBA16F paths). |
| `vrconmac-bridge --backend mock` | **Tested** (Linux build). Fake headset, dumps received eyes to PPM. |
| `vrconmac-bridge --backend openxr` (Metal) | **Works** on M4 Pro + OXRSys → Quest Pro (test pattern, 2026-11-04). |
| Full chain on the Mac | **Works** (2026-11-04): Valve `openvr_api.dll` → vrpath → OpenComposite → `vrconmac_xr.dll` in NotProton's CrossOver-FEX prefix (wined3d/MoltenVK) → bridge → OXRSys → Quest Pro, both eyes correct. |
| Beat Saber (native OpenXR, Unity single-pass instanced) | **Works** in NotProton with the **DXMT** backend (2026-11-04). Black frames on the default backend: it can't do Unity's single-pass instanced rendering. Check a backend with `prebuilt/vrtest/spi_probe.exe`. |
| VRChat | Blocked under CrossOver by design (EAC finds no macOS module); needs voile-lkd. |
| Under voile-lkd / Neptune | **Untested.** The big unknown is whether Neptune's D3D11 supports `CopySubresourceRegion` into a STAGING texture + `Map` (readback). |

## Layout

```
common/vrcm_protocol.h      wire protocol shared by both sides
runtime/                    Wine-side OpenXR runtime DLL (MinGW cross-compile)
  build-mingw.sh            → runtime/out/vrconmac_xr.dll (+ .json, vrconmac.ini)
prebuilt/                   vrconmac_xr.dll built from this tree, so you don't need mingw
bridge/                     native macOS bridge (CMake)
  build.sh                  → bridge/build/vrconmac-bridge
scripts/install-into-prefix.sh   puts runtime + OpenComposite + vrpath + registry into a prefix
scripts/uninstall-from-prefix.sh
tests/harness.cpp           drives the runtime like OpenComposite does
tests/vrapp.cpp             minimal OpenVR D3D11 app (for the full OpenComposite chain)
tests/run-wine-tests.sh     the whole test, reproducible with plain Wine
```

## Build

```sh
# bridge (on the Mac)
brew install cmake
bridge/build.sh

# runtime (optional – prebuilt/vrconmac_xr.dll is the same thing)
brew install mingw-w64
runtime/build-mingw.sh
```

You also need **OpenComposite, openxr branch, x64**:
`https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr` (downloads as `openvr_api.dll`).

## Bring-up, one layer at a time

Each step takes away one unknown.

### 1. Mac only: bridge → OXRSys → Quest Pro

Build and register OXRSys (its QuickStart), start the OXRSys client on the Quest, then:

```sh
bridge/build/vrconmac-bridge --backend openxr --test-pattern
```

You should see a red-tinted checkerboard in the left eye and a blue one in the right, stuck to your
face. That checks the Metal upload, swapchains and session handling with no Wine involved. Use
`--runtime-json /path/to/oxrsys.json` if it isn't registered as the active runtime. The Meta XR
Simulator works too if its macOS runtime supports `XR_KHR_metal_enable`.

### 2. Windows side only: game → runtime → mock bridge

```sh
bridge/build/vrconmac-bridge --backend mock --render-scale 0.5 --dump-dir /tmp/vrcm-dump
```

Install into the prefix the game runs in (see below), start VRChat in VR mode, and watch the bridge
output (`frames/s`, `MB/s`). It writes PPM snapshots of what the game rendered to `/tmp/vrcm-dump`.

Good **first** guinea pig: a non-EAC OpenVR game in a CrossOver/NotProton bottle. That checks
OpenComposite + runtime + D3DMetal readback before adding voile-lkd to the mix.

### 3. Everything

```sh
bridge/build/vrconmac-bridge --backend openxr --render-scale 0.6
```

then launch the game in VR mode.

## Installing into a prefix

```sh
scripts/install-into-prefix.sh --prefix /path/to/prefix --opencomposite ~/Downloads/openvr_api.dll \
    --runtime-dir prebuilt
```

This creates `C:\vrconmac\` (runtime, `vrconmac.ini`, OpenComposite as
`opencomposite\bin\vrclient_x64.dll`), writes `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` for each
user in the prefix (and backs up any existing one), and registers
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime = C:\vrconmac\vrconmac_xr.json`.

The registry key is required: Wine runs programs elevated, and the OpenXR loader ignores
`XR_RUNTIME_JSON` for elevated processes.

**voile-lkd / Obelisk:** Wine runs inside the guest, so a plain `wine reg import` on the host won't
reach the prefix. Use `--no-registry`, then import `C:\vrconmac\vrconmac.reg` with voile-lkd's
runner. For example, adapted from the voile-lkd README:

```sh
FEXDARWIN_LKD=1 FEXDARWIN_ROOT=<x86-64 debian root> FEXDARWIN_ROOT_RO=1 \
FEXDARWIN_BIND="/opt/wine=<voile-wine install>:ro;/home/user=<home holding the prefix>" \
fexdarwin/scripts/run-with-timeout.sh 60 fexdarwin/build/host/fexdarwin-run \
  --env HOME=/home/user --env WINEPREFIX=/home/user/<prefix> \
  /opt/wine/bin/wine reg import 'C:\vrconmac\vrconmac.reg'
```

Guest sockets are real macOS sockets under voile-lkd, so `127.0.0.1` in the guest reaches the
bridge. If it doesn't in your setup, start the bridge with `VRCONMAC_LISTEN_ANY=1` and set
`host = <Mac's IP>` in `C:\vrconmac\vrconmac.ini`.

## Knobs

| Where | Setting | What it does |
| --- | --- | --- |
| bridge | `--render-scale F` | Per-eye resolution the game is asked to render. Every frame crosses the CPU twice, so start at 0.5–0.7. |
| `vrconmac.ini` | `frame_latency = 1` | Read back the previous frame's GPU copy (no stall). `0` = read immediately (stalls; lower latency). |
| `vrconmac.ini` | `verbose = 1` | Logs every unsupported call and every suggested binding. |
| env | `VRCONMAC_HOST/PORT/FRAME_LATENCY/VERBOSE` | Override the ini. |

## Logs

- `C:\vrconmac\vrconmac_xr.log`: the runtime (session states, swapchains, connection, frame stats).
- `%LOCALAPPDATA%\OpenComposite\logs\opencomposite.log`: OpenComposite.
- Bridge stdout: host Hz, guest fps received/uploaded, MB/s.

## Graphics backend

Unity VR games render both eyes in one pass via `SV_RenderTargetArrayIndex` from the vertex shader
(D3D11.3). If a game shows pure black in the headset *and* in its desktop window while the runtime
log's `mean RGB` lines read 0, switch the bottle to **DXMT**. `prebuilt/vrtest/spi_probe.exe` tests a
backend directly: step [2] must come out red/green.

## Known limits / next steps

- **Frames cross the CPU twice** (GPU → staging → TCP → MTLBuffer → texture). Fine for a proof of
  concept. The real fix is zero-copy: Neptune already turns D3D11 textures into MTLTextures in a Mac
  process, so swapchain images could be IOSurface-backed and only their IDs would need to cross
  over. That means patching Neptune.
- TCP over loopback carries about 500 MB/s here. Shared memory (a file mapped on both sides) is the
  cheaper next step before going zero-copy.
- Single projection layer only. Quad/cylinder layers, the visibility mask, hand tracking and Quest
  Pro face/eye tracking aren't forwarded (VRChat face tracking goes over OSC anyway).
- Input is presented as `oculus/touch_controller`, which is what OpenComposite and VRChat expect
  from a Quest.
- Head/controller poses come from the latest bridge tick nearest the requested time. There's no
  extrapolation; OXRSys reprojects using the pose each frame was rendered with.

Protocol details are in `common/vrcm_protocol.h`.

## Testing without a Mac

```sh
OPENVR_HEADERS=/path/to/OpenComposite/OpenVRHeaders \
tests/run-wine-tests.sh ~/Downloads/openvr_api.dll /path/to/valve/openvr_api.dll
```
