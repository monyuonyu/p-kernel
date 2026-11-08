#!/bin/bash
# tests/ci/build_all.sh [logdir] — build (do not run) everything CI builds, with
# CI's own commands, one after another, and say which ones broke.
#
# Run it in a scratch tree before merging into the local master (inbox #27):
# the link errors of CI run 38093212146 (#22) were builds nobody had made by
# hand. Takes a few minutes. Exit 0 = every build passed; otherwise the number
# of broken builds. Logs: <logdir>/<name>.log (default: a mktemp dir).
#
# It cleans and rebuilds the tree it sits in (make clean in each boot dir).
#
# Covered: the tree checks of the script-modes job (doc-checks: V-MODEL, README
# counts, script modes, runner guard, CI exceptions), hosted x86_64 (plain, ASan+UBSan), every `make -C <dir> [-jN]
# EXTRA_CFLAGS=-D...` build written in ci.yml (tests/ci/test_build_all_list.py) (found by reading ci.yml: the
# anti-theater stubs, exit-dispatch / FP / no-switch falsifiers, R3WP and
# GEN_SURVIVE certs, ...), hosted aarch64 (cross), Windows PE (mingw-w64),
# bare i686 + ring3 userland (all disk), bare AArch64 (virt and rpi3), relay,
# and tests/llm/run_dmoe.sh (compiles its own translation units outside the
# Makefiles, broke in #22; it also runs its gates, ~40 s).
# NOT covered: the Android APK (needs the NDK and gradle; only its source list is
# checked, src-parity), and the builds that
# test scripts make themselves (falsifier kernels of run_tk_conform.sh,
# run_rng0_regression.sh, run_fp_bare.sh, run_nosw*.sh, tkconf rx, ...) or
# that ci.yml writes in another form (a variable, a different make variable);
# CI is still the judge for those. (run_ss3_blob.sh also broke in #22 but is not in CI: its
# [ss3-blob-merge] is a known red, see ci.yml.)
#
#   build_all.sh --list   print the "<dir>\t<flags>" pairs read from ci.yml and
#                         exit (BUILD_ALL_CI=<file> reads another file; used by
#                         tests/ci/test_build_all_list.py)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CIYML="${BUILD_ALL_CI:-$ROOT/.github/workflows/ci.yml}"
# Every `make -C <dir> EXTRA_CFLAGS=<-D...>` line written in ci.yml, read from
# ci.yml itself so a new one cannot be missed (audit-39: a hand-kept list had
# 3 of 14).
ci_pairs() {
python3 - "$CIYML" <<'PY'
import re, sys
seen = []
for line in open(sys.argv[1], encoding="utf-8"):
    # a step's name or a comment only talks about a build
    if re.match(r'\s*(#|(- )?name:)', line):
        continue
    # an optional -j / -jN between the dir and EXTRA_CFLAGS (audit-122: the
    # TRUNC and WIDEPUB lines have -j4); other words there are not read
    m = re.search(r'make -C (\S+)(?: -j\d*)* EXTRA_CFLAGS=("([^"]*)"|\'([^\']*)\'|(\S+))', line)
    if not m:
        continue
    flags = m.group(3) or m.group(4) or m.group(5) or ""
    if not flags.startswith("-D"):
        continue
    if (m.group(1), flags) not in seen:
        seen.append((m.group(1), flags))
for d, f in seen:
    print(d + "\t" + f)
PY
}
if [ "${1:-}" = "--list" ]; then
    ci_pairs
    exit
fi
LOG="${1:-$(mktemp -d)}"
mkdir -p "$LOG"
cd "$ROOT"
fails=0
failed=""
step() {  # $1 = name, rest = command (run with bash -eo pipefail, like CI)
    local name="$1"; shift
    local t0=$SECONDS
    if bash --noprofile --norc -eo pipefail -c "$*" > "$LOG/$name.log" 2>&1; then
        echo "[build] ok   $name ($((SECONDS - t0)) s)"
    else
        echo "[build] FAIL $name ($((SECONDS - t0)) s) — $LOG/$name.log"
        grep -a -m3 -E 'error|undefined reference' "$LOG/$name.log" | sed 's/^/         /'
        fails=$((fails + 1)); failed="$failed $name"
    fi
}

# The checks of the script-modes job that read the tree and need no toolchain
# (2026-11-06: ark-entry.md reached master with no V-MODEL row, CI 42086847022
# went red on check_vmodel.py --selftest, and build_all had been ALL OK for both
# the implementer and the auditor). Not the apt fixture (it times a fake apt-get).
# check_script_modes.py reads git's index: in a tree that is not a git checkout
# (the audit container builds from git archive) it is skipped with a line.
step doc-checks   "python3 tests/ci/check_vmodel.py --selftest && python3 tests/ci/check_vmodel.py \
                     && python3 tests/ci/check_readme_counts.py . \
                     && python3 tests/ci/check_runner_guard.py . && python3 tests/ci/check_ci_exceptions.py . \
                     && if git rev-parse --git-dir >/dev/null 2>&1; then python3 tests/ci/check_script_modes.py .; \
                        else echo 'check_script_modes: SKIPPED (not a git checkout)'; fi"
# The falsifier macros that tests/llm/*.sh build themselves (CI 42445849552).
step llm-falsifiers "python3 tests/ci/check_llm_falsifiers.py"
# The call sites the certs do not reach (test-holes H3 H4 H5 H6 H12, part of H2).
step callsites    "python3 tests/ci/check_callsites.py --selftest"
# The source lists of Android's CMake and both host Makefiles (the BLOCKING job
# "ソースリスト整合性"; audit-164: ark_crypto.c went into both Makefiles only, and
# build_all was ALL OK, since the APK is not built here).
step src-parity   "bash tools/android/check_parity.sh"
step x86_64      "make -C boot/linux_x86_64 clean >/dev/null && make -C boot/linux_x86_64"
step x86_64-san   "make -C boot/linux_x86_64 clean >/dev/null && make -C boot/linux_x86_64 NO_LIBTCC=1 \
                     EXTRA_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
                     LDFLAGS='-no-pie -lrt -lpthread -fsanitize=address,undefined'"
# The ci.yml builds (ci_pairs above). The sanitizer build above is the one
# with -f flags (it needs LDFLAGS).
while IFS=$'\t' read -r dir flags; do
    [ -n "$dir" ] || continue
    # A "/" in the flags (-include a/b.h) would make the log path a missing
    # directory and a 0 s false FAIL before make (audit-41), so it becomes "_".
    name="$(basename "$dir")$(echo "$flags" | sed 's/-D/-/g; s/ //g; s|/|_|g')"
    step "$name" "make -C $dir clean >/dev/null && make -C $dir EXTRA_CFLAGS='$flags'"
done < <(ci_pairs)
step x86_64-again "make -C boot/linux_x86_64 clean >/dev/null && make -C boot/linux_x86_64"
step aarch64      "make -C boot/linux clean >/dev/null && make -C boot/linux"
step windows      "make -C boot/windows/x86_64 clean >/dev/null && make -C boot/windows/x86_64"
step bare-x86     "make -C boot/x86 clean >/dev/null && make -C boot/x86 all disk"
step bare-rpi3    "make -C boot/aarch64 rpi3"
step bare-aarch64 "make -C boot/aarch64 clean >/dev/null && make -C boot/aarch64"
step relay        "make -C relay"
step dmoe         "bash tests/llm/run_dmoe.sh"

if [ "$fails" -eq 0 ]; then
    echo "[build] ALL OK (logs: $LOG)"
else
    echo "[build] $fails FAILED:$failed (logs: $LOG)"
fi
exit "$fails"
