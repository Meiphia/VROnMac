#!/usr/bin/env bash
# Read-only: collects what VRConMac needs to know about the VRChat install + NotProton prefix.
# Writes vrchat-report.txt next to this repo. Changes nothing.
set -u
STEAM="${STEAM:-$HOME/Library/Application Support/Steam/steamapps}"
VRC="$STEAM/common/VRChat"
PFX_ROOT="$STEAM/compatdata/438100"
OUT="$(cd "$(dirname "$0")/.." && pwd)/vrchat-report.txt"
exec > "$OUT" 2>&1

echo "== VRChat folder =="; ls -la "$VRC"
echo; echo "== Plugins/x86_64 =="; ls -la "$VRC/VRChat_Data/Plugins/x86_64" 2>/dev/null | grep -iE "openvr|openxr|ovr|xr|steam" 
echo; echo "== UnitySubsystems / XR loaders =="; find "$VRC/VRChat_Data" -maxdepth 3 \( -iname "*UnitySubsystemsManifest*" -o -iname "*openvr*" -o -iname "*openxr*" \) 2>/dev/null
for f in $(find "$VRC/VRChat_Data/UnitySubsystems" -name "*.json" 2>/dev/null); do echo "-- $f"; cat "$f"; echo; done
echo; echo "== Unity version =="; strings "$VRC/UnityPlayer.dll" 2>/dev/null | grep -m3 -E "^20[0-9]{2}\.[0-9]+\.[0-9]+f[0-9]"
echo; echo "== EAC files =="; ls "$VRC" | grep -iE "eac|easyanti|start_protected"; ls "$VRC/EasyAntiCheat" 2>/dev/null | head
echo; echo "== compatdata =="; ls -la "$STEAM/compatdata" 2>/dev/null | head -30
echo; echo "== VRChat prefix ($PFX_ROOT) =="; ls -la "$PFX_ROOT" "$PFX_ROOT/pfx" 2>/dev/null
ls "$PFX_ROOT/pfx/drive_c/users" 2>/dev/null
for d in "$PFX_ROOT"/pfx/drive_c/users/*/AppData/Local/openvr; do echo "-- $d"; ls -la "$d"; cat "$d/openvrpaths.vrpath" 2>/dev/null; done
grep -A2 -i 'Khronos\\\\OpenXR' "$PFX_ROOT/pfx/system.reg" 2>/dev/null
ls "$PFX_ROOT/pfx/drive_c" 2>/dev/null
echo; echo "== NotProton =="; ls -la "$HOME/Library/Application Support/notproton" "$HOME/Library/Application Support/notproton/runners" 2>/dev/null
find "$HOME/Library/Application Support/notproton/runners" -maxdepth 6 -path "*bin/wine*" 2>/dev/null | head
echo; echo "== Steam compat tools / launch config =="; ls "$HOME/Library/Application Support/Steam/compatibilitytools.d" 2>/dev/null
grep -A12 '"438100"' "$HOME/Library/Application Support/Steam/config/config.vdf" 2>/dev/null | head -20
echo; echo "done: $(date)"
