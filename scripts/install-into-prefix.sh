#!/usr/bin/env bash
# Installs the VRConMac Wine-side pieces into a Wine prefix:
#   C:\vrconmac\vrconmac_xr.dll + .json + vrconmac.ini      (our OpenXR runtime)
#   C:\vrconmac\opencomposite\bin\vrclient_x64.dll          (OpenComposite, "system-wide" mode)
#   %LOCALAPPDATA%\openvr\openvrpaths.vrpath                 (points OpenVR games at OpenComposite)
#   HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime             (points OpenComposite at our runtime)
#
# System-wide mode means the game's own openvr_api.dll is left untouched: Valve's openvr_api.dll
# reads openvrpaths.vrpath and loads OpenComposite instead of SteamVR. No game files are modified.
#
# Usage:
#   scripts/install-into-prefix.sh --prefix /path/to/wineprefix --opencomposite /path/to/openvr_api.dll \
#       [--runtime-dir runtime/out] [--wine "wine"] [--no-registry] [--direct-registry]
#
# --direct-registry edits the prefix's system.reg/user.reg directly instead of running wine: the OpenXR
# runtime key, plus a vrclient_x64=native DLL override (needed on Proton-derived Wines such as
# Obelisk's voile-wine). Only do this while nothing is running in the prefix.
#
# Get OpenComposite (OpenXR branch, x64) from:
#   https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr   (saves as openvr_api.dll)
#
# --wine is the command used to run `reg import` inside the prefix. For voile-lkd / Obelisk this is
# the full fexdarwin-run ... /opt/wine/bin/wine invocation; if you can't run it from a script, use
# --no-registry and import the generated vrconmac.reg with whatever launches regedit in that prefix.
set -euo pipefail

PREFIX="" OC="" RUNTIME_DIR="" WINE_CMD="${WINE:-wine}" DO_REG=1 DIRECT_REG=0
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME_DIR="$HERE/runtime/out"

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --opencomposite) OC="$2"; shift 2 ;;
    --runtime-dir) RUNTIME_DIR="$2"; shift 2 ;;
    --wine) WINE_CMD="$2"; shift 2 ;;
    --no-registry) DO_REG=0; shift ;;
    --direct-registry) DIRECT_REG=1; shift ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac
done

[ -n "$PREFIX" ] && [ -d "$PREFIX/drive_c" ] || { echo "--prefix must point at a Wine prefix (has drive_c/)" >&2; exit 1; }
[ -f "$RUNTIME_DIR/vrconmac_xr.dll" ] || { echo "no $RUNTIME_DIR/vrconmac_xr.dll - run runtime/build-mingw.sh first" >&2; exit 1; }

DEST="$PREFIX/drive_c/vrconmac"
mkdir -p "$DEST/opencomposite/bin" "$DEST/opencomposite/config" "$DEST/opencomposite/logs"
cp "$RUNTIME_DIR/vrconmac_xr.dll" "$DEST/"
cp "$HERE/runtime/vrconmac_xr.json" "$DEST/"
[ -f "$DEST/vrconmac.ini" ] || cp "$HERE/runtime/vrconmac.ini.example" "$DEST/vrconmac.ini"
echo "runtime  -> C:\\vrconmac\\vrconmac_xr.dll"

if [ -n "$OC" ]; then
  [ -f "$OC" ] || { echo "OpenComposite dll '$OC' not found" >&2; exit 1; }
  cp "$OC" "$DEST/opencomposite/bin/vrclient_x64.dll"
  echo "OpenComposite -> C:\\vrconmac\\opencomposite\\bin\\vrclient_x64.dll"
else
  echo "(no --opencomposite given: skipping OpenComposite; put vrclient_x64.dll in C:\\vrconmac\\opencomposite\\bin yourself)"
fi

# openvrpaths.vrpath for every user profile in the prefix
VRPATH_JSON='{
	"config" : [ "C:\\vrconmac\\opencomposite\\config" ],
	"external_drivers" : null,
	"jsonid" : "vrpathreg",
	"log" : [ "C:\\vrconmac\\opencomposite\\logs" ],
	"runtime" : [ "C:\\vrconmac\\opencomposite" ],
	"version" : 1
}'
found=0
for u in "$PREFIX"/drive_c/users/*/; do
  name="$(basename "$u")"
  [ "$name" = "Public" ] && continue
  d="$u/AppData/Local/openvr"
  mkdir -p "$d"
  if [ -f "$d/openvrpaths.vrpath" ] && ! grep -q vrconmac "$d/openvrpaths.vrpath"; then
    cp "$d/openvrpaths.vrpath" "$d/openvrpaths.vrpath.before-vrconmac"
    echo "backed up existing $d/openvrpaths.vrpath"
  fi
  printf '%s\n' "$VRPATH_JSON" > "$d/openvrpaths.vrpath"
  echo "vrpath   -> users/$name/AppData/Local/openvr/openvrpaths.vrpath"
  found=1
done
[ $found = 1 ] || echo "WARNING: no user profiles under drive_c/users - run the prefix once first, then re-run this" >&2

REG="$DEST/vrconmac.reg"
printf 'Windows Registry Editor Version 5.00\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1]\r\n"ActiveRuntime"="C:\\\\vrconmac\\\\vrconmac_xr.json"\r\n\r\n[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\r\n"vrclient_x64"="native"\r\n"vrclient"="native"\r\n' > "$REG"
if [ $DIRECT_REG = 1 ]; then
  SYSREG="$PREFIX/system.reg" USERREG="$PREFIX/user.reg"
  [ -f "$SYSREG" ] && [ -f "$USERREG" ] || { echo "no system.reg/user.reg in $PREFIX" >&2; exit 1; }
  if pgrep -x wineserver >/dev/null 2>&1; then
    echo "WARNING: a wineserver is running; it may overwrite the registry on exit. Quit games/wine and re-run." >&2
  fi
  [ -f "$SYSREG.before-vrconmac" ] || cp "$SYSREG" "$SYSREG.before-vrconmac"
  [ -f "$USERREG.before-vrconmac" ] || cp "$USERREG" "$USERREG.before-vrconmac"
  python3 "$HERE/scripts/regedit.py" "$SYSREG" 'Software\Khronos\OpenXR\1' ActiveRuntime 'C:\vrconmac\vrconmac_xr.json'
  # Proton-derived Wines (voile-wine/Obelisk, Proton) have a *builtin* vrclient_x64 that bridges to a
  # Linux SteamVR and shadows OpenComposite's file. Force the native DLL.
  python3 "$HERE/scripts/regedit.py" "$USERREG" 'Software\Wine\DllOverrides' vrclient_x64 native
  python3 "$HERE/scripts/regedit.py" "$USERREG" 'Software\Wine\DllOverrides' vrclient native
  echo "registry -> ActiveRuntime in system.reg, vrclient(_x64)=native in user.reg (backups: *.before-vrconmac)"
elif [ $DO_REG = 1 ]; then
  if WINEPREFIX="$PREFIX" $WINE_CMD reg import 'C:\vrconmac\vrconmac.reg' >/dev/null 2>&1; then
    echo "registry -> HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime = C:\\vrconmac\\vrconmac_xr.json"
  else
    echo "WARNING: couldn't run '$WINE_CMD reg import'. Import C:\\vrconmac\\vrconmac.reg in the prefix manually." >&2
  fi
else
  echo "registry -> skipped; import C:\\vrconmac\\vrconmac.reg yourself"
fi
echo "done. Start vrconmac-bridge on the Mac, then launch the game in VR mode."
