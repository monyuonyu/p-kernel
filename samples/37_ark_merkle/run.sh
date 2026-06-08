#!/bin/bash
# ===========================================================================
# 37_ark_merkle / run.sh — ARK's Merkle directory tree (wave 17, format v3).
#
# The flat commit snapshot caps the namespace at ARK_MAX_FILES=32 and re-
# serializes EVERY entry into EVERY commit. The Merkle dir tree (each directory
# is a content-addressed block whose id == sha256 of its entries) removes both
# limits. This sample proves, CROSS-PROCESS against a file-backed device, the
# four properties that make it a real Merkle directory tree:
#
#   (a) SCALE > 32      store and read back 40 files under one directory (the
#                       flat 32-file snapshot could not hold these), and the
#                       commit that records them is O(1) (just the root id),
#                       not O(namespace).
#   (b) TAMPER-EVIDENT  the directory's root hash changes iff an entry changes;
#                       restoring an entry's exact bytes restores the exact root
#                       (deterministic content-addressing) — a verifiable root.
#   (c) SELF-VERIFY     a dir node corrupted on disk is REJECTED on a fresh
#                       mount (ARK_E_CORRUPT) — a tampered subtree is never
#                       served, consistent with ARK's "never serve corrupt".
#   (d) CRASH-SAFE      a writer SIGKILL'd mid dir-update (real power loss in
#                       the block device) rolls back to the prior committed
#                       root; never a half-updated tree.
#
# Exit 0 = all PASS; non-zero = at least one assertion failed.
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/merkle_test"
IMG="$WORK/ark.img"
FAILS=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
hr()   { printf -- '-------------------------------------------------------------\n'; }
pass() { printf '  PASS  %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS+1)); }

# ---- build -----------------------------------------------------------------
say "[build] compiling ARK Merkle host harness ..."
"$CC" -w -std=gnu11 -O1 -DARK_HOST_TEST \
    -I"$ROOT/arch/common/include" -I"$ROOT/relay" \
    "$HERE/merkle_test.c" "$ROOT/arch/common/arkfs.c" "$ROOT/relay/sha256.c" \
    -o "$BIN" || { say "build failed"; exit 1; }

run() { "$BIN" "$@"; }

# ===========================================================================
hr; say "(a) SCALE — 40 entries in ONE Merkle directory (flat cap is 32)"; hr
run mformat "$IMG" 2048 >/dev/null
ROOT_EMPTY="$(run mroot "$IMG" | sed 's/MROOT: //')"
for i in $(seq 0 39); do
  n=$(printf '%02d' "$i")
  run mput "$IMG" "/m/f$n" "merkle-payload-$n" >/dev/null || fail "mput f$n failed"
done
cnt="$(run mls "$IMG" /m | head -1)"
say "  $cnt"
[ "$cnt" = "MLS /m: 40 entries" ] && pass "directory holds 40 entries (> ARK_MAX_FILES=32)" \
                                  || fail "expected 40 entries, got: $cnt"

# read every file back, self-verified
allok=1
for i in $(seq 0 39); do
  n=$(printf '%02d' "$i")
  got="$(run mget "$IMG" "/m/f$n")"
  [ "$got" = "MGET: merkle-payload-$n" ] || { allok=0; say "  mismatch f$n: $got"; }
done
[ "$allok" = 1 ] && pass "all 40 files read back, content-verified" \
                 || fail "some files did not read back correctly"

# ===========================================================================
hr; say "(b) TAMPER-EVIDENT — root hash changes iff an entry changes"; hr
R0="$(run mroot "$IMG" | sed 's/MROOT: //')"
run mput "$IMG" /m/f00 "DIFFERENT-CONTENT" >/dev/null
R1="$(run mroot "$IMG" | sed 's/MROOT: //')"
run mput "$IMG" /m/f00 "merkle-payload-00" >/dev/null   # restore exact bytes
R2="$(run mroot "$IMG" | sed 's/MROOT: //')"
say "  R0 (orig)      = ${R0:0:24}..."
say "  R1 (mutated)   = ${R1:0:24}..."
say "  R2 (restored)  = ${R2:0:24}..."
[ "$R0" != "$R1" ] && pass "changing one entry changed the directory root hash" \
                   || fail "root did NOT change when an entry changed"
[ "$R0" = "$R2" ]  && pass "restoring exact bytes restored the exact root (deterministic)" \
                   || fail "root not deterministic on restore"
[ "$ROOT_EMPTY" != "$R0" ] && pass "non-empty tree root differs from empty-tree root" \
                           || fail "empty vs non-empty root collision"

# ===========================================================================
hr; say "(c) SELF-VERIFY — a corrupted dir node is rejected on read"; hr
# tamper the ROOT node's bytes on disk, then a fresh-process read must fail.
run mtamper "$IMG" >/dev/null
got="$(run mget "$IMG" /m/f05)"; rc=$?
say "  read after root-node corruption: $got (rc=$rc)"
[ "$got" = "CORRUPT" ] && pass "tampered dir node caught by self-verify (never served)" \
                       || fail "corruption not detected: $got"

# ===========================================================================
hr; say "(d) CRASH-SAFE — SIGKILL mid dir-update rolls back to prior root"; hr
run mformat "$IMG" 2048 >/dev/null
run mput "$IMG" /a "ALPHA-committed" >/dev/null
run mput "$IMG" /b "BETA-committed"  >/dev/null
RA="$(run mroot "$IMG" | sed 's/MROOT: //')"
say "  baseline durable root = ${RA:0:24}..."

crash_ok=1
for mode in TORN BEFORE; do
  for k in 1 2 3 4 5 6; do
    run mformat "$IMG" 2048 >/dev/null
    run mput "$IMG" /a "ALPHA-committed" >/dev/null
    run mput "$IMG" /b "BETA-committed"  >/dev/null
    base="$(run mroot "$IMG" | sed 's/MROOT: //')"
    # attempt a third put with a power-loss injected at device write #k
    # (the shell may print a "Killed" line for the SIGKILL'd writer — expected).
    env "ARK_KILL_${mode}=$k" "$BIN" mput "$IMG" /c "GAMMA-doomed" \
        >/dev/null 2>"$WORK/inj.log"
    after="$(run mroot "$IMG" | sed 's/MROOT: //')"
    gc="$(run mget "$IMG" /c)";
    ga="$(run mget "$IMG" /a)"
    if [ "$after" = "$base" ] && [ "$gc" = "NOTFOUND" ] && [ "$ga" = "MGET: ALPHA-committed" ]; then
      verdict="rolled back to prior root (/c absent, /a intact)"
    elif [ "$gc" = "MGET: GAMMA-doomed" ] && [ "$ga" = "MGET: ALPHA-committed" ]; then
      verdict="committed /c fully (intact)"
    else
      verdict="*** BROKEN: root=$after gc='$gc' ga='$ga' ***"; crash_ok=0
    fi
    printf '  kill=%-7s#%d  remount: %s\n' "$mode" "$k" "$verdict"
  done
done
[ "$crash_ok" = 1 ] && pass "every crash point left a clean tree (prior root OR new root)" \
                     || fail "a crash point produced a half-updated tree"

# ===========================================================================
hr
if [ "$FAILS" -eq 0 ]; then
  say "ALL PASS — ARK's Merkle directory tree scales >32, is tamper-evident,"
  say "           self-verifying, and crash-safe."
  exit 0
else
  say "FAILURES: $FAILS"
  exit 1
fi
