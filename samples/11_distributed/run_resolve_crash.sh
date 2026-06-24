#!/bin/bash
# ---------------------------------------------------------------------------
# resolve-crash cert: a node handed an UNRESOLVABLE relay/seed host must
# DEGRADE (solo / loopback), NEVER SIGSEGV.
#
# Background (immune-system found this; the N-4 audit surfaced it, PRE-EXISTING):
# a non-numeric, unresolvable host fed to PKERNEL_RELAY / PKERNEL_SEED used to
# crash the node with SIGSEGV (exit 139). The fault was INSIDE glibc
# getaddrinfo() when called from inside a T-Kernel task (small stack, fixed
# mmap arena) on a name that needs NSS resolution — before resolve_relay's
# error check ever ran. The fix (arch/linux/*/net_relay.c resolve_relay):
# relay/seed endpoints in this build are NUMERIC IPs (the overlay is 10.1.0.x;
# real relays are given by IP — every samples/11_distributed/*.sh uses numeric
# PKERNEL_RELAY*). A non-numeric host is logged and SKIPPED (return -1), so the
# caller drops it and falls through to solo/loopback. No getaddrinfo, no crash.
#
# This script asserts, on the DEFAULT hosted build:
#   1. PKERNEL_RELAY=garbage:notaport  -> boots, logs the skip, exit 0 (loopback)
#   2. PKERNEL_SEED=garbage:notaport   -> boots, logs the skip, exit 0 (solo)
#   3. a VALID numeric relay still registers (back-compat, unchanged)
#
# FALSIFIER (proves the guard is load-bearing): rebuild the kernel with
#   make EXTRA_CFLAGS=-DRESOLVE_NO_GUARD
# which restores the getaddrinfo path; on a host whose libc getaddrinfo faults
# in-task (e.g. native aarch64 here) cases 1 & 2 SIGSEGV again (exit 139).
#
# Exit code 0 only if every assertion holds.
#
# Usage:   ./run_resolve_crash.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

# Run the x86_64 build under qemu if we are not on x86_64.
RUN=()
if [ "$(uname -m)" != "x86_64" ] && [ "$BOOT" = "$ROOT/boot/linux_x86_64" ]; then
    RUN=(qemu-x86_64)
fi

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
PASS=0 FAIL=0
ck() { # ck "name" expected_exit actual_exit
    if [ "$2" = "$3" ]; then echo "  PASS $1 (exit $3)"; PASS=$((PASS+1));
    else echo "  FAIL $1 (exit $3, want $2)"; FAIL=$((FAIL+1)); fi
}

echo "[resolve-crash] cert (degrade-not-crash on unresolvable relay/seed)"

# --- case 1: unresolvable PKERNEL_RELAY -> loopback, exit 0 ---------------
L1=$(mktemp)
printf 'net\nexit\n' | PKERNEL_RELAY=garbage:notaport "${RUN[@]}" "$BOOT/p-kernel" >"$L1" 2>&1
E1=$?
ck "RELAY unresolvable -> no crash" 0 "$E1"
grep -q "unresolvable host 'garbage'" "$L1" && { echo "  PASS RELAY logged the skip"; PASS=$((PASS+1)); } \
    || { echo "  FAIL RELAY skip not logged"; FAIL=$((FAIL+1)); }
grep -q "transport = loopback" "$L1" && { echo "  PASS RELAY degraded to loopback"; PASS=$((PASS+1)); } \
    || { echo "  FAIL RELAY did not degrade to loopback"; FAIL=$((FAIL+1)); }

# --- case 2: unresolvable PKERNEL_SEED -> solo, exit 0 -------------------
L2=$(mktemp)
printf 'net\nexit\n' | PKERNEL_SEED=garbage:notaport "${RUN[@]}" "$BOOT/p-kernel" >"$L2" 2>&1
E2=$?
ck "SEED unresolvable -> no crash" 0 "$E2"
grep -q "no usable seed — running solo" "$L2" && { echo "  PASS SEED degraded to solo"; PASS=$((PASS+1)); } \
    || { echo "  FAIL SEED did not degrade to solo"; FAIL=$((FAIL+1)); }

# --- case 3: VALID numeric relay still registers (back-compat) -----------
RLOG=$(mktemp); NLOG=$(mktemp); PORT=7433
PKERNEL_RELAY_KEY=$KEY "$ROOT/relay/relay" -p "$PORT" -v >"$RLOG" 2>&1 &
RP=$!
sleep 1
printf 'net\nexit\n' | PKERNEL_NODE_ID=9 PKERNEL_RELAY_KEY=$KEY \
    PKERNEL_RELAY=127.0.0.1:$PORT "${RUN[@]}" "$BOOT/p-kernel" >"$NLOG" 2>&1
E3=$?
sleep 1
kill $RP 2>/dev/null; wait 2>/dev/null
ck "numeric relay -> no crash" 0 "$E3"
grep -q "transport = relay" "$NLOG" && { echo "  PASS numeric relay selected"; PASS=$((PASS+1)); } \
    || { echo "  FAIL numeric relay not selected"; FAIL=$((FAIL+1)); }
grep -q "node 9 registered" "$RLOG" && { echo "  PASS relay saw the REGISTER"; PASS=$((PASS+1)); } \
    || { echo "  FAIL relay never saw the REGISTER"; FAIL=$((FAIL+1)); }

rm -f "$L1" "$L2" "$RLOG" "$NLOG"
echo "[resolve-crash] RESULT: $PASS PASS / $FAIL FAIL"
[ "$FAIL" = 0 ] && exit 0 || exit 1
