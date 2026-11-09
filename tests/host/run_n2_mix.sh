#!/bin/bash
# tests/host/run_n2_mix.sh — [n2-mix] old and new binaries in one swarm (ROADMAP N2,
# research/n2-wire16.md section 4). The minimum so far: X1 and X2, on the relay.
#
#   old 1,2,3 (N2_OLD_BIN) + new narrow 4,44 + new wide 260,300 (low bytes 4 and 44)
#   X1  the old nodes know {1,2,3,4,44} and nothing else (no phantom)
#   X2  the new narrow node knows all 7, 260 and 300 by their own numbers
#   XT  260 and 300 do not publish on a narrow node's one-char per-node topic (below)
#   XR  node 4 times the round trip to 260 and 300 (rtt= on their rows; not on an old relay)
#   XG  260 and 300 are region members of node 4 and of 260 (RTT <= tau; not on an old relay)
#
# N2_OLD_BIN  a p-kernel built from the last tree before N2 wire changes (master 64b7c909;
#             record the sha with any result). N2_NEW_BIN: default boot/linux_x86_64/p-kernel,
#             so an -DN2_NC_TRUNC build goes in there. N2_RELAY_BIN: default relay/relay.
# N2_WAIT     seconds to wait for membership, default 120. N2_LOGDIR keeps the logs.
# N2_NO_WIDE=1  leave 260 and 300 out: old and new narrow only, must be green already.
# N2_RELAY_OLD=1  the relay is an old one (N2_RELAY_BIN): 260 and 300 run but cannot get in. X1
#             and X2 then want the five narrow ids: a wide node on an old relay disturbs nobody.
#
# `nodes` prints ids from 0 (PKERNEL_NODE_ID 44 is row 43).
#
# Negative control: -DN2_NC_TRUNC cuts the wide ids to a byte, so 260 and 300 become 4 and 44.
# 260 and 300 are in SWIM's membership only (swim.c). N2_X3=1 adds X3 (a teach crosses the
# mixed swarm, below; in CI since the want tail, N2 (c)(3)). X4 is not written yet.
# N2_X3_PART=snow judges only X3's snow lines (wide -> new); fire (old -> wide) rides the want.
# N2_X3=1 also runs X3H (a wide sender's mind/have summary raises a want, below) after X3;
# N2_X3_PART=have runs X3H alone. N2_XP=1 adds XP (p-fs blocks between wide and narrow, below).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OLD="${N2_OLD_BIN:?N2_OLD_BIN: a p-kernel from before N2}"
NEW="${N2_NEW_BIN:-$ROOT/boot/linux_x86_64/p-kernel}"
REL="${N2_RELAY_BIN:-$ROOT/relay/relay}"
WAIT="${N2_WAIT:-120}"
NOWIDE="${N2_NO_WIDE:-0}"
PORT="${N2_PORT:-7595}"
[ -x "$OLD" ] && [ -x "$NEW" ] && [ -x "$REL" ] || { echo "[n2-mix] missing a binary"; exit 2; }

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_RTT_ZONE_SIZE
# XG judges region membership: a loaded CI box times localhost at 10..50 ms, around the
# 50 ms tau (CI-F, region-tau-env), so the test tau as in the multi-node certs
export PKERNEL_REGION_TAU_MS="${PKERNEL_REGION_TAU_MS:-250}"
WORK="$(mktemp -d /tmp/n2mix.XXXXXX)"
LOGDIR="${N2_LOGDIR:-$WORK/logs}"; mkdir -p "$LOGDIR"
declare -A PID FD
RH=0; REDS=""; NEXTFD=10

res() { echo "[n2-mix] $1 $2: $3"; [ "$2" = FAIL ] && REDS="$REDS $1"; return 0; }
cleanup() {
    for k in "${!PID[@]}"; do kill -9 "${PID[$k]}" 2>/dev/null; done
    [ "$RH" != 0 ] && kill -9 "$RH" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"/d* "$WORK"/f*
    [ -z "${N2_LOGDIR:-}" ] && rm -rf "$WORK"
}
trap cleanup EXIT

start() {  # <binary> <id>
    local bin="$1" i="$2" fifo="$WORK/f$2"
    mkfifo "$fifo"; mkdir -p "$WORK/d$i"
    env PKERNEL_NODE_ID="$i" PKERNEL_AUTONET=1 PKERNEL_PFS_DIR="$WORK/d$i" \
        PKERNEL_RELAY="127.0.0.1:$PORT" "$bin" < "$fifo" > "$LOGDIR/node$i.log" 2>&1 &
    PID[$i]=$!
    FD[$i]=$NEXTFD
    eval "exec $NEXTFD<>\"$fifo\""
    NEXTFD=$((NEXTFD + 1))
}
send() { printf '%s\n' "$2" >&"${FD[$1]}"; }
# The ids of the last `nodes` table (swim_nodes_print) of a log, alive or self, sorted.
known() {
    tr -d '\r' < "$LOGDIR/node$1.log" \
        | awk '/^\[cluster\]  ID/{s=""; next} /^ +[0-9]+  (SELF|ALIVE)/{s=s" "$1} END{print s}' \
        | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '
}

"$REL" -p "$PORT" > "$LOGDIR/relay.log" 2>&1 &
RH=$!
sleep 1
for i in 1 2 3; do start "$OLD" "$i"; sleep 0.2; done
WIDE="260 300"; [ "$NOWIDE" = 1 ] && WIDE=""
for i in 4 44 $WIDE; do start "$NEW" "$i"; sleep 0.2; done
for i in 1 2 3 4 44 $WIDE; do
    n=0
    while [ "$n" -lt 240 ] && ! grep -aq 'mind_net_task up' "$LOGDIR/node$i.log"; do sleep 0.5; n=$((n + 1)); done
    send "$i" "stop now"
done

WANT_A="0 1 2 3 43 "; WANT_B="0 1 2 3 43 259 299 "
[ "$NOWIDE" = 1 ] && WANT_B="$WANT_A"
[ "${N2_RELAY_OLD:-0}" = 1 ] && WANT_B="$WANT_A"
t=0; a=""; b=""
while [ "$t" -lt "$WAIT" ]; do
    sleep 3; t=$((t + 3))
    send 1 "nodes"; send 4 "nodes"; sleep 1
    a="$(known 1)"; b="$(known 4)"
    [ "$a" = "$WANT_A" ] && [ "$b" = "$WANT_B" ] && break
done

[ "$a" = "$WANT_A" ] && res X1 PASS "old node 1 knows {$a}" || res X1 FAIL "old node 1 knows {$a}, wanted {$WANT_A}"
[ "$b" = "$WANT_B" ] && res X2 PASS "new node 4 knows {$b}" || res X2 FAIL "new node 4 knows {$b}, wanted {$WANT_B}"

# XR  the v2 round trip comes back (audit-122: breaking the ACK, probe_target = me or seq 0,
#   left every check green). New node 4's last `nodes` table shows 259 and 299 with rtt=<n>ms.
#   Only with the wide nodes on a new relay, and only when X2 is green (else nothing to time).
wide_rtt() {  # the wide rows of node $1's last `nodes` table that carry an rtt, sorted
    tr -d '\r' < "$LOGDIR/node$1.log" \
        | awk '/^\[cluster\]  ID/{s=""; next} /^ +[0-9]+  ALIVE .* wide  rtt=[0-9]+ms/{s=s" "$1} END{print s}' \
        | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '
}
if [ -n "$WIDE" ] && [ "${N2_RELAY_OLD:-0}" != 1 ]; then
    if [ "$b" != "$WANT_B" ]; then
        res XR FAIL "not timed: X2 is red"
    else
        t=0; r="$(wide_rtt 4)"
        while [ "$t" -lt "$WAIT" ] && [ "$r" != "259 299 " ]; do
            sleep 3; t=$((t + 3)); send 4 "nodes"; sleep 1; r="$(wide_rtt 4)"
        done
        [ "$r" = "259 299 " ] && res XR PASS "new node 4 has a round-trip time for {$r}" \
                              || res XR FAIL "new node 4 has a round-trip time for {$r}, wanted {259 299 }"
    fi
fi

# XG  a wide peer within tau is a region member (N2b step (b), research/n2-wire16.md): node 4's
#   `region` lists node259 and node299, and wide 260 lists itself, node299 (wide to wide is
#   timed by the v2 PING as well) and the new narrow node3 and node43, not the old 0..2.
#   Only when XR is green (no rtt, no member). The tau is the
#   test one (PKERNEL_REGION_TAU_MS above), as in the multi-node certs.
#   Negative control: -DN2_NC_REGION_NARROW (the old loop over narrow ids only) reddens XG.
region_has() {  # the node ids in node $1's last `[region] members:` line, sorted
    tr -d '\r' < "$LOGDIR/node$1.log" | grep -a '^\[region\] members:' | tail -1 \
        | grep -oE 'node[0-9]+' | sed 's/^node//' | sort -n | tr '\n' ' '
}
region_size_of() {  # the size= in node $1's last `[region] id=` line
    tr -d '\r' < "$LOGDIR/node$1.log" | grep -a '^\[region\] id=' | tail -1 \
        | grep -oE 'size=[0-9]+' | sed 's/^size=//'
}
has_all() {  # 0 if the list $1 holds every id after it (each word on its own)
    local l=" $1" w; shift
    for w in "$@"; do case "$l" in *" $w "*) ;; *) return 1;; esac; done
    return 0
}
has_none() {  # 0 if the list $1 holds none of the ids after it
    local l=" $1" w; shift
    for w in "$@"; do case "$l" in *" $w "*) return 1;; esac; done
    return 0
}
has_wide() { has_all "$1" 259 299; }
# a wide self times the new narrow ones too (step (b2): it sends them a v2 PING, they answer);
# the old ones cannot answer v2, so they stay out of its region (no rtt is made up)
g260_ok() { has_all "$1" 3 43 259 299 && has_none "$1" 0 1 2; }
if [ -n "$WIDE" ] && [ "${N2_RELAY_OLD:-0}" != 1 ]; then
    if [ "${r:-}" != "259 299 " ]; then
        res XG FAIL "not judged: XR is red"
    else
        t=0; g4=""; g260=""
        while [ "$t" -lt "$WAIT" ]; do
            send 4 "region"; send 260 "region"; sleep 1
            g4="$(region_has 4)"; g260="$(region_has 260)"
            has_wide "$g4" && g260_ok "$g260" && break
            sleep 2; t=$((t + 3))
        done
        miss=""
        has_wide "$g4" || miss="$miss node4:{$g4}"
        g260_ok "$g260" || miss="$miss node260:{$g260}(wants 3 43 259 299, not 0 1 2)"
        # region_size counts the wide members too (audit-137: flagging a wide member without
        # counting it was green): the size= of the same print equals the listed members
        for n in 4 260; do
            l="$(region_has $n)"; c=$(wc -w <<< "$l"); sz="$(region_size_of $n)"
            [ "$sz" = "$c" ] || miss="$miss node$n:size=$sz but $c members {$l}"
        done
        [ -z "$miss" ] && res XG PASS "wide peers are region members: node4 {$g4}, node260 {$g260}, sizes match" \
                       || res XG FAIL "a wide peer is not a region member, or size= is off:$miss"
    fi
fi

# XT  a wide node does not publish on a narrow node's per-node topic (audit-120). world, reflex
#   and moe name a node's topic '0' + node in one char, vital the same: as a char 259 is '3'
#   (node 4's row) and 299 is '[' (node 44's row). A wide node may subscribe to the 64 narrow
#   names (one open each), but must not open its own cut name a second time as a publisher, nor
#   vital/ at all. Counted against row 20's name ('D', no node here), which only subs open.
if [ -n "$WIDE" ]; then
    xt=""
    for w in $WIDE; do
        c="$(printf "\\$(printf %03o $(( (48 + w - 1) % 256 )))")"
        for p in world/beacon reflex/alarm moe/score; do
            mine="$(tr -d '\r' < "$LOGDIR/node$w.log" | grep -aFc "[kdds] open  topic=\"$p/$c\"")"
            ref="$(tr -d '\r' < "$LOGDIR/node$w.log" | grep -aFc "[kdds] open  topic=\"$p/D\"")"
            [ "$mine" -gt "$ref" ] && xt="$xt $w:$p/$c($mine>$ref)"
        done
        grep -aqF '[kdds] open  topic="vital/' "$LOGDIR/node$w.log" && xt="$xt $w:vital"
    done
    [ -z "$xt" ] && res XT PASS "no wide node publishes on a narrow per-node topic" \
                 || res XT FAIL "a wide node publishes on a narrow name:$xt"
fi

# X3 (N2_X3=1 until a wide node has a mind): old 1 teaches fire, wide 300 teaches snow.
#   fire reaches every other node from node 0; snow reaches 4, 44 and 260 from node 299;
#   no old node holds snow at all (300's low byte is 44's: an old node must not read it as 44's).
#   Without the wide nodes (N2_NO_WIDE, N2_RELAY_OLD) only fire, over the five narrow ids.
#   N2_X3_PART=snow: judge the snow lines only (snow@4/44/260, snow-on-old, snow-not-from-299);
#   fire (old -> wide): a new node answers the wide asker's want (the want tail). Unset: all of X3.
if [ "${N2_X3:-0}" = 1 ]; then
    WIDE_IN="$WIDE"; [ "${N2_RELAY_OLD:-0}" = 1 ] && WIDE_IN=""
    X3_FIRE=1; X3_MAIN=1; X3_HAVE=1
    case "${N2_X3_PART:-}" in
        snow) X3_FIRE=0; X3_HAVE=0;;
        have) X3_MAIN=0;;
    esac
    [ -z "$WIDE_IN" ] && [ -n "${N2_X3_PART:-}" ] && { echo "[n2-mix] N2_X3_PART=${N2_X3_PART} needs the wide nodes"; exit 2; }
    arrived() { grep -aqE "remote teach arrived: \"$2\".* from node $3 " "$LOGDIR/node$1.log"; }
    # a stopped node refuses teaches, and a teach is sent once: wait until every node has
    # printed its resume (a fixed 2 s lost the race on a loaded box: 300 taught while 4 was
    # still stopped, and snow reached nobody; b5s5i kdw runs at load 10)
    resumed() { tr -d '\r' < "$LOGDIR/node$1.log" | grep -ac 'stop\] stopped 0 '; }
    declare -A R0
    for i in 1 2 3 4 44 $WIDE; do R0[$i]="$(resumed "$i")"; send "$i" "stop resume"; done
    t=0; late="?"
    while [ "$t" -lt 30 ] && [ -n "$late" ]; do
        sleep 1; t=$((t + 1)); late=""
        for i in 1 2 3 4 44 $WIDE; do [ "$(resumed "$i")" -gt "${R0[$i]}" ] || late="$late $i"; done
    done
    [ -n "$late" ] && echo "[n2-mix] X3 note: no resume line from:$late after 30 s"
    sleep 2
fi
if [ "${N2_X3:-0}" = 1 ] && [ "$X3_MAIN" = 1 ]; then
    send 1 "mind teach fire warm"
    [ -n "$WIDE_IN" ] && send 300 "mind teach snow white"
    t=0; miss="?"
    while [ "$t" -lt "$WAIT" ] && [ -n "$miss" ]; do
        sleep 3; t=$((t + 3)); miss=""
        if [ "$X3_FIRE" = 1 ]; then
            for i in 2 3 4 44 $WIDE_IN; do arrived "$i" fire 0 || miss="$miss fire@$i"; done
        fi
        if [ -n "$WIDE_IN" ]; then
            for i in 4 44 260; do arrived "$i" snow 299 || miss="$miss snow@$i"; done
        fi
    done
    for i in 1 2 3; do
        grep -aqE 'remote teach arrived: "snow"' "$LOGDIR/node$i.log" && miss="$miss snow-on-old@$i"
    done
    for i in 4 44 260; do   # snow from anyone but 299 (a cut id, 43 or 255) is wrong too
        grep -aE 'remote teach arrived: "snow"' "$LOGDIR/node$i.log" | grep -avqE ' from node 299 ' \
            && miss="$miss snow-not-from-299@$i"
    done
    X3_WHAT="fire from 0 everywhere${WIDE_IN:+, snow from 299 on the new}"
    [ "$X3_FIRE" = 0 ] && X3_WHAT="(snow part) snow from 299 on the new, none on the old"
    [ -z "$miss" ] && res X3 PASS "$X3_WHAT" \
                   || res X3 FAIL "missing or wrong:$miss"
fi

# X3H  a wide sender's mind/have summary raises a want (N2, research/n2-wire16.md "まだ" 1).
#   4, 44 and 260 are stopped (a stopped node takes no teach and polls no summary), then wide
#   300 teaches sky and after it sea. 300 re-drives only its LAST teach (m_republish_last),
#   so after the resume sea comes by the re-drive and sky only by a pull: some resumed node
#   must first see sky in 300's summary ("mind/have from node 299 ... -> want"; nobody else
#   holds sky, so the first such line is from 299), and then sky must arrive at all three from
#   299 (the want, the answer). Negative control: -DN2_NC_MH_NO_TAIL (300's summary says 0xFF
#   with no tail and every reader drops it) leaves both red. N2_X3_PART=have runs X3H alone.
if [ "${N2_X3:-0}" = 1 ] && [ -n "$WIDE_IN" ] && [ "$X3_HAVE" = 1 ]; then
    stops() { tr -d '\r' < "$LOGDIR/node$1.log" | grep -ac 'stop\] stopped 1 local 1'; }
    declare -A S0
    for i in 4 44 260; do S0[$i]="$(stops "$i")"; send "$i" "stop now"; done
    t=0; late="?"
    while [ "$t" -lt 30 ] && [ -n "$late" ]; do
        sleep 1; t=$((t + 1)); late=""
        for i in 4 44 260; do [ "$(stops "$i")" -gt "${S0[$i]}" ] || late="$late $i"; done
    done
    [ -n "$late" ] && echo "[n2-mix] X3H note: no stop line from:$late after 30 s"
    send 300 "mind teach sky blue"; sleep 3
    send 300 "mind teach sea deep"; sleep 3      # the re-drive now carries sea, not sky
    hint299() { grep -aqE '^(p-kernel> )*\[mind\] mind/have from node 299: key [0-9]+ \("sky"\) never held here -> want' "$LOGDIR/node$1.log"; }
    # node 4 resumes alone first: while 44 and 260 stay stopped nobody but 299 holds sky, so
    # node 4's first sky hint must name 299. Resuming all three at once raced (CI 42488883666,
    # first-sky-hint-not-299@4): a resumed peer that had already pulled sky put it in its own
    # summary first. The others resume after node 4's hint (or the wait), and only node 4's
    # first hint is judged (a truncated 299 reads 43, a real peer's id, so a peer's hint is no excuse)
    R0[4]="$(resumed 4)"; send 4 "stop resume"
    t=0; while [ "$t" -lt "$WAIT" ] && ! hint299 4; do sleep 1; t=$((t + 1)); done
    # The seconds alone measure mind/have only in the CI form (N2_X3=1): with N2_X3_PART=have,
    # sky is 300's first teach and its substrate pretrains first (15-18 s in the container, no
    # summary meanwhile), so both the old and the fixed reader wait ~29 s there (/build/mhd/ab)
    t4="$t"; echo "[n2-mix] X3H note: node 4 alone for ${t4} s (its hint from 299 or the wait, ${WAIT} s)"
    for i in 44 260; do R0[$i]="$(resumed "$i")"; send "$i" "stop resume"; done
    t=0; miss="?"
    while [ "$t" -lt "$WAIT" ] && [ -n "$miss" ]; do
        sleep 3; t=$((t + 3)); miss=""; h=""
        for i in 4 44 260; do hint299 "$i" && h="$h $i"; done
        [ -z "$h" ] && miss=" no-want-from-299"
        for i in 4 44 260; do arrived "$i" sky 299 || miss="$miss sky@$i"; done
    done
    # who printed node 4's first sky hint (it must name 299: nobody else held sky then)
    f="$(tr -d '\r' < "$LOGDIR/node4.log" | grep -aoE 'mind/have from node [0-9]+: key [0-9]+ \("sky"\)' | head -1)"
    case "$f" in *"node 299:"*) ;; *) miss="$miss first-sky-hint-not-299@4";; esac
    [ -z "$miss" ] && res X3H PASS "300's summary raised a want on{$h}, sky arrived from 299 at 4, 44, 260" \
                   || res X3H FAIL "missing or wrong:$miss"
    # CI keeps no node logs (CI 42656744519 was red here and green 7/7 on the box): on a red,
    # print what node 4 heard about sky and when it stopped and resumed, in log order, so the
    # next red says who came first (an empty first hint, another id, or sky with no hint)
    if [ -n "$miss" ]; then
        echo "[n2-mix] X3H diag: node 4's first sky hint {${f}}, node 4 alone ${t4} s"
        for i in 4 44 260; do
            tr -d '\r' < "$LOGDIR/node$i.log" \
                | grep -anE 'stop\] stopped|sky' | head -20 \
                | sed "s/^/[n2-mix] X3H diag node$i: /"
        done
    fi
fi

# XP  p-fs blocks cross the mixed swarm (N2 "まだ" 7, the first caller: pfs_repl). N2_XP=1.
#   Wide 300 puts a block: new 4, 44 and 260 replicate it (300's ANNOUNCE says 0xFF with the id
#   in a tail; their WANT reaches 300). Its origin there is n299, or n255 when the copy came
#   through an old node: an old node cannot carry a wide origin and holds the block as its own
#   (0xFF), so which a new node says depends on who streamed first (seen: 4 got it from an old
#   node first). The first copy anywhere comes from 300 itself (only 300 holds it, an old node
#   gets it from a new one), so at least one new node says n299 (the chunks' origin tail).
#   Nobody says n43 (299 cut to a byte) or any other id. New narrow 4 puts a block: wide 260 and
#   300 replicate it with origin=n3 (their WANT says 0xFF with the id in a tail, which 4 dropped
#   before); 44 too (narrow to narrow, green before).
#   Negative control: -DN2_NC_PFS_NO_TAIL (no tail is ever sent) leaves XP red.
#   Limit: a new -> new hop keeping 299 (44 -> 260 in one run) is not required (the race above).
if [ "${N2_XP:-0}" = 1 ] && [ -n "$WIDE" ] && [ "${N2_RELAY_OLD:-0}" != 1 ]; then
    putid() { tr -d '\r' < "$LOGDIR/node$1.log" | grep -aoE '\[pfs\] put len=[0-9]+  id=[0-9a-f]{16}' | tail -1 | sed 's/.*id=//'; }
    repl() { tr -d '\r' < "$LOGDIR/node$1.log" | grep -aqE "\[pfs\] replicated block id=$2  len=[0-9]+  origin=n($3)\$"; }
    send 300 "pfs put xp wide block from 300"
    send 4 "pfs put xp narrow block from 4"
    t=0; wid=""; nid=""
    while [ "$t" -lt 20 ] && { [ -z "$wid" ] || [ -z "$nid" ]; }; do
        sleep 1; t=$((t + 1)); wid="$(putid 300)"; nid="$(putid 4)"
    done
    if [ -z "$wid" ] || [ -z "$nid" ]; then
        res XP FAIL "no put line (300 {$wid}, 4 {$nid})"
    else
        t=0; miss="?"
        while [ "$t" -lt "$WAIT" ] && [ -n "$miss" ]; do
            sleep 3; t=$((t + 3)); miss=""
            for i in 4 44 260; do repl "$i" "$wid" '299|255' || miss="$miss wide@$i"; done
            for i in 44 260 300; do repl "$i" "$nid" 3 || miss="$miss narrow@$i"; done
        done
        n299=0
        for i in 1 2 3 4 44 260; do
            o="$(tr -d '\r' < "$LOGDIR/node$i.log" | grep -aoE "\[pfs\] replicated block id=$wid  len=[0-9]+  origin=n[0-9]+" | sed 's/.*origin=n//' | sort -u | tr '\n' ' ')"
            case "$i" in
                1|2|3) case " $o" in *" 43 "*) miss="$miss wide-as-43@$i";; esac;;
                *) case "$o" in ""|"299 "|"255 ") ;; *) miss="$miss wide-origin{$o}@$i";; esac
                   [ "$o" = "299 " ] && n299=$((n299 + 1));;
            esac
        done
        [ "$n299" -ge 1 ] || miss="$miss no-new-node-says-299"
        [ -z "$miss" ] && res XP PASS "300's block $wid on 4 44 260 ($n299 say n299, the rest n255 via an old node), 4's block $nid on 44 260 300 from n3" \
                       || res XP FAIL "missing or wrong:$miss"
    fi
fi

if [ -z "$REDS" ]; then echo "[n2-mix] ALL PASS"; exit 0; fi
echo "[n2-mix] RED:$REDS"
exit 1
