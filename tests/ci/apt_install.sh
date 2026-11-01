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
# worst case with the update before it is 22 minutes, and jobs that call this allow 35.
set -u
FIRST="${APT_FIRST_SECS:-300}"
if timeout "$FIRST" apt-get install -y --no-install-recommends "$@"; then exit 0; fi
echo "[apt_install] first try failed or passed ${FIRST}s; retrying on archive.ubuntu.com"
for f in /etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*; do
    [ -f "$f" ] && sed -i 's|azure\.archive\.ubuntu\.com|archive.ubuntu.com|g' "$f"
done
dpkg --configure -a || true    # in case the cut fell inside dpkg, not the download
"$(dirname "$(readlink -f "$0")")/apt_update.sh"   # the update has its limits too (audit-107)
timeout "$FIRST" apt-get install -y --no-install-recommends "$@"
