#!/bin/bash
# tests/ci/apt_update.sh — apt-get update with a time limit and a second mirror. Run as root
# (sudo). ci.yml uses this instead of a bare `sudo apt-get update`.
#
# Why: on CI 41251950684 (2026-11-01) three GitHub-hosted jobs stopped inside `apt-get update`
# right after the InRelease files (azure mirror "Ign", archive.ubuntu.com fine) and sat there
# until the job's time limit, 15 to 30 minutes later. "cancelled" is a new red for promote, so
# 18 commits stayed out of master for a mirror. tests/ci/apt_install.sh already guards the
# install; nothing guarded the update.
#
# Three tries, each cut at APT_UPDATE_SECS (default 150, so 7.5 minutes at most; the shortest
# job limit in ci.yml is 15): as is; with apt's own short timeouts and retries; the same after
# pointing every source at archive.ubuntu.com. Exit status is the last try's, except that a
# cut try exits 1 (never timeout's 124, which would read as the update failing in another way).
set -u
SECS="${APT_UPDATE_SECS:-150}"
SOURCES="${APT_SOURCES:-/etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*}"
SHORT=(-o Acquire::Retries=2 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20)

if timeout "$SECS" apt-get update; then exit 0; fi
echo "[apt_update] first try failed or passed ${SECS}s; short apt timeouts"
if timeout "$SECS" apt-get update "${SHORT[@]}"; then exit 0; fi
echo "[apt_update] second try failed or passed ${SECS}s; retrying on archive.ubuntu.com"
for f in $SOURCES; do
    [ -f "$f" ] && sed -i 's|azure\.archive\.ubuntu\.com|archive.ubuntu.com|g' "$f"
done
if timeout "$SECS" apt-get update "${SHORT[@]}"; then exit 0; fi
echo "[apt_update] third try failed or passed ${SECS}s"
exit 1
