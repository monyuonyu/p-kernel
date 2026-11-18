#!/bin/bash
# tests/host/run_swarm_leave.sh — ROADMAP 0-14 S1: a node that leaves keeps
# nothing of the swarm (docs/architecture/30-module/swarm-leave.md).
#
# Expectations fixed before the code (2026-11-15, the design's section 3):
#   L1 own block 1 + foreign blocks (origin 7, 300) 2, then `leave`:
#      "[leave] foreign 2 ..."; after a restart `pfs origins` says foreign 0,
#      the two foreign block files are gone, the own block file is still there
#   L2 after `leave` mind.rw and ns1_student.bin are gone
#   L3 restarted with the mark and PKERNEL_RELAY set: "[leave] left" and no
#      "[net] transport = " line; after `join`, the next restart has one
#   L4 (the precondition) restarted WITHOUT `leave`, the foreign blocks are
#      still foreign 2 (master reloads every block as PFS_ORIGIN_SELF: red)
#   L5 a block whose <id>.org was removed is "unknown 1", and `leave` moves
#      it to left-unknown/ (not deleted)
#   (L1 also keeps an own record put with `pfs put` while on the net, audit-181)
# Added after audit-181 (FAIL), before the fix:
#   L6a after `leave`, a foreign block put in the same run (as the net's
#       receive does) is refused ("[leave] seed: refused"), origins foreign 0
#   L6b a foreign block on disk with the mark there is removed at the next
#       start ("[leave] left at start: foreign 1 removed"), file and .org gone
# The blocks come from a test build (-DSWARM_LEAVE_TEST adds
# `leave seed self|<origin>`, which prints "[leave] seed id=<64 hex> origin=self|<n>").
# Falsifiers (each must turn its check red, the plain build must be ALL PASS):
#   -DS1_NC_LEAVE_KEEP_FOREIGN -> L1   -DS1_NC_ORIGIN_VOLATILE -> L4
#   -DS1_NC_LEAVE_NO_FLAG      -> L3   -DS1_NC_SELF_BY_NUMBER -> L1
#   -DS1_NC_LEFT_ACCEPTS       -> L6a  -DS1_NC_LEFT_NO_SWEEP  -> L6b
#   -DS1_NC_RESTORE_NO_REFUSE  -> L6c (a start with the mark takes a foreign block; H28)
# Output: "[swarm-leave] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET
if [ -n "${LEAVE_WORK:-}" ]; then          # keep the logs (LEAVE_WORK=<empty dir>)
    WORK="$LEAVE_WORK"; mkdir -p "$WORK"
    trap 'make -C "$BOOT" clean >/dev/null 2>&1' EXIT
else
    WORK="$(mktemp -d /tmp/leave.XXXXXX)"
    trap 'rm -rf "$WORK"; make -C "$BOOT" clean >/dev/null 2>&1' EXIT
fi
PORT="${LEAVE_RELAY_PORT:-7491}"   # nothing listens there: the line is about choosing, not reaching
OWN_TEXT="s1-own-record-put-on-the-net"
OWN_ID=$(printf '%s' "$OWN_TEXT" | sha256sum | cut -c1-64)

build() {  # <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    make -C "$BOOT" EXTRA_CFLAGS="$1" > "$WORK/build.log" 2>&1
}
run() {  # <dir> <log> <relay 0|1> <commands...>
    local d="$1" log="$2" rl="$3"; shift 3
    printf '%s\n' "$@" exit > "$WORK/in.txt"
    if [ "$rl" = 1 ]; then
        # PKERNEL_AUTONET: the net comes up at boot (cmd_net), where the mark must stop it
        timeout 120 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$d" PKERNEL_RELAY="127.0.0.1:$PORT" \
            PKERNEL_RELAY_AUTOFALLBACK=0 PKERNEL_AUTONET=1 "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
    else
        timeout 120 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$d" "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
    fi
}
seedid() {  # <log> <origin>: the id the seed line printed
    grep -a "^\[leave\] seed id=[0-9a-f]* origin=$2\b" "$1" | head -1 | tr -d '\r' | sed 's/.*id=\([0-9a-f]*\).*/\1/'
}
origins() {  # <log> <n>: the n-th "[pfs] origins" line
    grep -a '^\[pfs\] origins ' "$1" | sed -n "${2}p" | tr -d '\r'
}

suite() {  # <tag> <cflags>: prints the checks, sets SUITE_REDS
    local tag="$1" cf="$2" reds="" d d2 d3 d4 s6 s7 s300 sself su l2
    r() { echo "[swarm-leave] $tag $1 $2"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    if ! build "-DSWARM_LEAVE_TEST $cf"; then
        r build FAIL; tail -20 "$WORK/build.log"; SUITE_REDS="build"; return
    fi
    d="$WORK/$tag-d"; mkdir -p "$d"
    # L1 (audit-181 #2): an own record put while on the net (drpc_my_node = 1),
    # so a later `leave` off the net (drpc_my_node = 0xFF) must still keep it
    run "$d" "$WORK/$tag-a0.log" 1 "pfs put $OWN_TEXT"
    grep -aq '^\[pfs\] put len=' "$WORK/$tag-a0.log" && grep -aq '^\[net\] transport = ' "$WORK/$tag-a0.log" \
        && [ -e "$d/$OWN_ID" ] || echo "[swarm-leave] $tag note: the on-net put did not land ($OWN_ID)"
    run "$d" "$WORK/$tag-a.log" 0 "leave seed self" "leave seed 7" "leave seed 300" "pfs origins"
    sself=$(seedid "$WORK/$tag-a.log" self); s7=$(seedid "$WORK/$tag-a.log" 7); s300=$(seedid "$WORK/$tag-a.log" 300)
    if [ "${#sself}" = 64 ] && [ "${#s7}" = 64 ] && [ "${#s300}" = 64 ]; then r seed PASS; else r seed FAIL; fi
    # L4: a restart without leave keeps the foreign origins
    run "$d" "$WORK/$tag-b.log" 0 "pfs origins"
    case "$(origins "$WORK/$tag-b.log" 1)" in *" foreign 2 "*) r L4 PASS ;; *) r L4 FAIL ;; esac
    # L1 + L2: leave (weights files planted first, by name)
    echo w > "$d/mind.rw"; echo w > "$d/ns1_student.bin"
    run "$d" "$WORK/$tag-c.log" 0 "leave" "join"
    # L2 right after the leave run: the restart below (joined again) is a newborn
    # that may save its own fresh weights, which is not the swarm's
    if [ ! -e "$d/mind.rw" ] && [ ! -e "$d/ns1_student.bin" ]; then l2=PASS; else l2=FAIL; fi
    run "$d" "$WORK/$tag-c2.log" 0 "pfs origins"
    if grep -aq '^\[leave\] foreign 2 ' "$WORK/$tag-c.log" \
       && case "$(origins "$WORK/$tag-c2.log" 1)" in *" foreign 0 "*) true ;; *) false ;; esac \
       && [ -n "$s7" ] && [ ! -e "$d/$s7" ] && [ -n "$s300" ] && [ ! -e "$d/$s300" ] \
       && [ -n "$sself" ] && [ -e "$d/$sself" ] && [ -e "$d/$OWN_ID" ]; then r L1 PASS; else r L1 FAIL; fi
    r L2 "$l2"
    # L6a (audit-181 #1): after `leave`, in the same run, a foreign block that
    # arrives (the same pfs_put_origin as the net's receive) is not kept
    d3="$WORK/$tag-d3"; mkdir -p "$d3"
    run "$d3" "$WORK/$tag-k.log" 0 "leave" "leave seed 300" "pfs origins"
    if grep -aq '^\[leave\] seed: refused' "$WORK/$tag-k.log" \
       && case "$(origins "$WORK/$tag-k.log" 1)" in *" foreign 0 "*) true ;; *) false ;; esac; then
        r L6a PASS; else r L6a FAIL; fi
    # L6b: a foreign block on disk while the mark is there (a leave that died
    # half way, or one that came in before the mark) is removed at the next start
    d4="$WORK/$tag-d4"; mkdir -p "$d4"
    run "$d4" "$WORK/$tag-m.log" 0 "leave seed 300"
    s6=$(seedid "$WORK/$tag-m.log" 300)
    printf 'PKLEFT1\n' > "$d4/swarm-left.st"
    run "$d4" "$WORK/$tag-n.log" 0 "pfs origins" "leave seed 301"
    # H28 (audit-182): a start with the mark keeps refusing foreign blocks in that run
    if grep -aq '^\[leave\] seed: refused' "$WORK/$tag-n.log"; then r L6c PASS; else r L6c FAIL; fi
    if grep -aq '^\[leave\] left at start: foreign 1 removed' "$WORK/$tag-n.log" \
       && case "$(origins "$WORK/$tag-n.log" 1)" in *" foreign 0 "*) true ;; *) false ;; esac \
       && [ -n "$s6" ] && [ ! -e "$d4/$s6" ] && [ ! -e "$d4/$s6.org" ]; then r L6b PASS; else r L6b FAIL; fi
    # L3: the mark keeps the node off the net; join lifts it
    run "$d" "$WORK/$tag-e0.log" 0 "leave"
    run "$d" "$WORK/$tag-e.log" 1 "join"
    run "$d" "$WORK/$tag-f.log" 1 "stop"
    if grep -aq '^\[leave\] left: ' "$WORK/$tag-e.log" &&! grep -aq '^\[net\] transport = ' "$WORK/$tag-e.log" \
       && grep -aq '^\[net\] transport = ' "$WORK/$tag-f.log"; then r L3 PASS; else r L3 FAIL; fi
    # L5: a block without its .org is unknown and moved aside, not deleted
    d2="$WORK/$tag-d2"; mkdir -p "$d2"
    run "$d2" "$WORK/$tag-g.log" 0 "leave seed 7"
    su=$(seedid "$WORK/$tag-g.log" 7)
    [ -n "$su" ] && rm -f "$d2/$su.org"
    run "$d2" "$WORK/$tag-h.log" 0 "pfs origins" "leave" "join"
    if case "$(origins "$WORK/$tag-h.log" 1)" in *" unknown 1"*) true ;; *) false ;; esac \
       && grep -aq '^\[leave\] foreign 0 .* unknown 1 ' "$WORK/$tag-h.log" \
       && [ -n "$su" ] && [ -e "$d2/left-unknown/$su" ] && [ ! -e "$d2/$su" ]; then r L5 PASS; else r L5 FAIL; fi
    SUITE_REDS="$reds"
}

FAIL=0
suite plain ""
[ -z "$SUITE_REDS" ] || { echo "[swarm-leave] plain red:$SUITE_REDS"; FAIL=1; }
for nc in "S1_NC_LEAVE_KEEP_FOREIGN L1" "S1_NC_ORIGIN_VOLATILE L4" "S1_NC_LEAVE_NO_FLAG L3" \
          "S1_NC_SELF_BY_NUMBER L1" "S1_NC_LEFT_ACCEPTS L6a" "S1_NC_LEFT_NO_SWEEP L6b" "S1_NC_RESTORE_NO_REFUSE L6c"; do
    set -- $nc
    suite "$1" "-D$1"
    case " $SUITE_REDS " in
        *" $2 "*) echo "[swarm-leave] $1 turns $2 red (as it must)" ;;
        *) echo "[swarm-leave] $1 does NOT turn $2 red"; FAIL=1 ;;
    esac
done
if [ "$FAIL" = 0 ]; then echo "[swarm-leave] ALL PASS"; exit 0; fi
echo "[swarm-leave] RED"; exit 1
