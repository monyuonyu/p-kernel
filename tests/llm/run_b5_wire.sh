#!/bin/bash
# ---------------------------------------------------------------------------
# run_b5_wire.sh - B5 wire, stage 1 (research/b5-wire.md): the resident
# student merges with its region (st_region_round). student_region_test.c,
# judge b5_wire_check.py.
#
#   ./run_b5_wire.sh            unit (W0 W3-W6, the three falsifier builds),
#                               then the judgement seeds (W1 W2, NC_MEAN)
#   B5W_MODE=unit               the unit part only (seconds; CI)
#   B5W_SEEDS="181 ... 188"     the judgement seeds (fixed in the design)
#   B5W_OUT=<dir>  B5W_JOBS=3
#
# The unit links the real transport (gossip_learn/pfs_*, the ARK backend) the
# way run_ss3_blob.sh does, SOLO in one process; the arms read the blobs from
# a table in memory (W0 shows the two give the same bytes).
# Falsifiers (each must turn its line red and only it):
#   -DB5W_NC_NO_GEN   the generation is not read        -> W4 red (unit)
#   -DB5W_NC_NO_GATE  every candidate replaces live     -> W6 red (unit)
#   -DB5W_NC_MEAN     st_round_merge instead of the trim -> W2 red (judge)
# stage 2 (research/b5-wire2.md §3):
#   -DB5W_NC_NO_LEN   the envelope's length is not checked -> W7 red (unit)
#   -DB5W_NC_NO_DEDUP members are not de-duplicated       -> W8 red (unit)
#   -DB5W_NC_NO_CAP   every peer is fetched (no K = 7)     -> W9 red (unit)
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="${B5W_OUT:-$WORK/out}"
SEEDS="${B5W_SEEDS:-181 182 183 184 185 186 187 188}"
JOBS="${B5W_JOBS:-3}"
mkdir -p "$OUT"

AC="$ROOT/arch/common"
AL="$ROOT/arch/linux"
LLM="$AC/llm"
INCS="-I$ROOT/boot/linux -I$AL/aarch64/include -I$AL/include -I$ROOT/arch/aarch64/include \
      -I$AC/include/lp64 -I$AC/include -I$ROOT/relay \
      -I$ROOT/kernel/mtkernel3/kernel/knlinc -I$ROOT/kernel/mtkernel3/kernel/tkernel \
      -I$ROOT/kernel/mtkernel3/include -I$ROOT/kernel/mtkernel3/include/tm \
      -I$ROOT/kernel/mtkernel3/include/compat -I$ROOT/kernel/mtkernel3/config"
KDEFS="-D_APP_AARCH64_ -D_APP_LINUX_ -D_TK_HOSTED_LIBC_ -D_LINUX_AARCH64_ -DPK_NOSW_OFF"
KFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -fno-stack-protector -fno-common \
        -std=gnu11 -ffp-contract=off -ffunction-sections -fdata-sections -Werror=vla $KDEFS $INCS"
PFLAGS="-Wall -Wextra -g -O1 -no-pie -fno-pie -std=gnu11 -ffp-contract=off \
        -ffunction-sections -fdata-sections -Werror=vla -I$LLM"

python3 "$HERE/b5_wire_check.py" --self-test > "$OUT/self.log" 2>&1 \
    || { cat "$OUT/self.log"; echo "[result] FAIL  b5-wire judge self-test"; exit 1; }
echo "[b5-wire] judge self-test ok ($(grep -c ' ok ' "$OUT/self.log") cases)"

echo "[build] b5-wire: the transport TUs (kernel-hosted), then plain nogen nogate nomean"
for s in "$AC/gossip_learn.c" "$AC/pfs_block.c" "$AC/pfs_repl.c" "$AC/pfs_dag.c" "$AC/arkfs.c" \
         "$AL/pfs_ark.c" "$AL/pfs_durable.c" "$ROOT/relay/sha256.c"; do
    o="$WORK/k-$(basename "$s" .c).o"
    # shellcheck disable=SC2086
    $CC $KFLAGS -c "$s" -o "$o" || { echo "[build] FAILED on $s"; exit 1; }
done
# shellcheck disable=SC2086
$CC $PFLAGS -c "$HERE/student_blob_solo_shim.c" -o "$WORK/shim.o" || { echo "[build] FAILED on the shim"; exit 1; }
build() {  # build <name> <flags...>
    local n="$1"; shift
    local d="$WORK/$n"; mkdir -p "$d"
    for s in "$LLM/student.c" "$LLM/held_indep.c" "$LLM/cradle.c" "$HERE/student_region_test.c"; do
        # shellcheck disable=SC2086
        $CC $PFLAGS "$@" -c "$s" -o "$d/$(basename "$s" .c).o" || { echo "[build] $n FAILED on $s"; exit 1; }
    done
    $CC -no-pie -Wl,--gc-sections "$d"/*.o "$WORK"/shim.o "$WORK"/k-*.o -lm -o "$WORK/bin-$n" \
        || { echo "[build] $n link FAILED"; exit 1; }
}
build plain
build nogen  -DB5W_NC_NO_GEN
build nogate -DB5W_NC_NO_GATE
build nomean -DB5W_NC_MEAN
build nolen  -DB5W_NC_NO_LEN
build nodedup -DB5W_NC_NO_DEDUP
build nocap  -DB5W_NC_NO_CAP

unit() {  # unit <name>: its own ARK image
    PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$WORK/ark-$1.img" PKERNEL_ARK_SECTORS=262144 \
        "$WORK/bin-$1" unit > "$OUT/unit-$1.log" 2>&1
}
fail=0
unit plain; rc=$?
grep -E '^  (PASS|FAIL) |^  W' "$OUT/unit-plain.log"
if [ "$rc" -eq 0 ]; then echo "[b5-wire] unit plain: PASS"; else echo "[b5-wire] unit plain: FAIL (rc=$rc)"; fail=1; fi
nc() {  # nc <name> <the one id that must be red>
    unit "$1"
    local red; red="$(grep '^  FAIL  ' "$OUT/unit-$1.log" | sed 's/^  FAIL  \[\([^]]*\)\].*/\1/' | sort -u | tr '\n' ' ')"
    if [ "$red" = "$2 " ]; then echo "[b5-wire] falsifier $1: red on $2 only (as it must)"
    else echo "[b5-wire] falsifier $1: FAIL, red on '${red}', want '$2'"; fail=1; fi
}
nc nogen b5w-W4
nc nogate b5w-W6
nc nolen b5w-W7
nc nodedup b5w-W8
nc nocap b5w-W9
unit nomean
echo "[b5-wire] nomean unit (not judged here): $(grep -c '^  FAIL  ' "$OUT/unit-nomean.log") FAIL lines"
if [ "${B5W_MODE:-}" = unit ]; then
    if [ "$fail" = 0 ]; then echo "[result] PASS  b5-wire unit"; exit 0; fi
    echo "[result] FAIL  b5-wire unit"; exit 1
fi
[ "$fail" = 0 ] || { echo "[result] FAIL  b5-wire unit (the judgement not run)"; exit 1; }

{ for s in $SEEDS; do echo "plain $s all"; done
  for s in $SEEDS; do echo "nomean $s w2"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; "$w/bin-$b" "$h" "$s" "$st" > "$o/$b-$s.log" 2>&1; echo "[b5-wire] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_wire_check.py" --seeds "$SEEDS" "$OUT"
