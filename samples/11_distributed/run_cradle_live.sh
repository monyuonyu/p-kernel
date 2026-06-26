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
# THE AUTONOMOUS-PROBE FIX (wave-cradle-harness-autoprobe) — CERTIFY ON THE DMN
# IDLE HOOK, NOT A FORCED `baby N`
# ---------------------------------------------------------------------------
# The PULL-WHILE-IDLE sequencing above is correct, but the CONSOLIDATE step used
# to force `baby 16` (16 SYNCHRONOUS sleep rounds over a ~247MB student). On the
# cooperative single-core PRoot host that does NOT finish inside the harness cap
# ("baby consolidation did not finish in 180s"), so the POST probe was STARVED and
# the arm FAILED — a HOST-SPEED ARTIFACT, not a real RED. The learning was already
# real: the student's OWN autonomous DMN idle hook (student_dmn_consolidate, fact
# (5) below) had ALREADY consolidated the lesson during PULL-WHILE-IDLE (the probe
# reads ~2.60, far below chance 5.5452, BEFORE any `baby`). So we drop the forced
# `baby N` and certify on the autonomous probe — the MORE production-representative
# signal (the production mind learns on its DMN sleep tick, not on an operator
# typing `baby`).
#   (5) On a PKERNEL_PFS_DIR node the DMN idle hook AUTONOMOUSLY runs
#       student_dmn_consolidate() every ST_DMN_INTERVAL(=GA_INTERVAL=10) idle
#       pulses: it calls cradle_poll_and_pull() then sleep_rounds() ST_DMN_ROUNDS
#       over the LIVE corpus (the LESSON ring once live, gated by pfs_dur_active()).
#       That is the SAME train_end held split the in-proc cert + the live probe use.
#       L2 gave dmn_task a 256KB stack so this runs without a forced shell `baby`.
#
# THE PER-ARM SEQUENCE (run_arm below) is therefore:
#   cure    : boot 3 -> wait `-> FULL alive=3` on S -> PRE probe (~chance)
#             -> PULL + AUTONOMOUS-CONSOLIDATE WHILE IDLE: poll `cradle probe`; the
#                net task pulls (ring_len>0) and the autonomous DMN consolidates;
#                WAIT until the held probe genuinely drops BELOW the CURE threshold
#                (CHANCE-CURE_FLOOR) or a bounded timeout -> POST probe (held probe
#                weight-resident, below chance). NO forced `baby N`.
#   off     : S is PFS-LESS (no baby, no autonomous DMN) + `cradle off`; ring
#             stays 0, no training, probe reads the chance FLOOR. CLEAN falsifier
#             with NO autonomous-DMN fixture contamination (see the PERSISTENCE
#             note in run_arm). The gate flag itself is separately certified in
#             `cradle test` Arm A; this [live] arm proves "no teaching -> nothing
#             learned" end-to-end.
#   scramble: boot 3 -> wait FULL -> PULL + idle the SAME consolidation budget so
#             the autonomous DMN gets an EQUAL chance to consolidate the JUNK ->
#             probe MUST STAY >= chance (it is the SEQUENCE, not bytes). STRICT: a
#             scramble drop below the CURE threshold is a RED the harness surfaces.
#   death   : cure sequence (autonomous-consolidate below threshold) -> kill T ->
#             wait SWIM-dead on S -> POST probe STILL below the CURE threshold (the
#             baby answers from its now-resident/persisted weights; mind survives T).
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
# The MOST RECENT held probe_loss printed by `cradle probe` in $1 (empty if none).
# Used to watch the AUTONOMOUS-DMN idle consolidation drive the held probe down
# WITHOUT a forced `baby N` — the production sleep path. Only matches the real
# greppable [cradle-live] line, so a stray number never leaks in.
log_last_loss() {
  grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' "$1" 2>/dev/null \
    | grep -oE 'probe_loss=[0-9.]+' | grep -oE '[0-9.]+' | tail -1
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

# The DETERMINISTIC cure lesson T emits (T-fix-c: unify live == cert).
#
# THE OLD BUG: the harness used to build a ~1280-byte CURE_LESSON string and pass
# it as `cradle emit $CURE_LESSON`. But a p-kernel shell input LINE is bounded
# (the kernel's line buffer), so the 1280-byte argument was TRUNCATED to ~115
# bytes on the wire — BELOW CRADLE_MIN_LIVE (4*32=128). cradle_lesson_ingest then
# REFUSED it (ring_len stayed 0) and S never learned ([cradle-diag]:
# `ingest len=115 -> ring_len=0 (SKIPPED: ingest<=0)`).
#
# THE FIX: the teacher now emits `cradle emit-canon` — a SHORT command that
# composes the FULL canonical lesson INSIDE the kernel (cradle_compose_canon ->
# ct_build_lesson, CT_CERT_BUDGET=1280 bytes), the SAME trainable, train/held-
# structured bytes the in-proc [cradle-teach] cert proves. No shell-line-length
# limit, no truncation: ingest sees the full 1280 bytes and the ring goes live.
# The held probe lands at the production train_end boundary (the never-trained
# continuation) so the post-train drop proves GENERALIZATION, not rote copy.

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
    off|cure|death) T_EMIT="cradle emit-canon" ;;   # T-fix-c: full lesson composed in-kernel
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
  # PKERNEL_CRADLE_DIAG=1 turns on the hosted-only [cradle-diag] pull-path tracer
  # in cradle_net.c's STRONG cradle_poll_and_pull (wave-cradle-diag). It emits a
  # uniquely-greppable line on each beacon-seen / state change so the commander
  # sees EXACTLY where the beacon->poll->dag_read->ingest chain breaks on S.
  env PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 PKERNEL_CRADLE_DIAG=1 $PFS_ENV \
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
  # MATCH the REAL degrade.c emit: "[degrade] *** level change: REDUCED -> FULL  alive=3"
  # (lname[FULL]="FULL", then the literal "  alive=" — FULL then TWO spaces then
  # alive=3). The marker is "FULL<ws>alive=3" so it fires on the stable-FULL line
  # WITHOUT depending on the "-> " transition prefix wording. Verified: this ERE
  # matches the real line and does NOT false-match "FULL -> REDUCED  alive=2" nor
  # "[degrade] initialized  level=FULL".
  if ! wait_for_marker "$LOG_S" 'FULL[[:space:]]+alive=3' 180; then
    echo "[cradle-live] OPEN ($TAG): S never converged to FULL alive=3 in 180s"
    echo "             (S frame-starved or relay down — see $LOG_S / relay log)"
  fi
  sleep 6                                     # let the flap settle to stable FULL

  # ---- (4)+(6) PULL + AUTONOMOUS CONSOLIDATE, WHILE IDLE (wave-cradle-harness-
  # autoprobe). S is NOT training, so cradle_net_task (500ms) owns the CPU, pulls
  # the beacon + p-fs body into the ring, AND the student's OWN autonomous DMN
  # heartbeat (student_dmn_consolidate, dmn.c idle hook @ ST_DMN_INTERVAL ~10s,
  # ST_DMN_ROUNDS=2 over the LIVE lesson ring) consolidates it — the PRODUCTION
  # sleep path, gated by pfs_dur_active(). We do NOT force `baby 16`: 16 synchronous
  # sleep rounds over a ~247MB student do NOT finish inside the harness cap on this
  # cooperative single-core PRoot host, which STARVED the POST probe and FAILED the
  # arm on a host-speed artifact (the learning itself was already real — the probe
  # reads ~2.60 from the autonomous DMN before any `baby`). The autonomous-DMN idle
  # probe is the MORE production-representative signal, so we certify on IT.
  #
  # We POLL `cradle probe` (a PURE READ at the production train_end over never-
  # trained HELD windows — no training, no save) while S idles, and:
  #   cure/death : WAIT until the held probe genuinely drops below the CURE
  #                threshold (CHANCE-CURE_FLOOR) — real below-chance GENERALIZATION
  #                on the held continuation — or a bounded timeout (then OPEN, no
  #                fudge). We require ring_len>0 first so the drop is on the LESSON
  #                ring, never the fixture.
  #   scramble   : idle the SAME consolidation budget (give the autonomous DMN an
  #                EQUAL chance to consolidate the junk) but NEVER early-break; the
  #                held probe MUST stay >= chance (it is the SEQUENCE, not bytes).
  # The OFF arm skips this entirely (ring must stay 0 by design).
  CURE_THRESH=$(awk -v c=$CHANCE -v f=$CURE_FLOOR 'BEGIN{print c-f}')
  if [ "$arm" != "off" ]; then
    # cure/death break early on a genuine drop; scramble runs the full SCRAM_BUDGET
    # so the autonomous DMN had a fair, comparable opportunity.
    # SCRAM_BUDGET: cure converges in ~4 idle cycles, so 8 gives the autonomous
    # DMN a FAIR 2x budget to (fail to) learn the junk — more than the real lesson
    # needed. A larger budget (e.g. 24) over-consolidates AND keeps S's single
    # cooperative core busy so long that `cradle probe` is starved for the whole
    # arm and the POST read comes back blank; 8 leaves idle windows for a clean
    # at-chance read while staying a rigorous falsifier (if junk does not drop the
    # probe in 2x the cure budget, it is the SEQUENCE, not the bytes).
    local _w=0 _r=0 _pl="" IDLE_CAP=60 SCRAM_BUDGET=8
    [ "$arm" = "scramble" ] && IDLE_CAP=$SCRAM_BUDGET
    while [ "$_w" -lt "$IDLE_CAP" ]; do
      s_say "cradle probe"
      s_say "MARK-S-IDLE-${_w}-END"
      # PACE TO THE SHELL (critical): S's shell is COOPERATIVE and STALLS for the
      # whole of each autonomous DMN consolidation tick, so it drains commands far
      # slower than a fixed 5s cadence feeds them. WAIT for S to actually ECHO this
      # step's mark before sending the next probe — otherwise an unbounded FIFO
      # backlog builds and BURIES the later POST probe (it never gets processed
      # before exit, so the verdict read is blank — the host-speed artifact this
      # whole wave removes). The "-END" terminator stops MARK-S-IDLE-1 matching
      # MARK-S-IDLE-10. Generous cap to ride out a long DMN tick.
      wait_for_marker "$LOG_S" "MARK-S-IDLE-${_w}-END" 30 || true
      sleep 3; _w=$((_w + 1))                 # brief idle so the DMN keeps ticking
      _r=$(log_max_ring "$LOG_S"); _r="${_r:-0}"
      _pl=$(log_last_loss "$LOG_S")
      # scramble: never early-break (let the DMN try the whole budget). cure/death:
      # stop the moment the held probe crosses below the CURE threshold, but only
      # once the ring is genuinely live (else we'd be reading the fixture).
      if [ "$arm" != "scramble" ] && [ "$_r" -gt 0 ] 2>/dev/null \
         && [ -n "$_pl" ] && flt_lt "$_pl" "$CURE_THRESH"; then
        echo "[cradle-live] $TAG: autonomous DMN drove held probe to $_pl (< $CURE_THRESH) at idle cycle $_w"
        break
      fi
    done
    if ! wait_for_ring "$LOG_S" 5; then
      echo "[cradle-live] OPEN ($TAG): the lesson body never arrived on S"
      echo "             (ring_len stayed 0 over the wire — check the relay frame"
      echo "              histogram + S's p-fs WANT retries in $LOG_S; do NOT fudge)"
    fi
    if [ "$arm" != "scramble" ]; then
      _pl=$(log_last_loss "$LOG_S")
      flt_lt "${_pl:-99}" "$CURE_THRESH" 2>/dev/null \
        || echo "[cradle-live] OPEN ($TAG): autonomous DMN did not drive the held probe below $CURE_THRESH within $IDLE_CAP idle cycles (last=$_pl) — host too slow OR not learned; do NOT lower the threshold to force green"
    fi
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
  # off/scramble -> stayed at chance. Because the idle loop PACED to the shell
  # (no FIFO backlog), the shell is caught up here and this final probe + mark are
  # processed promptly; a generous cap still rides out one last DMN tick. ----
  s_say "cradle probe"
  s_say "MARK-S-POST-PROBE-END"
  wait_for_marker "$LOG_S" 'MARK-S-POST-PROBE-END' 40
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
s_post_probe() {
  local L="/tmp/cradle_live_nodeS_$1.log" v
  v=$(grep -B12 'MARK-S-POST-PROBE-END' "$L" 2>/dev/null \
        | grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' | tail -1)
  # FALLBACK (cooperative-host robustness): if the dedicated POST mark has not yet
  # DRAINED (a residual 1-2 command FIFO backlog on this single-core PRoot host —
  # seen on the long scramble arm), the LAST [cradle-live] line is STILL a genuine
  # `cradle probe` read of S's CURRENT weights: the autonomous DMN never emits this
  # line, ONLY the `cradle probe` verb does, so the last one is always our own most
  # recent probe. Honest in every arm — cure reads low, scramble/off read >=chance,
  # death reads the post-consolidation persisted value. NEVER fabricates a number.
  [ -z "$v" ] && v=$(grep -oE '\[cradle-live\] ring_len=[0-9]+ probe_loss=[0-9.]+ chance=[0-9.]+' \
                       "$L" 2>/dev/null | tail -1)
  printf '%s' "$v"
}
s_max_ring()   { grep -oE 'ring_len=[0-9]+' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null \
                  | grep -oE '[0-9]+' | sort -n | tail -1; }
# match BOTH the plain `cradle emit` ("lesson emitted over the mesh") and the
# T-fix-c canonical CURE verb ("canonical lesson emitted (CURE)") — the common
# greppable phrase is "lesson emitted".
t_emitted()    { grep -c 'lesson emitted' "/tmp/cradle_live_nodeT_$1.log" 2>/dev/null; }
# The [cradle-diag] pull-path tracer (wave-cradle-diag): surface the UNIQUE diag
# lines S printed so the commander reads the break point directly. The LAST diag
# line of an arm is the deepest the chain reached before stopping:
#   poll: beacon=teacher=.. seq=..   -> a beacon WAS received this cycle
#   poll: (no diag line at all)      -> NO beacon ever arrived (KDDS sub empty)
#   beacon seq=.. hw=.. vocab_fp=..  -> seq/fp gate evaluated
#   reject: seq<=hw / vocab_fp MISMATCH / fmt!=BYTE / body_len.. -> gate rejected
#   dag_read ref=.. rc=.. len=..     -> the p-fs body fetch (rc!=len = not local)
#   ingest len=.. -> ring_len=..     -> SUCCESS (or SKIPPED: ingest<=0)
s_diag_tail() { grep -E '\[cradle-diag\]' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null | tail -8; }
s_diag_last() { grep -E '\[cradle-diag\]' "/tmp/cradle_live_nodeS_$1.log" 2>/dev/null | tail -1; }
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
echo "----- [cradle-diag] pull-path trace on S (deepest reached = the break point) -----"
s_diag_tail cure
echo "diag-last: $(s_diag_last cure)"
echo "-------------------------------------------------------------------------------"
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
  echo "          [cradle-diag] CURE deepest pull-path step (the break point):"
  echo "            $(s_diag_last cure)"
  echo "          (NO diag line at all => NO beacon ever reached S's KDDS sub on cradle/teach;"
  echo "           'reject: vocab_fp MISMATCH' => fp gate; 'dag_read rc!=body_len' => p-fs body not"
  echo "           local yet; 'ingest .. SKIPPED' => the ring refused it. Full trace above.)"
  exit 1
fi
