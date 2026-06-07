#!/bin/bash
# ===========================================================================
# 30_ark_crash / run.sh  -  ARK crash-safety fuzzer (wave 15, ARK-fuzz隊).
#
# Honestly tests ARK's claim: "a fresh mount yields either the last committed
# version complete, or the new version complete, and NEVER serves corrupt
# data" -- under PERTURBATIONS the existing prefix-only harness cannot model:
# write reordering / tail drop, torn sectors with a surviving commit, and
# media bit-rot (incl. the un-replicated superblock).
#
# It builds the EXISTING samples/25 `arkfs_test` binary (which links the real
# arch/common/arkfs.c) exactly the way samples/25 does, and drives ARK ONLY
# through that binary's stable CLI verbs -- this harness's own source never
# includes arkfs.h nor calls any ark_* API, so it stays decoupled from ARK's
# evolving on-disk format. Perturbation is generic at the 512 B sector level.
#
# Exit 0 = ARK never served corrupt data and never wedged (SAFE-reject runs
#          are tolerated, degraded-but-safe).
# Exit 1 = at least one BUG run (corrupt bytes served, or store wedged).
#          This is the regression gate ARK-2 must turn fully green.
#
# Env: CC (default cc), ARK_FUZZ_SEED, ARK_FUZZ_SECTORS, ARK_FUZZ_RUNS.
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
SEED="${ARK_FUZZ_SEED:-1337}"
SECTORS="${ARK_FUZZ_SECTORS:-256}"
RUNS="${ARK_FUZZ_RUNS:-64}"

WORK="$(mktemp -d)"
BIN="$WORK/arkfs_test"
IMG="$WORK/ark.img"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# ---- build the existing samples/25 host harness (NOT our own arkfs link) ----
SRC="$ROOT/samples/25_survival_fs/arkfs_test.c"
if [ ! -f "$SRC" ]; then
    echo "FATAL: $SRC not found (samples/25 is the driver this fuzzer reuses)."
    exit 2
fi
echo "[build] compiling samples/25 arkfs_test (the black-box ARK driver) ..."
"$CC" -w -std=gnu11 -O1 -DARK_HOST_TEST \
    -I"$ROOT/arch/common/include" -I"$ROOT/relay" \
    "$SRC" "$ROOT/arch/common/arkfs.c" "$ROOT/relay/sha256.c" \
    -o "$BIN" || { echo "build failed"; exit 2; }

# ---- run the fuzzer --------------------------------------------------------
PY="${PYTHON:-python3}"
"$PY" "$HERE/fuzz.py" --bin "$BIN" --img "$IMG" \
    --seed "$SEED" --sectors "$SECTORS" --runs "$RUNS"
exit $?
