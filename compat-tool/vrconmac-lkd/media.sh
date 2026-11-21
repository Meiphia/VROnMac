#!/bin/bash
# Sourced by run for the tested VRChat media profile.
prepare_media_profile() {
    local state="$HOME/Library/Application Support/VRonMac/Media/v1"
    local stock="$OBELISK/Voile/v26.3.0-24/linux"
    local original="$HOMEDIR"
    local stage name target

    MEDIA="$HERE/media"
    [ -x "$stock/engine/fexdarwin-run" ] && [ -f "$MEDIA/wine/mf-range.dll" ] || return 1
    mkdir -p "$state" || return 1
    RUNTIME="$state/linux"
    HOMEDIR="$state/home"
    if [ "${VRCM_DRYRUN:-0}" = 1 ]; then
        HOMEDIR="$original"
        return
    fi

    local lock="$state/launch-lock" owner
    if ! mkdir "$lock" 2>/dev/null; then
        IFS= read -r owner < "$lock/pid" || return 1
        [[ "$owner" =~ ^[0-9]+$ ]] || return 1
        if kill -0 "$owner" 2>/dev/null; then return 1; fi
        /bin/mv "$lock" "$lock.stale.$owner" || return 1
        mkdir "$lock" || return 1
    fi
    printf '%s\n' "$$" > "$lock/pid" || return 1
    trap '/bin/rm -f "$HOME/Library/Application Support/VRonMac/Media/v1/launch-lock/pid"; rmdir "$HOME/Library/Application Support/VRonMac/Media/v1/launch-lock"' EXIT

    if [ ! -d "$RUNTIME" ]; then
        stage="$(mktemp -d "$state/.runtime.XXXXXX")" || return 1
        for name in engine graphics guest root steam windows; do
            ln -s "$stock/$name" "$stage/$name" || return 1
        done
        ditto "$stock/wine" "$stage/wine" || return 1
        cp "$MEDIA/wine/winegstreamer.so" "$stage/wine/lib/wine/x86_64-unix/" || return 1
        install_media_dlls "$stage/wine/lib/wine/x86_64-windows" || return 1
        mv "$stage" "$RUNTIME" || return 1
    fi

    if [ ! -d "$HOMEDIR" ]; then
        stage="$(mktemp -d "$state/.prefix.XXXXXX")" || return 1
        /bin/cp -cRpP "$original/." "$stage/" || return 1
        install_media_dlls "$stage/prefix/drive_c/windows/system32" || return 1
        mv "$stage" "$HOMEDIR" || return 1
    fi

    # Tracker toggles and runtime updates are applied to the standard prefix by VRonMac.
    name=prefix/drive_c/vrconmac/vrconmac_xr.dll
    target="$HOMEDIR/$name"
    cp "$original/$name" "$target.new" && mv -f "$target.new" "$target"
}

install_media_dlls() {
    local destination=$1
    cp "$MEDIA/wine/mf-range.dll" "$destination/mf.dll.new" &&
    mv -f "$destination/mf.dll.new" "$destination/mf.dll" &&
    cp "$MEDIA/wine/mfmediaengine-test.dll" "$destination/mfmediaengine.dll.new" &&
    mv -f "$destination/mfmediaengine.dll.new" "$destination/mfmediaengine.dll" &&
    cp "$MEDIA/wine/urlmon.dll" "$destination/urlmon.dll.new" &&
    mv -f "$destination/urlmon.dll.new" "$destination/urlmon.dll" &&
    cp "$MEDIA/wine/winegstreamer.dll" "$destination/winegstreamer.dll.new" &&
    mv -f "$destination/winegstreamer.dll.new" "$destination/winegstreamer.dll"
}
