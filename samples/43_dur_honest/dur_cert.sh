#!/bin/bash
# ===========================================================================
# 43_dur_honest / dur_cert.sh  —  SLICE 0: the durable layer stops lying.
#
# Two bugs from the persistence design (docs/architecture/persistence.md
# SLICE 0; gap-ledger rows DUR-SWALLOW 🔴 / DUR-REFTAB 🟠):
#
#   DUR-SWALLOW — pfs_put discarded the durable-write rc and returned PFS_OK
#     even when the write FAILED, after which FIFO eviction could drop the
#     only copy (the ark forgot silently). Fix: on a failed durable write
#     pfs_put returns NON-OK and pins the block un-evictable (sole copy).
#
#   DUR-REFTAB — refs.tab carried no CRC, so a same-length torn write loaded
#     garbage head/seq. Fix: a CRC32 over {count + entries}; restore REFUSES
#     a mismatch and keeps the previous good state.
#
# Both certs run INSIDE the production kernel against the real pfs_put /
# refs_persist / pfs_dag_restore paths (a backend fail-injection hook, NOT a
# sim), and are only meaningful with a durable backend active — so we set
# PKERNEL_PFS_DIR. The kernel verbs are `pfs durtest` and `pfs dagtest`.
#
# Usage:  ./dur_cert.sh        (builds the host target if needed)
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
KERNEL="$BOOT/p-kernel"
[ -x "$KERNEL" ] || make -C "$BOOT" >/dev/null || { echo "build failed"; exit 1; }

WORK="$(mktemp -d /tmp/dur_honest.XXXXXX)"
LOG="$WORK/cert.log"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "==========================================================="
echo " SLICE 0 — durable honesty: DUR-SWALLOW + DUR-REFTAB"
echo "==========================================================="
printf 'pfs durtest\npfs dagtest\nexit\n' \
    | timeout 120 env PKERNEL_PFS_DIR="$WORK/ark" "$KERNEL" >"$LOG" 2>&1

echo "----- cert log -----"
grep -E '\[pfs-durswallow\]|\[pfs-dagrefs\]|durable: refs.tab|durable failure' "$LOG" \
    | sed 's/^/    /'
echo "--------------------"

FAIL=0
grep -aqF '[pfs-durswallow] PASS' "$LOG" \
    && echo "  [PASS] DUR-SWALLOW: failed durable write is honest, sole copy un-evictable" \
    || { echo "  [FAIL] DUR-SWALLOW cert did not PASS"; FAIL=1; }
grep -aqF '[pfs-dagrefs] PASS' "$LOG" \
    && echo "  [PASS] DUR-REFTAB: torn refs.tab refused, clean refs.tab round-trips" \
    || { echo "  [FAIL] DUR-REFTAB cert did not PASS"; FAIL=1; }

echo
if [ "$FAIL" = 0 ]; then
    echo "==========================================================="
    echo " RESULT: PASS — the durable layer no longer lies."
    echo "==========================================================="
    exit 0
else
    echo "==========================================================="
    echo " RESULT: FAIL — see the cert log above"
    echo "==========================================================="
    exit 1
fi
