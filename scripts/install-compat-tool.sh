#!/usr/bin/env bash
# Installs the vrconmac-lkd Steam compatibility tool into Steam's compatibilitytools.d (macOS), next to
# NotProton's runners. Restart Steam afterwards, then pick "VRConMac Linux engine (voile-lkd)" in a game's
# Properties -> Compatibility.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="${STEAM_TOOLS_DIR:-$HOME/Library/Application Support/Steam/compatibilitytools.d}"
[ -d "$TOOLS" ] || { echo "no $TOOLS (is NotProton installed?)" >&2; exit 1; }

# Use the same target OS NotProton's tools declare, so Steam offers ours the same way.
to_os=""
for vdf in "$TOOLS"/notproton*/compatibilitytool.vdf; do
  [ -f "$vdf" ] || continue
  to_os=$(sed -n 's/.*"to_oslist"[[:space:]]*"\([^"]*\)".*/\1/p' "$vdf" | head -1)
  [ -n "$to_os" ] && break
done
to_os=${to_os:-macos}

# NotProton's shell-launch hook recognizes paths beginning with
# /compatibilitytools.d/notproton. Keep our separate tool in that namespace.
TOOL_ID=notproton-vrconmac-lkd
[ -f "$HERE/compat-tool/vrconmac-lkd/steam-client/linux64/steamclient.so" ] || {
  echo "Prepare the matched client first: scripts/prepare-steam-client.py" >&2
  exit 1
}

dest="$TOOLS/$TOOL_ID"
mkdir -p "$dest"
rm -f "$dest/vrconmac-lkd"
cp "$HERE/compat-tool/vrconmac-lkd/"{run,launch.sh,toolmanifest.vdf,LICENSE-launch.sh.GPL-3.0} "$dest/"
sed -e "s/TO_OSLIST/$to_os/" -e "s/\"vrconmac-lkd\"/\"$TOOL_ID\"/" "$HERE/compat-tool/vrconmac-lkd/compatibilitytool.vdf" > "$dest/compatibilitytool.vdf"
cp -R "$HERE/compat-tool/vrconmac-lkd/steam-client" "$dest/"
chmod +x "$dest/run" "$dest/launch.sh"
xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true
echo "installed $dest (to_oslist=$to_os). Restart Steam, then pick it in VRChat -> Properties -> Compatibility."
