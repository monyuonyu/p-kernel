#!/usr/bin/env bash
# run_gate_unit.sh — JVM unit test of PKernelService's run-gate decision
# (android/app/src/main/java/io/pkernel/RunGate.kt). No Android SDK needed:
# RunGate.kt has no Android imports, so plain kotlinc + java run it.
#
# Usage: tests/android/run_gate_unit.sh [mode]
#   mode = (empty)          the real RunGate.kt — must PASS (rc 0)
#          nc_ignore_pierce negative: reconcile ignores the pierce  — must FAIL
#          nc_never_clear   negative: an open gate never ends it     — must FAIL
#          nc_pierce_always negative: background starts pierce too   — must FAIL
#          nc_b5_no_charging  negative: the B5 gate ignores charging — must FAIL
#          nc_b5_unknown_open negative: an unknown network opens it  — must FAIL
#   env KOTLINC = path to kotlinc (default: kotlinc on PATH).
# rc: 0 all pass, 1 a check failed, 2 setup error (no kotlinc / marker missing).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$root/android/app/src/main/java/io/pkernel/RunGate.kt"
test="$here/rungate/RunGateTest.kt"
mode="${1:-}"
kc="${KOTLINC:-kotlinc}"
if ! command -v "$kc" >/dev/null 2>&1; then
    echo "[rungate] ERROR: kotlinc not found (set KOTLINC)"; exit 2
fi
d="$(mktemp -d)"
trap 'rm -rf "$d"' EXIT
cp "$src" "$d/RunGate.kt"

# Each negative deletes/rewrites ONE marked line; a missing marker is a setup
# error (never a silent pass of the negative).
nc() {  # nc <marker> <sed-expr>
    if ! grep -q "$1" "$d/RunGate.kt"; then
        echo "[rungate] ERROR: marker $1 not in RunGate.kt"; exit 2
    fi
    sed -i "$2" "$d/RunGate.kt"
}
case "$mode" in
    "") ;;
    nc_ignore_pierce) nc 'NC-KEEP-PIERCE' '/NC-KEEP-PIERCE/d' ;;
    nc_never_clear)   nc 'NC-CLEAR-PIERCE' '/NC-CLEAR-PIERCE/d' ;;
    nc_pierce_always) nc 'NC-EXPLICIT-ONLY' 's/if (userExplicit) {.*NC-EXPLICIT-ONLY/if (true) {/' ;;
    nc_b5_no_charging) nc 'NC-B5-CHARGING' '/NC-B5-CHARGING/d' ;;
    nc_b5_unknown_open) nc 'NC-B5-UNKNOWN-CLOSED' 's/return false     \/\/ NC-B5-UNKNOWN-CLOSED/return true/' ;;
    *) echo "[rungate] ERROR: unknown mode $mode"; exit 2 ;;
esac

echo "[rungate] mode=${mode:-real} kotlinc=$("$kc" -version 2>&1 | tail -1)"
"$kc" -nowarn "$d/RunGate.kt" "$test" -include-runtime -d "$d/rg.jar" 2>&1 | grep -v '^warning' || true
[ -f "$d/rg.jar" ] || { echo "[rungate] ERROR: compile failed"; exit 2; }
set +e
java -cp "$d/rg.jar" io.pkernel.RunGateTestKt
rc=$?
set -e
[ "$rc" -eq 0 ] && exit 0
exit 1
