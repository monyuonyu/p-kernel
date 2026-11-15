#!/bin/bash
# tests/host/run_ark_e5.sh — ark-entry.md §6 E5: a record whose owner did not
# consent never reaches the learner (cradle's lesson ring).
#
# Expectations fixed before the code (2026-11-15, ark-entry.md §6):
#   E5a two 200 B records (fill seeds 3 and 5), only 0 consented: `ark rec teach`
#       says records=1 skipped=1 bytes=200, ring = sha256(seed-3 body)[:16]
#   E5b `learn 1 yes`: records=2 bytes=401, ring = seed 3 + "\n" + seed 5;
#       `learn 1 no`: seed 3 alone again; `log 1` (newest first) learn=no,yes,no
#   E5c nothing consented: "teach nothing consented", `cradle probe` ring_len unchanged
#   E5d after a restart the consent is the last one set (teach = seed 3 alone)
#   E5e (added 2026-11-15, before its code) withdrawing a consent (`learn 0 no`) clears
#       the ring when it still holds the lesson teach made: ring_len 200 -> 0
# Falsifiers (each must turn its check red; the plain build must be ALL PASS):
#   -DARK_REC_LEARN_ALL -> E5a      -DARK_REC_LEARN_NOVER -> E5d
#   -DARK_REC_NO_WITHDRAW -> E5e
# Output: "[ark-e5] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET
if [ -n "${E5_WORK:-}" ]; then WORK="$E5_WORK"; mkdir -p "$WORK"; trap 'make -C "$BOOT" clean >/dev/null 2>&1' EXIT
else WORK="$(mktemp -d /tmp/arke5.XXXXXX)"; trap 'rm -rf "$WORK"; make -C "$BOOT" clean >/dev/null 2>&1' EXIT; fi

sha16() {  # <seed> [<seed>]: sha256 of the fill bodies joined by "\n", 16 hex
    python3 -c '
import hashlib, sys
def body(seed, n=200): return bytes(((seed + i*7 + (i>>9)*13) & 255) for i in range(n))
print(hashlib.sha256(b"\n".join(body(int(s)) for s in sys.argv[1:])).hexdigest()[:16])' "$@"
}
build() { make -C "$BOOT" clean >/dev/null 2>&1; make -C "$BOOT" EXTRA_CFLAGS="$1" > "$WORK/build.log" 2>&1; }
run() {  # <tag> <log> <commands...>: one node on the ARK image <tag>.img
    local tag="$1" log="$2"; shift 2
    printf '%s\n' "$@" exit > "$WORK/in.txt"
    timeout 120 env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$WORK/$tag.img" PKERNEL_ARK_SECTORS=8192 \
        PKERNEL_PFS_DIR="$WORK/$tag.dir" "$BOOT/p-kernel" < "$WORK/in.txt" 2>&1 | tr -d '\r' \
        | sed 's/^\(p-kernel> \)*//' > "$log"
}
teach() { grep -a '\[ark-rec\] teach ' "$1" | sed -n "${2}p"; }   # the n-th teach line

suite() {  # <tag> <cflags>
    local tag="$1" reds="" l
    r() { echo "[ark-e5] $tag $1 $2"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    if ! build "$2"; then r build FAIL; tail -20 "$WORK/build.log"; SUITE_REDS=build; return; fi
    local A3 A35 L="$WORK/$tag"
    A3=$(sha16 3); A35=$(sha16 3 5)
    run "$tag" "$L-1.log" "ark rec fill 1 200 3" "ark rec fill 1 200 5" "cradle probe" \
        "ark rec learn 0 no" "ark rec learn 1 no" "ark rec teach" "cradle probe" \
        "ark rec learn 0 yes" "ark rec teach" \
        "ark rec learn 1 yes" "ark rec teach" \
        "ark rec learn 1 no" "ark rec teach" "ark rec log 1"
    l=$(teach "$L-1.log" 2)
    case "$l" in *"records=1 skipped=1 "*"bytes=200 ring=$A3"*) r E5a PASS ;; *) r E5a FAIL ;; esac
    if case "$(teach "$L-1.log" 3)" in *"records=2 "*"bytes=401 ring=$A35"*) true ;; *) false ;; esac \
       && case "$(teach "$L-1.log" 4)" in *"records=1 "*"ring=$A3"*) true ;; *) false ;; esac \
       && [ "$(grep -a '^\[ark-rec\] log no=1 ' "$L-1.log" | grep -o 'learn=[a-z]*' | tr '\n' ' ')" = "learn=no learn=yes learn=no learn=no " ]; then
        r E5b PASS; else r E5b FAIL; fi
    # E5c: the first teach (nothing consented) leaves the lesson length as it was
    local c1 c2
    c1=$(grep -ao '^\[cradle-live\] ring_len=[0-9]*' "$L-1.log" | sed -n 1p); c2=$(grep -ao '^\[cradle-live\] ring_len=[0-9]*' "$L-1.log" | sed -n 2p)
    if teach "$L-1.log" 1 | grep -q 'nothing consented' && [ -n "$c1" ] && [ "$c1" = "$c2" ]; then r E5c PASS; else r E5c FAIL; fi
    run "$tag" "$L-2.log" "ark rec teach"
    case "$(teach "$L-2.log" 1)" in *"records=1 "*"ring=$A3"*) r E5d PASS ;; *) r E5d FAIL ;; esac
    # E5e: withdrawing a consent clears the ring when it still holds what teach put there
    run "$tag" "$L-3.log" "ark rec teach" "cradle probe" "ark rec learn 0 no" "cradle probe"
    local e1 e2
    e1=$(grep -ao '^\[cradle-live\] ring_len=[0-9]*' "$L-3.log" | sed -n 1p); e2=$(grep -ao '^\[cradle-live\] ring_len=[0-9]*' "$L-3.log" | sed -n 2p)
    if [ "$e1" = "[cradle-live] ring_len=200" ] && [ "$e2" = "[cradle-live] ring_len=0" ] \
       && grep -aq '^\[ark-rec\] learn: the lesson ring held' "$L-3.log"; then r E5e PASS; else r E5e FAIL; fi
    SUITE_REDS="$reds"
}

FAIL=0
suite plain ""
[ -z "$SUITE_REDS" ] || { echo "[ark-e5] plain red:$SUITE_REDS"; FAIL=1; }
for nc in "ARK_REC_LEARN_ALL E5a" "ARK_REC_LEARN_NOVER E5d" "ARK_REC_NO_WITHDRAW E5e"; do
    set -- $nc
    suite "$1" "-D$1"
    case " $SUITE_REDS " in
        *" $2 "*) echo "[ark-e5] $1 turns $2 red (as it must)" ;;
        *) echo "[ark-e5] $1 does NOT turn $2 red"; FAIL=1 ;;
    esac
done
if [ "$FAIL" = 0 ]; then echo "[ark-e5] ALL PASS"; exit 0; fi
echo "[ark-e5] RED"; exit 1
