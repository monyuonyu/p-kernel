#!/bin/bash
# tests/host/run_mind_conflicts.sh — ROADMAP 1-3a, the conflict history
# (docs/architecture/30-module/mind-conflicts.md, global-rules.md rule 2).
#
# When a belief is revised, the superseded value, who taught it and how firmly
# it was held must stay readable (`mind conflicts`, and under `mind ask`).
#   local : one node; mind teach sky blue, then sky green.
#           Expect a record "blue" (taught here) superseded by "green"
#           (taught here), in `mind conflicts` and in `mind ask sky`. (With no
#           networking the node has no id yet, so no "(node N)".)
#   remote: node A (id 1) teaches sky blue; once node B (id 2) has it, B
#           teaches sky green. B keeps "blue" taught by node 0 (the remote
#           teacher, with its prov) superseded by green "taught here
#           (node 1)"; A, receiving green, keeps its own "blue" ("taught here
#           (node 0)") superseded by "green" taught by node 1.
# The current answer is not checked here (history only; the belief rule is
# unchanged, MIND-GEN-0-B measures it).
# Output: "[mind-conflicts] <check> PASS|FAIL", then ALL PASS (exit 0) or
# RED (exit 1). Env: MC_PORT (default 7481).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"       >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay" >/dev/null || exit 1
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN
PORT="${MC_PORT:-7481}"
WORK="$(mktemp -d /tmp/mconf.XXXXXX)"
REDS=""; RELAY=0; PA=0; PB=0
res() { echo "[mind-conflicts] $1 $2"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for p in "$PA" "$PB" "$RELAY"; do [ "$p" != 0 ] && kill -9 "$p" 2>/dev/null; done
    exec 7>&- 8>&- 2>/dev/null; wait 2>/dev/null
}
trap cleanup EXIT
wait_for() {  # <file> <ERE> <secs>
    local n=0
    while [ "$n" -lt "$(( $3 * 2 ))" ]; do
        grep -aqE "$2" "$1" 2>/dev/null && return 0
        sleep 0.5; n=$((n + 1))
    done
    return 1
}
chk() {  # <name> <file> <ERE>
    if grep -aqE "$3" "$2"; then res "$1" PASS; else res "$1" FAIL; fi
}

# ---- local arm ------------------------------------------------------------
L="$WORK/local.log"; mkdir -p "$WORK/dl"
printf 'mind teach sky blue\nmind teach sky green\nmind conflicts\nmind ask sky\nexit\n' \
    | timeout 180 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$WORK/dl" "$BOOT/p-kernel" > "$L" 2>&1
chk local-recorded "$L" '^\[mind\] conflict recorded: key [0-9]+ "sky": "blue" \(taught here, seq [0-9]+, (PENDING|RETAINED) [0-9]+/[0-9]+, prov kept\) superseded by "green" taught here at'
chk local-list     "$L" '^\[mind\] conflict key [0-9]+ "sky": "blue" \(taught here, .*superseded by "green" taught here at'
chk local-count    "$L" '^\[mind\] conflicts: 1 kept, 0 older dropped'
chk local-ask      "$L" '^\[mind\]   conflict history: key [0-9]+ "sky": "blue"'

# ---- remote arm -----------------------------------------------------------
"$ROOT/relay/relay" -p "$PORT" > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 1
A="$WORK/a.log"; B="$WORK/b.log"
mkfifo "$WORK/fa" "$WORK/fb"; mkdir -p "$WORK/da" "$WORK/db"
env PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/da" PKERNEL_RELAY="127.0.0.1:$PORT" \
    "$BOOT/p-kernel" < "$WORK/fa" > "$A" 2>&1 &
PA=$!; exec 7<>"$WORK/fa"
env PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/db" PKERNEL_RELAY="127.0.0.1:$PORT" \
    "$BOOT/p-kernel" < "$WORK/fb" > "$B" 2>&1 &
PB=$!; exec 8<>"$WORK/fb"
wait_for "$A" 'mind_net_task up' 90 || echo "[mind-conflicts] note: A mind_net_task not seen"
wait_for "$B" 'mind_net_task up' 90 || echo "[mind-conflicts] note: B mind_net_task not seen"
t=0
while [ "$t" -lt 150 ] && ! grep -aq 'size=2' "$A"; do printf 'region\n' >&7; sleep 2; t=$((t + 2)); done
printf 'mind teach sky blue\n' >&7
wait_for "$B" 'remote teach arrived: "sky"->"blue"' 150 || echo "[mind-conflicts] note: blue never reached B"
printf 'mind teach sky green\n' >&8
wait_for "$A" 'remote REVISE key [0-9]+ "sky"' 150 || echo "[mind-conflicts] note: green never reached A"
printf 'mind conflicts\nmind ask sky\n' >&7
printf 'mind conflicts\nmind ask sky\n' >&8
wait_for "$A" 'ask "sky" ->' 60; wait_for "$B" 'ask "sky" ->' 60; sleep 2
chk remote-B-recorded "$B" '^\[mind\] conflict recorded: key [0-9]+ "sky": "blue" \(taught by node 0, seq [0-9]+, (PENDING|RETAINED) [0-9]+/[0-9]+, prov kept\) superseded by "green" taught here \(node 1\)'
chk remote-A-recorded "$A" '^\[mind\] conflict recorded: key [0-9]+ "sky": "blue" \(taught here \(node 0\), .*prov kept\) superseded by "green" taught by node 1'
chk remote-A-ask      "$A" '^\[mind\]   conflict history: key [0-9]+ "sky": "blue"'
chk remote-B-ask      "$B" '^\[mind\]   conflict history: key [0-9]+ "sky": "blue"'
echo "[mind-conflicts] logs: $WORK"
if [ -z "$REDS" ]; then echo "[mind-conflicts] ALL PASS"; exit 0; fi
echo "[mind-conflicts] RED:$REDS"; exit 1
