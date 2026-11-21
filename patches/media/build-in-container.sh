#!/bin/bash
set -euo pipefail
options=(
    --enable-win64 --enable-archs=x86_64 --disable-tests --without-x --without-fontconfig
    --with-gstreamer --without-pulse --without-alsa --without-cups --without-sdl --without-vulkan
    --without-opengl --without-dbus --without-udev --without-usb --without-v4l2 --without-gphoto
    --without-sane --without-pcap --without-krb5 --without-oss --without-capi --without-netapi
    --without-unwind --without-wayland --without-inotify
)
if [ ! -f Makefile ]; then
    /source/configure "${options[@]}" --prefix=/opt/wine LDFLAGS=-Wl,-z,max-page-size=16384
fi
make -j4 dlls/winegstreamer/winegstreamer.so dlls/winegstreamer/x86_64-windows/winegstreamer.dll
mkdir -p /out/bin /out/lib /out/plugins /out/wine
cp dlls/winegstreamer/winegstreamer.so dlls/winegstreamer/x86_64-windows/winegstreamer.dll /out/wine/
lib=/usr/lib/x86_64-linux-gnu
scanner="$lib/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner"
cp "$scanner" /out/bin/
plugins=()
for name in coreelements deinterlace videofilter isomp4 videoparsersbad libav playback typefindfunctions audioconvert audioresample videoconvertscale; do
    plugin="$lib/gstreamer-1.0/libgst$name.so"
    test -f "$plugin"
    plugins+=("$plugin")
done
cp "${plugins[@]}" /out/plugins/
LD_LIBRARY_PATH=/build/dlls/ntdll ldd "${plugins[@]}" "$scanner" /out/wine/winegstreamer.so > /out/dependencies.txt
if grep -q 'not found' /out/dependencies.txt; then
    cat /out/dependencies.txt >&2
    exit 1
fi
awk '$2 == "=>" && $3 ~ /^\// { print $3 }' /out/dependencies.txt | sort -u |
while read -r dependency; do
    case "${dependency##*/}" in
        ntdll.so|libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|ld-linux-x86-64.so.2) continue ;;
    esac
    cp -L "$dependency" /out/lib/
done
readelf -lW /out/wine/winegstreamer.so | awk '/ LOAD / { if ($NF != "0x4000") exit 1; count++ } END { if (!count) exit 1 }'

make -j4 dlls/urlmon/x86_64-windows/urlmon.dll
cp dlls/urlmon/x86_64-windows/urlmon.dll /out/wine/urlmon.dll

make -j4 dlls/mf/x86_64-windows/mf.dll
cp dlls/mf/x86_64-windows/mf.dll /out/wine/mf-range.dll

make -j4 dlls/mfmediaengine/x86_64-windows/mfmediaengine.dll
cp dlls/mfmediaengine/x86_64-windows/mfmediaengine.dll /out/wine/mfmediaengine-test.dll
