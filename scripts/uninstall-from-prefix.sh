#!/usr/bin/env bash
# Reverts install-into-prefix.sh: restores backed-up openvrpaths.vrpath files and removes the
# OpenXR runtime registration. Leaves C:\vrconmac in place (delete it yourself if you want).
#   scripts/uninstall-from-prefix.sh --prefix /path/to/wineprefix [--wine "wine"]
set -euo pipefail
PREFIX="" WINE_CMD="${WINE:-wine}"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --wine) WINE_CMD="$2"; shift 2 ;;
    *) echo "unknown arg $1" >&2; exit 2 ;;
  esac
done
[ -d "$PREFIX/drive_c" ] || { echo "--prefix must point at a Wine prefix" >&2; exit 1; }
for d in "$PREFIX"/drive_c/users/*/AppData/Local/openvr; do
  [ -d "$d" ] || continue
  if [ -f "$d/openvrpaths.vrpath.before-vrconmac" ]; then
    mv -f "$d/openvrpaths.vrpath.before-vrconmac" "$d/openvrpaths.vrpath"
    echo "restored $d/openvrpaths.vrpath"
  elif grep -q vrconmac "$d/openvrpaths.vrpath" 2>/dev/null; then
    mv -f "$d/openvrpaths.vrpath" "$d/openvrpaths.vrpath.vrconmac-disabled"
    echo "disabled $d/openvrpaths.vrpath"
  fi
done
WINEPREFIX="$PREFIX" $WINE_CMD reg delete 'HKLM\SOFTWARE\Khronos\OpenXR\1' /v ActiveRuntime /f >/dev/null 2>&1 \
  && echo "removed OpenXR ActiveRuntime" || echo "couldn't remove registry value (do it manually)"
