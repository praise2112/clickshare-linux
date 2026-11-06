#!/bin/sh
# Adds ClickShare to the app grid and dock, with the app's own icon when it can be read.
#
# Usage: sh scripts/install-launcher.sh [path/to/clickshare_native.exe]
set -e

DIR=$(cd "$(dirname "$0")/.." && pwd)
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
ICON=video-display

if [ -n "$1" ]; then
    PNG="$DATA/icons/hicolor/256x256/apps/clickshare-linux.png"
    mkdir -p "$(dirname "$PNG")"
    # GNOME only finds a new icon by name after logging in again, so use the full path.
    if python3 -I "$DIR/scripts/extract-icon.py" "$1" "$PNG"; then ICON=$PNG; fi
fi

mkdir -p "$DATA/applications"
cat > "$DATA/applications/clickshare-linux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ClickShare
Comment=Share your screen to a ClickShare room with the Button
Exec=sh "$DIR/scripts/launch.sh"
Icon=$ICON
StartupWMClass=clickshare_native.exe
Categories=Network;
Terminal=false
EOF
echo "Launcher installed: look for ClickShare in your app grid."
