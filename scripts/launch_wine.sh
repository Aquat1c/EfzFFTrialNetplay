#!/usr/bin/env bash

# Launch EFZ FF Netplay with Wine. --game-dir defaults to this folder or ../game.

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
game_dir=${TRIALNET_GAME_DIR:-}

if [[ ${1:-} == --game-dir ]]; then
    (($# >= 2)) || { printf '%s\n' 'Usage: launch_wine.sh [--game-dir PATH]' >&2; exit 2; }
    game_dir=$2
    shift 2
fi
if (($# != 0)); then
    printf '%s\n' 'Usage: launch_wine.sh [--game-dir PATH]' >&2
    exit 2
fi

if [[ -z $game_dir ]]; then
    if [[ -f "$script_dir/EFZ.EXE" ]]; then
        game_dir=$script_dir
    elif [[ -f "$script_dir/../game/EFZ.EXE" ]]; then
        game_dir=$script_dir/../game
    else
        printf '%s\n' 'EFZ.EXE was not found; set TRIALNET_GAME_DIR or pass --game-dir.' >&2
        exit 1
    fi
fi
game_dir=$(CDPATH= cd -- "$game_dir" && pwd -P)

for file in 'EFZ.EXE' 'EFZ FF Netplay.exe' 'netplay.dll' 'SDL3.dll' 'ddraw.dll' 'KEY.ini'; do
    if [[ ! -f "$game_dir/$file" ]]; then
        printf 'Missing required file: %s\n' "$game_dir/$file" >&2
        exit 1
    fi
done
for directory in AKANE AYU MAI MISAKI SYSTEM WAVE; do
    if [[ ! -d "$game_dir/$directory" ]]; then
        printf 'Missing required game data: %s\n' "$game_dir/$directory/" >&2
        exit 1
    fi
done

trim_spaces() {
    local value=$1
    value=${value#"${value%%[![:space:]]*}"}
    value=${value%"${value##*[![:space:]]}"}
    printf '%s' "$value"
}

# Strip ddraw entries (wildcards too) and append one last rule; it must win.
preserved_overrides=()
if [[ -n ${WINEDLLOVERRIDES:-} ]]; then
    IFS=';' read -r -a override_entries <<<"$WINEDLLOVERRIDES"
    for raw_entry in "${override_entries[@]}"; do
        entry=$(trim_spaces "$raw_entry")
        [[ -n $entry ]] || continue
        if [[ $entry != *=* ]]; then
            preserved_overrides+=("$entry")
            continue
        fi

        module_list=${entry%%=*}
        load_order=$(trim_spaces "${entry#*=}")
        kept_modules=()
        IFS=',' read -r -a modules <<<"$module_list"
        for raw_module in "${modules[@]}"; do
            module=$(trim_spaces "$raw_module")
            [[ -n $module ]] || continue
            normalized=${module,,}
            normalized=${normalized#\*}
            normalized=${normalized%.dll}
            [[ $normalized == ddraw ]] && continue
            kept_modules+=("$module")
        done
        if ((${#kept_modules[@]})); then
            joined_modules=$(IFS=','; printf '%s' "${kept_modules[*]}")
            preserved_overrides+=("$joined_modules=$load_order")
        fi
    done
fi
preserved_overrides+=('ddraw=n,b')
export WINEDLLOVERRIDES=$(IFS=';'; printf '%s' "${preserved_overrides[*]}")

wine_command=${WINE:-wine}
if ! command -v "$wine_command" >/dev/null 2>&1; then
    printf "Wine command not found: %s (override with WINE=/path/to/wine)\n" "$wine_command" >&2
    exit 1
fi

cd -- "$game_dir"
exec "$wine_command" "$game_dir/EFZ FF Netplay.exe"
