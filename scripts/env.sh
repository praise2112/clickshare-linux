# Sourced by clickshare.sh and setup.sh. Exports the settings in
# ~/.config/clickshare-linux/env (one VAR=value per line; variables already set
# in the environment win) and finds wineserver.
CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/clickshare-linux/env
if [ -f "$CONFIG" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in *=*) ;; *) continue ;; esac
        key=${line%%=*}
        case $key in
            CLICKSHARE_PREFIX|CLICKSHARE_NO_AUDIO|CLICKSHARE_MAX_FPS|WINEDLLOVERRIDES|WINEDEBUG)
                printenv "$key" >/dev/null || export "$key=${line#*=}" ;;
        esac
    done < "$CONFIG"
fi

# Ubuntu's Wine keeps wineserver off PATH; empty if Wine is missing.
WINESERVER=$(command -v wineserver) || WINESERVER=
for candidate in /usr/lib/x86_64-linux-gnu/wine/wineserver /usr/lib/i386-linux-gnu/wine/wineserver; do
    if [ -z "$WINESERVER" ] && [ -x "$candidate" ]; then WINESERVER=$candidate; fi
done
