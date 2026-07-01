#!/bin/bash
# ---------------------------------------------------------------------------
# run.sh — the soul test for ARK (arch/common/arkfs.c).
#
# Proves the three properties that make ARK fit p-kernel's "library that does
# not perish" (docs/architecture/30-module/survival-fs.md):
#
#   (a) CRUD + versioning   create/read/update; the OLD version survives in
#                           the append-only log (readv recovers it).
#   (b) CRASH CONSISTENCY   a writer SIGKILL'd mid-commit (real power loss,
#                           injected in the block device) NEVER corrupts the
#                           store: a fresh process remounts to either the last
#                           committed version in full, or the new one in full
#                           — never a half-written state. Tested at EVERY
#                           device-write point of an update.
#   (c) CONTENT-ADDRESS     dedup (same bytes stored once) + self-verify
#                           (a flipped byte is caught on read, fsck-free).
#
# Exit 0 = all PASS; non-zero = at least one assertion failed.
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
BIN="$WORK/arkfs_test"
IMG="$WORK/ark.img"
FAILS=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
hr()   { printf -- '-------------------------------------------------------------\n'; }
pass() { printf '  PASS  %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS+1)); }

# ---- build -----------------------------------------------------------------
say "[build] compiling ARK host harness ..."
"$CC" -Wall -Wextra -std=gnu11 -O1 -DARK_HOST_TEST \
    -I"$ROOT/arch/common/include" -I"$ROOT/relay" \
    "$HERE/arkfs_test.c" "$ROOT/arch/common/arkfs.c" "$ROOT/relay/sha256.c" \
    -o "$BIN" || { say "build failed"; exit 1; }

run() { "$BIN" "$@"; }      # returns child's exit code

# ===========================================================================
hr; say "(0) in-process RAM self-test (logic-level)"; hr
if run selftest; then pass "ark_self_test"; else fail "ark_self_test"; fi

# ===========================================================================
hr; say "(a) CRUD + versioning — old version survives the new write"; hr
run format "$IMG" 256 >/dev/null
run write "$IMG" /report.txt "ALPHA-the-first-recording" >/dev/null
run write "$IMG" /report.txt "BETA-revised-second-version" >/dev/null
run write "$IMG" /report.txt "GAMMA-third-and-current" >/dev/null

cur="$(run read "$IMG" /report.txt)"
ver="$(run version "$IMG" /report.txt)"
old1="$(run readv "$IMG" /report.txt 1)"
old2="$(run readv "$IMG" /report.txt 2)"
say "  $cur"; say "  $ver"; say "  $old1"; say "  $old2"
[ "$cur" = "READ: GAMMA-third-and-current" ] && pass "current = v3" || fail "current wrong: $cur"
[ "$ver" = "VERSION: 3" ]                    && pass "version counter = 3" || fail "version: $ver"
[ "$old1" = "READV(1): ALPHA-the-first-recording" ] && pass "v1 still recoverable (library)" || fail "v1 lost: $old1"
[ "$old2" = "READV(2): BETA-revised-second-version" ] && pass "v2 still recoverable" || fail "v2 lost: $old2"
say "  $(run history "$IMG" /report.txt | head -1)"

# directories (entries are created implicitly by their full path)
run write "$IMG" /logs/a.txt "aaa" >/dev/null
run write "$IMG" /logs/b.txt "bbb" >/dev/null
ls="$(run ls "$IMG" /logs)"
say "  $ls"
echo "$ls" | grep -q "a.txt" && echo "$ls" | grep -q "b.txt" && pass "readdir lists children" || fail "readdir: $ls"

# ===========================================================================
hr; say "(c) content-address: dedup + self-verify"; hr
run format "$IMG" 256 >/dev/null
b0="$(run blocks "$IMG" | sed 's/BLOCKS: //')"
run write "$IMG" /one.txt "duplicate-payload-XYZ" >/dev/null
run write "$IMG" /two.txt "duplicate-payload-XYZ" >/dev/null     # same bytes
b1="$(run blocks "$IMG" | sed 's/BLOCKS: //')"
say "  blocks before=$b0 after two identical writes=$b1"
# two files, identical content -> exactly ONE new data block (dedup)
[ "$b1" = "$((b0+1))" ] && pass "identical content stored once (dedup)" || fail "dedup: $b0 -> $b1"

# self-verify: corrupt a byte somewhere in the log payload region and read.
run format "$IMG" 256 >/dev/null
run write "$IMG" /v.txt "this-block-will-be-rotted-on-disk" >/dev/null
# byte offset: sector 4 (first data block payload) + a few bytes in.
OFF=$(( 4 * 512 + 10 ))
run corrupt "$IMG" "$OFF" >/dev/null
rd="$(run read "$IMG" /v.txt)"; rc=$?
say "  read after rot injection: $rd (rc=$rc)"
[ "$rd" = "CORRUPT" ] && pass "self-verify caught the flipped byte on read" || fail "rot not detected: $rd"

# ===========================================================================
hr; say "(b) CRASH CONSISTENCY — SIGKILL the writer mid-commit, remount"; hr
# Establish a durable v1, then try to write v2 while a real SIGKILL hits the
# block device at write point #k, for every k. After each crash the store must
# remount to EITHER v1-in-full OR v2-in-full — never anything else.
run format "$IMG" 256 >/dev/null
run write "$IMG" /x.txt "COMMITTED-ALPHA" >/dev/null
base="$(run read "$IMG" /x.txt)"
say "  baseline durable: $base"
[ "$base" = "READ: COMMITTED-ALPHA" ] || fail "baseline write failed"

crash_ok=1
for mode in TORN BEFORE; do
  for k in 1 2 3 4 5 6 7 8; do
    # fresh image each time so every run starts from the same durable v1
    run format "$IMG" 256 >/dev/null
    run write "$IMG" /x.txt "COMMITTED-ALPHA" >/dev/null
    # attempt v2 with a power-loss injected at device write #k
    env "ARK_KILL_${mode}=$k" "$BIN" write "$IMG" /x.txt "DOOMED-BETA" \
        >/dev/null 2>"$WORK/inj.log"
    wrc=$?
    # remount in a brand-new process and read
    after="$(run read "$IMG" /x.txt)"; arc=$?
    av="$(run version "$IMG" /x.txt)"
    if [ "$after" = "READ: COMMITTED-ALPHA" ] && [ "$av" = "VERSION: 1" ]; then
      verdict="rolled back to v1 (intact)"
    elif [ "$after" = "READ: DOOMED-BETA" ] && [ "$av" = "VERSION: 2" ]; then
      verdict="committed v2 fully (intact)"
    else
      verdict="*** BROKEN: '$after' $av rc=$arc ***"; crash_ok=0
    fi
    printf '  kill=%-7s#%d  writer_rc=%-3d  remount: %s\n' "$mode" "$k" "$wrc" "$verdict"
  done
done
[ "$crash_ok" = 1 ] && pass "every crash point left a clean, fully-consistent store" \
                     || fail "a crash point produced a corrupt/torn state"

# one explicit torn-commit trace for the report (fresh image, single run)
hr; say "    raw injection trace of one torn-commit run:"; hr
run format "$IMG" 256 >/dev/null
run write "$IMG" /y.txt "GOOD-V1" >/dev/null
say "    before crash: $(run read "$IMG" /y.txt) / $(run version "$IMG" /y.txt)"
# write #4 is the commit's payload sector -> a torn COMMIT.
env ARK_KILL_TORN=4 "$BIN" write "$IMG" /y.txt "BAD-V2" 1>/dev/null 2>"$WORK/t.log"
trc=$?
say "    writer exited rc=$trc (137 = SIGKILL); device-level injection log:"
sed 's/^/      /' "$WORK/t.log"
say "    remount in a fresh process after the torn commit:"
say "      $(run read "$IMG" /y.txt)"
say "      $(run version "$IMG" /y.txt)"
tafter="$(run read "$IMG" /y.txt)"
[ "$tafter" = "READ: GOOD-V1" ] && pass "torn commit rolled back to GOOD-V1" \
                                || fail "torn-commit rollback: $tafter"

# ===========================================================================
hr
if [ "$FAILS" -eq 0 ]; then
  say "ALL PASS — ARK is content-addressed, versioned, and crash-safe."
  exit 0
else
  say "FAILURES: $FAILS"
  exit 1
fi
