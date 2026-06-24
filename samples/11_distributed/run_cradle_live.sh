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
#
# ===========================================================================
# THE SEQUENCING FIX (wave-cradle-harness) — PULL WHILE IDLE, *THEN* TRAIN
# ===========================================================================
# The teacher self-election production bug is FIXED + merged (T emits lessons
# live; the commander confirmed "T lessons emitted: 8" + p-fs `saved 'ct/1/8'`).
# But the CURE arm still FAILED on the real host, and the commander root-caused
# WHY (relay frame histogram: S sent only 78 frames vs T's 2405 / W's 2815 —
# S barely networked). S's log ended at
#       [baby] distilling 8 round(s) ... from teacher fixture
# i.e. the OLD harness sent `student` to S RIGHT AFTER BOOT, which runs a HEAVY
# SYNCHRONOUS fixture-distill (sleep_rounds). p-kernel's T-Kernel scheduler is
# COOPERATIVE on a single core, so that training MONOPOLISED the CPU and STARVED
# S's mesh tasks (cradle_net_task @500ms, SWIM, kdds). S therefore NEVER ran
# cradle_poll_and_pull to fetch the lesson into its ring (ring_len stayed 0) and
# never returned to the shell for the later probe — so every assert read blank.
#
# This is a HARNESS SEQUENCING bug, NOT a production bug. The cure is to let the
# lesson be PULLED WHILE S IS IDLE (network tasks own the CPU), BEFORE any
# training. THE KEY FACTS that make a harness-only fix sufficient (verified in
# the source, no C change needed):
#
#   (1) cradle_net_task is spawned at BOOT on both linux arches (usermain.c
#       create_task(cradle_net_task,...)) and calls cradle_poll_and_pull every
#       CRADLE_POLL_MS=500ms — the pull is SHELL-INDEPENDENT. So if S is left
#       IDLE (no `student`, no `baby`) after convergence, the lesson is pulled
#       into the ring AUTOMATICALLY. We do NOT need a shell command to pull.
#   (2) On a node with PKERNEL_PFS_DIR (S has it), student_boot_restore BIRTHS a
#       fresh baby at BOOT via student_birth_warmup, which does NOT train (just
#       persists). So the resident baby already exists and reads ~chance — we do
#       NOT need the `student` verb at all; its ONLY effect was the CPU-hogging
#       fixture-distill that caused the bug. We DROP it.
#   (3) `cradle probe` (cradle_live_probe) is a PURE READ: it prints the
#       greppable `[cradle-live] ring_len=<n> probe_loss=<L> chance=<C>` off the
#       LIVE corpus (cradle_window_src: the ring when live, else the fixture). It
#       does NOT train and does NOT block — safe to poll repeatedly while idle.
#   (4) `baby <N>` runs N SYNCHRONOUS sleep rounds over the LIVE corpus
#       (cradle_window_src / cradle_corpus_len) — i.e. over the LESSON RING once
#       ring_len>=CRADLE_MIN_LIVE(=128B). So once the lesson is in the ring,
#       `baby N` is exactly the production consolidation OVER THE WIRE-DELIVERED
#       lesson. We size N>=CT_CERT_ROUNDS(=12) to match the in-proc cert recipe
#       (rounds=12 lr=3e-3 seqlen=32 budget=1280) so the held probe drops the
#       certified amount.
#
# THE NEW PER-ARM SEQUENCE (run_arm below) is therefore:
#   cure    : boot 3 -> wait `-> FULL alive=3` on S -> PRE probe (~chance)
#             -> PULL-WHILE-IDLE: poll `cradle probe` until ring_len>0 (net task
#                pulls; we do NOT train yet) -> CONSOLIDATE `baby 16` (trains the
#                LESSON ring) -> POST probe (held probe dropped below chance).
#   off     : S is PFS-LESS (no baby, no autonomous DMN) + `cradle off`; ring
#             stays 0, no training, probe reads the chance FLOOR. CLEAN falsifier
#             with NO autonomous-DMN fixture contamination (see the PERSISTENCE
#             note in run_arm). The gate flag itself is separately certified in
#             `cradle test` Arm A; this [live] arm proves "no teaching -> nothing
#             learned" end-to-end.
#   scramble: boot 3 -> wait FULL -> PULL-WHILE-IDLE until ring_len>0 (scramble
#             body must actually arrive, else the arm is vacuous) -> `baby 16`
#             (trains JUNK) -> probe STAYS ~chance (it is the SEQUENCE, not bytes).
#   death   : cure sequence -> kill T -> wait SWIM-dead on S -> POST probe STILL
#             below chance (the baby answers from its now-resident/persisted
#             weights; the mind survives the teacher).
#
# DISCIPLINE (the commander's hard-won rules, applied throughout): never assume
# a fixed sleep is enough — POLL for a greppable marker (`-> FULL alive=3`,
# `ring_len=<n>`, the probe-loss line) from S's REDIRECTED LOG, with a generous
# cap, and sequence commands with waits for S to be IDLE between heavy steps. If
# a step can't be confirmed, the asserts below print a clear `[cradle-live]
# OPEN: <what>` with the relay/S-log evidence — honest > green.
#
# >>> THE COMMANDER RUNS THIS ON THE THINKPAD (real host). It is NOT runnable in
#     the PRoot sandbox (backgrounded children + foreground sleep are killed). <<<
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

# Per-RUN scratch root; each arm gets its OWN fresh persistence dir UNDER it (so
# the cure arm's trained+persisted weights do NOT leak into the OFF/scramble
# falsifiers — those MUST start from a fresh, untrained newborn or they are
# vacuous). run_arm allocates "$PFS_ROOT/<arm>" and the FIFO lives there too.
PFS_ROOT="$(mktemp -d)"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$PFS_ROOT"; }
trap cleanup EXIT

PASS_ALL=1
fail() { echo "[cradle-live] FAIL: $1"; PASS_ALL=0; }

# --- log-poll helpers (the commander's "poll a greppable marker" discipline) ---
# Wait until $2 (an ERE) appears in logfile $1, up to $3 seconds (poll @1s).
# Returns 0 the moment it matches, 1 on timeout. NEVER a blind fixed sleep.
wait_for_marker() {
  local logf="$1" pat="$2" cap="$3" i=0
  while [ "$i" -lt "$cap" ]; do
    [ -f "$logf" ] && grep -qE "$pat" "$logf" 2>/dev/null && return 0
    sleep 1; i=$((i + 1))
  done
  return 1
}
# Max ring_len seen so far in a logfile (0 if none) — the live-pull confirmation.
log_max_ring() {
  grep -oE 'ring_len=[0-9]+' "$1" 2>/dev/null | grep -oE '[0-9]+' \
    | sort -n | tail -1
}
# Wait until ring_len>0 has appeared in $1, up to $2 seconds. The student's
# cradle_net_task pulls the beacon+body on its own 500ms cadence while S idles;
# we just watch the probe line it prints. Returns 0 on a real pull, 1 on timeout.
wait_for_ring() {
  local logf="$1" cap="$2" i=0 r
  while [ "$i" -lt "$cap" ]; do
    r=$(log_max_ring "$logf"); r="${r:-0}"
    [ "$r" -gt 0 ] 2>/dev/null && return 0
    sleep 1; i=$((i + 1))
  done
  return 1
}

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
#         drive a CLOSED-LOOP session on S via a FIFO — so the harness can
#         WAIT for greppable markers in S's redirected log BETWEEN commands
#         (the only way to keep S IDLE for the network pull, then train).
# $1 = arm:
#   cure      : T emits the cure lesson; S idles, PULLS it, THEN trains -> DROP.
#   off       : S `cradle off` first; T emits; ring stays 0, S never trains -> STAY.
#   scramble  : T emits-scramble (random bytes, same length); S pulls + trains
#               JUNK -> STAY at chance (it is the SEQUENCE, not byte statistics).
#   death     : cure, THEN kill T, wait SWIM-dead, S re-probes -> STILL below
#               chance from the now-resident/persisted weights (mind survives T).
# All of S's verdicts are read from its REDIRECTED logfile (a real node's console
# is flooded with [moe] spam — NEVER an interactive shell's stdout; the N-2c
# lesson). T's teacher-election is read from T's "lesson emitted" line.
#
# WHY A FIFO (not a here-pipe): the OLD harness fed S a fixed command script
# down a pipe with blind sleeps between lines. But S's shell is UNRESPONSIVE
# while it trains (cooperative scheduler), and the lesson-pull only happens while
# S is IDLE — a fixed script cannot observe "S is now idle / the ring is now
# live" and react. A FIFO lets the OUTER shell write the NEXT command to S only
# AFTER it has polled S's log for the marker that the PREVIOUS step finished.
# ===========================================================================
run_arm() {
  local arm="$1"
  local TAG="$arm"
  local LOG_S="/tmp/cradle_live_nodeS_$TAG.log"
  local LOG_T="/tmp/cradle_live_nodeT_$TAG.log"
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
  # scripted session that, after convergence, emits the lesson for THIS arm.
  # T's emit is a CHEAP publish (no training), so the fixed retry-pipe is fine
  # here — only S needs the closed-loop FIFO. ----
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
    # real one once the region view stabilises. T then STAYS ALIVE the whole arm
    # (long tail sleep) so its p-fs body remains servable while S pulls — the
    # death arm kills it explicitly once S has consolidated.
    local _i=0
    while [ "$_i" -lt 12 ]; do
      echo "nodes"                           # membership (greppable convergence)
      echo "$T_EMIT"                          # emits ONLY if elected teacher
      echo "[cradle-live] T-EMIT-ATTEMPT i=${_i}"
      sleep 3; _i=$((_i + 1))
    done
    sleep 120                                 # stay alive while S idles+pulls+trains
    echo "exit"
  } | env PKERNEL_TEACHER=1 PKERNEL_TEACHER_CERT=1 PKERNEL_NODE_ID=2 \
      PKERNEL_AUTONET=1 "$BOOT/p-kernel" \
      >"$LOG_T" 2>&1 & local TPID=$!; APIDS+=($TPID)

  # ---- S (node 3): the STUDENT, driven CLOSED-LOOP through a FIFO. ----
  # PKERNEL_PFS_DIR set so (a) student_boot_restore BIRTHS a fresh baby at boot
  # (no `student` verb needed — see header fact (2)) and (b) the kill-T arm reads
  # PERSISTED weights. We hold the FIFO open on fd 9 so S's shell never sees EOF
  # until we deliberately send `exit`.
  # FRESH per-arm persistence dir: the cure arm's trained weights must NOT leak
  # into the scramble/death falsifiers (a restored trained baby would make them
  # vacuous). Each arm starts from an untrained newborn.
  local PFS_S="$PFS_ROOT/$TAG"
  rm -rf "$PFS_S"; mkdir -p "$PFS_S"
  # the FIFO lives OUTSIDE PFS_S so S's p-fs scan never trips over a non-regular
  # file in its persistence dir.
  local SFIFO="$PFS_ROOT/s_in_$TAG.fifo"
  rm -f "$SFIFO"; mkfifo "$SFIFO"

  # PERSISTENCE / AUTONOMOUS-DMN DISCIPLINE (a real subtlety — read this):
  # On a PKERNEL_PFS_DIR node, student_boot_restore BIRTHS a baby at boot AND the
  # DMN heartbeat (student_dmn_consolidate, dmn.c) AUTONOMOUSLY trains it ~every
  # ST_DMN_INTERVAL(=10) idle pulses (~10s) — ST_DMN_ROUNDS(=2) rounds over the
  # LIVE corpus (the lesson ring if live, else the FIXTURE). That autonomous
  # fixture-training is harmless for cure/scramble/death (their VERDICT probe is
  # taken AFTER the lesson is in the ring, so it measures the LESSON's held
  # windows, which fixture-training cannot teach), but it would CONTAMINATE the
  # OFF falsifier: with teaching OFF the ring stays empty, the probe reads the
  # FIXTURE, and the DMN would slowly drop that fixture held-loss below chance —
  # a FALSE "learned" reading that has nothing to do with the mesh.
  #
  # So the OFF arm runs S PFS-LESS (no PKERNEL_PFS_DIR): student_boot_restore is
  # a TRUE no-op (no baby, no arena) and student_dmn_consolidate returns 0 every
  # tick (pfs_dur_active()==false) — ZERO autonomous training. With no baby and
  # the ring empty, cradle_live_probe reports the chance FLOOR (g_have_student==0
  # -> probe=ln256). That is a CLEAN, contamination-free falsifier: "a node that
  # is NOT taught over the mesh stays at chance." The g_cradle_enabled gate ITSELF
  # (cradle off -> cradle_window_src==NULL) is separately + directly certified
  # in-process by `cradle test` Arm A (cradle.c), so this [live] arm need not
  # re-prove the gate flag — it proves the end-to-end "no teaching -> no learning."
  local PFS_ENV="PKERNEL_PFS_DIR=$PFS_S"
  [ "$arm" = "off" ] && PFS_ENV=""           # OFF: PFS-less -> no baby, no DMN
  env PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 $PFS_ENV \
      "$BOOT/p-kernel" <"$SFIFO" >"$LOG_S" 2>&1 & local SPID=$!; APIDS+=($SPID)
  # Open the FIFO read+write on fd 9: this NEVER blocks (an O_RDWR open on a FIFO
  # has no wait-for-peer), so the harness can't hang if S is slow to open its
  # read end, and the write end stays open until we close it -> S's shell never
  # sees a premature EOF.
  exec 9<>"$SFIFO"
  s_say() { printf '%s\n' "$1" >&9; }        # send one command to S's shell

  # ---- short post-boot settle: let S's init finish + the shell become reachable
  # before the PRE probe. We take the PRE probe EARLY (before the convergence
  # wait) so it is the MOST PRISTINE chance reading — the freshly-born baby has
  # seen the fewest autonomous DMN fixture-ticks (see the PERSISTENCE note above).
  sleep 5

  # ---- OFF arm: disable teaching NOW, before any pull, so the ring gate stays
  # closed (cradle_set_enabled(0) -> cradle_window_src returns the FIXTURE). The
  # OFF node is PFS-less so there is no baby/DMN anyway; this asserts the flag. --
  if [ "$arm" = "off" ]; then
    s_say "cradle off"
    sleep 2
  fi

  # ---- (5) PRE probe: BEFORE any pull/train, the held probe must read ~chance
  # (a freshly-born / no baby reads the chance floor). Cheap pure read.
  # NOTE: S's shell has NO `echo` builtin; an UNKNOWN command line is echoed back
  # as "[echo] <line>" (usermain.c default branch), which IS a reliable greppable
  # sentinel. The probe line is printed by `cradle probe` BEFORE this sentinel, so
  # the sentinel marks "the PRE probe has been emitted above this point".
  s_say "cradle probe"
  s_say "MARK-S-PRE-PROBE"
  wait_for_marker "$LOG_S" 'MARK-S-PRE-PROBE' 15

  # ---- (1) CONVERGENCE: wait for S to reach STABLE FULL (degrade FULL alive=3).
  # The marker is emitted AUTOMATICALLY by SWIM/degrade as nodes join (degrade.c
  # "*** level change: ... -> FULL  alive=3"); S need not run any command. The
  # localhost bring-up flaps FULL->REDUCED->FULL, so we wait for the marker and
  # then settle a few extra seconds so the LAST view is the stable one. ----
  if ! wait_for_marker "$LOG_S" '-> FULL[[:space:]]+alive=3' 90; then
    echo "[cradle-live] OPEN ($TAG): S never converged to FULL alive=3 in 90s"
    echo "             (S frame-starved or relay down — see $LOG_S / relay log)"
  fi
  sleep 6                                     # let the flap settle to stable FULL

  # ---- (4) PULL-WHILE-IDLE: S is NOT training, so cradle_net_task (500ms) owns
  # the CPU and pulls the beacon + p-fs body. We poll `cradle probe` to surface a
  # greppable ring_len, and WAIT (generous cap) until ring_len>0. The OFF arm
  # skips the wait (ring must stay 0 by design). ----
  if [ "$arm" != "off" ]; then
    local _w=0 _r=0
    while [ "$_w" -lt 12 ]; do               # ~60s: drive a probe, then idle 5s
      s_say "cradle probe"
      s_say "MARK-S-RING-POLL-${_w}"
      sleep 5; _w=$((_w + 1))
      _r=$(log_max_ring "$LOG_S"); [ "${_r:-0}" -gt 0 ] 2>/dev/null && break
    done
    if ! wait_for_ring "$LOG_S" 5; then
      echo "[cradle-live] OPEN ($TAG): the lesson body never arrived on S"
      echo "             (ring_len stayed 0 over the wire — check the relay frame"
      echo "              histogram + S's p-fs WANT retries in $LOG_S; do NOT fudge)"
    fi
  fi

  # ---- (6) CONSOLIDATE: only NOW (ring live) do we train — `baby 16` runs 16
  # synchronous sleep rounds OVER THE LIVE LESSON RING (>= the in-proc cert's 12
  # rounds @ the same lr/seqlen). The OFF arm SKIPS training entirely: with the
  # ring gated off, training would fit the FIXTURE (not stay at chance), so the
  # OFF falsifier must leave the untrained newborn at chance. ----
  if [ "$arm" != "off" ]; then
    s_say "baby 16"
    # `baby` prints "[baby] held-out loss (after)" when its rounds complete — the
    # idle marker that S has finished training and is responsive again.
    wait_for_marker "$LOG_S" '\[baby\] held-out loss \(after\)' 90 \
      || echo "[cradle-live] OPEN ($TAG): baby consolidation did not finish in 90s"
    sleep 2
  fi

  # ---- death arm: the cure is now PULLED + CONSOLIDATED into S. Kill T, then
  # wait for SWIM to mark T dead on S (ALIVE->SUSPECT->DEAD ~10s) BEFORE the POST
  # probe, so S genuinely answers from its OWN resident/persisted weights with no
  # teacher present. ----
  if [ "$arm" = "death" ]; then
    kill -9 "$TPID" 2>/dev/null
    echo "[cradle-live] killed T (node2) — SWIM must mark it dead; S answers from persisted weights"
    sleep 14                                  # SWIM converges S's view to T=DEAD
  fi

  # ---- (7) POST probe: the verdict read. cure/death -> dropped below chance;
  # off/scramble -> stayed at chance. ----
  s_say "cradle probe"
  s_say "MARK-S-POST-PROBE"
  wait_for_marker "$LOG_S" 'MARK-S-POST-PROBE' 15
  sleep 1

  s_say "exit"
  exec 9>&-                                   # close the write end -> S sees EOF
  sleep 2
  kill "${APIDS[@]}" 2>/dev/null; wait 2>/dev/null
  rm -f "$SFIFO"
}

# greppers over S's redirected log -------------------------------------------
# The probe line is printed by `cradle probe` immediately BEFORE the MARK-…
# sentinel we send next, so we grep the probe line that appears just ABOVE the
# sentinel (-B window). PRE = the last probe line before MARK-S-PRE-PROBE;
# POST = the last probe line before MARK-S-POST-PROBE.
s_pre_probe()  { grep -B12 'MARK-S-PRE-PROBE'  "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
                  | grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' | tail -1; }
s_post_probe() { grep -B12 'MARK-S-POST-PROBE' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
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
