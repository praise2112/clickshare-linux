#!/bin/sh
# One-time setup: creates a dedicated Wine prefix, extracts the ClickShare app
# from ClickShare_Setup.exe into it, builds the shim, and adds a launcher and
# autostart on Button plug-in.
#
# Usage: sh scripts/setup.sh /path/to/ClickShare_Setup.exe
#
# Environment:
#   CLICKSHARE_PREFIX   Wine prefix to use (default: ~/.wine-clickshare)
set -e

DIR=$(cd "$(dirname "$0")/.." && pwd)
. "$DIR/scripts/env.sh"
SETUP=${1:-$HOME/Downloads/ClickShare_Setup.exe}
export WINEPREFIX=${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}
# Keep Wine from offering Mono/Gecko installs; the app does not need them.
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG=-all

if [ ! -f "$SETUP" ]; then
    echo "ClickShare_Setup.exe not found: $SETUP" >&2
    echo "Usage: sh scripts/setup.sh /path/to/ClickShare_Setup.exe" >&2
    exit 1
fi

if ! command -v wine >/dev/null 2>&1 || [ -z "$WINESERVER" ]; then
    echo "Wine is not installed. Run: sudo sh scripts/install-winehq.sh" >&2
    exit 1
fi
WINE_MAJOR=$(wine --version | sed -E 's/^wine-([0-9]+).*/\1/')
if [ "$WINE_MAJOR" -lt 9 ] 2>/dev/null; then
    echo "Wine $(wine --version) is too old (need >= 9, tested with 10.0 and 11.0)." >&2
    echo "Run: sudo sh scripts/install-winehq.sh" >&2
    exit 1
fi

echo "== Wine prefix: $WINEPREFIX"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    wineboot -i
    "$WINESERVER" -w
fi

echo "== Extracting ClickShare app"
LOCALAPPDATA=$(winepath -u "$(wine cmd /c echo %LOCALAPPDATA% 2>/dev/null | tr -d '\r')")
python3 -I "$DIR/scripts/extract-clickshare-app.py" "$SETUP" "$LOCALAPPDATA/ClickShare"

echo "== Building shim"
sh "$DIR/scripts/build.sh"

if [ -n "$CLICKSHARE_PREFIX" ]; then
    mkdir -p "$(dirname "$CONFIG")"
    { grep -v '^CLICKSHARE_PREFIX=' "$CONFIG" 2>/dev/null || true; echo "CLICKSHARE_PREFIX=$CLICKSHARE_PREFIX"; } > "$CONFIG.new"
    mv "$CONFIG.new" "$CONFIG"
fi

echo "== Adding launcher and autostart"
APP=$(ls -d "$LOCALAPPDATA"/ClickShare/app-*/clickshare_native.exe | sort -V | tail -n 1)
sh "$DIR/scripts/install-launcher.sh" "$APP"
sh "$DIR/scripts/install-autostart.sh"

echo
if grep -qs SYSTEMD_USER_WANTS /etc/udev/rules.d/70-clickshare.rules; then
    echo "Setup complete. Plug in the ClickShare Button: the app starts by itself."
else
    echo "Setup complete. One step left, it needs your password:"
    echo "    sudo sh $DIR/scripts/install-udev-rule.sh"
    echo "Then plug in the ClickShare Button: the app starts by itself."
fi
