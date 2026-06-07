#!/bin/bash
# ===========================================================================
# 23_durable / persist.sh  —  G24: make the library non-volatile.
#
# THE SOUL TEST. p-kernel's highest purpose is "survive humanity and reboot".
# Until now the p-fs P0 block store was in-memory only: every node losing
# power at once == the swarm's whole memory gone. This proves the hole is
# closed — content-addressed blocks + named refs are persisted to
# $PKERNEL_PFS_DIR (filename = sha256 block-id, fsync'd), and a reboot
# restores them with self-verification.
#
# Three acts, each asserts; any failure exits non-zero.
#   ACT 1  one node: store memory (pfs put / pfs save / dtr train+save),
#          kill -9 it (simulated power loss, NO graceful flush), restart on
#          the SAME dir -> blocks + ref + trained brain come back.
#   ACT 2  corrupt a block file + plant a fake-named file -> on reboot the
#          sha256 self-check REJECTS both; the rest of the library survives.
#   ACT 3  two nodes killed at the SAME instant -> both reboot -> the
#          swarm's memory (each node's library) is back. Total power loss,
#          collective recovery.
#
# Usage:  ./persist.sh           (builds the host target if needed)
# Logs:   /tmp/durable_*.log
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
KERNEL="$BOOT/p-kernel"
[ -x "$KERNEL" ] || make -C "$BOOT" >/dev/null || { echo "build failed"; exit 1; }

WORK="$(mktemp -d /tmp/durable_work.XXXXXX)"
KPIDS=()
cleanup() {
    [ "${#KPIDS[@]}" -gt 0 ] && kill -9 "${KPIDS[@]}" 2>/dev/null
    pkill -9 -P $$ 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

FAIL=0
ok()   { echo "  [PASS] $*"; }
bad()  { echo "  [FAIL] $*"; FAIL=1; }

# Launch a SOLO node on $1=dir, logging to $2, fed the remaining args as
# shell commands (one per ~1.5s), then held alive by a long tail-sleep so
# the caller can kill -9 it mid-flight (a real power loss leaves no chance
# to flush — durability must come from the per-write fsync, not shutdown).
# Echoes the kernel PID.
launch() {
    local dir="$1" log="$2"; shift 2
    local -a cmds=("$@")
    { sleep 3
      for c in "${cmds[@]}"; do printf '%s\n' "$c"; sleep 1.5; done
      sleep 600
    } | env PKERNEL_PFS_DIR="$dir" "$KERNEL" >"$log" 2>&1 &
    echo $!
}

# Poll $1=log for regex $2 up to $3 seconds. 0 if seen.
wait_for() {
    local log="$1" pat="$2" secs="$3" i=0
    while [ "$i" -lt "$((secs * 4))" ]; do
        grep -qE "$pat" "$log" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

# Run a node to completion (graceful), feeding cmds, for read-back checks.
run_once() {
    local dir="$1" log="$2"; shift 2
    local script=""
    for c in "$@"; do script+="$c"$'\n'; done
    printf '%s' "$script" | timeout 40 env PKERNEL_PFS_DIR="$dir" "$KERNEL" \
        >"$log" 2>&1
}

PUT_TEXT="hello-after-the-end"
GREET="the-library-survives-power-loss"

# ---------------------------------------------------------------------------
echo "==========================================================="
echo " ACT 1 — one node: kill -9 (power loss) then reboot"
echo "==========================================================="
DIR1="$WORK/node1"
L=/tmp/durable_act1_run1.log
echo "[act1] node1 stores memory, then we yank its power (kill -9)"
KPID=$(launch "$DIR1" "$L" \
    "pfs put $PUT_TEXT" \
    "pfs save greeting $GREET" \
    "dtr train 80" \
    "dtr save")
KPIDS+=("$KPID")
if wait_for "$L" "saved 'dtr/weights'" 60; then
    ok "node1 trained + saved a brain to p-fs"
else
    bad "node1 never confirmed dtr save (see $L)"
fi
ACC1=$(grep -oE 'eval train[^%]*[0-9]+\.[0-9]+%' "$L" | head -1)
# Yank power — NO exit, NO flush. fsync on each write must have done its job.
echo "[act1] *** kill -9 $KPID  (simulated total power loss) ***"
kill -9 "$KPID" 2>/dev/null
sleep 1
echo "[act1] disk after power loss:"
ls -1 "$DIR1" | sed 's/^/    /'

L2=/tmp/durable_act1_run2.log
echo "[act1] reboot node1 on the SAME dir — expect memory to return"
run_once "$DIR1" "$L2" "pfs ls" "pfs cat greeting" "dtr load" "dtr eval"
echo "----- reboot restore log -----"
grep -E '\[pfs\] durable|cat .greeting|weights loaded|eval train' "$L2" | sed 's/^/    /'
echo "------------------------------"

grep -qE "durable: restored [1-9][0-9]* block" "$L2" \
    && ok "blocks restored from disk" || bad "no blocks restored"
grep -qE "durable: restored [1-9][0-9]* named ref" "$L2" \
    && ok "named ref(s) restored" || bad "refs not restored"
grep -qE "cat 'greeting'.*$GREET" "$L2" \
    && ok "pfs object 'greeting' content survived verbatim" \
    || bad "greeting content did not survive"
grep -qE "weights loaded from p-fs" "$L2" \
    && ok "trained weights reloaded from p-fs" || bad "weights did not reload"
ACC2=$(grep -oE 'eval train[^%]*[0-9]+\.[0-9]+%' "$L2" | tail -1)
if [ -n "$ACC1" ] && [ "$ACC1" = "$ACC2" ]; then
    ok "post-reboot accuracy == pre-loss accuracy ($ACC2) — the brain is back"
else
    bad "accuracy mismatch: before='$ACC1' after='$ACC2'"
fi

# ---------------------------------------------------------------------------
echo
echo "==========================================================="
echo " ACT 2 — corruption is caught by content-addressed self-check"
echo "==========================================================="
# Tamper the standalone 'pfs put' block (not part of any ref) so the rest
# of the library still restores. Its id == its name; flipping the bytes
# breaks that equality. Also plant a file with a valid-looking 64-hex name
# but garbage content.
PUT_ID=$(grep -oE 'put len=[0-9]+  id=[0-9a-f]+' "$L" | grep -oE '[0-9a-f]{16}$' | head -1)
VICTIM=$(ls "$DIR1" | grep -E "^${PUT_ID}" | head -1)
if [ -z "$VICTIM" ]; then bad "could not find the put block file to corrupt"; fi
printf 'TAMPERED-BYTES-not-the-original' > "$DIR1/$VICTIM"
FAKE="0000000000000000000000000000000000000000000000000000000000000000"
printf 'planted garbage with a fake hex name' > "$DIR1/$FAKE"
echo "[act2] corrupted block $PUT_ID... and planted $FAKE"

L3=/tmp/durable_act2.log
run_once "$DIR1" "$L3" "pfs ls"
echo "----- reboot-with-corruption log -----"
grep -E '\[pfs\] durable' "$L3" | sed 's/^/    /'
echo "--------------------------------------"
REJ=$(grep -cE 'durable: REJECT corrupt block' "$L3")
[ "$REJ" -ge 2 ] && ok "both tampered files rejected (sha256 mismatch x$REJ)" \
    || bad "expected >=2 rejects, got $REJ"
grep -qE "durable: restored [0-9]+ block.*rejected [2-9].*corrupt" "$L3" \
    && ok "summary reports the rejected count" || bad "reject summary missing"
grep -qE "cat 'greeting'|durable: restored [1-9]" "$L3" >/dev/null
# The corrupt block's id legitimately appears in the kernel's own
# "REJECT corrupt block <id> (sha256 mismatch)" log line — that is PROOF it was
# kept out, not a leak. A real leak = the id served from the store (cat/restored),
# i.e. an occurrence on a line that is NOT a reject/mismatch message.
if grep -E "$PUT_ID" "$L3" | grep -vqE "REJECT|reject|mismatch|corrupt"; then
    bad "corrupt block leaked into the store"
else
    ok "corrupt block kept OUT of the store"
fi

# ---------------------------------------------------------------------------
echo
echo "==========================================================="
echo " ACT 3 — two nodes killed at once; the swarm's memory survives"
echo "==========================================================="
DIRA="$WORK/swarmA"; DIRB="$WORK/swarmB"
LA=/tmp/durable_act3_A1.log; LB=/tmp/durable_act3_B1.log
echo "[act3] two nodes each store a distinct memory"
PA=$(launch "$DIRA" "$LA" "pfs save mem nodeA-remembers-this")
PB=$(launch "$DIRB" "$LB" "pfs save mem nodeB-remembers-this")
KPIDS+=("$PA" "$PB")
RA=1; RB=1
wait_for "$LA" "saved 'mem'" 30 && RA=0
wait_for "$LB" "saved 'mem'" 30 && RB=0
{ [ "$RA" = 0 ] && [ "$RB" = 0 ]; } && ok "both nodes saved their memory" \
    || bad "a node failed to save (A=$RA B=$RB)"
echo "[act3] *** kill -9 BOTH at the same instant (total swarm power loss) ***"
kill -9 "$PA" "$PB" 2>/dev/null
sleep 1

LA2=/tmp/durable_act3_A2.log; LB2=/tmp/durable_act3_B2.log
echo "[act3] reboot the whole swarm"
run_once "$DIRA" "$LA2" "pfs cat mem"
run_once "$DIRB" "$LB2" "pfs cat mem"
grep -E "cat 'mem'" "$LA2" "$LB2" | sed 's/^/    /'
grep -qE "cat 'mem'.*nodeA-remembers-this" "$LA2" \
    && ok "node A's memory survived" || bad "node A lost its memory"
grep -qE "cat 'mem'.*nodeB-remembers-this" "$LB2" \
    && ok "node B's memory survived" || bad "node B lost its memory"

# ---------------------------------------------------------------------------
echo
if [ "$FAIL" = 0 ]; then
    echo "==========================================================="
    echo " RESULT: PASS — the library is no longer volatile."
    echo " 'Even if every node loses power, the swarm reboots with"
    echo "  its memory intact.'"
    echo "==========================================================="
    exit 0
else
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/durable_*.log"
    echo "==========================================================="
    exit 1
fi
