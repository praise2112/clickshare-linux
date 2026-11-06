#!/bin/sh
# What the app-grid launcher runs: opens ClickShare without stopping a copy that
# is already running, so a share in progress is never cut off.
DIR=$(cd "$(dirname "$0")/.." && pwd)
. "$DIR/scripts/env.sh"
PREFIX=${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}

if command -v systemctl >/dev/null 2>&1 \
        && systemctl --user is-active --quiet 'dev-clickshare\x2dbutton.device'; then
    # With the Button plugged in, the autostart service owns the app; starting it is a no-op if it runs.
    exec systemctl --user start clickshare.service
fi

for pid in $(pgrep -f 'clickshare_native\.exe'); do
    if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$PREFIX"; then
        echo "ClickShare is already running." >&2
        exit 0
    fi
done

# Run like the autostart service, so GNOME sees one app and asks for screen access only once.
if command -v systemd-run >/dev/null 2>&1; then
    exec systemd-run --user --collect --quiet sh "$DIR/clickshare.sh"
fi
exec sh "$DIR/clickshare.sh"
