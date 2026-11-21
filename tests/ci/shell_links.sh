#!/bin/bash
# tests/ci/shell_links.sh — do the test programs that pull in student_shell.c
# still LINK? (work queue BA-SHELL, audit-185.)
#
# CI 43854354327 went red because student_shell.c started calling
# gl_student_publish / region_contains / drpc_my_node, and the tests that
# #include it (run_yield.sh, run_held_indep.sh, ...) build their own programs
# without gossip_learn.c / region.c / drpc.c. build_all.sh did not see it: it
# builds the Makefiles, not what test scripts build themselves. B5W's
# implementer and auditor both had build_all ALL OK.
#
# This runs each such script with CC=tests/ci/link_only_cc.sh: every compile and
# link is real, but a program that linked is replaced by a stub that exits 0,
# so the certs do not run (seconds, not minutes). The script's own verdict is
# ignored; we read only the link log.
#
# Which scripts: tests/llm/*.sh that take ${CC:-...} and name student_shell.c or
# a tests/llm/*.c that #includes it, and tests/host/*.sh that take ${CC:-...}
# and name a tests/host/*.c that #includes a product .c (found from the tree
# each time, so a new one is not missed).
# A script is RED if any of its links failed, or if it linked nothing (it did
# not reach its build: we would be blind to it).
# Audit-187: the pick is wider than the target. run_depth_compound, run_ss1 and
# run_ss3 only grep student_shell.c and never link it (an undefined call put
# into student_shell.c turned exactly the other 6 red). Harmless, a few seconds.
# A script stops at its first failed link, so its later links are seen only
# after that one is fixed.
#   shell_links.sh            -> "[shell-links] <script> ok|RED (...)", exit = number of RED
#   shell_links.sh --list     -> the scripts it would run
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1
WRAP="$ROOT/tests/ci/link_only_cc.sh"
pick() {
python3 - <<'PY'
import glob, re, os
inc = [os.path.basename(c) for c in sorted(glob.glob("tests/llm/*.c"))
       if re.search(r'#\s*include\s*"[^"]*student_shell\.c"', open(c, encoding="utf-8", errors="replace").read())]
names = ["student_shell.c"] + inc
for s in sorted(glob.glob("tests/llm/*.sh")):
    t = open(s, encoding="utf-8", errors="replace").read()
    if "${CC:-" in t and any(n in t for n in names):
        print(s)
# 11-21: tests/host/*.c that #include a product .c (relay_unicast_test.c takes
# net_relay.c) went red the same way in CI 44838481621 (N2C3 added a call to
# pk_node_wide_id, the test had no stand-in) and build_all did not see it.
host = [os.path.basename(c) for c in sorted(glob.glob("tests/host/*.c"))
        if re.search(r'#\s*include\s*"\.\./\.\./(arch|kernel|relay)/[^"]*\.c"', open(c, encoding="utf-8", errors="replace").read())]
for s in sorted(glob.glob("tests/host/*.sh")):
    t = open(s, encoding="utf-8", errors="replace").read()
    if "${CC:-" in t and any(n in t for n in host):
        print(s)
PY
}
if [ "${1:-}" = "--list" ]; then pick; exit 0; fi
LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT
red=0; n=0
while read -r s; do
    [ -n "$s" ] || continue
    n=$((n + 1))
    b="$(basename "$s" .sh)"
    L="$LOGDIR/$b.links"; : > "$L"
    LINK_ONLY_LOG="$L" CC="$WRAP" timeout 300 bash "$s" > "$LOGDIR/$b.out" 2>&1
    ok=$(grep -c '^LINK ok' "$L"); bad=$(grep -c '^LINK FAIL' "$L")
    if [ "$bad" -gt 0 ] || [ "$ok" -eq 0 ]; then
        echo "[shell-links] $b RED (links ok $ok, failed $bad)"
        grep -a -m4 -E 'undefined reference|error:' "$LOGDIR/$b.out" | sed 's/^/    /'
        red=$((red + 1))
    else
        echo "[shell-links] $b ok (links $ok)"
    fi
done < <(pick)
if [ "$n" -eq 0 ]; then echo "[shell-links] RED: no script found"; exit 1; fi
echo "[shell-links] $n scripts, $red RED"
exit "$red"
