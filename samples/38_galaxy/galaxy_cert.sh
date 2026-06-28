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
#                    observed node and a matching "drpc_in" event appears in
#                    that node's event ring (read one-shot via GET /log.txt —
#                    the SAME ring the /events SSE streams) within bound.
#                    Ring-read (not held-open SSE) so the gate is deterministic
#                    under mesh load: galaxy is a single cooperative task that
#                    can starve a held-open SSE socket, but galaxy_emit records
#                    into the ring unconditionally. Same property, no flake.
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

# port_busy P -> 0 (true) if SOMETHING is listening on 127.0.0.1:P. A galaxy
# HTTP listening socket is closed the instant its process dies (a LISTEN
# socket never enters TIME_WAIT), so this only stays true while a real node
# still holds the port.
port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&- 3<&- 2>/dev/null; return 0; }; return 1; }
# wait_ports_free P... — block (bounded) until each galaxy port is releasable,
# so a gate that rebinds 7800/7801/7802 can never inherit a LEFTOVER node from
# the previous gate (the "FULL@0s / stale mesh" env-flake). Returns non-zero if
# a port is still held after the bound (the caller then fails honestly).
wait_ports_free() {
    local p t
    for p in "$@"; do
        t=0
        while port_busy "$p"; do
            [ $t -ge 30 ] && { log "port $p still busy after 15s"; return 1; }
            sleep 0.5; t=$((t+1))
        done
    done
    return 0
}

have_python() { command -v python3 >/dev/null 2>&1; }

# ----------------------------------------------------------------- serve
gate_serve() {
    log "--- [galaxy-serve]: 2-node loopback mesh ---"
    if ! wait_ports_free 7800 7801; then
        fail "[galaxy-serve]" "galaxy ports 7800/7801 not free at gate entry — env, not product"; return
    fi
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
    # offline-honest: the page must load NO external resource. The page is a
    # single self-contained file (zero <script src>, zero <link href>, zero
    # cdn). The ONLY URL it contains is a LOOPBACK http://127.0.0.1 inside a JS
    # *comment* (galaxy.html, the clipboard-fallback note) — it loads nothing.
    # So we flag any http(s):// URL whose host is NOT loopback, plus any
    # protocol-relative resource attr (src=//host / href=//host) — a real CDN
    # or external <script> still fails this gate, but the loopback comment does
    # not (the old `https?://|src=|cdn` regex over-matched the comment).
    local BADURL
    BADURL=$(echo "$PG" | grep -oiE 'https?://[a-z0-9._-]+' \
                 | grep -viE '^https?://(127\.0\.0\.1|localhost)$') || true
    if [ -n "$BADURL" ] || echo "$PG" | grep -qiE '(src|href)=["'"'"']?//[a-z]'; then
        fail "[galaxy-serve]" "page references an external URL: ${BADURL:-protocol-relative resource}"; killall_nodes; return
    fi
    killall_nodes
    pass "[galaxy-serve]"
}

# ---------------------------------------------------------------- events
gate_events() {
    log "--- [galaxy-events]: 3-node FULL mesh, remote drpc INFER ---"
    rm -f /tmp/gx38_e1.log /tmp/gx38_e2.log /tmp/gx38_e3.log /tmp/gx38_sse.log
    # ENV-FLAKE FIX: a leftover node from a previous gate still holding 7800/
    # 7801/7802 would make THIS gate's node1 fail to rebind 7800 and curl would
    # hit the stale node (a 2-node serve mesh = "FULL@0s", no node2/3 drpc) ->
    # spurious "no drpc_in". Refuse to boot until the ports are actually free.
    if ! wait_ports_free 7800 7801 7802; then
        fail "[galaxy-events]" "galaxy ports 7800/7801/7802 not free (leftover node) — env, not product"; return
    fi
    local F1=/tmp/gx38_ef1 F2=/tmp/gx38_ef2 F3=/tmp/gx38_ef3
    rm -f "$F1" "$F2" "$F3"; mkfifo "$F1" "$F2" "$F3"
    exec 6<>"$F1" 7<>"$F2" 8<>"$F3"
    PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F1" >/tmp/gx38_e1.log 2>&1 & PIDS+=($!)
    PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F2" >/tmp/gx38_e2.log 2>&1 & PIDS+=($!)
    PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" <"$F3" >/tmp/gx38_e3.log 2>&1 & PIDS+=($!)
    # readiness: a FRESH boot of node1 (its boot banner is in this just-rm'd
    # log) reaching FULL. The fresh-boot marker guarantees the FULL we accept
    # belongs to THIS node1, never a stale one.
    local t=0
    while [ $t -lt 30 ]; do
        grep -aq "=== p-kernel linux boot ===" /tmp/gx38_e1.log \
            && grep -aq "FULL" /tmp/gx38_e1.log && break
        sleep 1; t=$((t+1))
    done
    # Certify the remote drpc_in via node1's EVENT RING (GET /log.txt), NOT a
    # held-open SSE. WHY (the determinism fix): galaxy is a SINGLE cooperative
    # task. Under 3-node mesh + remote-infer load it can starve a held-open
    # /events SSE socket — the connection may never even be serviced, giving a
    # 0-byte capture that is a FLAKE, not a missed event (observed FAIL/PASS/
    # FAIL on aarch64; isolated single-node SSE is fine, so this is load
    # starvation, the cooperative-single-thread H3 family). But galaxy_emit
    # records EVERY event into node1's UI event ring UNCONDITIONALLY
    # (galaxy.c:104, independent of any SSE client), and GET /log.txt is a
    # ONE-SHOT read of that SAME 256-slot ring — the exact record the SSE
    # streams (galaxy.c §"LAZY read of the SAME event ring the SSE stream
    # drains"). A one-shot GET is serviced in any settle window AFTER the load,
    # so the assertion is DETERMINISTIC while certifying the IDENTICAL property:
    # a remote moe inference LANDS on the observed node and is OBSERVABLE as a
    # drpc_in (src=2 == the remote node PKERNEL_NODE_ID=3 / cluster id 2). The
    # line format is "<ms>ms drpc_in src=2 ...". Still falsifiable: with no
    # remote inference landing, no such line exists and the gate FAILs.
    sleep 3                                   # let node1's task become responsive
    rm -f /tmp/gx38_log.txt
    local k seen=0
    for k in $(seq 1 24); do
        printf 'moe 30 40 90 5\n' >&7         # node2 (cluster id 1) drives
        printf 'moe 30 40 90 5\n' >&8         # node3 (cluster id 2) drives
        sleep 1                               # let the drpc land + ring record + task settle
        curl -s --max-time 8 127.0.0.1:7800/log.txt > /tmp/gx38_log.txt 2>/dev/null
        if grep -aq 'drpc_in src=2' /tmp/gx38_log.txt; then seen=1; break; fi
    done
    exec 6>&- 7>&- 8>&- 2>/dev/null
    rm -f "$F1" "$F2" "$F3"
    log "ring drpc_in lines: $(grep -ac 'drpc_in' /tmp/gx38_log.txt 2>/dev/null) ; src=2 present: $seen"
    if [ $seen = 1 ]; then
        killall_nodes; pass "[galaxy-events]"
    else
        fail "[galaxy-events]" "no remote drpc_in (src=2) in node1's event ring (/log.txt) within bound"; killall_nodes
    fi
}

# ----------------------------------------------------------------- teach
gate_teach() {
    log "--- [galaxy-teach]: single node, web mouth == console mouth ---"
    if ! wait_ports_free 7800; then
        fail "[galaxy-teach]" "galaxy port 7800 not free at gate entry — env, not product"; return
    fi
    rm -f /tmp/gx38_t.log /tmp/gx38_tsse.log
    PKERNEL_NODE_ID=1 "$BOOT/p-kernel" </dev/null >/tmp/gx38_t.log 2>&1 & PIDS+=($!)
    sleep 3
    # SSE window spans the lazy pretrain (~23s, inside the first teach) PLUS
    # the up-to-120s drain, so the consolidate event is never missed.
    ( curl -sN --max-time 200 127.0.0.1:7800/events > /tmp/gx38_tsse.log 2>&1 ) & PIDS+=($!)
    sleep 1
    # ark-profile v1 (ark-profile.md §7.3): the web /teach now passes the
    # 共感 consent gate first. An ack-ONLY profile (consent != disclosure)
    # unlocks teaching without any pseudonym; bind to the served manifesto
    # id. This is the intended behavior change galaxy v1 reserved a seam for.
    local MID; MID=$(curl -s -D - -o /dev/null --max-time 5 127.0.0.1:7800/manifesto \
                     | grep -i 'X-Manifesto-Id' | tr -d '\r' | awk '{print $2}')
    curl -s --max-time 5 -d "ack=1&mid=$MID" 127.0.0.1:7800/profile >/dev/null
    # LM-8 (galaxy.c:1413): the web mouth moved from integer slots to real
    # WORDS resolved through the SHARED vocab (arch/common/r3_vocab.c). The
    # off-bias pair the integer cert used (k=2,v=3) IS, by construction, the
    # words sun->yellow: vk_img index 2 == "sun", vv_img index 3 == "yellow"
    # (the prefix ids never moved). So this is the SAME MEASURED pair
    # (pre_share 0.0% at N=100), now spoken through the LIVE API. yellow (id 3)
    # is NOT the untrained default (id 0 == "blue"), so recall stays falsifiable.
    # IMPORTANT (--max-time): the FIRST mind request lazily pretrains the R3
    # substrate (~23s host, PRINTED "[mind] substrate pretrained ... in N s"),
    # and that runs SYNCHRONOUSLY in the single galaxy server task — so the
    # teach POST must allow for it. (The old integer-slot cert never hit this:
    # k=2 was rejected as OOV *before* mind_cmd, so it never triggered a
    # pretrain. The valid word pair does — hence the generous bound here.)
    local R; R=$(curl -s --max-time 90 -d 'k=sun&v=yellow' 127.0.0.1:7800/teach)
    log "teach: $R"
    # accept check. FLAKE: the first teach POST can occasionally return an EMPTY
    # body though the teach landed (pending increments). So: trust the body's
    # "ok":true when present; else confirm via /galaxy.json pending; else retry
    # once. (assert on pending, not solely the teach response body.)
    local ACCEPT=0
    echo "$R" | grep -q '"ok":true' && ACCEPT=1
    if [ $ACCEPT -eq 0 ]; then
        local PEND; PEND=$(curl -s --max-time 30 127.0.0.1:7800/galaxy.json | grep -o '"pending":[0-9]*' | head -1 | grep -o '[0-9]*$')
        if [ -n "${PEND:-}" ] && [ "$PEND" -ge 1 ]; then ACCEPT=1; log "teach accepted via /galaxy.json pending=$PEND"; fi
    fi
    if [ $ACCEPT -eq 0 ]; then
        sleep 1
        R=$(curl -s --max-time 90 -d 'k=sun&v=yellow' 127.0.0.1:7800/teach)
        log "teach (retry): $R"
        echo "$R" | grep -q '"ok":true' && ACCEPT=1
    fi
    [ $ACCEPT -eq 1 ] || { fail "[galaxy-teach]" "teach not accepted (ok:true absent and pending<1)"; killall_nodes; return; }
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
    # ask returns the taught value, by WORD and by id. Falsifiable: if
    # teach->learn->recall failed, the answer defaults to id 0 == "blue", so
    # BOTH asserts fail (pred 3 != 0, word "yellow" != "blue").
    local A; A=$(curl -s --max-time 5 -d 'k=sun' 127.0.0.1:7800/ask)
    log "ask: $A"
    echo "$A" | grep -q '"pred":3' || { fail "[galaxy-teach]" "ask pred != 3 (taught value)"; killall_nodes; return; }
    echo "$A" | grep -q '"word":"yellow"' || { fail "[galaxy-teach]" "ask word != yellow (taught value)"; killall_nodes; return; }
    killall_nodes
    pass "[galaxy-teach]"
}

gate_serve
gate_events
gate_teach

echo "------------------------------------------------------------"
if [ $FAIL -eq 0 ]; then echo "galaxy_cert: ALL GATES PASS"; else echo "galaxy_cert: FAILURES ABOVE"; fi
exit $FAIL
