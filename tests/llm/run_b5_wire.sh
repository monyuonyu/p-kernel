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
# B5 ⓪ (research/b5-m-default.md §3 2):
#   -DB5W_NC_SEND_MOMENTS the published copy keeps mu, vu  -> W15 red (unit)
# B5 q8 (research/b5-q8-copy.md §4): unit Q0 Q4 (the int8 copy, BRG2);
# B5 q8 2 (research/b5-recv-mem.md §3): unit Q5 (the bytes a round holds);
#   -DB5R_NC_FULL_PEER (each peer buffer the full copy again) -> Q5 red
#   B5W_MODE=q8 runs the judgement seeds 261..268 (B5W_Q8_SEEDS), stage q8,
#   on plain and nq8, judge b5q8_check.py (Q1 Q2 Q3 and the NC)
#   -DB5Q_NC_INT2T    the copy is the int2 per-tensor grid  -> Q0 red (unit), Q2 red (judge)
# B5 q8 3 (research/b5-merge-mem.md §3): q7 (an L round merging seven peers,
# about 3 minutes and 1.4 GB, plain and its falsifier only);
#   -DB5M_NC_CAND     the candidate model as before             -> Q7 red
# B5 q8 4 (research/b5-merge-q8direct.md §3): q7 QD3 (<= 350 MB, the BRG2 peers
# merged from their int8 copies, st_q8_merge);
#   -DB5QD_NC_DECODE  the BRG2 peers decoded to fp32 as before  -> QD3 red (q7)
#   -DB5QD_MUT_ROWSCALE each row read with the row before's scale -> Q0 red (unit)
#   -DB5QD_MUT_GROWSKIP a BRG1 buffer that can not grow dropped, not refused -> Q0 red (unit)
#   -DB5QD_MUT_NOPEERFIN a BRG2 peer's own finite walk skipped     -> b5qd-F red (unit, audit-224)
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
build nolatch -DB5W_NC_NO_LATCH   # D6b (research/b5-m-round.md §10): W14 red
build nomom  -DB5W_NC_SEND_MOMENTS   # B5 ⓪ (research/b5-m-default.md §3 2): W15 red
build nq8    -DB5Q_NC_INT2T          # B5 q8 (research/b5-q8-copy.md §4): Q0 red, Q2 red
build nfull  -DB5R_NC_FULL_PEER      # B5 q8 2 (research/b5-recv-mem.md §3): Q5 red
build ncand  -DB5M_NC_CAND           # B5 q8 3 (research/b5-merge-mem.md §3): Q7 red
build ndec   -DB5QD_NC_DECODE        # B5 q8 4 (research/b5-merge-q8direct.md §3): QD3 red
build mrow   -DB5QD_MUT_ROWSCALE     # B5 q8 4: the int8 rows read with the scale of the row before -> Q0 red
build mgrow  -DB5QD_MUT_GROWSKIP     # B5 q8 4: a BRG1 buffer that can not grow is dropped, the round goes on -> Q0 red
build mfin   -DB5QD_MUT_NOPEERFIN    # audit-224: a BRG2 peer whose values overflow is not dropped on its own -> b5qd-F red

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
nc nolatch b5w-W14
nc nomom b5w-W15
nc nq8 b5q-Q0
nc nfull b5r-Q5
nc mrow b5q-Q0
nc mgrow b5q-Q0
nc mfin b5qd-F
unit nomean
echo "[b5-wire] nomean unit (not judged here): $(grep -c '^  FAIL  ' "$OUT/unit-nomean.log") FAIL lines"
# Q7 (research/b5-merge-mem.md §3): its own entry, plain and the falsifier only
"$WORK/bin-plain" q7 > "$OUT/q7-plain.log" 2>&1; rc=$?
grep -E '^  (PASS|FAIL) |^  Q7' "$OUT/q7-plain.log"
if [ "$rc" -eq 0 ]; then echo "[b5-wire] q7 plain: PASS"; else echo "[b5-wire] q7 plain: FAIL (rc=$rc)"; fail=1; fi
"$WORK/bin-ncand" q7 > "$OUT/q7-ncand.log" 2>&1
red="$(grep '^  FAIL  ' "$OUT/q7-ncand.log" | sed 's/^  FAIL  \[\([^]]*\)\].*/\1/' | sort -u | tr '\n' ' ')"
if [ "$red" = "b5m-Q7 b5m-QD3 " ] && grep -q '^  PASS  \[b5m-Q7\] precondition' "$OUT/q7-ncand.log"; then
    echo "[b5-wire] falsifier ncand: red on b5m-Q7 and b5m-QD3 only, the precondition green (as it must)"
else echo "[b5-wire] falsifier ncand: FAIL, red on '${red}', want 'b5m-Q7 b5m-QD3' with the precondition green"; fail=1; fi
# QD3 (research/b5-merge-q8direct.md §3): decoded to fp32 as before -> QD3 red only
"$WORK/bin-ndec" q7 > "$OUT/q7-ndec.log" 2>&1
red="$(grep '^  FAIL  ' "$OUT/q7-ndec.log" | sed 's/^  FAIL  \[\([^]]*\)\].*/\1/' | sort -u | tr '\n' ' ')"
if [ "$red" = "b5m-QD3 " ]; then echo "[b5-wire] falsifier ndec: red on b5m-QD3 only (as it must)"
else echo "[b5-wire] falsifier ndec: FAIL, red on '${red}', want 'b5m-QD3'"; fail=1; fi
if [ "${B5W_MODE:-}" = unit ]; then
    if [ "$fail" = 0 ]; then echo "[result] PASS  b5-wire unit"; exit 0; fi
    echo "[result] FAIL  b5-wire unit"; exit 1
fi
[ "$fail" = 0 ] || { echo "[result] FAIL  b5-wire unit (the judgement not run)"; exit 1; }

# stage 2, W2b (research/b5-wire2.md §3): a weak poison. B5W_MODE=w2b-cal runs
# the calibration seeds 209 210 at s 0.5 1 2 and names s; B5W_MODE=w2b runs
# the judgement seeds 211..218 at B5W_W2B_S (the calibrated s) and judges.
case "${B5W_MODE:-}" in
q8)
    python3 "$HERE/b5q8_check.py" --self-test > "$OUT/q8-self.log" 2>&1 \
        || { cat "$OUT/q8-self.log"; echo "[result] FAIL  b5-q8 judge self-test"; exit 1; }
    Q8SEEDS="${B5W_Q8_SEEDS:-261 262 263 264 265 266 267 268}"
    { for s in $Q8SEEDS; do echo "plain q8 q8 $s"; done
      for s in $Q8SEEDS; do echo "nq8 q8nc nq8 $s"; done; } \
        | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; t="$4"; g="$5"; s="$6"; "$w/bin-$b" "$h" "$s" "$t" > "$o/$g-$s.log" 2>&1; echo "[b5-q8] $b $t seed $s rc=$?"' \
            "$WORK" "$HERE/b1_held.txt" "$OUT"
    python3 "$HERE/b5q8_check.py" --seeds "$Q8SEEDS" "$OUT"
    exit $? ;;
w2b-cal)
    python3 "$HERE/b5_wire2_check.py" --self-test > "$OUT/w2b-self.log" 2>&1 \
        || { cat "$OUT/w2b-self.log"; echo "[result] FAIL  w2b judge self-test"; exit 1; }
    for sv in 0.5 1 2; do for s in 209 210; do echo "$sv $s"; done; done \
        | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; sv="$3"; s="$4"; "$w/bin-plain" "$h" "$s" "w2b:$sv" > "$o/cal-$sv-$s.log" 2>&1; echo "[b5-wire] w2b cal s $sv seed $s rc=$?"' \
            "$WORK" "$HERE/b1_held.txt" "$OUT"
    python3 "$HERE/b5_wire2_check.py" --cal "209 210" "0.5 1 2" "$OUT"
    exit $? ;;
w2b)
    SV="${B5W_W2B_S:?B5W_W2B_S (the calibrated s) is required}"
    W2SEEDS="${B5W_W2B_SEEDS:-211 212 213 214 215 216 217 218}"
    python3 "$HERE/b5_wire2_check.py" --self-test > "$OUT/w2b-self.log" 2>&1 \
        || { cat "$OUT/w2b-self.log"; echo "[result] FAIL  w2b judge self-test"; exit 1; }
    for s in $W2SEEDS; do echo "$s"; done \
        | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; sv="$3"; s="$4"; "$w/bin-plain" "$h" "$s" "w2b:$sv" > "$o/w2b-$s.log" 2>&1; echo "[b5-wire] w2b seed $s rc=$?"' \
            "$WORK" "$HERE/b1_held.txt" "$OUT" "$SV"
    python3 "$HERE/b5_wire2_check.py" --seeds "$W2SEEDS" "$OUT"
    exit $? ;;
esac

{ for s in $SEEDS; do echo "plain $s all"; done
  for s in $SEEDS; do echo "nomean $s w2"; done; } \
    | xargs -P "$JOBS" -L 1 sh -c 'w="$0"; h="$1"; o="$2"; b="$3"; s="$4"; st="$5"; "$w/bin-$b" "$h" "$s" "$st" > "$o/$b-$s.log" 2>&1; echo "[b5-wire] $b seed $s rc=$?"' \
        "$WORK" "$HERE/b1_held.txt" "$OUT"
python3 "$HERE/b5_wire_check.py" --seeds "$SEEDS" "$OUT"
