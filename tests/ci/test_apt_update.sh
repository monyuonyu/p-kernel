#!/bin/bash
# tests/ci/test_apt_update.sh — [apt-update]: tests/ci/apt_update.sh against a fake apt-get
# (no root, no network). tests/ci/test_apt_update.sh <path to apt_update.sh>
#   U1 first try hangs, second try is cut by the time limit, third works -> rc 0, 3 tries
#   U2 first try works -> rc 0, 1 try, the mirror files untouched
#   U3 every try hangs -> rc != 0 and the whole thing ends near the limits (never the job's)
#   U4 the last try reads the sources rewritten to archive.ubuntu.com
#   U5 a try that ignores SIGTERM is killed all the same (audit-107)
#   U4b the same for any <region>.archive.ubuntu.com mirror (the runners' ec2 one, 2026-11-19)
#   U8 apt_install.sh's second install reads the rewritten sources too
#   U9 apt_install.sh waits for a dpkg its cut left running (holding the lock) before the retry
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
  # U9: the dpkg it started outlives the cut (timeout signals its own process group; the
  # runner's dpkg lived on, CI 45188947825, so this one is out of that group)
  orphan) setsid "$FAKE/bin/dpkg" hold 3 & exec sleep 300 ;;
  lockok) [ -e "$FAKE/lock" ] && exit 100; exit 0 ;;     # U9: an install that needs the dpkg lock
  ok) exit 0 ;;
  *) exit 100 ;;
esac
EOF
chmod +x "$d/bin/apt-get"
# U9's fake dpkg: `dpkg hold <s>` holds the lock (a file) for s seconds, as the real dpkg does
# while it sets up packages; `dpkg --configure -a` fails while the lock is held, as on the runner.
cat > "$d/bin/dpkg" <<'EOF'
#!/bin/bash
if [ "$1" = hold ]; then touch "$FAKE/lock"; sleep "$2"; rm -f "$FAKE/lock"; exit 0; fi
[ -e "$FAKE/lock" ] && { echo "dpkg: error: dpkg database lock was locked (fake)"; exit 2; }
exit 0
EOF
chmod +x "$d/bin/dpkg"

MIRROR='http://azure.archive.ubuntu.com/ubuntu priority:1'
run() { # plan words...
    rm -f "$d"/count "$d"/mirrors.seen.*
    printf '%s\n' "$@" > "$d/plan"
    echo "$MIRROR" > "$d/mirrors"
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

# U4b: CI 44370186784 (2026-11-19): the runners' mirror is now us-east-2.ec2.archive.ubuntu.com,
# the rewrite knew only azure.archive.ubuntu.com, so the "archive.ubuntu.com" try went to the
# same slow mirror (the bare-metal job ended at exit 124, a new red for promote). Any
# <region>.archive.ubuntu.com host goes to archive.ubuntu.com; security/ports are not touched.
MIRROR='http://us-east-2.ec2.archive.ubuntu.com/ubuntu priority:1
http://security.ubuntu.com/ubuntu priority:2'
run hang hang ok
if grep -q '^http://archive.ubuntu.com/ubuntu priority:1' "$d/mirrors.seen.3" && ! grep -q 'ec2' "$d/mirrors.seen.3" \
   && grep -q '^http://security.ubuntu.com/ubuntu priority:2' "$d/mirrors.seen.3"
then ok U4b; else bad "U4b $(tr '\n' ' ' < "$d/mirrors.seen.3")"; fi
MIRROR='http://azure.archive.ubuntu.com/ubuntu priority:1'

# U8: apt_install.sh's own retry rewrites the same way (first install cut, then update, then the
# second install must read archive.ubuntu.com). Its fake plan: install hang, update ok, install ok.
INST="$(dirname "$SUT")/apt_install.sh"
rm -f "$d"/count "$d"/mirrors.seen.*
printf '%s\n' hang ok ok > "$d/plan"
echo 'http://us-east-2.ec2.archive.ubuntu.com/ubuntu priority:1' > "$d/mirrors"
FAKE="$d" PATH="$d/bin:$PATH" APT_FIRST_SECS=2 APT_UPDATE_SECS=2 APT_UPDATE_KILL=1 APT_SOURCES="$d/mirrors" \
    timeout 30 bash "$INST" gcc > "$d/out" 2>&1
rc=$?
n=$(cat "$d/count" 2>/dev/null || echo 0)
if [ "$rc" = 0 ] && [ "$n" = 3 ] && grep -q '^http://archive.ubuntu.com/' "$d/mirrors.seen.3" && ! grep -q 'ec2' "$d/mirrors.seen.3"
then ok U8; else bad "U8 rc=$rc tries=$n $(cat "$d/mirrors.seen.3" 2>/dev/null)"; fi

# U9: CI 45188947825 (DEPTH, 2026-11-23): the first install passed 300 s while dpkg was still
# setting up the cross gcc; timeout killed apt-get, not that dpkg, so `dpkg --configure -a` and
# the second install failed on its lock (exit 100, a new red for promote). apt_install.sh must
# wait for the dpkg it left. Plan: install (its dpkg holds the lock 3 s past the 2 s cut),
# update ok, install that fails while the lock is held.
rm -f "$d"/count "$d"/mirrors.seen.* "$d/lock"
printf '%s\n' orphan ok lockok > "$d/plan"
echo "$MIRROR" > "$d/mirrors"
FAKE="$d" PATH="$d/bin:$PATH" APT_FIRST_SECS=2 APT_UPDATE_SECS=2 APT_UPDATE_KILL=1 APT_SOURCES="$d/mirrors" \
    timeout 30 bash "$INST" gcc > "$d/out" 2>&1
rc=$?
n=$(cat "$d/count" 2>/dev/null || echo 0)
if [ "$rc" = 0 ] && [ "$n" = 3 ] && ! grep -q 'lock was locked' "$d/out"
then ok U9; else bad "U9 rc=$rc tries=$n $(grep -a 'lock' "$d/out" | head -1)"; fi
sleep 3; rm -f "$d/lock"   # the fake dpkg of a red run may still hold it

run ok
if [ "$rc" = 0 ] && [ "$n" = 1 ] && grep -q azure "$d/mirrors"; then ok U2; else bad "U2 rc=$rc tries=$n"; fi

run hang hang hang
if [ "$rc" != 0 ] && [ "$rc" != 124 ] && [ "$n" = 3 ] && [ "$e" -lt 15 ]; then ok U3; else bad "U3 rc=$rc tries=$n secs=$e"; fi

run deaf ok
if [ "$rc" = 0 ] && [ "$n" = 2 ] && [ "$e" -lt 10 ]; then ok U5; else bad "U5 rc=$rc tries=$n secs=$e"; fi

# U6 (audit-108, audit-110): a job that calls apt_install.sh can spend 32 minutes on apt alone:
# update 8.5, install 5, the wait for a dpkg the cut left 5 (U9), update again 8.5, install 5.
# Its time limit must leave room for that; 35 is the floor (32 and a short job), this does not know how long the job's own work takes.
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
prc=$?   # H32 (audit-190): an unreadable ci.yml made python fail with no output = PASS
if [ "$prc" != 0 ]; then bad "U6 cannot read $CI (python rc $prc)"
elif [ -z "$short" ]; then ok U6; else bad "U6 $(echo "$short" | tr '\n' ' ')"; fi

# U7: a GitHub-hosted job installs through apt_install.sh. CI 41469844784 (2026-11-03): the
# RNG0 job's bare `apt-get install` sat until the job's 30 minutes, "cancelled", a new red for
# promote. Self-hosted jobs are out of this: they do not use the azure mirror.
# Any place on a line of a run: (one-line form, after && or ;), apt as well as apt-get, with or
# without sudo and options (audit-116: the first form saw only `sudo apt-get install` at the
# head of a line). apt_install.sh has an underscore, so it is not this. Audit-117 found three
# more it missed: a path (/usr/bin/apt-get), an option with a value (-o K=V), and a line
# continued with a backslash; continued lines are joined first. Audit-119: inside quotes
# (bash -c "apt-get install ...", sh -c '...') and a backquote were not seen either.
bare="$(python3 - "$CI" <<'PY'
import re, sys
BARE = re.compile(r'(^|[\s;&|(/"\'`])apt(-get)?(\s+-\S+(\s+[^-\s]\S*)?)*\s+install(\s|$)')
s = open(sys.argv[1]).read()
parts = re.split(r'^  ([A-Za-z0-9_-]+):\s*$', s[s.index('\njobs:'):], flags=re.M)
for name, body in zip(parts[1::2], parts[2::2]):
    r = re.search(r'^    runs-on: *(.*)$', body, re.M)
    if not r or 'self-hosted' in r[1]:
        continue
    for line in re.sub(r'\\\n\s*', ' ', body).splitlines():
        line = line.strip()
        if line.startswith('#') or line.startswith('- name:') or line.startswith('name:'):
            continue
        if BARE.search(line):
            print(name)
            break
PY
)"
prc=$?
if [ "$prc" != 0 ]; then bad "U7 cannot read $CI (python rc $prc)"
elif [ -z "$bare" ]; then ok U7; else bad "U7 $(echo "$bare" | tr '\n' ' ')"; fi

[ "$fails" = 0 ] && { echo "[apt-update] PASS"; exit 0; }
echo "[apt-update] FAIL ($fails)"
exit 1
