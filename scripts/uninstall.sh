#!/bin/sh
# Removes what setup.sh installed: the autostart service, the launcher and icon,
# the settings, and (after asking) the Wine prefix with the ClickShare app.
# The udev rule needs root and is removed separately, see README.md.
#
# Usage: sh scripts/uninstall.sh
DIR=$(cd "$(dirname "$0")/.." && pwd)
. "$DIR/scripts/env.sh"
PREFIX=${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
UNIT=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/clickshare.service

# Stop the app before its files go; this touches only ClickShare's own prefix.
if command -v systemctl >/dev/null 2>&1; then systemctl --user stop clickshare.service 2>/dev/null; fi
if [ -n "$WINESERVER" ] && [ -d "$PREFIX" ]; then WINEPREFIX=$PREFIX "$WINESERVER" -k 2>/dev/null; fi

rm -f "$UNIT"
if command -v systemctl >/dev/null 2>&1; then systemctl --user daemon-reload 2>/dev/null; fi
rm -f "$DATA/applications/clickshare-linux.desktop" "$DATA/icons/hicolor/256x256/apps/clickshare-linux.png"
rm -rf "$(dirname "$CONFIG")"
echo "Removed the autostart service, the launcher and the settings."

if [ -d "$PREFIX" ]; then
    printf 'Delete the Wine prefix %s, with the ClickShare app in it? [y/N] ' "$PREFIX"
    read -r answer
    case $answer in
        [yY]*) rm -rf "$PREFIX"; echo "Deleted $PREFIX." ;;
        *) echo "Kept $PREFIX." ;;
    esac
fi
echo "To remove the udev rule: sudo rm /etc/udev/rules.d/70-clickshare.rules && sudo udevadm control --reload"
