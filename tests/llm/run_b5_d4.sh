#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_d4.sh - B5 D4 (research/b5-m-round.md §4, §5 W13): the student's
# chunks are not fsync'd one by one; one syncfs ends the publish / the fetch.
#
# A relay and two hosted nodes (flat store, PKERNEL_PFS_DIR). Node 1 trains a
# little and runs `baby region 7` alone (publish; nobody to fetch). Then node 2
# does the same: its one verb publishes its blob AND fetches node 1's. The verb
# says "[baby] region: fsync publish P merge F (chunks C)".
# The check, fixed before the code (2026-11-19):
#   W13  node 2: C >= 50, P <= 8, F <= 8 (the count does not grow with the
#        chunks), and its merge line has "fetched 1" (node 1's chunks came in
#        and re-hashed right; pfs_get checks every read against the id)
#   -> 2026-11-19 05:10 run: RED at e34128ac, P=11 F=1 with 5578 chunks (M). The 11
#      is per publish, not per chunk: 1 syncfs + one pfs_dag_save of the
#      descriptor (a content block and a manifest block, each with its .org, and
#      the ref: 5 durable writes x 2 fsyncs), read in pfs_dag.c. W13 is kept as it
#      was and only reported; the gate is W13b, fixed AFTER that run (weaker):
#   W13b node 2: C >= 50, P <= 11, F <= 1, "fetched 1"; and with the arm plainM
#        (tier M) also run, P(plain, S) == P(plainM, M): the count does not
#        follow the chunks
#   -> 2026-11-19 05:10 run: W13b RED too (M: P=19 on the 2nd run; the count is
#      process-wide, other tasks' durable writes enter it). Both kept, reported.
#   W13c (THE GATE, fixed 2026-11-19 20:3x BEFORE it ever ran): the verb also says
#        "lazy publish Lp merge Lf syncfs publish Sp merge Sf" = the fsyncs of the
#        lazy door only (the student's chunks and their .org) and the syncfs calls.
#        node 2, at tier S (plain) AND tier M (plainM): C >= 50, Lp == 0, Lf == 0,
#        Sp == 1, Sf == 1, and "fetched 1"
# Falsifier -DB5W_NC_FSYNC_EACH (every chunk fsync'd as before): W13c red BY THE
#        COUNT: Lp >= C and Lf >= C (else FAIL-OTHER, which is not the falsifier).
#   W13d (D4b, fixed 2026-11-20 01:4x BEFORE it ever ran, after audit-194 FAIL):
#        W13c AND the whole counts P < C/4 and F < C/4. W13c alone does not see
#        a chunk that skips the lazy door (audit-194 x1: P 963 F 965, Lp 0).
#        THE GATE is now W13d.
# Falsifier -DB5W_NC_HALF_LAZY (chunks with an odd id[0] skip the lazy door,
#        the x1 form): W13d red BY THE COUNT: P >= C/4 or F >= C/4 while W13c
#        alone is green (else FAIL-OTHER).
# The wait for node 2 to see node 1 (audit-194 ①): node 1 is "node0(" in node
#        2's members line (the number is ID-1; "node1(self)" is node 2 itself).
#        Not seen in 120 s -> the arm says WAIT-RED and is red, whatever follows.
# B5 q8 (research/b5-q8-copy.md, 2026-11-22): the node publishes the int8 copy;
#        at tier S that is 43 chunks (< the C >= 50 above), so the S arms (plain,
#        nc, nchalf) publish the fp copy (PKERNEL_B5_COPY=fp, 482 chunks, the
#        copy these lines were fixed on) and plainM publishes the int8 copy that
#        ships (M: 480 chunks). The lines are not changed.
# Arms: D4_ARMS="plain plainM nc nchalf" (default all). Ports D4_PORT (relay, default 7631).
# Output: "[b5-d4] <arm> W13c PASS|FAIL (...)", then ALL PASS (exit 0) or RED.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
B="$ROOT/boot/linux_x86_64"
case "$(uname -m)" in aarch64|arm64) B="$ROOT/boot/linux" ;; esac
ARMS="${D4_ARMS:-plain plainM nc nchalf}"
PORT="${D4_PORT:-7631}"
WORK="$(mktemp -d /tmp/b5d4.XXXXXX)"
PIDS=""
cleanup() { [ -n "$PIDS" ] && kill -9 $PIDS 2>/dev/null; rm -rf "$WORK"; make -C "$B" clean >/dev/null 2>&1; }
trap cleanup EXIT
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_NODE_SEED PKERNEL_GALAXY
# D4b (2026-11-20 01:4x): the test-only region tau, as samples/13 27 28 32 46 do.
# This test is about the fsync counts, not the region: at the product tau 50 ms,
# two copies of this test at once left node 1 out of node 2's region at the
# merge's start (plainM 2/2: members 1 fetched 0, right after a members line
# with node0 rtt=36ms; the M publish of 5578 chunks loads the box). That is
# the D6 subject (feat/b5-d6), not D4's. The gate W13d is unchanged.
export PKERNEL_REGION_TAU_MS="${PKERNEL_REGION_TAU_MS:-250}"
make -C "$ROOT/relay" > /dev/null 2>&1 || { echo "[b5-d4] relay build FAIL"; exit 1; }
REDS=""

lines() { grep -a "$2" "$1" 2>/dev/null | tr -d '\r'; }
wait_for() { # <log> <pattern> <count> <seconds>
    local n=0
    while [ "$n" -lt "$(( $4 * 2 ))" ]; do
        [ "$(grep -ac "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0
        sleep 0.5; n=$((n + 1))
    done
    return 1
}

PS=""; PM=""
arm() { # <name> <cflags> <expect PASS|FAIL> <tier>
    local a="$1" W="$WORK/$1" tier="$4" copy=q8
    [ "$tier" = S ] && copy=fp
    case " $ARMS " in *" $a "*) ;; *) REDS="$REDS $a-skipped"; return ;; esac
    mkdir -p "$W"
    make -C "$B" clean >/dev/null 2>&1
    if ! make -C "$B" EXTRA_CFLAGS="$2" > "$W/build.log" 2>&1; then
        tail -20 "$W/build.log"; echo "[b5-d4] $a build FAIL"; REDS="$REDS $a-build"; return
    fi
    cp "$B/p-kernel" "$W/p-kernel"
    "$ROOT/relay/relay" -p "$PORT" > "$W/relay.log" 2>&1 &
    local rp=$!; PIDS="$PIDS $rp"
    sleep 1
    local k
    for k in 1 2; do
        mkfifo "$W/f$k"; mkdir -p "$W/d$k"
        env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$W/d$k" \
            PKERNEL_RELAY=127.0.0.1:$PORT PKERNEL_DEVICE_TIER="$tier" PKERNEL_B5_COPY="$copy" "$W/p-kernel" < "$W/f$k" > "$W/n$k.log" 2>&1 &
        PIDS="$PIDS $!"
    done
    exec 7<>"$W/f1" 8<>"$W/f2"
    wait_for "$W/n1.log" 'mind_merge_task up' 1 180; wait_for "$W/n2.log" 'mind_merge_task up' 1 180
    printf 'baby 1 0.001 32\n' >&7; printf 'baby 1 0.002 32\n' >&8
    wait_for "$W/n1.log" 'held-out loss (after)' 1 900; wait_for "$W/n2.log" 'held-out loss (after)' 1 900
    sleep 5
    printf 'baby region 7\n' >&7
    wait_for "$W/n1.log" 'region: fsync publish' 1 900
    # node 2 must have node 1 in its region, or its verb fetches nothing (members 1)
    # (node 1 is "node0(" there: the number is ID-1, "node1(self)" is node 2)
    local i=0 waitred=""
    while [ "$i" -lt 60 ] && ! grep -aq '\[region\] members:.* node0(' "$W/n2.log"; do
        printf 'region\n' >&8; sleep 2; i=$((i + 1))
    done
    if ! grep -aq '\[region\] members:.* node0(' "$W/n2.log"; then
        echo "[b5-d4] $a WAIT-RED: node 2 never saw node 1 (node0) in its region in 120 s"
        waitred=1
    fi
    printf 'baby region 7\n' >&8
    wait_for "$W/n2.log" 'region: fsync publish' 1 1500
    local l1 l2 m2 c p f
    l1=$(lines "$W/n1.log" 'region: fsync publish' | tail -1)
    l2=$(lines "$W/n2.log" 'region: fsync publish' | tail -1)
    m2=$(lines "$W/n2.log" '\[baby\] region: gen 7' | tail -1)
    c=$(echo "$l2" | sed -n 's/.*(chunks \(-\{0,1\}[0-9]*\)).*/\1/p')
    p=$(echo "$l2" | sed -n 's/.*fsync publish \([0-9]*\) .*/\1/p')
    f=$(echo "$l2" | sed -n 's/.*fsync publish [0-9]* merge \([0-9]*\) .*/\1/p')
    local lp lf sp sf
    lp=$(echo "$l2" | sed -n 's/.* lazy publish \([0-9]*\) merge [0-9]* syncfs.*/\1/p')
    lf=$(echo "$l2" | sed -n 's/.* lazy publish [0-9]* merge \([0-9]*\) syncfs.*/\1/p')
    sp=$(echo "$l2" | sed -n 's/.* syncfs publish \([0-9]*\) merge [0-9]*$/\1/p')
    sf=$(echo "$l2" | sed -n 's/.* syncfs publish [0-9]* merge \([0-9]*\)$/\1/p')
    echo "[b5-d4] $a node1: $l1"
    echo "[b5-d4] $a node2: $l2"
    echo "[b5-d4] $a node2: $m2"
    local w13=FAIL v=FAIL
    if [ -n "$c" ] && [ -n "$p" ] && [ -n "$f" ] && [ "$c" -ge 50 ] && [ "$p" -le 8 ] && [ "$f" -le 8 ] \
       && echo "$m2" | grep -q 'fetched 1 '; then w13=PASS; fi
    echo "[b5-d4] $a W13 (reported, not the gate) $w13"
    if [ -n "$c" ] && [ -n "$p" ] && [ -n "$f" ] && [ "$c" -ge 50 ] && [ "$p" -le 11 ] && [ "$f" -le 1 ] \
       && echo "$m2" | grep -q 'fetched 1 '; then v=PASS; fi
    [ "$a" = plain ] && PS="$p"
    [ "$a" = plainM ] && PM="$p"
    echo "[b5-d4] $a W13b (reported, not the gate) $v (P=$p F=$f C=$c)"
    local wc=FAIL
    if [ -n "$c" ] && [ -n "$lp" ] && [ -n "$lf" ] && [ -n "$sp" ] && [ -n "$sf" ] && [ "$c" -ge 50 ] \
       && [ "$lp" -eq 0 ] && [ "$lf" -eq 0 ] && [ "$sp" -eq 1 ] && [ "$sf" -eq 1 ] \
       && echo "$m2" | grep -q 'fetched 1 '; then wc=PASS; fi
    if [ "$a" = nc ] && [ "$wc" = FAIL ] && { [ -z "$c" ] || [ -z "$lp" ] || [ -z "$lf" ] || [ "$lp" -lt "$c" ] || [ "$lf" -lt "$c" ]; }; then
        echo "[b5-d4] $a note: red, but not by the lazy fsync count (Lp=$lp Lf=$lf C=$c)"; wc=FAIL-OTHER
    fi
    echo "[b5-d4] $a W13c $wc (Lp=$lp Lf=$lf Sp=$sp Sf=$sf C=$c)"
    # W13d = W13c (as computed before the nc note) and P, F < C/4
    local wd=FAIL
    if [ -n "$c" ] && [ -n "$lp" ] && [ -n "$lf" ] && [ -n "$sp" ] && [ -n "$sf" ] && [ -n "$p" ] && [ -n "$f" ] \
       && [ "$c" -ge 50 ] && [ "$lp" -eq 0 ] && [ "$lf" -eq 0 ] && [ "$sp" -eq 1 ] && [ "$sf" -eq 1 ] \
       && [ "$((p * 4))" -lt "$c" ] && [ "$((f * 4))" -lt "$c" ] \
       && echo "$m2" | grep -q 'fetched 1 '; then wd=PASS; fi
    if [ "$a" = nc ] && [ "$wc" = FAIL-OTHER ]; then wd=FAIL-OTHER; fi
    if [ "$a" = nchalf ] && [ "$wd" = FAIL ]; then
        # red by the count only: W13c's own parts green, and P or F >= C/4
        if ! { [ -n "$c" ] && [ -n "$lp" ] && [ "$c" -ge 50 ] && [ "$lp" -eq 0 ] && [ "$lf" -eq 0 ] \
               && [ "$sp" -eq 1 ] && [ "$sf" -eq 1 ] && echo "$m2" | grep -q 'fetched 1 ' \
               && { [ "$((p * 4))" -ge "$c" ] || [ "$((f * 4))" -ge "$c" ]; }; }; then
            echo "[b5-d4] $a note: red, but not by P or F >= C/4 with W13c green (P=$p F=$f C=$c)"; wd=FAIL-OTHER
        fi
    fi
    [ -n "$waitred" ] && wd=WAIT-RED
    echo "[b5-d4] $a W13d (THE GATE) $wd (P=$p F=$f C=$c, C/4=$((${c:-0} / 4)))"
    [ "$wd" = "$3" ] || REDS="$REDS $a-W13d"
    printf 'exit\n' >&7; printf 'exit\n' >&8
    exec 7>&- 8>&-
    sleep 2
    kill -9 $PIDS 2>/dev/null; PIDS=""
    cp "$W/n1.log" "/tmp/b5_d4_${PORT}_${a}_n1.log"; cp "$W/n2.log" "/tmp/b5_d4_${PORT}_${a}_n2.log"
}

arm plain "" PASS S
arm plainM "" PASS M
arm nc "-DB5W_NC_FSYNC_EACH" FAIL S
arm nchalf "-DB5W_NC_HALF_LAZY" FAIL S
if [ -n "$PS" ] && [ -n "$PM" ]; then
    if [ "$PS" = "$PM" ]; then echo "[b5-d4] W13b same P at S and M ($PS)"
    else echo "[b5-d4] W13b P differs: S $PS M $PM (reported, not the gate)"; fi
fi
if [ -z "$REDS" ]; then echo "[b5-d4] ALL PASS"; exit 0; fi
echo "[b5-d4] RED:$REDS"; exit 1
