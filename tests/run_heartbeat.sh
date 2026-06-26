#!/bin/bash
# ---------------------------------------------------------------------------
# run_heartbeat.sh — connect-anywhere SLICE 1 cert: the UNCONDITIONAL relay
# keepalive (arch/linux/*/net_relay.c::net_relay_heartbeat).
#
# WHY: a two-machine debug showed the deadlock this slice cures — a node
# registered with the relay, sent ~4 packets, then originated NO cluster
# traffic while waiting for an admission grant; with nothing leaving, the
# NAT/AP UDP return mapping aged out (~30 s) and the grant could never arrive.
# net_relay_heartbeat() beats a keepalive every KEEPALIVE_SEC (15 s)
# REGARDLESS of peer discovery or cluster admission, keeping the return path
# open.
#
# This drives the SHIPPED net_relay_heartbeat() IN-PROCESS (mock monotonic
# clock + a sendto seam that counts emits, NO sockets) via `heartbeat test`:
#
#   CURE      : an ISOLATED, UN-ADMITTED node (no peers, drpc would read 0xFF)
#               STILL emits >= floor(T/KEEPALIVE_SEC) keepalives over T=120 s
#               AND the max inter-keepalive gap stays < NAT_TIMEOUT_FLOOR(30).
#   FALSIFIER : rebuilt with -DHEARTBEAT_FALSIFIER the beat is gated on
#               admission (peer_count>0) -> emits ZERO -> RESULT: FAIL. Proves
#               the cert has teeth (a beat that needs admission cannot pass).
#
# Both arms run on BOTH host arches (aarch64-hosted native + x86_64-hosted via
# qemu-x86_64 when present). Exit 0 = cure PASSES and falsifier FAILS.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FAIL=0

run_bin() {  # $1 = binary path -> drive `heartbeat test` and echo the cert block
    local bin="$1"
    case "$bin" in
        */linux_x86_64/*)
            if command -v qemu-x86_64 >/dev/null 2>&1 && [ "$(uname -m)" != "x86_64" ]; then
                printf 'heartbeat test\nexit\n' | qemu-x86_64 "$bin" 2>/dev/null
            else
                printf 'heartbeat test\nexit\n' | "$bin" 2>/dev/null
            fi ;;
        *)  printf 'heartbeat test\nexit\n' | "$bin" 2>/dev/null ;;
    esac
}

one_arch() {  # $1 = boot dir, $2 = human label
    local boot="$ROOT/$1" label="$2"
    [ -d "$boot" ] || { echo "[$label] SKIP (no $1)"; return; }

    # ---- CURE: -DHEARTBEAT_CERT -> RESULT: 3/3 PASS -----------------------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS=-DHEARTBEAT_CERT >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (cure)"; FAIL=1; return
    fi
    local out; out="$(run_bin "$boot/p-kernel")"
    echo "$out" | grep -E '^\[heartbeat\]'
    if echo "$out" | grep -q '\[heartbeat\] RESULT: 3/3 PASS'; then
        echo "[$label] CURE PASS"
    else
        echo "[$label] CURE FAIL (expected 3/3 PASS)"; FAIL=1
    fi

    # ---- FALSIFIER: +-DHEARTBEAT_FALSIFIER -> RESULT: FAIL (teeth) --------
    make -C "$boot" clean >/dev/null 2>&1
    if ! make -C "$boot" EXTRA_CFLAGS="-DHEARTBEAT_CERT -DHEARTBEAT_FALSIFIER" >/dev/null 2>&1; then
        echo "[$label] BUILD FAILED (falsifier)"; FAIL=1; return
    fi
    out="$(run_bin "$boot/p-kernel")"
    if echo "$out" | grep -q '\[heartbeat\] RESULT: FAIL'; then
        echo "[$label] FALSIFIER correctly FAILS (cert has teeth)"
    else
        echo "[$label] FALSIFIER DID NOT FAIL — cert is toothless!"; FAIL=1
    fi
    make -C "$boot" clean >/dev/null 2>&1
}

one_arch "boot/linux"        "aarch64-hosted"
one_arch "boot/linux_x86_64" "x86_64-hosted"

if [ "$FAIL" -eq 0 ]; then echo "[heartbeat] ALL PASS"; exit 0
else echo "[heartbeat] FAILURES ABOVE"; exit 1; fi
