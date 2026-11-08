#!/bin/bash
# ---------------------------------------------------------------------------
# run_nosw.sh — the hosted exit dispatch's no-switch regions and compute pool
#   (gap-ledger EXIT-DISPATCH-SHARED-STATE / RNG0-HOSTED; arch/common/include/
#   pk_nosw.h, arch/linux/pool_preempt.c, shell verbs `tkconf nosw` and
#   `tkconf pool`).
#
# On hosted Linux x86_64 the timer may switch tasks at any instruction, so
# shared state is bracketed in per-task no-switch regions. The probes race a
# pri-19 task against a pri-20 runner on each guarded state (kdds slot, p-fs,
# student, ARK, dtr, dlb, ...) and must print ALL PASS.
#
# Arms (each a separate build of the same tree):
#   plain    `tkconf nosw` and `tkconf pool` ALL PASS
#   off      -DPK_NOSW_OFF (regions empty): `tkconf nosw` must be RED on N1
#            (kdds slot) and N20 (net_lan send buffer; 27 foreign packets in each of 2 runs)
#   slow     -DARK_FAULT_SLOW_GET=200000 (ARK get widened): ALL PASS
#   slowoff  the same with -DPK_NOSW_OFF: must be RED on N9 (unwidened, N9
#            catches the ARK race only 1 time in 2, so CI asks it widened)
#   oncewide -DPK_POOL_INIT_SPIN (pk_pool_init widened): `tkconf pool` ALL PASS
#            with no hang (POOL-ONCE)
#   oncewideoff  the same with -DPK_POOL_ONCE_PREEMPTIBLE: must hang (rc 124)
#   slowpeer -DNETLAN_FAULT_SLOW_LEARN=200000 (learn_peer's gap widened): ALL PASS
#   slowpeeroff  the same with -DPK_NOSW_OFF: must be RED on N24
#   netlive  plain build with the LAN up (PKERNEL_LAN=1 + key): the net_lan
#            probe must refuse (N23 PASS, N20-N22 SKIPPED, no ALL PASS)
#   netliveng  -DPK_NETLAN_PROBE_NO_GUARD, LAN up: must be RED on N23
#   dropfp   -DPK_PREEMPT_DROP_FP (xmm0-15 zeroed, not restored): `tkconf fp`
#            must be RED on F1 (plain also runs `tkconf fp`: ALL PASS)
#   dropfphi -DPK_PREEMPT_DROP_FP_HI (only xmm12-15 zeroed): RED on F3 (H11)
#   dropmxcsr -DPK_PREEMPT_DROP_MXCSR (default MXCSR put back): RED on F4 (H11)
#   slowclaim  -DDRPC_FAULT_SLOW_CLAIM=200000 (drpc_claim's gap widened): ALL PASS
#   slowclaimoff  the same with -DPK_NOSW_OFF: must be RED on N26
# A negative-control arm that stays green fails this script.
#
#   tests/host/run_nosw.sh            (x86_64 host)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rc_all=0

drive() {  # $1 = tree root, $2 = verb -> its output on stdout
    printf '%s\nexit\n' "$2" | PKERNEL_GALAXY=0 timeout 300 "$1/boot/linux_x86_64/p-kernel" 2>&1 \
        | tr -d '\r' | grep -a '^\[\(nosw\|pool\|fp\)\]'
}

arm() {  # $1 = name, $2 = EXTRA_CFLAGS -> tree root on stdout ("" on failure)
    local d="$WORK/$1"
    mkdir -p "$d"
    tar -C "$ROOT" --exclude=./.git -cf - . | tar -C "$d" -xf -
    make -C "$d/boot/linux_x86_64" clean > /dev/null 2>&1
    if ! make -C "$d/boot/linux_x86_64" -j4 EXTRA_CFLAGS="$2" > "$WORK/build-$1.log" 2>&1; then
        tail -20 "$WORK/build-$1.log" >&2; echo ""; return
    fi
    echo "$d"
}

echo "[build] plain"
if ! make -C "$ROOT/boot/linux_x86_64" -j4 > "$WORK/build-plain.log" 2>&1; then
    tail -20 "$WORK/build-plain.log"; echo "[nosw-cert] FAIL (build)"; exit 1
fi
for v in "tkconf nosw" "tkconf pool" "tkconf fp"; do
    drive "$ROOT" "$v" > "$WORK/plain.log"
    cat "$WORK/plain.log"
    if grep -q '^\[\(nosw\|pool\|fp\)\] ALL PASS' "$WORK/plain.log"; then echo "[result] plain $v GREEN (expected)"
    else echo "[result] plain $v RED (UNEXPECTED)"; rc_all=1; fi
    echo ""
done

expect_red() {  # $1 = arm, $2 = EXTRA_CFLAGS, $3 = check ids that must each FAIL
    echo "[build] $1 ($2)"
    local d c; d=$(arm "$1" "$2")
    [ -n "$d" ] || { echo "[result] $1 build FAILED"; rc_all=1; return; }
    drive "$d" "tkconf nosw" > "$WORK/$1.log"
    grep '^\[nosw\] FAIL\|ALL PASS\|RED' "$WORK/$1.log"
    for c in $3; do
        if grep -q "^\[nosw\] FAIL $c " "$WORK/$1.log"; then echo "[result] $1 RED on $c (expected)"
        else echo "[result] $1 not red on $c (UNEXPECTED: the probe missed the empty regions)"; rc_all=1; fi
    done
    echo ""
}
expect_green() {  # $1 = arm, $2 = EXTRA_CFLAGS
    echo "[build] $1 ($2)"
    local d; d=$(arm "$1" "$2")
    [ -n "$d" ] || { echo "[result] $1 build FAILED"; rc_all=1; return; }
    drive "$d" "tkconf nosw" > "$WORK/$1.log"
    grep '^\[nosw\] FAIL\|ALL PASS\|RED' "$WORK/$1.log"
    if grep -q '^\[nosw\] ALL PASS' "$WORK/$1.log"; then echo "[result] $1 GREEN (expected)"
    else echo "[result] $1 RED (UNEXPECTED)"; rc_all=1; fi
    echo ""
}
expect_red off "-DPK_NOSW_OFF" "N1 N20 N28"
expect_green slow "-DARK_FAULT_SLOW_GET=200000"
expect_red slowoff "-DARK_FAULT_SLOW_GET=200000 -DPK_NOSW_OFF" N9
expect_green slowpeer "-DNETLAN_FAULT_SLOW_LEARN=200000"
expect_red slowpeeroff "-DNETLAN_FAULT_SLOW_LEARN=200000 -DPK_NOSW_OFF" N24

# POOL-ONCE: a switch inside pthread_once(pk_pool_init) left the next task's
# pthread_once waiting, on the same OS thread, for an init that never ends.
# The window is a few microseconds; -DPK_POOL_INIT_SPIN widens it so a tick
# always lands inside. `tkconf pool` must then still finish (a hang = rc 124).
once() {  # $1 = arm, $2 = EXTRA_CFLAGS, $3 = green|hang
    echo "[build] $1 ($2)"
    local d; d=$(arm "$1" "$2")
    [ -n "$d" ] || { echo "[result] $1 build FAILED"; rc_all=1; return; }
    printf 'tkconf pool\nexit\n' | PKERNEL_GALAXY=0 timeout 120 "$d/boot/linux_x86_64/p-kernel" > "$WORK/$1.raw" 2>&1
    local rc=$?
    tr -d '\r' < "$WORK/$1.raw" | grep -a '^\[pool\] \(FAIL\|ALL PASS\|RED\)'
    echo "[once] $1 rc=$rc"
    if [ "$3" = green ]; then
        if [ "$rc" -eq 0 ] && grep -aq '^\[pool\] ALL PASS' "$WORK/$1.raw"; then echo "[result] $1 GREEN (expected)"
        else echo "[result] $1 RED rc=$rc (UNEXPECTED)"; rc_all=1; fi
    else
        if [ "$rc" -eq 124 ]; then echo "[result] $1 HUNG (expected)"
        else echo "[result] $1 did not hang rc=$rc (UNEXPECTED: the widened window missed)"; rc_all=1; fi
    fi
    echo ""
}
once oncewide "-DPK_POOL_INIT_SPIN=200000000" green
once oncewideoff "-DPK_POOL_INIT_SPIN=200000000 -DPK_POOL_ONCE_PREEMPTIBLE" hang

# The net_lan probe uses an all-zero key; with the LAN up that key is the live
# one (net_task receives with it). Live, the probe must refuse: N23 PASS,
# N20-N22 SKIPPED and the summary must not say ALL PASS. The LAN starts on the
# shell verb `net`; the [net_lan] node line shows it came up.
LIVE="PKERNEL_LAN=1 PKERNEL_LAN_PORT=17351 PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
live() {  # $1 = arm, $2 = tree root, $3 = pass|red
    printf 'net\ntkconf nosw\nexit\n' | env $LIVE PKERNEL_GALAXY=0 timeout 300 "$2/boot/linux_x86_64/p-kernel" 2>&1 \
        | tr -d '\r' | grep -a '^\[\(nosw\|net_lan\)\]' > "$WORK/$1.log"
    grep -a '^\[nosw\] \(FAIL\|PASS N23\|SKIP\|net_lan\|ALL PASS\|PASS,\|RED\)\|^\[net_lan\] node' "$WORK/$1.log"
    if ! grep -aq '^\[net_lan\] node ' "$WORK/$1.log"; then
        echo "[result] $1 the LAN did not come up (UNEXPECTED)"; rc_all=1
    elif [ "$3" = pass ]; then
        if grep -aq '^\[nosw\] PASS N23 ' "$WORK/$1.log" && grep -aq '^\[nosw\] SKIP N20 N21 N22 ' "$WORK/$1.log" \
           && ! grep -aq '^\[nosw\] ALL PASS' "$WORK/$1.log"; then echo "[result] $1 N23 PASS, N20-N22 SKIPPED (expected)"
        else echo "[result] $1 UNEXPECTED (want PASS N23, SKIP N20-N22, no ALL PASS)"; rc_all=1; fi
    else
        if grep -aq '^\[nosw\] FAIL N23 ' "$WORK/$1.log"; then echo "[result] $1 RED on N23 (expected)"
        else echo "[result] $1 not red on N23 (UNEXPECTED: the probe swapped the live key unseen)"; rc_all=1; fi
    fi
    echo ""
}
echo "[run] netlive (plain build, LAN up)"
live netlive "$ROOT" pass
echo "[build] netliveng (-DPK_NETLAN_PROBE_NO_GUARD)"
d=$(arm netliveng "-DPK_NETLAN_PROBE_NO_GUARD")
if [ -n "$d" ]; then live netliveng "$d" red; else echo "[result] netliveng build FAILED"; rc_all=1; fi

# FP-PRESERVE: the trampoline must give the task back its FP/SIMD registers.
echo "[build] dropfp (-DPK_PREEMPT_DROP_FP)"
d=$(arm dropfp "-DPK_PREEMPT_DROP_FP")
if [ -z "$d" ]; then echo "[result] dropfp build FAILED"; rc_all=1
else
    drive "$d" "tkconf fp" > "$WORK/dropfp.log"
    grep '^\[fp\] \(FAIL\|PASS\|ALL PASS\|RED\)' "$WORK/dropfp.log"
    if grep -q '^\[fp\] FAIL F1 ' "$WORK/dropfp.log"; then echo "[result] dropfp RED on F1 (expected)"
    else echo "[result] dropfp not red on F1 (UNEXPECTED: lost FP registers went unseen)"; rc_all=1; fi
    echo ""
fi
# test-holes H11: a partial loss (xmm12-15 only, or MXCSR only) left F1 green
fparm() {  # <name> <cflags> <check that must be red>
    echo "[build] $1 ($2)"
    local d; d=$(arm "$1" "$2")
    if [ -z "$d" ]; then echo "[result] $1 build FAILED"; rc_all=1; return; fi
    drive "$d" "tkconf fp" > "$WORK/$1.log"
    grep '^\[fp\] \(FAIL\|PASS\|ALL PASS\|RED\)' "$WORK/$1.log"
    if grep -q "^\[fp\] FAIL $3 " "$WORK/$1.log"; then echo "[result] $1 RED on $3 (expected)"
    else echo "[result] $1 not red on $3 (UNEXPECTED)"; rc_all=1; fi
    echo ""
}
fparm dropfphi "-DPK_PREEMPT_DROP_FP_HI" F3
fparm dropmxcsr "-DPK_PREEMPT_DROP_MXCSR" F4
expect_green slowclaim "-DDRPC_FAULT_SLOW_CLAIM=200000"
expect_red slowclaimoff "-DDRPC_FAULT_SLOW_CLAIM=200000 -DPK_NOSW_OFF" N26

if [ "$rc_all" -eq 0 ]; then echo "[nosw-cert] PASS"; else echo "[nosw-cert] FAIL"; fi
exit "$rc_all"
