#!/bin/bash
# ---------------------------------------------------------------------------
# run_supernode_fwd.sh — the N-2c [supernode-forward][live] cert: a packet
# from node-A to node-B FORWARDED BY the elected supernode node-S over ./relay
# (NOT the central relay's own fan-out), proven by a counter on S
# (forwarded>0) AND B receiving the byte-identical payload.
#
# N-2 shipped the deterministic SELECTOR region_supernode() (lowest-id region
# member that is supernode-capable); N-2b gossips that capability over SWIM.
# NEITHER moved a datagram. N-2c makes the elected supernode actually FORWARD:
# A wraps its payload as SNF_FWD to S, S re-forwards it as SNF_DELIVER to B,
# B receives it byte-identical. Fail-closed: no/unreachable supernode -> DIRECT
# to B (today's central-relay behavior).
#
# Topology (all on localhost = ONE region; SWIM gossips capability).
# NOTE the id mapping: PKERNEL_NODE_ID is the relay-wire id (1..255); the
# INTERNAL cluster id the code reasons about is (PKERNEL_NODE_ID - 1) — see
# arch/linux/x86_64/usermain.c "nid = mac[5]-1" and snf_node_ip(n)=10.1.0.(n+1).
# region_supernode(), snf_send(dst), my_supernode= ALL speak the INTERNAL id.
#   PKERNEL_NODE_ID 1 = A (internal 0, 10.1.0.1, sender, NOT supernode-capable)
#   PKERNEL_NODE_ID 2 = S (internal 1, 10.1.0.2, supernode: PKERNEL_SUPERNODE=1)
#   PKERNEL_NODE_ID 3 = B (internal 2, 10.1.0.3, destination, runs the cert SINK)
# So node A's region_supernode() elects INTERNAL id 1 (=S); `snf send 2` on A
# (2 = B's INTERNAL id) routes THROUGH internal-1; S's forwarded_count
# increments; B receives the byte-identical probe via_super=1.
# (The in-proc cert in region.c uses synthetic internal ids A=1,S=2,B=3; do NOT
#  confuse those with this LIVE run's PKERNEL_NODE_IDs — here the internal ids
#  are 0/1/2 because of the mac[5]-1 mapping. The audit's "off-by-mapping" bug.)
#
# Three arms in ONE harness:
#   MAIN       : S=2 capable -> A->B through S; S.forwarded>0, B byte-identical.
#   FALSIFIER a: NO node capable -> A->B DIRECT; S.forwarded==0, B still gets it.
#   FALSIFIER b: S capable but KILLED before the send -> A fails closed DIRECT;
#                no packet lost, B still receives byte-identical.
#
# Usage:  ./run_supernode_fwd.sh
# Watch:  /tmp/snf_*.log
# Exit 0 = all three arms behaved; exit 1 = a real divergence (reported honestly).
#
# STATUS (honest): this is the [supernode-forward][live] arm. The forwarding
# PLANE + its byte-identity + the two falsifiers are ALREADY proven IN-PROCESS,
# byte-identical cross-arch, by `region fwd` (supernode_forward_self_test, 20/20)
# — INCLUDING a "REAL production code path" sub-arm that drives the SHIPPED
# snf_rx/snf_forward/deliver functions + counters in-process (sabotage-tested
# RED). THIS multi-process run cashes the true 3-OS-process [live] proof and is
# meant for a REAL host. It was NOT runnable in the implementer's PRoot sandbox
# (foreground `sleep` + backgrounded long-lived `relay &`/`p-kernel &` children
# are killed there — the same wall the existing run_ss6_live.sh / run_4node_
# regions.sh [live] rows hit), so the multi-process PASS is a DEFERRED [live]
# row to be cashed on a real host, exactly the SS-6 -> SS-6-live pattern.
# ---------------------------------------------------------------------------
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
export PKERNEL_RELAY_PORT=7413
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY   # one region on localhost

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

PASS_ALL=1

# ===========================================================================
# helper: launch a 3-node mesh; $1 = "super" (node2 capable) | "flat" (none).
#         returns after the requester node1's scripted session completes.
#         $2 = extra action ("killS" -> kill node2 right before the send).
# ===========================================================================
run_arm() {
  local mode="$1" extra="${2:-}"
  local TAG="$mode${extra:+_$extra}"
  local SUPER2=""
  [ "$mode" = "super" ] && SUPER2="PKERNEL_SUPERNODE=1"
  # CONVERGENCE WAIT (the [live] bring-up fix): the 3-node localhost startup
  # flaps FULL->REDUCED->FULL while SWIM discovers peers + gossips capability.
  # The ORIGINAL harness probed ~11s in, BEFORE A had a stable region view /
  # capability gossip from S, so region_supernode() returned 0xFF at send time
  # (S.forwarded stayed 0). run_ss6_live.sh succeeds [live] because it probes
  # only AFTER stable convergence. We mirror that: A emits `region` + `snf stat`
  # in a polling loop and waits for a STABLE [snf-live] READY marker (degrade
  # FULL on A AND region size>=3 AND a non-0xFF elected supernode) before it
  # issues the send. PRESEND is the total pre-send settle budget the loop spans.
  # killS waits longer so SWIM marks the killed S DEAD on node1 (ALIVE->SUSPECT
  # 2 misses + SUSPECT->DEAD 3 rounds @1s probe -> ~10s) before the send, so the
  # send genuinely exercises the runtime fail-closed (dead supernode -> DIRECT).
  local PRESEND=22
  [ "$extra" = "killS" ] && PRESEND=34

  echo "[snf-live] === arm $TAG : relay :$PKERNEL_RELAY_PORT ==="
  local APIDS=()
  "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/snf_relay_$TAG.log 2>&1 & APIDS+=($!)
  sleep 1

  # node 2 (S) comes up as the (capable) supernode/responder.
  env $SUPER2 PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/snf_node2_$TAG.log 2>&1 & local N2=$!; APIDS+=($N2)
  # node 3 = destination: install the cert sink so it records the probe bytes.
  {
    sleep 9
    echo "snf sink"                       # arm the destination sink
    sleep $((PRESEND + 8))                 # stay alive past node1's send + recv
    echo "snf recv"                        # byte-identity verdict
    sleep 1
    echo "exit"
  } | env PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
      >/tmp/snf_node3_$TAG.log 2>&1 & APIDS+=($!)
  sleep 4

  # For the killS arm, kill node 2 (S) ~10s in — AFTER capability has gossiped
  # (so node1 once ELECTED S) but well before the send, so SWIM converges S=DEAD
  # on node1 and the send fails closed to DIRECT.
  if [ "$extra" = "killS" ]; then
    ( sleep 10; kill -9 "$N2" 2>/dev/null; echo "[snf-live] killed S (node2)" ) &
    APIDS+=($!)
  fi

  # node 1 = sender A. Piped DIRECTLY into the process (no intermediate file).
  # CONVERGENCE WAIT: a fixed pipe can't read its own log back, so instead of a
  # single 11s probe we SAMPLE `region`+`snf stat` repeatedly across the whole
  # PRESEND settle window (every 2s). This (a) spans past the FULL->REDUCED->FULL
  # startup flap so the LAST pre-send sample reflects a STABLE converged view,
  # and (b) lets the verdict grep the *last* `my_supernode=`/`size=`/`level=`
  # line — exactly the "probe only after stable convergence" discipline that
  # makes run_ss6_live.sh pass [live]. The send fires only AFTER the loop.
  {
    sleep 9                               # SWIM discovers nodes 2,3 + cap gossip
    _t=9                                  # (subshell-local; never leaks to run_arm)
    while [ "$_t" -lt "$PRESEND" ]; do
      echo "dist"                         # degrade level (FULL when 3+ alive)
      echo "region"                       # region membership + size
      echo "snf stat"                     # elected supernode (my_supernode=)
      echo "[snf-live] READY-SAMPLE t=${_t}s"   # greppable settle marker
      sleep 2; _t=$((_t + 2))
    done
    echo "snf send 2"                     # A -> B (B's INTERNAL id = 2); through
                                          # the elected supernode (internal 1) if
                                          # alive, else fail-closed DIRECT.
    sleep 3
    echo "snf stat"                       # via_super/direct counters after send
    sleep 1
    echo "exit"
  } | env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 \
      "$BOOT/p-kernel" >/tmp/snf_node1_$TAG.log 2>&1

  sleep 3   # let node 3 print its `snf recv`
  kill "${APIDS[@]}" 2>/dev/null; wait 2>/dev/null
}

# ---------------------------------------------------------------------------
# ARM MAIN: node 2 is the supernode; A->B must go THROUGH it.
# ---------------------------------------------------------------------------
run_arm super
echo
echo "===== MAIN (supernode internal-1 [=PKERNEL_NODE_ID 2 =S] forwards A->B) ====="
echo "--- node 1 (A) convergence settle samples (last = the stable view) ---"
grep -E '\[snf-live\] READY-SAMPLE' /tmp/snf_node1_super.log | tail -3
grep -iE 'level=|\[region\].*size=' /tmp/snf_node1_super.log | tail -3
echo "--- node 1 (A) region + send ---"
grep -iE 'supernode-fwd|\[region\]' /tmp/snf_node1_super.log | grep -iE 'supernode-fwd|coordinator'
echo "--- node 2 (S) forwarder counters ---"
grep -iE 'supernode-fwd. bound port' /tmp/snf_node2_super.log | head -1   # SNF_PORT (7377; was 7380==PMESH, the live bug)
S_FWD_MAIN=$(printf '%s' "$(grep -oE 'forwarded=[0-9]+' /tmp/snf_node2_super.log | tail -1)")
echo "--- node 3 (B) received ---"
grep -iE 'supernode-fwd. recv' /tmp/snf_node3_super.log | tail -1

# READINESS (greppable for the commander): the LAST pre-send region size + the
# LAST elected supernode A saw. If size<3 or elected=none here, the [live] run
# probed before convergence -> the FAIL is a harness-timing issue, not the code.
RDY_SIZE_MAIN=$(grep -oE '\[region\][^\r]*size=[0-9]+' /tmp/snf_node1_super.log | grep -oE 'size=[0-9]+' | tail -1)
echo "[snf-live] READINESS (MAIN): last pre-send $RDY_SIZE_MAIN  (want size=3 = A+S+B converged)"

# node 1 emits a `snf stat` -> reads node 2's forwarded via the wire? No: each
# node's `forwarded` is LOCAL. We read S's counter from node 2's own log. But
# node 2 runs no shell (</dev/null), so it prints no `snf stat`. Instead we
# prove S forwarded by node 1's via_super_cnt (it SENT through S) AND node 3's
# via_super=1 (it RECEIVED a forwarded packet) — the forward is the only way
# a via_super delivery can occur. We ALSO assert node1 elected the supernode at
# its INTERNAL id (=1; see id-mapping note at the top — NOT the PKERNEL_NODE_ID).
ELECT_MAIN=$(grep -oE 'my_supernode=[0-9]+' /tmp/snf_node1_super.log | tail -1)
VIA_MAIN=$(grep -oE 'via_super_cnt=[0-9]+' /tmp/snf_node1_super.log | tail -1)
DIR_MAIN=$(grep -oE 'direct_cnt=[0-9]+'    /tmp/snf_node1_super.log | tail -1)
B_VIA_MAIN=$(grep -oE 'via_super=[0-9]+' /tmp/snf_node3_super.log | tail -1)
B_PAY_MAIN=$(grep -oE 'payload=(BYTE-IDENTICAL|MISMATCH)' /tmp/snf_node3_super.log | tail -1)
echo "elected: $ELECT_MAIN   node1 $VIA_MAIN $DIR_MAIN   node3 $B_VIA_MAIN $B_PAY_MAIN"
echo

# ---------------------------------------------------------------------------
# ARM FALSIFIER a: nobody capable -> A->B DIRECT; S forwards 0; B still gets it.
# ---------------------------------------------------------------------------
run_arm flat
echo "===== FALSIFIER (a): no supernode -> DIRECT ====="
ELECT_A=$(grep -oE 'my_supernode=(none\(0xFF\)|[0-9]+)' /tmp/snf_node1_flat.log | tail -1)
VIA_A=$(grep -oE 'via_super_cnt=[0-9]+' /tmp/snf_node1_flat.log | tail -1)
DIR_A=$(grep -oE 'direct_cnt=[0-9]+'    /tmp/snf_node1_flat.log | tail -1)
S_FWD_A=$(grep -oE 'forwarded=[0-9]+' /tmp/snf_node2_flat.log | tail -1)
B_VIA_A=$(grep -oE 'via_super=[0-9]+' /tmp/snf_node3_flat.log | tail -1)
B_PAY_A=$(grep -oE 'payload=(BYTE-IDENTICAL|MISMATCH)' /tmp/snf_node3_flat.log | tail -1)
echo "elected: $ELECT_A   node1 $VIA_A $DIR_A   node2 $S_FWD_A   node3 $B_VIA_A $B_PAY_A"
echo

# ---------------------------------------------------------------------------
# ARM FALSIFIER b: S capable but KILLED before the send -> fail-closed DIRECT.
# ---------------------------------------------------------------------------
run_arm super killS
echo "===== FALSIFIER (b): elected S killed -> fail-closed DIRECT, no loss ====="
ELECT_B=$(grep -oE 'my_supernode=(none\(0xFF\)|[0-9]+)' /tmp/snf_node1_super_killS.log | tail -1)
VIA_B=$(grep -oE 'via_super_cnt=[0-9]+' /tmp/snf_node1_super_killS.log | tail -1)
DIR_B=$(grep -oE 'direct_cnt=[0-9]+'    /tmp/snf_node1_super_killS.log | tail -1)
B_PAY_B=$(grep -oE 'payload=(BYTE-IDENTICAL|MISMATCH)' /tmp/snf_node3_super_killS.log | tail -1)
echo "elected: $ELECT_B   node1 $VIA_B $DIR_B   node3 $B_PAY_B"
echo

# ===========================================================================
# verdict
# ===========================================================================
echo "===================== VERDICT ====================="
fail() { echo "[snf-live] FAIL: $1"; PASS_ALL=0; }

# MAIN: node1 elected the supernode (INTERNAL id 1 = S = PKERNEL_NODE_ID 2);
# sent via_super>=1; node3 received via_super=1 + byte-identical. (The forward
# THROUGH S is the only path that produces a via_super delivery; a direct send
# sets relayed_by=0xFF -> via_super=0.)
# OFF-BY-MAPPING FIX (audit, 2026-06-21): the code reports my_supernode at the
# INTERNAL 0-indexed id (nid = PKERNEL_NODE_ID-1; see usermain.c "mac[5]-1" and
# snf_node_ip(n)=10.1.0.(n+1)). S is launched as PKERNEL_NODE_ID=2 -> internal
# id 1 -> the CORRECT reported value is my_supernode=1. The old harness asserted
# =2 (the wire id) and so falsely FAILED a correct election. Assert =1.
[ "$ELECT_MAIN" = "my_supernode=1" ] || fail "MAIN: node1 did not elect S (internal id 1) ($ELECT_MAIN)"
case "$VIA_MAIN" in via_super_cnt=0|"") fail "MAIN: node1 did not send via supernode ($VIA_MAIN)";; esac
[ "$B_VIA_MAIN" = "via_super=1" ] || fail "MAIN: node3 did not receive a FORWARDED packet ($B_VIA_MAIN)"
[ "$B_PAY_MAIN" = "payload=BYTE-IDENTICAL" ] || fail "MAIN: node3 payload not byte-identical ($B_PAY_MAIN)"

# FALSIFIER a: no supernode -> node1 elected none; sent DIRECT; B got it direct.
[ "$ELECT_A" = "my_supernode=none(0xFF)" ] || fail "(a): node1 elected a supernode when none capable ($ELECT_A)"
[ "$DIR_A" != "direct_cnt=0" ] && [ -n "$DIR_A" ] || fail "(a): node1 did not send DIRECT ($DIR_A)"
[ "$B_VIA_A" = "via_super=0" ] || fail "(a): node3 saw it as forwarded, not direct ($B_VIA_A)"
[ "$B_PAY_A" = "payload=BYTE-IDENTICAL" ] || fail "(a): node3 payload not byte-identical ($B_PAY_A)"

# FALSIFIER b: S elected but killed -> node1 falls back DIRECT; B still gets it.
[ "$B_PAY_B" = "payload=BYTE-IDENTICAL" ] || fail "(b): packet LOST or corrupted after S died ($B_PAY_B)"
case "$DIR_B" in direct_cnt=0|"") fail "(b): node1 did not fall back to DIRECT after S died ($DIR_B)";; esac

if [ "$PASS_ALL" = "1" ]; then
  echo "[snf-live] PASS  A->B forwarded BY the elected supernode (byte-identical);"
  echo "          no-supernode -> DIRECT; killed-supernode -> fail-closed DIRECT, no loss."
  exit 0
else
  echo "[snf-live] OPEN: see /tmp/snf_*.log — do NOT fudge green."
  exit 1
fi
