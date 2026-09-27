#!/bin/bash
# tests/host/run_mind_gen0_a.sh — MIND-GEN-0-A, the memory cert (ROADMAP 1-1).
#
#   teach -> (pending fact) -> sleep -> persist -> kill -9 -> restore -> ask
#
# WHAT IS NEW HERE: samples/44_persist/persist_cert.sh already walks this path,
# but its "kill" is the shell's `exit` — a clean shutdown. Here the process is
# killed with SIGKILL while it is running, with no chance to flush or tidy
# up, and the next boot must still answer from the persisted weights.
#
# "engram" in the review's chain (external review 2026-09-28 §76.3) is, for
# this mind (r3_incontext.c), the pending fact that `mind teach` queues and the
# DMN sleep consolidates into rw[]. The dtr engrams of lm_consolidate.c are a
# different store and are NOT exercised here.
#
# ARMS (each on its own fresh PKERNEL_PFS_DIR, one hosted x86_64/aarch64 process
# at a time):
#   plain       teach sky->blue, wait for "[dmn] sleep: persisted rw[]", SIGKILL,
#               reboot the same dir: restore line, no pretrain, `ask sky` ->
#               blue, and self/prov comes back with the same content-id.
#   nc-early    teach sky->blue, SIGKILL after 1 s — before the sleep persisted
#               anything. Reboot: must NOT answer blue. (The answer in `plain`
#               comes from the persist, not from the binary or the teach alone.)
#   nc-freshdir the `plain` arm's reboot, but on a new empty dir. Must NOT answer
#               blue (the answer lives in the dir, not in the process image).
# An NC arm that goes green, or a plain arm that goes red, fails the script.
#
# Output: one "[mind-gen0-a] <arm> PASS|FAIL ..." line per check and a final
#   [mind-gen0-a] ALL PASS   (exit 0)   or   [mind-gen0-a] FAIL (exit 1)
# Env: MG0A_PERSIST_WAIT (s, default 180), MG0A_LOGDIR (default mktemp).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
KERNEL="$BOOT/p-kernel"
[ -x "$KERNEL" ] || make -C "$BOOT" >/dev/null || { echo "[mind-gen0-a] FATAL: build failed"; exit 1; }
WAIT="${MG0A_PERSIST_WAIT:-180}"
LOGDIR="${MG0A_LOGDIR:-$(mktemp -d /tmp/mg0a.XXXXXX)}"
mkdir -p "$LOGDIR"

FAILS=0
ok()  { echo "[mind-gen0-a] $1 PASS: $2"; }
bad() { echo "[mind-gen0-a] $1 FAIL: $2"; FAILS=$((FAILS + 1)); }

KPID=""
stop_kernel() {
    if [ -n "$KPID" ]; then kill -9 "$KPID" 2>/dev/null; wait "$KPID" 2>/dev/null; fi
    KPID=""
    exec 7>&- 2>/dev/null
}
trap stop_kernel EXIT

# start_kernel <dir> <log>: a long-lived kernel fed through a FIFO on fd 7
start_kernel() {
    local dir="$1" log="$2" fifo="$LOGDIR/in.$$"
    rm -f "$fifo"; mkfifo "$fifo" || return 1
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$dir" "$KERNEL" <"$fifo" >"$log" 2>&1 &
    KPID=$!
    exec 7>"$fifo"
    rm -f "$fifo"
}

# boot_ask <dir> <log>: a second life on <dir>, ask and read the lineage, exit
boot_ask() {
    printf 'mind ask sky\npfs log self/prov\nexit\n' \
        | timeout 220 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$1" "$KERNEL" >"$2" 2>&1
}

# the same two reads persist_cert.sh [persist-identity] uses
id_saved()    { grep -a "saved 'self/prov'" "$1" | grep -oE 'content=[0-9a-f]+' | head -1; }
id_restored() { grep -aE 'seq=1.*content=' "$1" | grep -oE 'content=[0-9a-f]+' | head -1; }

echo "[mind-gen0-a] kernel=$KERNEL logs=$LOGDIR persist-wait=${WAIT}s"

# ------------------------------------------------------------------ plain
D="$LOGDIR/plain.dir"; rm -rf "$D"; mkdir -p "$D"
start_kernel "$D" "$LOGDIR/plain-life1.log"
printf 'mind teach sky blue\n' >&7
printf 'pfs log self/prov\n' >&7
t=0
while ! grep -aqF '[dmn] sleep: persisted rw[] -> durable store' "$LOGDIR/plain-life1.log" && [ "$t" -lt "$WAIT" ]; do
    sleep 1; t=$((t + 1))
done
if grep -aqF '[dmn] sleep: persisted rw[] -> durable store' "$LOGDIR/plain-life1.log"; then
    ok plain "sleep persisted rw[] after ${t}s"
else
    bad plain "no persist line in ${WAIT}s"
fi
sleep 1
kill -9 "$KPID"; wait "$KPID" 2>/dev/null; KPID=""; exec 7>&-
ok plain "SIGKILLed the first life (no exit command sent)"
[ -f "$D/mind.rw" ] && ok plain "mind.rw on disk after the kill" || bad plain "mind.rw absent after the kill"
ID1="$(id_saved "$LOGDIR/plain-life1.log")"
boot_ask "$D" "$LOGDIR/plain-life2.log"
grep -aqF '[mind] restored learned weights from durable store' "$LOGDIR/plain-life2.log" \
    && ok plain "second life restored the weights" || bad plain "restore line absent"
grep -aqF '[mind] substrate pretrained' "$LOGDIR/plain-life2.log" \
    && bad plain "a pretrain ran on the second life (a re-learn, not a restore)" || ok plain "no pretrain on the second life"
grep -aqE 'ask "sky" -> "blue"' "$LOGDIR/plain-life2.log" \
    && ok plain "ask sky -> blue after kill -9" || bad plain "ask sky did not answer blue"
ID2="$(id_restored "$LOGDIR/plain-life2.log")"
if [ -n "$ID1" ] && [ "$ID1" = "$ID2" ]; then
    ok plain "self/prov lineage survived ($ID1)"
else
    bad plain "self/prov changed or missing (life1=[$ID1] life2=[$ID2])"
fi

# --------------------------------------------------------------- nc-early
D2="$LOGDIR/early.dir"; rm -rf "$D2"; mkdir -p "$D2"
start_kernel "$D2" "$LOGDIR/early-life1.log"
printf 'mind teach sky blue\n' >&7
sleep 1
if grep -aqF '[dmn] sleep: persisted rw[]' "$LOGDIR/early-life1.log"; then
    bad nc-early "VOID: the persist happened within 1 s, this arm cannot test anything"
fi
kill -9 "$KPID"; wait "$KPID" 2>/dev/null; KPID=""; exec 7>&-
boot_ask "$D2" "$LOGDIR/early-life2.log"
grep -aq 'ask "sky" ->' "$LOGDIR/early-life2.log" || bad nc-early "no answer line at all (boot problem, not a red)"
grep -aqE 'ask "sky" -> "blue"' "$LOGDIR/early-life2.log" \
    && bad nc-early "answered blue although nothing was persisted" \
    || ok nc-early "not blue when killed before the sleep persisted (expected red of the memory)"

# ------------------------------------------------------------ nc-freshdir
D3="$LOGDIR/fresh.dir"; rm -rf "$D3"; mkdir -p "$D3"
boot_ask "$D3" "$LOGDIR/fresh-life2.log"
grep -aq 'ask "sky" ->' "$LOGDIR/fresh-life2.log" || bad nc-freshdir "no answer line at all (boot problem, not a red)"
grep -aqE 'ask "sky" -> "blue"' "$LOGDIR/fresh-life2.log" \
    && bad nc-freshdir "answered blue from an empty dir" \
    || ok nc-freshdir "not blue on an empty dir (expected)"

if [ "$FAILS" -eq 0 ]; then
    echo "[mind-gen0-a] ALL PASS"
    exit 0
fi
echo "[mind-gen0-a] FAIL ($FAILS)"
exit 1
