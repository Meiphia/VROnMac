#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-x86_64-w64-mingw32-gcc}"
"$CC" -shared -O2 -Wno-discarded-qualifiers -I"$HERE" -include stdlib.h -include wchar.h \
    "$HERE/main.c" "$HERE/network_information.c" "$HERE/exports.def" \
    -o "$HERE/windows.networking.connectivity.dll" -lole32 -lruntimeobject -luuid
"$CC" -O2 -I"$HERE" -include stdlib.h -include wchar.h "$HERE/probe.c" \
    -o "$HERE/probe.exe" -lole32 -lruntimeobject -luuid
