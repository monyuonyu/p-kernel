#!/bin/bash
# ---------------------------------------------------------------------------
# Wave 7 — "the OS that does not die": task fault isolation + auto-respawn
# with weight recovery from p-fs. Single node, no relay.
#
# Story (all inside ONE kernel process):
#   1. dtr train  -> the 635-param Transformer actually learns (~95%/100%)
#   2. dtr save   -> weights persist as the p-fs object "dtr/weights"
#   3. dtr crash  -> the guarded ring-0 worker ZEROES the in-memory weights
#                    and writes through NULL: SIGSEGV in kernel task context
#   4. fault.c + guard.c isolate the fault: ONLY the task dies; the shell
#      still answers (we assert with `ver`)
#   5. the guard supervisor respawns the worker and runs recover_fn, which
#      reloads the trained weights from p-fs
#   6. dtr eval   -> trained accuracy again. Because step 3 zeroed the
#      weights, this can ONLY pass if step 5 really restored them.
#
# Exit code: 0 = all assertions hold, 1 = something failed.
# Log: /tmp/crashrec_node1.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
# BOOT/RUN overridable: e.g. BOOT=.../boot/linux_x86_64 RUN=qemu-x86_64
BOOT="${BOOT_OVERRIDE:-$BOOT}"
RUN="${RUN:-}"

[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || exit 1

LOG=/tmp/crashrec_node1.log
FIFO=/tmp/crashrec_in.$$
: > "$LOG"
mkfifo "$FIFO" || exit 1

KPID=
cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "$KPID" ] && kill "$KPID" 2>/dev/null
    rm -f "$FIFO"
}
trap cleanup EXIT

$RUN "$BOOT/p-kernel" < "$FIFO" > "$LOG" 2>&1 &
KPID=$!
exec 3> "$FIFO"          # keep the writer open across commands

FAIL=0
send()    { echo "$1" >&3; }
# waitfor <pattern> [timeout_s] — poll the log until pattern appears
waitfor() {
    local pat="$1" t="${2:-60}" i
    for ((i = 0; i < t * 10; i++)); do
        grep -Eq "$pat" "$LOG" && return 0
        kill -0 "$KPID" 2>/dev/null || { echo "FAIL: kernel process died waiting for: $pat"; FAIL=1; return 1; }
        sleep 0.1
    done
    echo "FAIL: timeout waiting for: $pat"
    FAIL=1
    return 1
}

echo "[demo] kernel pid=$KPID  log=$LOG"
waitfor "Type 'help' for commands" 30 || { exit 1; }
waitfor "guarding 'dtr-worker'" 10

echo "[demo] 1) baseline eval (untrained, ~33%)"
send "dtr eval"
waitfor "eval held-out" 30

echo "[demo] 2) train (300 epochs, full-batch SGD)"
send "dtr train 300"
waitfor "trained 300 epochs" 600
waitfor "eval held-out: acc (9[0-9]|100)" 30   # trained: >= 90% held-out

echo "[demo] 3) save weights into p-fs object dtr/weights"
send "dtr save"
waitfor "saved as p-fs object 'dtr/weights'" 30

echo "[demo] 4) crash the guarded ring-0 worker (NULL write)"
send "dtr crash"
waitfor "weights ZEROED in memory" 10
waitfor "\[guard\] FAULT in task 'dtr-worker'" 10

echo "[demo] 5) assert the kernel survived the SIGSEGV"
sleep 1
send "ver"
waitfor "T-Kernel core" 10 && echo "[demo]    kernel alive: shell answered 'ver' after the fault"

echo "[demo] 6) wait for guard respawn + p-fs weight recovery"
waitfor "weights restored from p-fs" 30
waitfor "\[guard\] respawned 'dtr-worker'" 30

echo "[demo] 7) eval again — only passes if recovery really reloaded weights"
send "dtr eval"
# need the SECOND held-out line after recovery; count instead of grep -q
for ((i = 0; i < 300; i++)); do
    n=$(grep -c "eval held-out" "$LOG")
    [ "$n" -ge 3 ] && break
    sleep 0.1
done

send "guard"
sleep 1
send "exit"
sleep 1

# ---- assertions on the final state -----------------------------------
final_acc=$(grep "eval held-out" "$LOG" | tail -1 | sed -n 's/.*acc \([0-9]*\)\..*/\1/p')
echo
echo "[demo] final held-out accuracy after crash+recovery: ${final_acc:-??}%"
if [ -z "${final_acc:-}" ] || [ "$final_acc" -lt 90 ]; then
    echo "FAIL: post-recovery accuracy ${final_acc:-none} < 90% — recovery did not restore the brain"
    FAIL=1
fi
grep -q "\[guard\] isolating: killing task only" "$LOG" || { echo "FAIL: no isolation line"; FAIL=1; }
grep -q "death 1/5" "$LOG" || { echo "FAIL: no respawn-count line"; FAIL=1; }

echo
echo "===== key log lines ====="
grep -E "\[guard\]|\[dtr\] (worker|crash|eval|weights|trained 300)|\[dtr\] training" "$LOG" | sed 's/^/  /'
echo "========================="
if [ "$FAIL" -eq 0 ]; then
    echo "[demo] PASS — task died, node lived, brain came back from p-fs"
else
    echo "[demo] FAIL — see $LOG"
fi
exit $FAIL
