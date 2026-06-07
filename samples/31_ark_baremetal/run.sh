#!/bin/bash
# ===========================================================================
# 31_ark_baremetal / run.sh  —  ARK on a REAL block device (wave 15, ARK-3隊).
#
# The audit (docs/architecture/arkfs-audit.md, finding 🔴3) said it plainly:
# "today ARK saves a Linux process, not a Pi." ARK's durable wiring lived
# entirely behind _TK_HOSTED_LIBC_; on bare metal it was dead code. This
# sample proves ARK now mounts and round-trips on the bare-metal x86 block
# device (ATA-PIO ide0 — the SAME disk FAT32 rides), under QEMU, with ZERO
# changes to arch/common/arkfs.c. The bridge is arch/x86/ark_bdev.c:
# ARK_BDEV{read,write,sync} -> blk_ssy "ide0".
#
# What it proves, on a RAW disk image (NOT a host file, NOT FAT32 — ARK lays
# down its own superblock at sector 0):
#   BOOT 1 (fresh disk):  format ARK -> write a 600-byte content-addressed
#                         file -> commit/fsync -> ark_unmount (discard ALL
#                         in-RAM state) -> ark_mount (replay the log off the
#                         physical device) -> read back -> sha256 verify.
#                         => "WRITE+REMOUNT PASS"
#   BOOT 2 (same image):  a full QEMU power-cycle. Mount the existing image,
#                         find the file still there, sha256 verify.
#                         => "REBOOT-VERIFY PASS"  (cross-power-loss durable)
#
# The kernel is built with -DARK_BAREMETAL_SMOKE, which runs the driver in
# boot/x86/main.c (before T-Kernel; IDE PIO is polled) and halts with a
# verdict. Nothing else in the kernel changes.
#
# Usage:  ./run.sh
# Exit 0 = both boots PASS. Exit 1 = any failure / blocker.
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/x86"

for t in qemu-system-x86_64 i686-linux-gnu-gcc; do
    command -v "$t" >/dev/null 2>&1 || { echo "FATAL: $t not found"; exit 2; }
done

IMG="$(mktemp /tmp/ark_bm.XXXXXX.img)"
BIN="$BOOT/bootloader.bin"
cleanup() { rm -f "$IMG"; }
trap cleanup EXIT

FAIL=0
ok()  { echo "  [PASS] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=1; }

# ---- build the smoke kernel (clean, so the -D reaches main.o) -------------
echo "[build] make EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE (clean rebuild) ..."
make -C "$BOOT" clean >/dev/null 2>&1
if ! make -C "$BOOT" EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE bootloader.bin \
        >/tmp/ark_bm_build.log 2>&1; then
    echo "FATAL: kernel build failed (see /tmp/ark_bm_build.log)"; exit 2
fi
[ -f "$BIN" ] || { echo "FATAL: $BIN not produced"; exit 2; }

# ---- a fresh RAW disk (zeroed: ARK sees no superblock -> formats) ---------
dd if=/dev/zero of="$IMG" bs=1M count=8 >/dev/null 2>&1

run_qemu() {  # $1=logfile
    timeout 60 qemu-system-x86_64 -m 256 -kernel "$BIN" -serial stdio \
        -cpu qemu64 -display none -no-reboot \
        -drive file="$IMG",format=raw,if=ide,index=0 >"$1" 2>&1 || true
}

# ---- BOOT 1: format + write + sync + remount + read ----------------------
echo
echo "==========================================================="
echo " BOOT 1 — fresh disk: format -> write -> sync -> remount -> read"
echo "==========================================================="
L1=/tmp/ark_bm_boot1.log
run_qemu "$L1"
grep -aE 'ark-smoke|SMOKE RESULT' "$L1" | sed 's/^/    /'
grep -qa "formatting a fresh ARK image" "$L1" \
    && ok "ARK formatted a fresh image on the physical ATA disk" \
    || bad "ARK did not format the fresh disk"
grep -qa "remounted by replaying the log from the physical device" "$L1" \
    && ok "remounted off the real device (in-RAM state was discarded)" \
    || bad "did not remount from the device"
grep -qa "WRITE+REMOUNT PASS" "$L1" \
    && ok "write -> sync -> remount -> read round-trip verified (sha256)" \
    || bad "write/remount round-trip failed"
grep -qa "ARK\] SMOKE RESULT: PASS" "$L1" \
    && ok "boot 1 overall verdict: PASS" || bad "boot 1 verdict not PASS"

# ---- BOOT 2: power-cycle on the SAME image -------------------------------
echo
echo "==========================================================="
echo " BOOT 2 — same disk, full QEMU reboot: mount -> verify persisted"
echo "==========================================================="
L2=/tmp/ark_bm_boot2.log
run_qemu "$L2"
grep -aE 'ark-smoke|SMOKE RESULT' "$L2" | sed 's/^/    /'
grep -qa "mounted an existing ARK image" "$L2" \
    && ok "mounted the image written by the previous boot" \
    || bad "could not mount the previously written image"
grep -qa "REBOOT-VERIFY PASS" "$L2" \
    && ok "file survived a full power-cycle, sha256 verified on real device" \
    || bad "data did not survive the reboot"
grep -qa "ARK\] SMOKE RESULT: PASS" "$L2" \
    && ok "boot 2 overall verdict: PASS" || bad "boot 2 verdict not PASS"

echo
if [ "$FAIL" = 0 ]; then
    echo "==========================================================="
    echo " RESULT: PASS — ARK mounts and round-trips on a REAL block"
    echo " device. The survival FS is no longer a Linux-process demo:"
    echo " a file written through ARK_BDEV/ide0 survives an in-process"
    echo " remount AND a full QEMU power-cycle, sha256-verified, with"
    echo " ZERO changes to arch/common/arkfs.c."
    echo "==========================================================="
    exit 0
else
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines and /tmp/ark_bm_boot{1,2}.log"
    echo "==========================================================="
    exit 1
fi
