#!/usr/bin/env bash

# Host-side compatibility check for the Windows EFZ netplay package.

set -u

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
game_dir=""
port=10800
network_probe=0

usage() {
    printf '%s\n' \
        'Usage: bash wine_preflight.sh [--game-dir PATH] [--port PORT] [--network]' \
        '  --game-dir PATH  EFZ folder (defaults to the script folder or ../game)' \
        '  --port PORT      UDP host port to test (default: 10800)' \
        '  --network        also verify the AutoPunch coordinator reply'
}

while (($# > 0)); do
    case "$1" in
        --game-dir)
            (($# >= 2)) || { usage >&2; exit 2; }
            game_dir=$2
            shift 2
            ;;
        --port)
            (($# >= 2)) || { usage >&2; exit 2; }
            port=$2
            shift 2
            ;;
        --network)
            network_probe=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! $port =~ ^[0-9]+$ ]] || ((port < 1 || port > 65535)); then
    printf 'ERROR: invalid UDP port: %s\n' "$port" >&2
    exit 2
fi

if [[ -z $game_dir ]]; then
    if [[ -f "$script_dir/EFZ.EXE" ]]; then
        game_dir=$script_dir
    elif [[ -f "$script_dir/../game/EFZ.EXE" ]]; then
        game_dir=$script_dir/../game
    else
        game_dir=$PWD
    fi
fi

if [[ ! -d $game_dir ]]; then
    printf 'ERROR: game directory does not exist: %s\n' "$game_dir" >&2
    exit 1
fi
game_dir=$(CDPATH= cd -- "$game_dir" && pwd -P)

errors=0
warnings=0

ok() { printf 'OK: %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*"; warnings=$((warnings + 1)); }
fail() { printf 'ERROR: %s\n' "$*"; errors=$((errors + 1)); }

printf 'EFZ FF Netplay Wine/Proton preflight\n'
printf 'Game directory: %s\n' "$game_dir"
printf 'Host port: %s/udp\n\n' "$port"

if [[ $(uname -s 2>/dev/null || true) != Linux ]]; then
    warn 'this script is intended to run on the Linux host, outside Wine'
fi

required_files=(
    'EFZ.EXE'
    'EFZ FF Netplay.exe'
    'netplay.dll'
    'SDL3.dll'
    'ddraw.dll'
    'KEY.ini'
)
for file in "${required_files[@]}"; do
    if [[ -f "$game_dir/$file" ]]; then
        ok "found $file"
    else
        fail "missing exact-case file: $file"
    fi
done

for directory in AKANE AYU MAI MISAKI SYSTEM WAVE; do
    if [[ -d "$game_dir/$directory" ]]; then
        ok "found $directory/"
    else
        fail "missing exact-case game data directory: $directory/"
    fi
done

if command -v file >/dev/null 2>&1; then
    for file in 'EFZ.EXE' 'EFZ FF Netplay.exe' 'netplay.dll' 'SDL3.dll' 'ddraw.dll'; do
        [[ -f "$game_dir/$file" ]] || continue
        description=$(file -b -- "$game_dir/$file" 2>/dev/null || true)
        if [[ $description == *PE32* && $description != *PE32+* ]]; then
            ok "$file is 32-bit PE"
        else
            fail "$file is not a readable 32-bit PE image ($description)"
        fi
    done
else
    warn "the 'file' utility is unavailable; PE architecture was not checked"
fi

ddraw_exports=''
if command -v objdump >/dev/null 2>&1; then
    ddraw_exports=$(objdump -p -- "$game_dir/ddraw.dll" 2>/dev/null || true)
elif command -v llvm-objdump >/dev/null 2>&1; then
    ddraw_exports=$(llvm-objdump --private-headers -- "$game_dir/ddraw.dll" 2>/dev/null || true)
else
    warn 'objdump is unavailable; the launcher will perform the authoritative ddraw export check'
fi
if [[ -n $ddraw_exports ]]; then
    if grep -q 'EFZDDRAW_SetFrameLimit' <<<"$ddraw_exports" \
        && grep -Eq 'EFZDDRAW_RegisterOverlayCallback|EFZDX9_Register(NetplayHudProvider|OverlayProvider|TrainingMenuBridge)' <<<"$ddraw_exports"; then
        ok 'ddraw.dll exposes the packaged EFZ wrapper contract'
    else
        fail 'ddraw.dll is a generic or stale wrapper (required EFZ exports are missing)'
    fi
fi

probe="$game_dir/.trialnet_wine_write_probe_$$"
if (umask 077 && : >"$probe") 2>/dev/null; then
    rm -f -- "$probe"
    ok 'game directory is writable (configuration, session, and crash logs can be created)'
else
    fail 'game directory is read-only; EFZ and the DirectDraw wrapper still require game-local writes'
fi

if command -v findmnt >/dev/null 2>&1; then
    fs_type=$(findmnt -n -o FSTYPE -T "$game_dir" 2>/dev/null || true)
    fs_options=$(findmnt -n -o OPTIONS -T "$game_dir" 2>/dev/null || true)
    case "$fs_type" in
        ntfs*|fuseblk|exfat|vfat)
            warn "game is on $fs_type; Valve does not support shared NTFS-style Proton libraries"
            ;;
        '')
            warn 'could not determine the game filesystem type'
            ;;
        *)
            ok "filesystem is $fs_type"
            ;;
    esac
    case ",$fs_options," in
        *,ro,*) fail 'game filesystem is mounted read-only; EFZ cannot be treated as fully read-only' ;;
    esac
    case ",$fs_options," in
        *,noexec,*) warn 'game filesystem is mounted noexec; PE/DLL mapping can fail under Wine' ;;
    esac
fi

wine_command=${WINE:-wine}
if command -v "$wine_command" >/dev/null 2>&1; then
    wine_version=$($wine_command --version 2>/dev/null || true)
    [[ -n $wine_version ]] && ok "Wine runtime found: $wine_version" || warn 'Wine exists but did not report a version'
else
    warn "Wine command not found ('$wine_command'); this is normal when launching only through Steam/Proton"
fi

if [[ -n ${WINEPREFIX:-} ]]; then
    if [[ -d $WINEPREFIX && -w $WINEPREFIX ]]; then
        ok "WINEPREFIX is writable: $WINEPREFIX"
    elif [[ -e $WINEPREFIX ]]; then
        fail "WINEPREFIX is not a writable directory: $WINEPREFIX"
    else
        warn "WINEPREFIX does not exist yet and Wine will create it: $WINEPREFIX"
    fi
fi
if [[ -n ${STEAM_COMPAT_DATA_PATH:-} ]]; then
    ok "Proton compatdata path is set: $STEAM_COMPAT_DATA_PATH"
fi

if [[ ${WINEDLLOVERRIDES:-} == *ddraw=n* ]]; then
    ok 'WINEDLLOVERRIDES selects the packaged native ddraw.dll'
else
    ok 'launcher will add ddraw=n,b to the EFZ child environment automatically'
fi

python3_usable=0
if command -v python3 >/dev/null 2>&1 && python3 -c 'import socket' >/dev/null 2>&1; then
    python3_usable=1
fi

if ((python3_usable)); then
    if python3 - "$port" <<'PY'
import socket
import sys

port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    sock.bind(("0.0.0.0", port))
finally:
    sock.close()
PY
    then
        ok "UDP port $port can be bound on IPv4"
    else
        fail "UDP port $port is unavailable (already in use or blocked by host policy)"
    fi

    if python3 - <<'PY'
import socket

socket.getaddrinfo("api.ipify.org", 80, socket.AF_INET, socket.SOCK_STREAM)
PY
    then
        ok 'IPv4 DNS resolution works for optional public-IP display'
    else
        warn 'IPv4 DNS resolution failed; numeric JOIN and pinned AutoPunch can still work'
    fi
else
    warn 'a runnable python3 is unavailable; UDP bind and DNS checks were skipped'
fi

if ((network_probe)); then
    if ((!python3_usable)); then
        fail '--network requires python3'
    elif python3 - <<'PY'
import socket
import struct

coordinator = ("65.108.63.185", 14763)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(4.0)
sock.bind(("0.0.0.0", 0))
internal_port = sock.getsockname()[1]
sock.sendto(struct.pack("!H", internal_port), coordinator)
payload, source = sock.recvfrom(64)
sock.close()
if source != coordinator:
    raise SystemExit(f"reply came from unexpected endpoint {source}")
if len(payload) != 8:
    raise SystemExit(f"expected 8-byte mapping, received {len(payload)} bytes")
reported_internal, nat_port = struct.unpack("!HH", payload[:4])
if reported_internal != internal_port or nat_port == 0 or payload[4:] == b"\0\0\0\0":
    raise SystemExit("coordinator returned an invalid mapping")
print(f"AutoPunch mapping reply: internal={reported_internal} public_port={nat_port}")
PY
    then
        ok 'AutoPunch coordinator is reachable over UDP and returned a valid mapping'
    else
        fail 'AutoPunch coordinator probe failed (UDP egress/replies may be blocked)'
    fi
fi

printf '\nResult: %d error(s), %d warning(s)\n' "$errors" "$warnings"
if ((errors)); then
    exit 1
fi
exit 0
