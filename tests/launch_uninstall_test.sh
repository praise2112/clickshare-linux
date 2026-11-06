#!/bin/sh
# Checks scripts/launch.sh and scripts/uninstall.sh with stub systemctl, systemd-run
# and wineserver, in a temporary home; nothing real is started or deleted.
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'kill $APP 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/home" "$T/cfg/clickshare-linux" "$T/data/applications" "$T/prefix" "$T/other-prefix"
cat > "$T/bin/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$LOG"
case "$*" in *is-active*) [ -n "$BUTTON" ] ;; esac
EOF
cat > "$T/bin/systemd-run" <<'EOF'
#!/bin/sh
echo "systemd-run $*" >> "$LOG"
EOF
cat > "$T/bin/wineserver" <<'EOF'
#!/bin/sh
[ -d "$WINEPREFIX" ] && state=exists || state=gone
echo "wineserver $* $WINEPREFIX $state" >> "$LOG"
EOF
chmod +x "$T/bin/"*
echo "CLICKSHARE_PREFIX=$T/prefix" > "$T/cfg/clickshare-linux/env"

run() {
    : > "$T/log"
    env -i PATH="$T/bin:$PATH" HOME="$T/home" XDG_CONFIG_HOME="$T/cfg" XDG_DATA_HOME="$T/data" \
        LOG="$T/log" "$@"
}
check() {
    if eval "$2"; then echo "ok: $1"; else echo "FAIL: $1"; cat "$T/log"; exit 1; fi
}

run BUTTON=1 sh "$REPO/scripts/launch.sh"
check "with the Button plugged in, the launcher starts the service instead of a second app" \
    'grep -qx "systemctl --user start clickshare.service" "$T/log" && ! grep -q systemd-run "$T/log"'

run sh "$REPO/scripts/launch.sh"
check "without the Button, the launcher starts the app through systemd-run" \
    'grep -q "systemd-run --user --collect --quiet sh $REPO/clickshare.sh" "$T/log"'

WINEPREFIX="$T/prefix" python3 -c 'import time; time.sleep(30)' clickshare_native.exe &
APP=$!
sleep 0.2
run sh "$REPO/scripts/launch.sh" 2>/dev/null
check "a running app in this prefix is left alone" '! grep -q systemd-run "$T/log" && kill -0 $APP'
kill $APP

touch "$T/data/applications/clickshare-linux.desktop"
echo n | run sh "$REPO/scripts/uninstall.sh" > /dev/null
check "answering no keeps the prefix" '[ -d "$T/prefix" ]'

mkdir -p "$T/cfg/clickshare-linux"
echo "CLICKSHARE_PREFIX=$T/prefix" > "$T/cfg/clickshare-linux/env"
echo y | run sh "$REPO/scripts/uninstall.sh" > /dev/null
check "uninstall stops the service, then the configured prefix's Wine, before deleting it" \
    '[ "$(head -n 2 "$T/log")" = "$(printf "systemctl --user stop clickshare.service\nwineserver -k $T/prefix exists")" ]'
check "uninstall deletes the configured prefix, settings and launcher, nothing else" \
    '[ ! -e "$T/prefix" ] && [ ! -e "$T/cfg/clickshare-linux" ] && [ ! -e "$T/data/applications/clickshare-linux.desktop" ] && [ -d "$T/other-prefix" ]'
