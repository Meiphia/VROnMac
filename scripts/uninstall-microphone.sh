#!/bin/bash
set -euo pipefail
TARGET="/Library/Audio/Plug-Ins/HAL/VRConMac Microphone.driver"
if [[ "${1:-}" != --remove ]]; then
 echo "Select another Mac input, quit VRConMac Audio, then run: sudo bash '$0' --remove"
 exit 0
fi
[[ $EUID -eq 0 ]] || { echo "Administrator access is required."; exit 1; }
[[ ! -e "$TARGET" ]] || mv "$TARGET" "/Library/Audio/Plug-Ins/HAL/VRConMac Microphone.removed-$(date +%Y%m%d-%H%M%S)"
echo "Driver disabled and preserved. Restart the Mac to unload it."
