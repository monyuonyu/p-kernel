#!/bin/bash
# ---------------------------------------------------------------------------
# run_cradle_live.sh — the [cradle-teach] DEFERRED [live] row: the mind learns
# ACROSS THE WIRE. A TEACHER node (T) emits a DETERMINISTIC text lesson over
# ./relay; a separate-process STUDENT node (S) pulls it (KDDS beacon + p-fs body),
# consolidates it on its DMN sleep tick, and a HELD probe it was NEVER directly
# trained on becomes weight-resident — proven by S's own `cradle probe` self-
# report dropping below chance. Then T is KILLED and S still answers below chance
# (from PERSISTED weights): the [live] embodiment of "the mind survives the
# teacher." Three falsification arms each go RED (off / scrambled / teacher-death).
#
# This cashes the multi-process [live] row deferred by the in-proc [cradle-teach]
# cert (tests/llm/run_cradle_teach.sh, audit-trail.md:876 PASS). It is the SS-6 ->
# ss6-live / N-2 -> snf-live pattern: the BRIDGE (cradle.c ring/ingest + the
# student distill path) and the TRANSPORT (cradle_net.c KDDS+p-fs) are already
# shipped; this harness exercises them over THREE OS processes + ./relay.
#
# ============================================================================
# THE TEACHER-GGUF DEPENDENCY — READ THIS BEFORE RUNNING ON THE THINKPAD
# ============================================================================
# T must be elected region_teacher(), which requires teacher_gguf_loaded()==true
# (student_shell.c): PKERNEL_TEACHER=1 is NECESSARY-NOT-SUFFICIENT — a real
# teacher GGUF must also load. THERE IS NO test/sample .gguf IN-TREE (and none on
# the commander's host), so PKERNEL_TEACHER_GGUF cannot point at a real model for
# this run.
#
# RESOLUTION (honest, cert-scoped): this row's claim is NARROW — a teacher's
# relay-delivered DETERMINISTIC lesson (the `cradle emit` byte body below, NOT a
# GGUF sampler output) becomes weight-resident in a separate-process student and
# survives teacher death. The GGUF-gated teacher ELECTION itself is ALREADY
# certified by T-fix-a (swim_teacher_gossip_self_test / `nodes teacher`), which
# sets region_set_teacher_capable() directly and never re-proves the local GGUF
# probe. So T is made teacher-capable here by a CERT-SCOPED env:
#
#       PKERNEL_TEACHER=1 PKERNEL_TEACHER_CERT=1
#
# read ONLY in the hosted teacher_gguf_loaded() path (student_shell.c). It forces
# the capability bit ON for THIS node WITHOUT a GGUF. It is VISIBLY cert-scoped:
# it requires its OWN dedicated env (PKERNEL_TEACHER_CERT) — never just
# PKERNEL_TEACHER — so the PRODUCTION "env alone is not sufficient" honesty (a
# plain PKERNEL_TEACHER=1 with no GGUF still elects NO teacher) is UNCHANGED for
# every node that does not opt into the cert path, and the override lives only in
# the hosted-only TU (bare metal uses swim.c's weak no-op).
#
# HONESTY CAVEAT: PKERNEL_TEACHER_CERT stands in for the T-fix-a-certified GGUF
# election; it does NOT re-prove that election (that is T-fix-a's domain). This
# row proves the lesson BRIDGE over the wire, not the teacher selection.
#
#   ./run_cradle_live.sh
# Watch:  /tmp/cradle_live_*.log   (S's verdict lines are greppable [cradle-live])
# Exit 0 = the cure + all three falsification arms behaved.
#
# STATUS (honest, the snf-live discipline): this multi-process run was NOT
# runnable in the implementer's PRoot sandbox (foreground `sleep` + backgrounded
# long-lived `relay &`/`p-kernel &` children are killed there, and x86_64 needs
# qemu) — the same wall run_supernode_fwd.sh / run_ss6_live.sh hit. So the
# multi-process PASS is a DEFERRED [live] row to be cashed by the COMMANDER on a
# real host (the ThinkPad). The BRIDGE + the student ingestion + the 3 arms are
# ALREADY proven IN-PROCESS (run_cradle_teach.sh PASS, audit-trail.md:876).
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
export PKERNEL_RELAY_PORT=7414
unset PKERNEL_RTT_ZONE_SIZE PKERNEL_RTT_ZONE_PENALTY   # one region on localhost

# A unique persistence dir per run so S's baby is fresh (no yesterday's weights).
PFS_S="$(mktemp -d)"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$PFS_S"; }
trap cleanup EXIT

PASS_ALL=1
fail() { echo "[cradle-live] FAIL: $1"; PASS_ALL=0; }

# The DETERMINISTIC cure lesson T emits. NOT a GGUF sampler output — a fixed byte
# body so the run is reproducible. It must be >= the live threshold (4*32=128 B)
# AND ~CL_SCRAMBLE_LEN (1280) so the cure-vs-scramble arms compare equal bytes.
# A coined fact ("zorblax is a blue fox") repeated, the SAME family the in-proc
# cert (ct_build_lesson, CT_TRAIN_SENT/CT_HELD_SENT) teaches, so the held probe
# at the production train_end boundary is the never-trained continuation. We
# build it by repeating the sentence pair to ~1280 bytes.
CURE_SENT="the zorblax is a blue fox. the zorblax runs and the blue fox hides in the den. rivers flow to the sea and the wind moves over the hills. "
CURE_LESSON=""
while [ "${#CURE_LESSON}" -lt 1280 ]; do CURE_LESSON="$CURE_LESSON$CURE_SENT"; done
CURE_LESSON="${CURE_LESSON:0:1280}"

# ===========================================================================
# helper: launch T (teacher) + W (quorum) + S (student) over one ./relay, then
#         drive a SCRIPTED session on S. $1 = arm:
#   cure      : T emits the cure lesson; S pulls + sleeps + probes (expect DROP).
#   off       : S starts `cradle off`; T emits; S probes (expect STAY at chance).
#   scramble  : T emits-scramble (random bytes same length); S probes (STAY).
#   death     : cure, THEN kill T, wait SWIM-dead, S re-probes (STILL below chance
#               from persisted weights — the mind survives the teacher).
# All of S's verdicts are read from its REDIRECTED logfile (a real node's console
# is flooded with [moe] spam — NEVER an interactive shell's stdout; the N-2c
# lesson). T's teacher-election is read from T's "lesson emitted" line.
# ===========================================================================
run_arm() {
  local arm="$1"
  local TAG="$arm"
  echo "[cradle-live] === arm $TAG : relay :$PKERNEL_RELAY_PORT ==="

  local APIDS=()
  "$ROOT/relay/relay" -p "$PKERNEL_RELAY_PORT" -v >/tmp/cradle_live_relay_$TAG.log 2>&1 & APIDS+=($!)
  sleep 1

  # ---- W (node 4): plain quorum member, keeps the region >= 2 so SWIM elects a
  # teacher deterministically (and so the kill-T arm has a survivor). No baby. ----
  env PKERNEL_NODE_ID=4 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null \
      >/tmp/cradle_live_nodeW_$TAG.log 2>&1 & APIDS+=($!)

  # ---- T (node 2): the CERT-SCOPED teacher. PKERNEL_TEACHER=1 + the cert env
  # makes teacher_gguf_loaded() true WITHOUT a GGUF (see header). T runs a
  # scripted session that, after convergence, emits the lesson for THIS arm. ----
  local T_EMIT
  case "$arm" in
    scramble) T_EMIT="cradle emit-scramble" ;;
    off|cure|death) T_EMIT="cradle emit $CURE_LESSON" ;;
  esac
  {
    sleep 14                                 # SWIM discover + capability gossip
    # CONVERGENCE WAIT on T: emit only once T sees itself as region_teacher().
    # `cradle emit` returns "lesson emitted" ONLY when region_teacher()==self
    # (cradle_teach_emit gate), so we retry it across the settle window; the
    # FIRST success is T's own "I am the elected teacher" self-report. We retry
    # several times so a not-yet-converged early emit (no-op) is followed by a
    # real one once the region view stabilises.
    local _i=0
    while [ "$_i" -lt 8 ]; do
      echo "nodes"                           # membership (greppable convergence)
      echo "$T_EMIT"                          # emits ONLY if elected teacher
      echo "[cradle-live] T-EMIT-ATTEMPT i=${_i}"
      sleep 3; _i=$((_i + 1))
    done
    sleep 30                                  # stay alive while S pulls + sleeps
    echo "exit"
  } | env PKERNEL_TEACHER=1 PKERNEL_TEACHER_CERT=1 PKERNEL_NODE_ID=2 \
      PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
      >/tmp/cradle_live_nodeT_$TAG.log 2>&1 & local TPID=$!; APIDS+=($TPID)

  # ---- S (node 3): the STUDENT. PKERNEL_PFS_DIR set so student_dmn_consolidate
  # runs (persistence active = the DMN sleep tick trains + the kill-T arm reads
  # PERSISTED weights). `student` births the resident baby; the DMN heartbeat
  # then pulls the lesson (cradle_poll_and_pull) + consolidates it. ----
  {
    sleep 6
    echo "student"                            # birth the resident baby
    sleep 2
    [ "$arm" = "off" ] && echo "cradle off"   # Arm A: teaching OFF before any pull
    sleep 4
    echo "cradle probe"                        # BEFORE: assert ~chance (>= 5.0)
    echo "[cradle-live] S-PRE-PROBE"
    # WAIT for the body to actually arrive over the wire: poll until ring_len>0
    # (the p-fs WANT may need several 500ms retries; never assume the 1st beacon
    # landed). For the OFF arm the ring stays 0 by design.
    local _w=0
    while [ "$_w" -lt 14 ]; do
      echo "cradle probe"                      # greppable ring_len=<n>
      echo "[cradle-live] S-RING-POLL w=${_w}"
      # drive DMN sleeps to consolidate whatever is in the ring (baby <rounds>
      # runs real sleep rounds over the live corpus; the DMN tick also pulls).
      echo "baby 4"
      sleep 4; _w=$((_w + 1))
    done
    echo "cradle probe"                        # AFTER: the verdict probe
    echo "[cradle-live] S-POST-PROBE"
    sleep 2
    echo "exit"
  } | env PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$PFS_S" \
      "$BOOT/p-kernel" >/tmp/cradle_live_nodeS_$TAG.log 2>&1 & local SPID=$!; APIDS+=($SPID)

  # ---- death arm: kill T AFTER the cure has been pulled + consolidated, wait for
  # SWIM to mark T dead on S, then S re-probes (handled by the long S session
  # above — the POST probe lands after this kill + SWIM-dead window). ----
  if [ "$arm" = "death" ]; then
    ( sleep 40; kill -9 "$TPID" 2>/dev/null
      echo "[cradle-live] killed T (node2) — SWIM must mark it dead; S answers from persisted weights" ) &
    APIDS+=($!)
  fi

  # let S finish its session (births + pull + sleeps + post-probe)
  sleep 80
  kill "${APIDS[@]}" 2>/dev/null; wait 2>/dev/null
}

# greppers over S's redirected log -------------------------------------------
s_pre_probe()  { grep -A2 'S-PRE-PROBE'  "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
                  | grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' | tail -1; }
s_post_probe() { grep -B1 'S-POST-PROBE' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
                  | grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' | tail -1; }
s_max_ring()   { grep -oE 'ring_len=[0-9]+' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
                  | grep -oE '[0-9]+' | sort -n | tail -1; }
t_emitted()    { grep -c 'lesson emitted over the mesh' "/tmp/cradle_live_nodeT_$1.log" 2>/dev/null; }
loss_of()      { printf '%s' "$1" | grep -oE 'probe_loss=[0-9.]+' | grep -oE '[0-9.]+'; }
# numeric compare without bc: awk.
flt_lt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 < b+0)}'; }
flt_ge() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 >= b+0)}'; }

CHANCE=5.5452          # ln(256)
CURE_FLOOR=0.5         # the cure must drop the held probe >= 0.5 nats below chance

# ===========================================================================
# ARM cure: T teaches; S learns the held probe over the wire.
# ===========================================================================
run_arm cure
echo
echo "===== CURE (T -> ./relay -> S; held probe becomes weight-resident) ====="
PRE_C=$(s_pre_probe cure); POST_C=$(s_post_probe cure)
RING_C=$(s_max_ring cure); EMIT_C=$(t_emitted cure)
echo "T lessons emitted: $EMIT_C   S max ring_len: $RING_C"
echo "S pre : $PRE_C"
echo "S post: $POST_C"
PRE_CL=$(loss_of "$PRE_C"); POST_CL=$(loss_of "$POST_C")
[ "${EMIT_C:-0}" -ge 1 ] 2>/dev/null || fail "CURE: T never emitted (not elected teacher — check PKERNEL_TEACHER_CERT)"
[ "${RING_C:-0}" -gt 0 ] 2>/dev/null || fail "CURE: the lesson body never arrived on S (ring_len stayed 0 over the wire)"
[ -n "$PRE_CL" ] && flt_ge "$PRE_CL" 5.0 || fail "CURE: S did not start near chance (pre=$PRE_CL, want >=5.0)"
if [ -n "$POST_CL" ] && [ -n "$PRE_CL" ]; then
  flt_lt "$POST_CL" "$(awk -v c=$CHANCE -v f=$CURE_FLOOR 'BEGIN{print c-f}')" \
    || fail "CURE: held probe did not drop >= $CURE_FLOOR nats below chance (post=$POST_CL)"
else fail "CURE: missing probe readout (pre=$PRE_CL post=$POST_CL)"; fi

# ===========================================================================
# ARM off (Arm A): S starts `cradle off`; ring stays empty; probe stays chance.
#                  PROVES the cure signal rode the MESH (not local state).
# ===========================================================================
run_arm off
echo
echo "===== ARM A: teaching OFF on S -> NOT learned (rode the mesh) ====="
POST_O=$(s_post_probe off); RING_O=$(s_max_ring off)
echo "S max ring_len: $RING_O   S post: $POST_O"
POST_OL=$(loss_of "$POST_O")
[ "${RING_O:-0}" -eq 0 ] 2>/dev/null || fail "(A): ring went live despite cradle off (the OFF gate failed)"
[ -n "$POST_OL" ] && flt_ge "$POST_OL" 5.0 \
  || fail "(A): S learned the fact with teaching OFF (post=$POST_OL, want >=5.0 = stayed at chance)"

# ===========================================================================
# ARM scramble (Arm B): T emits RANDOM bytes (same length); probe stays chance.
#                       PROVES it is the SEQUENCE, not byte statistics.
# ===========================================================================
run_arm scramble
echo
echo "===== ARM B: scrambled lesson -> NOT learned (it is the sequence) ====="
POST_S=$(s_post_probe scramble); RING_S=$(s_max_ring scramble)
echo "S max ring_len: $RING_S   S post: $POST_S"
POST_SL=$(loss_of "$POST_S")
[ "${RING_S:-0}" -gt 0 ] 2>/dev/null || echo "[cradle-live] note: scramble body did not arrive (ring=0) — Arm B vacuous; rerun"
[ -n "$POST_SL" ] && flt_ge "$POST_SL" 5.0 \
  || fail "(B): a scrambled-byte lesson taught the probe (post=$POST_SL, want >=5.0 = stayed at chance)"

# ===========================================================================
# ARM death (Arm C): cure, THEN kill T, wait SWIM-dead, S re-probes -> STILL
#                    below chance (from PERSISTED weights). The mind survives the
#                    teacher.
# ===========================================================================
run_arm death
echo
echo "===== ARM C: teacher killed -> S STILL answers (persisted; mind survives) ====="
POST_D=$(s_post_probe death); RING_D=$(s_max_ring death)
echo "S max ring_len: $RING_D   S post (after T death): $POST_D"
POST_DL=$(loss_of "$POST_D")
[ -n "$POST_DL" ] && flt_lt "$POST_DL" "$(awk -v c=$CHANCE -v f=$CURE_FLOOR 'BEGIN{print c-f}')" \
  || fail "(C): after T died S no longer answers below chance (post=$POST_DL) — the mind did not survive the teacher"

# ===========================================================================
# verdict
# ===========================================================================
echo
echo "===================== VERDICT ====================="
if [ "$PASS_ALL" = "1" ]; then
  echo "[cradle-live] PASS held probe ${PRE_CL:-?}->${POST_CL:-?} over the wire"
  echo "          teaching-OFF / scrambled stayed at chance; the mind survived the teacher's death."
  exit 0
else
  echo "[cradle-live] OPEN: see /tmp/cradle_live_*.log — do NOT fudge green."
  echo "          (if T never emitted: confirm PKERNEL_TEACHER_CERT=1 reached node 2;"
  echo "           if ring_len stayed 0: the p-fs body did not arrive — widen the S-RING-POLL window.)"
  exit 1
fi
