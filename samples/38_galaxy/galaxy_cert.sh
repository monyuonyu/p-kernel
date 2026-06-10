#!/bin/bash
# ---------------------------------------------------------------------------
# galaxy_cert.sh — the falsifiable data-plane gate for the galaxy
# observation window v1 (docs/architecture/galaxy.md §8). curl-driven,
# non-flaky (end-state within a bound, never timing). Three tags:
#
#   [galaxy-serve]   2-node loopback mesh; after SWIM converges,
#                    /galaxy.json on node A is valid JSON with me + an
#                    ALIVE peer, and GET / returns the embedded page.
#   [galaxy-events]  3-node FULL mesh; remote moe inferences land on the
#                    observed node and a matching SSE "drpc_in" event is
#                    captured within bound.
#   [galaxy-teach]   single node; POST /teach -> pending:1 -> drains via
#                    REAL DMN pulses (<=120s) -> POST /ask returns the
#                    taught value. Rides the LM-6 mouth end-to-end through
#                    HTTP, proving the web mouth and the console mouth are
#                    one mouth. The SSE capture contains a consolidate.
#
# NOTE on node ids: the hosted cluster is 0-based (nid = mac[5]-1, see
# arch/linux/*/usermain.c), so PKERNEL_NODE_ID=1 -> me.id 0 on port 7800,
# PKERNEL_NODE_ID=2 -> me.id 1 on port 7801. The galaxy honestly reports
# drpc_my_node, so this script asserts the 0-based identity (a deviation
# from galaxy.md §8's literal "me.id==1", which assumed 1-based — the
# 0-based value is the node's real cluster identity).
#
# Pixels are NOT certified (galaxy.md §7) — only the data plane.
# Exit non-zero if any assertion fails. Logs: /tmp/gx38_*.log
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || exit 1

FAIL=0
PIDS=()
TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
pass() { echo "$1 PASS"; }
fail() { echo "$1 FAIL: $2"; FAIL=1; }

killall_nodes() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; PIDS=(); wait 2>/dev/null; sleep 1; }
trap killall_nodes EXIT

have_python() { command -v python3 >/dev/null 2>&1; }

# ----------------------------------------------------------------- serve
gate_serve() {
    log "--- [galaxy-serve]: 2-node loopback mesh ---"
    rm -f /tmp/gx38_s1.log /tmp/gx38_s2.log
    PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/gx38_s1.log 2>&1 &
    PIDS+=($!)
    PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" </dev/null >/tmp/gx38_s2.log 2>&1 &
    PIDS+=($!)
    # bounded readiness: node1 sees a peer ALIVE (REDUCED/FULL)
    local t=0
    while [ $t -lt 30 ]; do
        grep -aq "REDUCED\|FULL" /tmp/gx38_s1.log && break
        sleep 1; t=$((t+1))
    done
    sleep 2
    local J; J=$(curl -s --max-time 5 127.0.0.1:7800/galaxy.json)
    log "galaxy.json: $J"
    # valid JSON?
    if have_python; then
        echo "$J" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null \
            || { fail "[galaxy-serve]" "/galaxy.json is not valid JSON"; killall_nodes; return; }
    fi
    # me.id == 0 (0-based cluster identity of PKERNEL_NODE_ID=1)
    echo "$J" | grep -q '"me":{"id":0' || { fail "[galaxy-serve]" "me.id != 0"; killall_nodes; return; }
    # an ALIVE peer (state 1) present
    echo "$J" | grep -q '"state":1' || { fail "[galaxy-serve]" "no ALIVE peer in peers[]"; killall_nodes; return; }
    # GET / returns the embedded page with a <canvas
    local PG; PG=$(curl -s --max-time 5 127.0.0.1:7800/)
    echo "$PG" | grep -q "<canvas" || { fail "[galaxy-serve]" "GET / missing <canvas"; killall_nodes; return; }
    # the page references no external URL (offline-honest)
    if echo "$PG" | grep -qE 'https?://|src=|cdn'; then
        fail "[galaxy-serve]" "page references an external URL"; killall_nodes; return
    fi
    killall_nodes
    pass "[galaxy-serve]"
}

# ---------------------------------------------------------------- events
gate_events() {
    log "--- [galaxy-events]: 3-node FULL mesh, remote drpc INFER ---"
    rm -f /tmp/gx38_e1.log /tmp/gx38_e2.log /tmp/gx38_e3.log /tmp/gx38_sse.log
    local F1=/tmp/gx38_ef1 F2=/tmp/gx38_ef2 F3=/tmp/gx38_ef3
    rm -f "$F1" "$F2" "$F3"; mkfifo "$F1" "$F2" "$F3"
    exec 6<>"$F1" 7<>"$F2" 8<>"$F3"
    PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F1" >/tmp/gx38_e1.log 2>&1 & PIDS+=($!)
    PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F2" >/tmp/gx38_e2.log 2>&1 & PIDS+=($!)
    PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F3" >/tmp/gx38_e3.log 2>&1 & PIDS+=($!)
    local t=0
    while [ $t -lt 30 ]; do grep -aq "FULL" /tmp/gx38_e1.log && break; sleep 1; t=$((t+1)); done
    # capture node1's SSE while nodes 2/3 drive moe inferences that route remotely
    ( curl -sN --max-time 45 127.0.0.1:7800/events > /tmp/gx38_sse.log 2>&1 ) & local SSE=$!; PIDS+=($SSE)
    sleep 2
    local k
    for k in $(seq 1 16); do
        printf 'moe 30 40 90 5\n' >&7
        printf 'moe 30 40 90 5\n' >&8
        sleep 1
        # end-state check: a drpc_in with src:2 reached node1's stream
        if grep -aq '"type":"drpc_in".*"src":2' /tmp/gx38_sse.log; then break; fi
    done
    sleep 2
    exec 6>&- 7>&- 8>&- 2>/dev/null
    rm -f "$F1" "$F2" "$F3"
    log "drpc_in events captured: $(grep -ac '"type":"drpc_in"' /tmp/gx38_sse.log)"
    if grep -aq '"type":"drpc_in".*"src":2' /tmp/gx38_sse.log; then
        killall_nodes; pass "[galaxy-events]"
    else
        fail "[galaxy-events]" "no drpc_in event with src:2 in the SSE capture"; killall_nodes
    fi
}

# ----------------------------------------------------------------- teach
gate_teach() {
    log "--- [galaxy-teach]: single node, web mouth == console mouth ---"
    rm -f /tmp/gx38_t.log /tmp/gx38_tsse.log
    PKERNEL_NODE_ID=1 "$BOOT/p-kernel" </dev/null >/tmp/gx38_t.log 2>&1 & PIDS+=($!)
    sleep 3
    ( curl -sN --max-time 140 127.0.0.1:7800/events > /tmp/gx38_tsse.log 2>&1 ) & PIDS+=($!)
    sleep 1
    # (k*,v*)=(2,3): LM-6's MEASURED off-bias pair (pre_share 0.0% at N=100).
    local R; R=$(curl -s --max-time 5 -d 'k=2&v=3' 127.0.0.1:7800/teach)
    log "teach: $R"
    echo "$R" | grep -q '"ok":true' || { fail "[galaxy-teach]" "teach not ok"; killall_nodes; return; }
    echo "$R" | grep -q '"pending":1' || { fail "[galaxy-teach]" "pending != 1 after teach"; killall_nodes; return; }
    # drain via REAL DMN pulses, bound 120s (LM-6 8x-margin)
    local t=0 drained=0 P=""
    while [ $t -lt 120 ]; do
        P=$(curl -s --max-time 5 127.0.0.1:7800/galaxy.json | grep -o '"pending":[0-9]*' | head -1 | grep -o '[0-9]*$')
        if [ "$P" = "0" ]; then drained=1; log "drained after ~${t}s"; break; fi
        sleep 3; t=$((t+3))
    done
    [ $drained = 1 ] || { fail "[galaxy-teach]" "did not drain in 120s (pending=$P)"; killall_nodes; return; }
    # a consolidate event must have crossed the wire between teach and drain
    grep -aq '"type":"consolidate"' /tmp/gx38_tsse.log \
        || { fail "[galaxy-teach]" "no consolidate event in SSE"; killall_nodes; return; }
    # ask returns the taught value
    local A; A=$(curl -s --max-time 5 -d 'k=2' 127.0.0.1:7800/ask)
    log "ask: $A"
    echo "$A" | grep -q '"pred":3' || { fail "[galaxy-teach]" "ask pred != 3 (taught value)"; killall_nodes; return; }
    killall_nodes
    pass "[galaxy-teach]"
}

gate_serve
gate_events
gate_teach

echo "------------------------------------------------------------"
if [ $FAIL -eq 0 ]; then echo "galaxy_cert: ALL GATES PASS"; else echo "galaxy_cert: FAILURES ABOVE"; fi
exit $FAIL
