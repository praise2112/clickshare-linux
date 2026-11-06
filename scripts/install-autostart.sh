#!/bin/sh
# Installs a systemd user service that starts the ClickShare app when the
# Button is plugged in and stops it when the Button is removed.
# Needs the udev rule from install-udev-rule.sh.
#
# Usage: sh scripts/install-autostart.sh
set -e

DIR=$(cd "$(dirname "$0")/.." && pwd)
UNIT_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
mkdir -p "$UNIT_DIR"
cat > "$UNIT_DIR/clickshare.service" <<UNIT
[Unit]
Description=ClickShare app for the plugged-in Button
BindsTo=dev-clickshare\\x2dbutton.device
After=dev-clickshare\\x2dbutton.device

[Service]
ExecStart=/bin/sh "$DIR/clickshare.sh"
# winedevice.exe ignores SIGTERM once the Button is gone; kill it quickly so a replug can start the app.
TimeoutStopSec=3
SuccessExitStatus=143
RestartForceExitStatus=75
RestartSec=3
UNIT
systemctl --user daemon-reload
echo "Autostart installed. The app now starts when you plug in the Button."
