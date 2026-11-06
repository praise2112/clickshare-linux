#!/bin/sh
# Starts the ClickShare app under Wine with the DXGI Desktop Duplication shim
# injected, and routes system audio to the ClickShare Button.
#
# Usage: sh clickshare.sh
#
# Settings, from the environment or ~/.config/clickshare-linux/env (VAR=value lines):
#   CLICKSHARE_PREFIX   Wine prefix to use (default: ~/.wine-clickshare)
#   CLICKSHARE_NO_AUDIO set to 1 to leave the audio output unchanged
#   CLICKSHARE_MAX_FPS  cap on frames handed to the app per second (default 0: no cap)
#   WINEDLLOVERRIDES    extra DLL overrides, e.g. d3d11,dxgi=n for DXVK
DIR=$(cd "$(dirname "$0")" && pwd)

. "$DIR/scripts/env.sh"
if [ -z "$WINESERVER" ]; then
    echo "Wine is not installed. See README.md, Installation." >&2
    exit 1
fi
# Started by systemd before the desktop exported its display (Button plugged in at login):
# exit status 75 makes clickshare.service try again a few seconds later.
if [ -z "$WAYLAND_DISPLAY$DISPLAY" ]; then
    echo "No graphical session yet." >&2
    exit 75
fi
export WINEPREFIX=${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}
export WINEDLLOVERRIDES="mscoree,mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export WINEDEBUG=${WINEDEBUG:--all}
export CLICKSHARE_MAX_FPS=${CLICKSHARE_MAX_FPS:-0}

LOCALAPPDATA=$(winepath -u "$(wine cmd /c echo %LOCALAPPDATA% 2>/dev/null | tr -d '\r')")
APP=$(ls -d "$LOCALAPPDATA"/ClickShare/app-*/clickshare_native.exe 2>/dev/null | sort -V | tail -n 1)
if [ -z "$APP" ]; then
    echo "ClickShare app not found in $WINEPREFIX. Run scripts/setup.sh first." >&2
    exit 1
fi
if [ ! -f "$DIR/build/csdup.dll" ] || [ ! -f "$DIR/build/inject.exe" ]; then
    echo "Shim not built. Run: sh scripts/build.sh" >&2
    exit 1
fi

# The Button is a USB audio device. It forwards audio to the room only while
# a share is running, so it can stay the default output for the whole session.
if [ "${CLICKSHARE_NO_AUDIO:-0}" != 1 ] && command -v pactl >/dev/null 2>&1; then
    SINK=$(pactl list short sinks | awk '/Barco_ClickShare/ {print $2; exit}')
    if [ -n "$SINK" ]; then
        pactl set-default-sink "$SINK"
        pactl set-sink-mute "$SINK" 0
        for i in $(pactl list short sink-inputs | cut -f1); do
            pactl move-sink-input "$i" "$SINK" 2>/dev/null
        done
        echo "Audio output -> $SINK"
    else
        echo "ClickShare Button audio device not found; audio output unchanged."
    fi
fi

"$WINESERVER" -k 2>/dev/null

# Under Wayland, Wine cannot read the screen; the helper captures it through the portal.
if [ "$XDG_SESSION_TYPE" = wayland ]; then
    FRAMES="$XDG_RUNTIME_DIR/clickshare-frames"
    /usr/bin/python3 -I "$DIR/helper/clickshare-capture.py" "$FRAMES" &
    HELPER=$!
    trap 'kill $HELPER 2>/dev/null' EXIT INT TERM
    CLICKSHARE_FRAMES=$(winepath -w "$FRAMES")
    export CLICKSHARE_FRAMES
fi

wine "$DIR/build/inject.exe" "$(winepath -w "$APP")" "$(winepath -w "$DIR/build/csdup.dll")" 3000
# inject.exe returns once the shim is loaded; wait for the app itself to exit.
"$WINESERVER" -w
