#!/bin/sh
# Runs all tests. Needs a host C compiler (cc), or the Zig toolchain that scripts/build.sh downloads.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
CC=$(command -v cc || ls "$DIR"/../.toolchain/zig-*/zig 2>/dev/null | head -n 1)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

case $CC in
    */zig) "$CC" cc -o "$OUT/pacing_test" "$DIR/pacing_test.c" ;;
    *) "$CC" -o "$OUT/pacing_test" "$DIR/pacing_test.c" ;;
esac
"$OUT/pacing_test"
sh "$DIR/settings_test.sh"
sh "$DIR/launch_uninstall_test.sh"
