#!/bin/bash
# ===========================================================================
# 27_protect / run.sh  —  ground the threat axis and CLOSE the loop (G28).
#
# survival §2: 守る単位 (the protected UNIT) と 守る力 (the protecting POWER) を
# 分離し、全網の力を一点へ注ぐ。wave 13 はその *符号* (rally, not flee) を入れた
# が、ループは開いていた: 脅威は温度バケツ発の信号で、何の実体にも接地して
# おらず、脅威を下げる行動も無かった (固定タイマで episode が終わるだけ)。
#
# This demo proves the loop is now GROUNDED and CLOSED with real numbers:
#
#   TREATMENT (actuator ON, default)
#     node1 declares a named p-fs object as a protected UNIT ("must survive").
#     Its threat starts HIGH because the unit is UNDER-REPLICATED (0/R). The
#     actuator (the protecting POWER) pours force on the point: it re-announces
#     the unit so neighbours pull it into their DURABLE store and announce back.
#     As the replica count climbs 0 -> R, the *measured threat FALLS to 0* —
#     because of the replication, NOT because a timer expired.
#     Then we kill -9 node1. The unit SURVIVES on a neighbour, served back.
#
#   CONTROL (actuator OFF, PKERNEL_PROTECT_OFF=1)
#     Same declaration, but the protecting POWER is disabled. The unit is held
#     quietly and never evacuated: replicas stay 0, the grounded threat stays
#     pinned HIGH. kill -9 node1 -> the unit is LOST (no neighbour has it).
#
# The contrast is in the numbers: threat 40->0 (treatment) vs 40->40 (control);
# object SURVIVES vs LOST. The drop is caused by replication, full stop.
#
# SYNC/protection semantics (wave 14 fix2 — option (a), the deliberate-spread
# rule): on boot a joining node SYNC-pulls everything its peers hold. A
# protected unit that the actuator has NOT yet driven is EXCLUDED from that
# ambient SYNC stream (and from ambient announce — already quiet). Rationale
# (§2): the protecting POWER replicates the unit ON PURPOSE; it is never leaked
# accidentally by boot-sync. Without this, a CONTROL node could survive the kill
# purely because node2/node3 happened to SYNC-pull the block before the kill —
# making "control loses the object" an accident, not a proof. Ordinary
# (non-protected) blocks and already-actuator-driven units are SYNC-served
# normally, so P1 boot-sync (samples/11_distributed, 13_survival_loop) is
# unchanged. The actuator drives replication over the announce->WANT->transfer
# path (NOT via SYNC), so treatment is unaffected by the SYNC exclusion.
#
# target_r determinism (wave 14 fix2): target_r is region-capped to
# region_size()-1, and region_size() needs each peer's RTT measured, which lags
# the degrade "-> FULL" (ALIVE count) signal. cluster_up now waits until node1's
# region is size 3 before declaring, so target_r caps to 2 deterministically;
# the kernel additionally re-caps target_r UPWARD toward the requested R when
# the region grows after an early declare (belt-and-suspenders self-heal).
#
# Also runs the in-kernel property self-test ([protect-ground]/[protect-loop]).
#
# Exit 0 = all PASS. Logs: /tmp/p27_*.log
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RELAY_PORT=7427

WORK="$(mktemp -d /tmp/p27_work.XXXXXX)"
FIFO="$WORK/fifo"
NODE_PID=(0 0 0 0)
RELAY_PID=0
FAIL=0

TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
ok()   { log "ok  : $*"; }
bad()  { log "FAIL: $*"; FAIL=1; }

send() { local i="$1"; shift; log "node$i <- '$*'"; printf '%s\n' "$*" > "$FIFO.$i"; }

cluster_down() {
    exec 3>&- 4>&- 5>&- 2>/dev/null || true
    for i in 1 2 3; do
        [ "${NODE_PID[$i]}" != 0 ] && kill -9 "${NODE_PID[$i]}" 2>/dev/null
        NODE_PID[$i]=0
    done
    [ "$RELAY_PID" != 0 ] && kill -9 "$RELAY_PID" 2>/dev/null
    RELAY_PID=0
    wait 2>/dev/null
    rm -f "$FIFO".{1,2,3}
    sleep 1
}
cleanup() { cluster_down; rm -rf "$WORK"; }
trap cleanup EXIT

# cluster_up <tag> <extra-env...>  — fresh relay + 3 nodes, each with its own
# durable p-fs dir, held alive on a FIFO; wait until node1 sees FULL.
cluster_up() {
    local tag="$1"; shift
    log "--- cluster up ($tag): relay :$PKERNEL_RELAY_PORT + 3 nodes ($*) ---"
    "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v \
        > "$WORK/${tag}_relay.log" 2>&1 &
    RELAY_PID=$!; disown "$RELAY_PID"; sleep 1
    for i in 1 2 3; do rm -f "$FIFO.$i"; mkfifo "$FIFO.$i"; done
    exec 3<>"$FIFO.1" 4<>"$FIFO.2" 5<>"$FIFO.3"
    for i in 1 2 3; do
        mkdir -p "$WORK/${tag}_dir$i"
        env PKERNEL_NODE_ID=$i PKERNEL_AUTONET=1 \
            PKERNEL_PFS_DIR="$WORK/${tag}_dir$i" "$@" \
            "$BOOT/p-kernel" < "$FIFO.$i" > "/tmp/p27_${tag}_node$i.log" 2>&1 &
        NODE_PID[$i]=$!; disown "${NODE_PID[$i]}"
        log "node$i up pid=${NODE_PID[$i]} log=/tmp/p27_${tag}_node$i.log"
    done
    # cluster-FULL wait: SWIM needs several ~1s rounds for 3 nodes to mutually
    # ALIVE-converge; on a shared self-hosted runner that can take well past the
    # old 30s. Give it 60s (~3x the realistic convergence latency).
    local t=0
    while [ $t -lt 60 ]; do
        grep -aq -- "-> FULL" "/tmp/p27_${tag}_node1.log" && break
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge 60 ]; then bad "($tag) cluster never reached FULL"; return; fi
    log "cluster FULL after ~${t}s"; sleep 2

    # FULL (degrade) only counts ALIVE peers; region_size() additionally needs
    # the RTT to each peer measured (<= tau). Declaring before region_size==3
    # caps target_r to 1 (the old flake). Wait until node1's region is size 3 so
    # target_r deterministically caps to 2. (The kernel also self-heals via an
    # upward re-cap of target_r when the region grows, but waiting here keeps
    # the demo's numbers crisp and the first SAFE transition lands at 2/2.)
    # region convergence needs the RTT to each peer measured (<= tau), which lags
    # the ALIVE -> FULL signal by several more SWIM rounds. Wait up to 60s
    # (120 * 0.5s) for node1's region to reach size=3 (was 20s — too tight on a
    # busy runner; the kernel self-heals via an upward re-cap, but waiting keeps
    # the first SAFE transition crisp).
    local r=0
    while [ $r -lt 120 ]; do
        printf '%s\n' "region" > "$FIFO.1"
        grep -aqE 'size=3' "/tmp/p27_${tag}_node1.log" && break
        sleep 0.5; r=$((r + 1))
    done
    if [ $r -ge 120 ]; then
        log "WARN: node1 region never reached size=3; relying on kernel re-cap"
    else
        log "node1 region converged to size=3 (target_r will cap to 2)"
    fi
    sleep 1
}

wait_for() {  # <file> <pattern> <secs>
    local f="$1" p="$2" s="$3" i=0
    while [ "$i" -lt "$((s * 4))" ]; do
        grep -aqE "$p" "$f" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

SECRET="the-soul-that-must-survive-node1"

# ===========================================================================
echo "==========================================================="
echo " (0) in-kernel property self-test: grounded threat + closed loop"
echo "==========================================================="
ST=/tmp/p27_selftest.log
printf 'protect test\nexit\n' | timeout 30 "$BOOT/p-kernel" > "$ST" 2>&1 || true
grep -E '\[protect-(ground|loop|test)\]' "$ST" | sed 's/^/    /'
grep -qE '\[protect-ground\] PASS' "$ST" && ok "threat is grounded in under-replication (monotone; 0 only at >=R)" \
    || bad "grounding self-test did not pass"
grep -qE '\[protect-loop\] PASS' "$ST"   && ok "closed loop: actuator replication drops threat; actuator-off stays at-risk" \
    || bad "closed-loop self-test did not pass"

# ===========================================================================
echo
echo "==========================================================="
echo " TREATMENT — actuator ON: replication drops the threat, unit survives"
echo "==========================================================="
cluster_up tr
L1=/tmp/p27_tr_node1.log
L2=/tmp/p27_tr_node2.log

send 1 "protect $SECRET"
log "waiting for the actuator to evacuate the unit to R neighbours ..."
# PRE-KILL replication-to-R wait. The actuator must announce -> WANT -> transfer
# the unit to R durable neighbours; that rides several SWIM/announce rounds and
# the peers' durable-store confirmations. The old 20s sometimes expired before
# replication reached R on a busy runner, so the post-kill window was moot (the
# REAL CI failure: "node1 did not report the unit SAFE" BEFORE the kill). Widen
# to 60s (~3x the realistic replication latency). The assertion is UNCHANGED:
# the unit must actually reach R replicas (the grounded threat must close to 0).
if wait_for "$L1" '\*\*\* SAFE' 60; then
    ok "node1's grounded threat fell to 0 BECAUSE the unit reached R replicas"
else
    bad "unit never reached R replicas (threat did not close) — see $L1"
fi
echo "----- node1 threat curve (grounded in replication, not a timer) -----"
grep -E '\[protect\] (DECLARE|replica confirmed|ACTUATE)' "$L1" | sed 's/^/    /'
echo "--------------------------------------------------------------------"
# the drop is caused by replication: >=R DISTINCT neighbours confirmed holding
# it. (Internal node ids are PKERNEL_NODE_ID-1, so node1 == internal 0 sees its
# two peers as "node1" and "node2"; we just count distinct confirming peers.)
NCONF=$(grep -aoE 'replica confirmed: node[0-9]+' "$L1" | sort -u | wc -l)
[ "${NCONF:-0}" -ge 2 ] \
    && ok "the drop is caused by replication: $NCONF distinct neighbours confirmed holding the unit" \
    || bad "did not observe >=2 distinct neighbours confirm the replica (got $NCONF)"
# `protect stat` is a one-shot console print; the SAFE transition is eventual,
# so a single grep races a slow-but-converging node. Re-issue `protect stat` and
# poll up to 60s for the threat=0/SAFE line (retry instead of a one-shot grep).
# The success predicate is UNCHANGED: node1 must report 'threat=0  SAFE'.
STAT_OK=0
for _ in $(seq 1 30); do
    send 1 "protect stat"; sleep 1
    if grep -qE 'threat=0  SAFE' "$L1"; then STAT_OK=1; break; fi
    sleep 1
done
[ "$STAT_OK" -eq 1 ] && ok "node1 reports the unit SAFE (threat=0)" \
    || bad "node1 did not report the unit SAFE"

log "*** kill -9 node1 (pid ${NODE_PID[1]}) — the owner of the protected unit dies ***"
kill -9 "${NODE_PID[1]}" 2>/dev/null; NODE_PID[1]=0; sleep 1

send 2 "pfs get $SECRET"
# Post-kill FAILOVER window. node2 already holds the replica, but its console
# can only serve `pfs get` once it stops blocking on the now-dead owner — i.e.
# after SWIM declares node1 DEAD. SWIM death-detection latency (swim.h):
#   SUSPECT after SWIM_SUSPECT_ROUNDS=2 + DEAD after SWIM_DEAD_ROUNDS=3 = 5
#   consecutive missed probes of the dead target; with a ~1s round period
#   (SWIM_PROBE_INTERVAL_MS=1000) plus the 400ms direct + 500ms indirect probe
#   timeouts per missed round, and round-robin probing 2 peers, that is
#   ~15-20s before node1 is DEAD. The old 10s window was below that floor and
#   flaked. Give it death_latency + a ~3x margin (and jitter headroom for the
#   shared self-hosted runner). The assertion is UNCHANGED: node2 must actually
#   serve the secret back from its own durable store.
if wait_for "$L2" "\[pfs\] get: $SECRET" 60; then
    ok "the protected unit SURVIVED on node2 and was served back from its durable store"
else
    bad "the unit was NOT served back from node2 after node1 died — see $L2"
fi
echo "----- node2 serves the survivor -----"
grep -E '\[pfs\] get' "$L2" | tail -2 | sed 's/^/    /'
echo "-------------------------------------"
cluster_down

# ===========================================================================
echo
echo "==========================================================="
echo " CONTROL — actuator OFF: threat stays high, unit is LOST on kill"
echo "==========================================================="
SECRET2="the-soul-with-no-protecting-power"
cluster_up ct PKERNEL_PROTECT_OFF=1
C1=/tmp/p27_ct_node1.log
C2=/tmp/p27_ct_node2.log

send 1 "protect $SECRET2"
log "waiting (actuator disabled — nothing should evacuate the unit) ..."
sleep 8
send 1 "protect stat"; sleep 1
echo "----- node1 protect stat (actuator OFF) -----"
grep -E '\[protect\] (actuator|DECLARE|protected units|  id=|grounded-threat)' "$C1" | tail -8 | sed 's/^/    /'
echo "---------------------------------------------"
if grep -qE '\*\*\* SAFE' "$C1"; then
    bad "control unexpectedly reached SAFE (actuator was supposed to be OFF)"
else
    ok "control: the unit never reached safety (no replica was driven out)"
fi
grep -qE 'AT-RISK' "$C1" && grep -qE 'replicas=0/' "$C1" \
    && ok "control: grounded threat stays pinned HIGH (replicas=0, AT-RISK)" \
    || bad "control: expected the unit to remain AT-RISK with replicas=0"

log "*** kill -9 node1 (pid ${NODE_PID[1]}) — owner dies with no power behind it ***"
kill -9 "${NODE_PID[1]}" 2>/dev/null; NODE_PID[1]=0; sleep 1

send 2 "pfs get $SECRET2"
sleep 3
if grep -qE "\[pfs\] get: $SECRET2" "$C2"; then
    bad "control: the unit unexpectedly survived (it should have been LOST)"
else
    ok "control: the unit was LOST — no neighbour held it (object dies with its owner)"
fi
echo "----- node2 has nothing to serve -----"
grep -E '\[pfs\] get' "$C2" | tail -2 | sed 's/^/    /'
echo "--------------------------------------"
cluster_down

# ===========================================================================
echo
if [ "$FAIL" -ne 0 ]; then
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/p27_*.log"
    echo "==========================================================="
    exit 1
fi
echo "==========================================================="
echo " RESULT: PASS — the threat is grounded and the loop is closed."
echo " A declared protected unit's threat falls to 0 BECAUSE the swarm"
echo " replicated it to R durable neighbours (not because a timer fired),"
echo " and the unit then survives the kill -9 of its owner. With the"
echo " protecting power disabled the threat stays high and the unit dies"
echo " with its owner. 守る単位と守る力の分離 — proven in numbers."
echo "==========================================================="
exit 0
