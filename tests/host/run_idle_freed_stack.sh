#!/bin/bash
# ---------------------------------------------------------------------------
# run_idle_freed_stack.sh — host cert for D5-k (linux_x86_64 only)
#
# THE BUG: tk_exd_tsk frees its own system stack (knl_del_tsk -> knl_Ifree)
# and then force-dispatches while still standing on it. If no task is runnable,
# the hosted idle (knl_idle_wait) runs on that freed Imalloc block.
#
# THE FIX: knl_dispatch_to_schedtsk switches rsp to the static knl_tmp_stack
# first (as upstream armv7m / rxv2 do).
#
# THE DETECTOR: .Lidle calls knl_idle_sp_check(rsp), which aborts if rsp is
# inside a free Imalloc area.
#
# HOW: no single-node shell command reaches tk_exd_tsk from an Imalloc stack
# (the guard killer runs on the static death stack; drpc's remote_* tasks are
# started by other nodes). So gdb stops the shell task in cmd_ver and calls
# drpc_local_restart(2, 4, 0): remote_counter, which sleeps 5 x 1 s and then
# tk_exd_tsk's while the shell is blocked on input -> the dispatcher idles.
#   PASS: "Counter done", no detector abort, the kernel exits normally.
#   FAIL: the detector fires (pre-fix: 2/2 on 2026-09-25), or no counter.
#
# Needs gdb. Exit 0 = PASS, 1 = FAIL, 2 = cannot run here.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/linux_x86_64"

[ "$(uname -m)" = x86_64 ] || { echo "SKIP: x86_64 only"; exit 2; }
command -v gdb >/dev/null || { echo "SKIP: gdb not installed"; exit 2; }
[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || exit 1

CMDS=$(mktemp)
OUT=/tmp/idle_freed_stack.log
trap 'rm -f "$CMDS"' EXIT
cat > "$CMDS" <<'EOF'
set pagination off
set confirm off
set unwindonsignal on
handle SIGALRM nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGIO nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIGABRT stop print
tbreak cmd_ver
run
print (long)drpc_local_restart(2, 4, 0)
continue
bt 8
kill
quit
EOF

{ sleep 3; echo ver; sleep 10; echo ver; sleep 2; echo exit; sleep 2; } | \
    timeout 90 gdb -q -batch -x "$CMDS" "$BOOT/p-kernel" > "$OUT" 2>&1

FAIL=0
if grep -q 'idle is running on a freed' "$OUT"; then
    echo "FAIL: idle ran on a freed Imalloc area after tk_exd_tsk"
    grep -a 'idle is running on a freed' "$OUT" | head -1
    FAIL=1
fi
grep -q 'Counter done' "$OUT" || { echo "FAIL: remote_counter never finished (harness did not reach tk_exd_tsk)"; FAIL=1; }
grep -q 'exited normally' "$OUT" || { echo "FAIL: kernel did not exit normally"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
    echo "PASS — tk_exd_tsk with nothing runnable idles off the freed stack"
fi
echo "log: $OUT"
exit "$FAIL"
