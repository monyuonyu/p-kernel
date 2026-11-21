#!/bin/bash
# tests/host/run_galaxy_slots.sh — galaxy's 4 client slots and the held SSE stream
# (ark-entry.md 7.14: after quick reloads of the first screens a GET was reset, and the
# page had to try again; audit-207 break bE, the retry taken out, turned A8-resume red).
#
# Expectations fixed before the code (2026-11-21, test first; one hosted node, galaxy on
# a fixed port, driven by tests/host/galaxy_slots.py):
#   S0  the page answers; four /events streams open (the 4 slots, GALAXY_MAX_CLIENTS)
#   S1  three streams held and a fourth closed by the client: GET /manifesto answers 200
#       within 5 s (a closed stream frees its slot at once, not when a keepalive write
#       fails), and the oldest held stream stays open (S1-keep; added after the first run
#       of the fix, when S1 was four closed streams and NO_READ stayed green: the oldest-
#       gives-way rule alone let the GET in)
#   S2  four streams held open: GET /manifesto answers 200 within 5 s; the oldest
#       stream is closed by the node (S2-oldest), the newest stays (S2-newest)
# Falsifiers (each must turn its check red; plain must be ALL PASS):
#   -DGX_NC_SSE_NO_READ (a stream is never read, as before) -> S1-keep
#   -DGX_NC_NO_EVICT    (a full table drops the new connection, as before) -> S2-held
# Output: "[gx-slots] <arm> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards. GX_SLOTS_PORT (17831).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET PKERNEL_GALAXY
PORT="${GX_SLOTS_PORT:-17831}"
WORK="$(mktemp -d /tmp/gxslots.XXXXXX)"
trap 'make -C "$BOOT" clean >/dev/null 2>&1; rm -rf "$WORK"' EXIT
REDS=""

arm() {  # <name> <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" EXTRA_CFLAGS="$2" > "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[gx-slots] $1 build FAIL"; REDS="$REDS $1-build"; return 1
    fi
    mkdir -p "$WORK/$1"
    timeout 300 python3 "$HERE/galaxy_slots.py" "$BOOT/p-kernel" "$WORK/$1" "$PORT" > "$WORK/$1.out" 2>&1
    sed "s/^\[gx-slots\] /[gx-slots] $1 /" "$WORK/$1.out"
}
is() { grep -q "^\[gx-slots\] $2 $3" "$WORK/$1.out"; }

if arm plain ""; then
    for c in S0-up S0-open S1-closed S1-keep S2-open S2-held S2-oldest S2-newest; do
        is plain "$c" PASS || REDS="$REDS plain-$c"
    done
fi
if arm noread "-DGX_NC_SSE_NO_READ"; then
    if is noread S1-keep FAIL; then echo "[gx-slots] GX_NC_SSE_NO_READ turns S1-keep red (as it must)"
    else echo "[gx-slots] GX_NC_SSE_NO_READ does NOT turn S1-keep red"; REDS="$REDS noread"; fi
fi
if arm noevict "-DGX_NC_NO_EVICT"; then
    if is noevict S2-held FAIL; then echo "[gx-slots] GX_NC_NO_EVICT turns S2-held red (as it must)"
    else echo "[gx-slots] GX_NC_NO_EVICT does NOT turn S2-held red"; REDS="$REDS noevict"; fi
fi
if [ -z "$REDS" ]; then echo "[gx-slots] ALL PASS"; exit 0; fi
echo "[gx-slots] RED:$REDS"; exit 1
