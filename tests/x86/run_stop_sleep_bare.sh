#!/bin/sh
# tests/x86/run_stop_sleep_bare.sh — ROADMAP 0-14 S2 on bare x86, stage 1
# (docs/architecture/30-module/swarm-stop.md "ベアへ" ①): the bare console
# has `stop`, and a stopped node does not do its DMN sleep work (ga_step,
# the engram replay, the r3 distillation), the same as hosted [stop-sleep].
# Before this stage swarm_stop.c was hosted-only: the bare shell answered
# `stop` with "unknown command" and the sleep never looked at a stop.
#
# Expectations fixed before the code (one QEMU node, timed serial input):
#   B0 `stop` prints "[stop] stopped 0 local 0 signed 0 epoch 0 keys 0"
#   B1 `stop test` runs the [swarm-stop] self-test on bare metal: "<n> PASS /
#      0 FAIL" with n >= 10 (the same T1..T8 items as hosted `tkconf stop`)
#   B2 `stop now` stops it ("stopped 1 local 1")
#   B3 between `ga`/`dmn` snapshot A (2 s after the stop) and snapshot B
#      (25 s later): the DMN idle runs went up by >= 20 (the node kept
#      sleeping: two GA_INTERVALs of 10 were due), ga_step was not called
#      (steps + skipped did not move) and no "[ga]  step=" / "[dmn] sleep:"
#      line came out
#   B4 after `stop resume` (a second "stopped 0 local 0" line, B0 is the
#      first), 25 s later snapshot C:
#      ga_step is called again (steps + skipped above B)
# What is seen is the ga_step call (its counters move whether it runs or
# skips for want of log entries). The engram replay and the r3 distillation
# sit behind the same gate; nothing on the bare console fills them, so they
# are not observed here (bare has no `mind teach`).
# Falsifier: -DSWARM_STOP_NO_SLEEP_OBEY (the idle work does not look at the
# stop) must turn B3 red.
# Output: "[stop-bare] <check> PASS|FAIL", then ALL PASS (exit 0) or RED (1).
# Arg: "nc" builds the falsifier and expects B3 red (exit 0 when so).
# Rebuilds boot/x86 with EXTRA_CFLAGS and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/x86"
QEMU="${QEMU:-qemu-system-x86_64}"
WAIT="${STOP_BARE_BOOT_WAIT:-30}"
MODE="${1:-plain}"
CF=""
[ "$MODE" = nc ] && CF="-DSWARM_STOP_NO_SLEEP_OBEY"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/stopbare.XXXXXX")"
trap 'rm -rf "$WORK"; make -C "$BOOT" clean >/dev/null 2>&1' EXIT
REDS=""
res() { echo "[stop-bare] $1 $2"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }

make -C "$BOOT" clean >/dev/null 2>&1
if ! make -C "$BOOT" EXTRA_CFLAGS="$CF" bootloader.bin > "$WORK/build.log" 2>&1; then
    tail -30 "$WORK/build.log"; echo "[stop-bare] RED: build"; exit 1
fi

( sleep "$WAIT"; printf 'stop\r'
  sleep 2;  printf 'stop test\r'
  sleep 8;  printf 'stop now\r'
  sleep 2;  printf 'ga\r'; sleep 1; printf 'dmn\r'        # snapshot A
  sleep 25; printf 'ga\r'; sleep 1; printf 'dmn\r'        # snapshot B
  sleep 1;  printf 'stop resume\r'
  sleep 25; printf 'ga\r'; sleep 1; printf 'dmn\r'        # snapshot C
  sleep 3 ) \
  | ( cd "$BOOT" && timeout -k 3 $((WAIT + 80)) "$QEMU" -m 256 -kernel bootloader.bin \
        -serial stdio -cpu qemu64 -display none -no-reboot ) > "$WORK/raw.log" 2>&1
tr -d '\r' < "$WORK/raw.log" > "$WORK/n.log"
L="$WORK/n.log"

# the k-th value of a counter line ("<label> : N")
nth() { grep -a "$1" "$L" | sed -n "${2}p" | sed 's/.*: *\([0-9][0-9]*\).*/\1/'; }
# line number of the k-th `ga` snapshot
gl() { grep -an '\[ga\]  steps         :' "$L" | sed -n "${1}p" | cut -d: -f1; }
calls() { s=$(nth '\[ga\]  steps         :' "$1"); k=$(nth '\[ga\]  skipped       :' "$1")
          [ -n "$s" ] && [ -n "$k" ] && echo $((s + k)); }
idle() { nth '\[dmn\] idle runs     :' "$1"; }
GA=$(calls 1); GB=$(calls 2); GC=$(calls 3)
IA=$(idle 1); IB=$(idle 2); IC=$(idle 3)
LA=$(gl 1); LB=$(gl 2)
WIN=0
[ -n "$LA" ] && [ -n "$LB" ] &&
    WIN=$(awk -v a="$LA" -v b="$LB" 'NR > a && NR < b && (/\[ga\]  step=/ || /\[dmn\] sleep:/)' "$L" | grep -c .)
echo "[stop-bare] ga calls A=${GA:-?} B=${GB:-?} C=${GC:-?}; idle runs A=${IA:-?} B=${IB:-?} C=${IC:-?}; work lines A..B=$WIN"

grep -aq '^\[stop\] stopped 0 local 0 signed 0 epoch 0 keys 0' "$L" && res B0-command PASS || res B0-command FAIL
ST=$(grep -a '\[swarm-stop\] [0-9]* PASS / [0-9]* FAIL' "$L" | head -1)
P=$(echo "$ST" | sed -n 's/.*\] \([0-9]*\) PASS \/ \([0-9]*\) FAIL.*/\1/p')
F=$(echo "$ST" | sed -n 's/.*\] \([0-9]*\) PASS \/ \([0-9]*\) FAIL.*/\2/p')
[ -n "$P" ] && [ "$P" -ge 10 ] && [ "$F" = 0 ] && res B1-selftest PASS || res B1-selftest FAIL
grep -aq 'stopped 1 local 1' "$L" && res B2-stopped PASS || res B2-stopped FAIL
if [ -n "$GA" ] && [ -n "$GB" ] && [ -n "$IA" ] && [ -n "$IB" ] &&
   [ $((IB - IA)) -ge 20 ] && [ "$GA" = "$GB" ] && [ "$WIN" = 0 ]; then
    res B3-no-sleep PASS
else
    res B3-no-sleep FAIL
fi
if [ "$(grep -ac 'stopped 0 local 0' "$L")" -ge 2 ] && [ -n "$GB" ] && [ -n "$GC" ] && [ "$GC" -gt "$GB" ]; then
    res B4-after-resume PASS
else
    res B4-after-resume FAIL
fi
cp "$L" /tmp/stop_bare.log 2>/dev/null
if [ "$MODE" = nc ]; then
    case "$REDS" in
        *B3-no-sleep*) echo "[stop-bare] NC RED as expected:$REDS"; exit 0 ;;
        *) echo "[stop-bare] NC FAIL: the falsifier stayed green ($REDS)"; exit 1 ;;
    esac
fi
if [ -z "$REDS" ]; then echo "[stop-bare] ALL PASS"; exit 0; fi
echo "[stop-bare] RED:$REDS"; exit 1
