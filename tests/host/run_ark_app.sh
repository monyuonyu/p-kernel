#!/bin/bash
# tests/host/run_ark_app.sh — ark-entry.md §7.3: the one-person entry over galaxy.
# Part ①: A7, who may call galaxy (§7.2 6, §7.6). Part ② (2026-11-19): the record
# routes A1-A6 (§7.2 1-4, 7); A8 (the page's JS, a DOM test) is still to come.
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
#   part ② (plain: A1-gate A1 A2 A3 A4 A4-import A5 A6 AH PASS; AH = /ark/heir)
#   nc-ark-gate     (-DGALAXY_ARK_NO_GATE: no consent check)      A1-gate FAIL, A1 PASS
#   nc-normalize    (-DGALAXY_ARK_NORMALIZE: trailing spaces cut)  A1 FAIL, A1-gate PASS
#   nc-backup-gate  (-DGALAXY_ARK_NO_BACKUP_GATE: no 409)          A4 FAIL, A1 PASS
#   nc-backup-twice (-DGALAXY_BACKUP_TWICE: the key every time)    A4 FAIL, A1 PASS
#   A1-gate2 (plain PASS, nc-ark-gate FAIL): key written down, no consent -> 403
#   lock    (-DARK_REC_TEST_HOLD, only A9-lock: the shell and the page take one flag) PASS
#   nc-lock (... -DARK_REC_SHELL_NO_FLAG: the shell does not take it)          A9-lock FAIL
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
ARMS="${ARK_APP_ARMS:-plain lock nc-lock nc-token nc-origin nc-ark-gate nc-normalize nc-backup-gate nc-backup-twice}"
WORK="$(mktemp -d /tmp/arkapp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
REDS=""

build() {  # <name> <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" EXTRA_CFLAGS="$2" > "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[ark-app] $1 build FAIL"; return 1
    fi
}
arm() {  # <name> <cflags> [part: lock]
    case " $ARMS " in *" $1 "*) ;; *) REDS="$REDS $1-skipped"; return 1 ;; esac
    build "$1" "$2" || { REDS="$REDS $1-build"; return 1; }
    mkdir -p "$WORK/$1"
    ARK_APP_PART="${3:-}" PKERNEL_ARK_REC_HOLD_MS="${3:+3000}" \
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

ALL="A7-file A7-public A7-no-token A7-bad-token A7-good-token A7-origin A7-host A7-options A7-nowrite
     A1-gate A1-gate2 A1 A2 A3 A4 A4-import A5 A6 AH"
# shellcheck disable=SC2086
if arm plain ""; then want plain PASS $ALL; fi
if arm lock "-DARK_REC_TEST_HOLD" lock; then want lock PASS A9-lock; fi
if arm nc-lock "-DARK_REC_TEST_HOLD -DARK_REC_SHELL_NO_FLAG" lock; then want nc-lock FAIL A9-lock; fi
if arm nc-token "-DGALAXY_NO_TOKEN"; then
    want nc-token FAIL A7-no-token A7-bad-token A7-nowrite
    want nc-token PASS A7-public A7-good-token A7-origin A7-host
fi
if arm nc-origin "-DGALAXY_NO_ORIGIN"; then
    want nc-origin FAIL A7-origin A7-host
    want nc-origin PASS A7-no-token A7-bad-token A7-good-token A7-nowrite
fi
if arm nc-ark-gate "-DGALAXY_ARK_NO_GATE"; then
    want nc-ark-gate FAIL A1-gate A1-gate2; want nc-ark-gate PASS A1
fi
if arm nc-normalize "-DGALAXY_ARK_NORMALIZE"; then
    want nc-normalize FAIL A1; want nc-normalize PASS A1-gate
fi
if arm nc-backup-gate "-DGALAXY_ARK_NO_BACKUP_GATE"; then
    want nc-backup-gate FAIL A4; want nc-backup-gate PASS A1
fi
if arm nc-backup-twice "-DGALAXY_BACKUP_TWICE"; then
    want nc-backup-twice FAIL A4; want nc-backup-twice PASS A1
fi
make -C "$BOOT" clean >/dev/null 2>&1
echo "[ark-app] logs: /tmp/ark_app_<arm>.log"
if [ -z "$REDS" ]; then echo "[ark-app] ALL PASS"; exit 0; fi
echo "[ark-app] RED:$REDS"; exit 1
