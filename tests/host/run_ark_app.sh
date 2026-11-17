#!/bin/bash
# tests/host/run_ark_app.sh — ark-entry.md §7.3: the one-person entry over galaxy.
# Part ① (this file today): A7, who may call galaxy (§7.2 6, §7.6). The record
# routes (A1-A6, A8) come with part ②.
#
# One hosted node, galaxy on a fixed port, driven by tests/host/ark_app.py.
# The checks and what each arm must show are fixed before the code:
#   plain     A7-file A7-public A7-no-token A7-bad-token A7-good-token
#             A7-origin A7-host A7-options A7-nowrite all PASS
#   nc-token  (-DGALAXY_NO_TOKEN: galaxy does not look at the pass)
#             A7-no-token, A7-bad-token and A7-nowrite FAIL; A7-public,
#             A7-good-token, A7-origin and A7-host still PASS
#   nc-origin (-DGALAXY_NO_ORIGIN: galaxy does not look at Host / Origin)
#             A7-origin and A7-host FAIL; A7-no-token, A7-bad-token,
#             A7-good-token and A7-nowrite still PASS
# Output: "[ark-app] <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
# ARK_APP_ARMS="plain" runs only the named arms (the negatives are skipped
# and named in the RED line, so a partial run is never ALL PASS).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET PKERNEL_GALAXY
PORT="${ARK_APP_PORT:-17831}"
ARMS="${ARK_APP_ARMS:-plain nc-token nc-origin}"
WORK="$(mktemp -d /tmp/arkapp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
REDS=""

build() {  # <name> <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" EXTRA_CFLAGS="$2" > "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[ark-app] $1 build FAIL"; return 1
    fi
}
arm() {  # <name> <cflags>
    case " $ARMS " in *" $1 "*) ;; *) REDS="$REDS $1-skipped"; return 1 ;; esac
    build "$1" "$2" || { REDS="$REDS $1-build"; return 1; }
    mkdir -p "$WORK/$1"
    timeout 600 python3 "$HERE/ark_app.py" "$BOOT/p-kernel" "$WORK/$1" "$PORT" > "$WORK/$1.out" 2>&1
    sed "s/^\[ark-app\] /[ark-app] $1 /" "$WORK/$1.out"
    cp "$WORK/$1/node.log" "/tmp/ark_app_$1.log" 2>/dev/null
}
is() {  # <arm> <check> <PASS|FAIL>
    grep -q "^\[ark-app\] $2 $3" "$WORK/$1.out"
}
want() {  # <arm> <PASS|FAIL> <check>...
    local a="$1" v="$2"; shift 2
    for c in "$@"; do is "$a" "$c" "$v" || REDS="$REDS $a-$c"; done
}

ALL="A7-file A7-public A7-no-token A7-bad-token A7-good-token A7-origin A7-host A7-options A7-nowrite"
# shellcheck disable=SC2086
if arm plain ""; then want plain PASS $ALL; fi
if arm nc-token "-DGALAXY_NO_TOKEN"; then
    want nc-token FAIL A7-no-token A7-bad-token A7-nowrite
    want nc-token PASS A7-public A7-good-token A7-origin A7-host
fi
if arm nc-origin "-DGALAXY_NO_ORIGIN"; then
    want nc-origin FAIL A7-origin A7-host
    want nc-origin PASS A7-no-token A7-bad-token A7-good-token A7-nowrite
fi
make -C "$BOOT" clean >/dev/null 2>&1
echo "[ark-app] logs: /tmp/ark_app_<arm>.log"
if [ -z "$REDS" ]; then echo "[ark-app] ALL PASS"; exit 0; fi
echo "[ark-app] RED:$REDS"; exit 1
