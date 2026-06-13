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

FAIL=0

# Run the certs under each durable backend. CRITICAL (auditor's finding):
# the FLAT backend never evicts, so the eviction-skip loop — the actual
# sole-copy-preservation surface DUR-SWALLOW adds — is ONLY exercised under
# ARK. Gating only flat would let a regression in that loop pass CI silently.
# So we run a second pass with PKERNEL_PFS_BACKEND=ark.
run_pass() {
    local name="$1"; shift
    local plog="$WORK/$name.log"
    printf 'pfs durtest\npfs dagtest\nexit\n' \
        | timeout 120 env "$@" "$KERNEL" >"$plog" 2>&1
    echo "----- cert log ($name) -----"
    grep -E '\[pfs-durswallow\]|\[pfs-dagrefs\]|durable: refs.tab|durable failure' "$plog" \
        | sed 's/^/    /'
    echo "--------------------"
    grep -aqF '[pfs-durswallow] PASS' "$plog" \
        && echo "  [PASS] DUR-SWALLOW ($name): failed durable write is honest, sole copy un-evictable" \
        || { echo "  [FAIL] DUR-SWALLOW ($name) cert did not PASS"; FAIL=1; }
    grep -aqF '[pfs-dagrefs] PASS' "$plog" \
        && echo "  [PASS] DUR-REFTAB ($name): torn refs.tab refused, clean refs.tab round-trips" \
        || { echo "  [FAIL] DUR-REFTAB ($name) cert did not PASS"; FAIL=1; }
}

# Pass 1: FLAT backend (one-file-per-block under $PKERNEL_PFS_DIR).
run_pass flat PKERNEL_PFS_DIR="$WORK/flat"
# Pass 2: ARK backend (the one that actually evicts — gates the skip loop).
run_pass ark  PKERNEL_PFS_DIR="$WORK/arkdir" \
              PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$WORK/ark.img"

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
