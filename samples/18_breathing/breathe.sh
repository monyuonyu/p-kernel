#!/bin/bash
# ---------------------------------------------------------------------------
# R3b — "breathing parameters": expert specialization, demonstrated in numbers.
#
# Audit §4 said the MoE was a fraud of scale: every "expert" (= node) held the
# SAME 635 parameters, so a cluster of N was N identical copies, not a mixture.
# This demo refutes that with a measurement:
#
#   * CONTROL (all-same-weights, the audit §4 old state): N copies score the
#     SAME as 1 — adding nodes does nothing.
#   * JOIN: 1 generalist + band-specialists. Each specialist OWNS one region of
#     input space (the §7 gate routes to it) and learns only that region's rule,
#     so the mixture beats the single generalist. Accuracy RISES with N:
#         1 expert  <  2 experts  <  4 experts   (strict; printed as a table)
#   * LEAVE: kill 1 of 4 (the domain-1 specialist). The §7 router reroutes that
#     region to the generalist, so ONLY domain 1 degrades; domains 0 and 2 are
#     untouched and overall falls gracefully (no cliff). Per-domain table shows
#     the localized degradation.
#
# The breathing itself is computed deterministically INSIDE the kernel (`breathe`
# shell verb -> arch/common/spec.c): trains 4 genuinely-different experts, then
# evaluates join/leave teams over a held-out split. Deterministic seeds => the
# numbers are bit-identical on every run and every ABI. When the sibling ABI
# (x86_64<->aarch64) build + qemu are present, this script runs it too and
# asserts the SAME numbers cross-ABI (float32 weights are LE/IEEE754 on all
# targets). Real distributed inference over the relay is intentionally NOT used
# here — net/relay/dkva are owned by sibling work and out of scope for R3b; the
# in-kernel harness is the deterministic proof, `breathe save` is the
# distribution substrate (each expert -> its own content-addressed p-fs block
# "dtr/expert/<k>").
#
# Usage:  ./breathe.sh
# Exit:   0 = every node PASSed (join smarter, leave graceful); non-0 otherwise.
# Logs:   /tmp/breathe_<arch>.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux";        XBOOT="$ROOT/boot/linux_x86_64"; XQEMU=qemu-x86_64 ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64"; XBOOT="$ROOT/boot/linux";        XQEMU=qemu-aarch64 ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || { echo "build failed"; exit 1; }

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

# Run one ./p-kernel with the `breathe` verb, return its log path. Single
# blocking command; ~1 s of training. Background + PID-tracked so the trap can
# reap it (nodes <= 2, killed by PID).
run_node() {
    local cmd="$1"
    local arch="$2"
    local log="/tmp/breathe_${arch}.log"
    printf 'breathe\nexit\n' | timeout 300 $cmd >"$log" 2>&1 &
    local pid=$!; PIDS+=("$pid")
    wait "$pid"
    echo "$log"
}

report() {                                   # pretty-print the demo tables
    local log="$1"
    grep -E '\[breathe\] (R3b|dataset|trained|control)' "$log"
    echo "  --- JOIN (participation rises -> accuracy rises) ---"
    grep -E '^  N=[0-9] experts' "$log"
    echo "  --- LEAVE (kill 1 of 4 -> only its domain degrades) ---"
    grep -E '^  (before|after) ' "$log"
    grep -E '^  (join|leave):' "$log"
}

verdict() {                                  # 0 if this node PASSed
    grep -q '\[breathe\] PASS' "$1" && ! grep -q 'FAIL' "$1"
}

rc=0

echo "[breathe] node A — host ABI $(uname -m)"
LOGA="$(run_node "$BOOT/p-kernel" "$(uname -m)")"
report "$LOGA"
if verdict "$LOGA"; then echo "[breathe] node A: PASS"; else echo "[breathe] node A: FAIL"; rc=1; fi

# Sibling ABI (optional): same deterministic numbers prove specialization is in
# the weights, not the host. Two nodes max.
if command -v "$XQEMU" >/dev/null 2>&1 && [ -x "$XBOOT/p-kernel" ]; then
    SARCH="${XQEMU#qemu-}"
    echo
    echo "[breathe] node B — sibling ABI $SARCH (via $XQEMU)"
    LOGB="$(run_node "$XQEMU $XBOOT/p-kernel" "$SARCH")"
    if verdict "$LOGB"; then echo "[breathe] node B: PASS"; else echo "[breathe] node B: FAIL"; rc=1; fi
    # cross-ABI determinism: the JOIN/LEAVE numbers must match byte-for-byte
    if [ -f "$LOGB" ]; then
        A_NUM="$(grep -E '^  (N=|before|after|join:|leave:)' "$LOGA")"
        B_NUM="$(grep -E '^  (N=|before|after|join:|leave:)' "$LOGB")"
        if [ "$A_NUM" = "$B_NUM" ]; then
            echo "[breathe] cross-ABI: numbers identical $(uname -m) == $SARCH"
        else
            echo "[breathe] cross-ABI: MISMATCH (float32 portability broken?)"; rc=1
        fi
    fi
else
    echo
    echo "[breathe] (sibling ABI unavailable — skipping cross-ABI check)"
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "[breathe] DEMO PASS — join makes it smarter, leave degrades gracefully."
else
    echo "[breathe] DEMO FAIL"
fi
exit "$rc"
