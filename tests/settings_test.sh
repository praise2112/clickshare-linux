#!/bin/sh
# Checks that clickshare.sh reads its settings file safely.
SETTINGS="$(cd "$(dirname "$0")/.." && pwd)/scripts/env.sh"
CONFIG_HOME=$(mktemp -d)
trap 'rm -rf "$CONFIG_HOME"' EXIT
mkdir -p "$CONFIG_HOME/clickshare-linux"
printf '%s\n' '# comment' 'CLICKSHARE_MAX_FPS=20' 'WINEDLLOVERRIDES=winemenubuilder.exe=' \
    'CLICKSHARE_PREFIX=/opt/prefix with space' 'WINEDEBUG=$(echo executed)' 'PATH=/nowhere' \
    'CLICKSHARE_NO_AUDIO' > "$CONFIG_HOME/clickshare-linux/env"
printf 'CLICKSHARE_NO_AUDIO=1' >> "$CONFIG_HOME/clickshare-linux/env"

# check <description> <shell test> [VAR=value ...]; sources the file under set -e, like setup.sh
check() {
    description=$1 test=$2
    shift 2
    if env -i PATH="$PATH" XDG_CONFIG_HOME="$CONFIG_HOME" "$@" sh -ec ". '$SETTINGS'; $test"; then
        echo "ok: $description"
    else
        echo "FAIL: $description"
        exit 1
    fi
}

check "values are read" '[ "$CLICKSHARE_MAX_FPS" = 20 ]'
check "a value ending in = keeps it" '[ "$WINEDLLOVERRIDES" = "winemenubuilder.exe=" ]'
check "spaces and a last line without newline are read" \
    '[ "$CLICKSHARE_PREFIX" = "/opt/prefix with space" ] && [ "$CLICKSHARE_NO_AUDIO" = 1 ]'
check "values are never executed" '[ "$WINEDEBUG" = "\$(echo executed)" ]'
check "unknown variables are ignored" '[ "$PATH" != /nowhere ]'
check "the environment wins over the file" '[ "$CLICKSHARE_MAX_FPS" = 0 ]' CLICKSHARE_MAX_FPS=0
