#!/bin/bash
# tests/ci/apt_install.sh <pkg>... — apt-get install with a time limit and a
# second mirror, for the GitHub-hosted jobs that pull the aarch64 cross gcc
# (~45 MB). Run as root (sudo), after the step's own apt-get update.
#
# Why: on 2026-10-22 and 10-23 the runner's azure.archive.ubuntu.com mirror
# served the package files at ~40 KB/s (the index files at 10 MB/s), so the
# install step ran into the job's time limit and four or two jobs ended
# "cancelled" — not a code red, but promote saw them as new reds (CI runs
# 39822762399, 39940037954). apt's mirror list only falls back on errors,
# not on a slow mirror.
#
# First try: the same install, cut at APT_FIRST_SECS (default 300). If that
# fails, point every apt source at archive.ubuntu.com instead of the azure
# mirror, apt-get update, and try again (the .debs already fetched stay in
# /var/cache/apt/archives), cut at the same limit. Exit status is the last install's; the
# worst case with the update before it is 32 minutes (update 8.5, install 5, the wait for a
# dpkg the cut left 5, update 8.5, install 5), and jobs that call this allow at least 35, plus
# what the job itself takes.
set -u
FIRST="${APT_FIRST_SECS:-300}"
if timeout "$FIRST" apt-get install -y --no-install-recommends "$@"; then exit 0; fi
echo "[apt_install] first try failed or passed ${FIRST}s; retrying on archive.ubuntu.com"
# any <region>.archive.ubuntu.com, as apt_update.sh (CI 44370186784: the mirror is us-east-2.ec2
# now, and the azure-only rewrite sent the retry back to it). APT_SOURCES is for the test.
for f in ${APT_SOURCES:-/etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*}; do
    [ -f "$f" ] && sed -i 's|[A-Za-z0-9.-]*\.archive\.ubuntu\.com|archive.ubuntu.com|g' "$f"
done
# The cut kills apt-get, not the dpkg it started: CI 45188947825 (DEPTH, 2026-11-23) cut the
# first try while dpkg was setting up the cross gcc, and the dpkg below and the second install
# failed on its lock (exit 100). Wait for that dpkg first (at most FIRST seconds more).
w=0
while pgrep -x dpkg >/dev/null 2>&1 && [ "$w" -lt "$FIRST" ]; do sleep 1; w=$((w + 1)); done
[ "$w" -gt 0 ] && echo "[apt_install] waited ${w}s for the dpkg the cut left running"
dpkg --configure -a || true    # in case the cut fell inside dpkg, not the download
"$(dirname "$(readlink -f "$0")")/apt_update.sh"   # the update has its limits too (audit-107)
timeout "$FIRST" apt-get install -y --no-install-recommends "$@"
