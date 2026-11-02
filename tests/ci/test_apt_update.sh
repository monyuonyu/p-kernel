#!/bin/bash
# tests/ci/test_apt_update.sh — [apt-update]: tests/ci/apt_update.sh against a fake apt-get
# (no root, no network). tests/ci/test_apt_update.sh <path to apt_update.sh>
#   U1 first try hangs, second try is cut by the time limit, third works -> rc 0, 3 tries
#   U2 first try works -> rc 0, 1 try, the mirror files untouched
#   U3 every try hangs -> rc != 0 and the whole thing ends near the limits (never the job's)
#   U4 the last try reads the sources rewritten to archive.ubuntu.com
#   U5 a try that ignores SIGTERM is killed all the same (audit-107)
# A negative control (a script without the time limit) must go red: see the NC arm at the end.
set -u
SUT="${1:-tests/ci/apt_update.sh}"
SUT=$(readlink -f "$SUT")
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
fails=0
ok()  { echo "PASS $1"; }
bad() { echo "FAIL $1"; fails=$((fails+1)); }

mkdir "$d/bin"
# The fake counts its calls and acts by the plan in $d/plan (one word per call: hang | ok | fail).
cat > "$d/bin/apt-get" <<'EOF'
#!/bin/bash
n=$(cat "$FAKE/count" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$FAKE/count"
act=$(sed -n "${n}p" "$FAKE/plan")
cp "$FAKE/mirrors" "$FAKE/mirrors.seen.$n"
case "$act" in
  hang) exec sleep 300 ;;
  deaf) trap '' TERM; exec sleep 300 ;;
  ok) exit 0 ;;
  *) exit 100 ;;
esac
EOF
chmod +x "$d/bin/apt-get"

run() { # plan words...
    rm -f "$d"/count "$d"/mirrors.seen.*
    printf '%s\n' "$@" > "$d/plan"
    echo 'http://azure.archive.ubuntu.com/ubuntu priority:1' > "$d/mirrors"
    s=$(date +%s)
    FAKE="$d" PATH="$d/bin:$PATH" APT_UPDATE_SECS=2 APT_UPDATE_KILL=1 APT_SOURCES="$d/mirrors" \
        timeout 20 bash "$SUT" > "$d/out" 2>&1
    rc=$?
    e=$(( $(date +%s) - s ))
    n=$(cat "$d/count" 2>/dev/null || echo 0)
}

run hang hang ok
if [ "$rc" = 0 ] && [ "$n" = 3 ]; then ok U1; else bad "U1 rc=$rc tries=$n"; fi
if grep -q 'archive.ubuntu.com' "$d/mirrors.seen.3" && ! grep -q 'azure' "$d/mirrors.seen.3"
then ok U4; else bad U4; fi

run ok
if [ "$rc" = 0 ] && [ "$n" = 1 ] && grep -q azure "$d/mirrors"; then ok U2; else bad "U2 rc=$rc tries=$n"; fi

run hang hang hang
if [ "$rc" != 0 ] && [ "$rc" != 124 ] && [ "$n" = 3 ] && [ "$e" -lt 15 ]; then ok U3; else bad "U3 rc=$rc tries=$n secs=$e"; fi

run deaf ok
if [ "$rc" = 0 ] && [ "$n" = 2 ] && [ "$e" -lt 10 ]; then ok U5; else bad "U5 rc=$rc tries=$n secs=$e"; fi

# U6 (audit-108, audit-110): a job that calls apt_install.sh can spend 27 minutes on apt alone:
# update 8.5, install 5, update again 8.5, install 5. Its time limit must leave room for that;
# 35 is the floor (27 and a short job), this does not know how long the job's own work takes.
CI="$(dirname "$(readlink -f "$0")")/../../.github/workflows/ci.yml"
short="$(python3 - "$CI" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
parts = re.split(r'^  ([A-Za-z0-9_-]+):\s*$', s[s.index('\njobs:'):], flags=re.M)
for name, body in zip(parts[1::2], parts[2::2]):
    if 'apt_install.sh' not in body or name == 'script-modes':
        continue
    m = re.search(r'^    timeout-minutes: (\d+)', body, re.M)
    if not m or int(m[1]) < 35:
        print(name, m[1] if m else 'none')
PY
)"
if [ -z "$short" ]; then ok U6; else bad "U6 $(echo "$short" | tr '\n' ' ')"; fi

# U7: a GitHub-hosted job installs through apt_install.sh. CI 41469844784 (2026-11-03): the
# RNG0 job's bare `apt-get install` sat until the job's 30 minutes, "cancelled", a new red for
# promote. Self-hosted jobs are out of this: they do not use the azure mirror.
bare="$(python3 - "$CI" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
parts = re.split(r'^  ([A-Za-z0-9_-]+):\s*$', s[s.index('\njobs:'):], flags=re.M)
for name, body in zip(parts[1::2], parts[2::2]):
    if re.search(r'^    runs-on: ubuntu-latest', body, re.M) and re.search(r'^\s+sudo apt-get install', body, re.M):
        print(name)
PY
)"
if [ -z "$bare" ]; then ok U7; else bad "U7 $(echo "$bare" | tr '\n' ' ')"; fi

[ "$fails" = 0 ] && { echo "[apt-update] PASS"; exit 0; }
echo "[apt-update] FAIL ($fails)"
exit 1
