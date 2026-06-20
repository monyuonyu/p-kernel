#!/bin/sh
# tests/idle_cpu.sh — headline cert for wave-idle-yield.
#
# Boot ./p-kernel to the shell with NO input and measure the PROCESS CPU%
# over a fixed idle window. Before the fix (.Lidle busy-spinning on
# sched_yield) one core is pegged at ~100%. After the fix (.Lidle sleeps
# in knl_idle_wait/sigsuspend until the next SIGALRM tick) the process is
# near-idle (single-digit %).
#
# Usage:  tests/idle_cpu.sh [path-to-p-kernel] [window-seconds]
# Default: boot/linux/p-kernel, 10s window.
#
# Method: read utime+stime jiffies from /proc/<pid>/stat at the start and
# end of the window; CPU% = 100 * (delta_jiffies / CLK_TCK) / window.

set -u

KPATH="${1:-$(dirname "$0")/../boot/linux/p-kernel}"
WINDOW="${2:-10}"
SETTLE=2   # seconds to let the kernel finish booting before we sample

if [ ! -x "$KPATH" ]; then
    echo "idle_cpu: not executable: $KPATH" >&2
    exit 2
fi

CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)

# Hold stdin open but send nothing: a background sleep feeding a pipe.
# The shell blocks on read; the kernel goes idle. Kill the whole group at
# the end.
sleep $((SETTLE + WINDOW + 5)) | "$KPATH" >/dev/null 2>&1 &
KPID=$!

# The pipeline's left side is `sleep`; the p-kernel pid is the right side.
# Find the actual p-kernel pid (child of this shell running the binary).
sleep 1
PID=$(pgrep -P $$ -f "$(basename "$KPATH")" 2>/dev/null | head -1)
[ -z "$PID" ] && PID=$(pgrep -f "$(basename "$KPATH")" 2>/dev/null | head -1)
if [ -z "$PID" ] || [ ! -r "/proc/$PID/stat" ]; then
    echo "idle_cpu: could not find running p-kernel pid" >&2
    kill $KPID 2>/dev/null
    exit 3
fi

# Let it finish booting.
sleep "$SETTLE"

read_jiffies() {
    # fields 14 (utime) + 15 (stime) of /proc/PID/stat. comm may contain
    # spaces/parens, so split on the last ')'.
    awk '{ s=$0; sub(/^[^)]*\) /,"",s); n=split(s,a," ");
           # after stripping "pid (comm) ", field indices shift by 2:
           # original 14,15 -> a[12],a[13]
           print a[12]+a[13] }' "/proc/$PID/stat"
}

J0=$(read_jiffies)
T0=$(date +%s.%N)
sleep "$WINDOW"
J1=$(read_jiffies)
T1=$(date +%s.%N)

kill $KPID 2>/dev/null
wait $KPID 2>/dev/null

DJ=$((J1 - J0))
CPU=$(awk -v dj="$DJ" -v tck="$CLK_TCK" -v t0="$T0" -v t1="$T1" \
      'BEGIN{ dt=t1-t0; if(dt<=0)dt=1; printf "%.1f", 100.0*(dj/tck)/dt }')

echo "idle_cpu: pid=$PID window=${WINDOW}s CLK_TCK=$CLK_TCK delta_jiffies=$DJ"
echo "idle_cpu: process CPU = ${CPU}%"

# Pass threshold: under 25% (busy-spin is ~100%; idle should be << 10%).
PASS=$(awk -v c="$CPU" 'BEGIN{ print (c < 25.0) ? 1 : 0 }')
if [ "$PASS" = "1" ]; then
    echo "idle_cpu: PASS (near-idle)"
    exit 0
else
    echo "idle_cpu: FAIL (looks like a busy-spin)"
    exit 1
fi
