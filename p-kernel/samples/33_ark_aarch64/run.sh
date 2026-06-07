#!/bin/bash
# ===========================================================================
# 33_ark_aarch64 / run.sh — ARK on a REAL block device, on AArch64 bare metal.
#
# Closes the ARK-3 honest residual: until now ARK ran on real hardware only on
# x86 (ATA-PIO ide0, see samples/31_ark_baremetal). AArch64 bare metal had NO
# block driver — arch/aarch64/vfs_stub.c was a stub. This sample proves ARK now
# mounts and round-trips on the QEMU `virt` machine through a from-scratch
# virtio-blk MMIO driver (arch/aarch64/virtio_blk.c), bridged to ARK_BDEV by
# arch/aarch64/ark_bdev.c, with ZERO changes to arch/common/arkfs.c.
#
# Transport: virtio-mmio (QEMU virt, 0x0a000000). The driver supports both
# legacy (Version 1, QueuePFN) and modern (Version 2, QueueReady) transports;
# QEMU 10.x defaults this board to the legacy interface, which it negotiates at
# runtime. Polled (no IRQ) — runs before T-Kernel in boot/aarch64/main.c.
#
# What it proves, on a RAW disk image (NOT a host file — ARK lays down its own
# superblock at sector 0):
#   BOOT 1 (fresh disk):  format ARK -> write a 600-byte content-addressed
#                         file -> commit/fsync (virtio flush) -> ark_unmount
#                         (discard ALL in-RAM state) -> ark_mount (replay the
#                         log off the physical device) -> read back -> sha256.
#                         => "WRITE+REMOUNT PASS"
#   BOOT 2 (same image):  a full QEMU power-cycle. Mount the existing image,
#                         find the file still there, sha256 verify.
#                         => "REBOOT-VERIFY PASS"  (cross-power-loss durable)
#
# Usage:  ./run.sh
# Exit 0 = both boots PASS. Exit 1 = any failure / blocker. Exit 2 = toolchain.
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/aarch64"

for t in qemu-system-aarch64 aarch64-linux-gnu-gcc; do
    command -v "$t" >/dev/null 2>&1 || { echo "FATAL: $t not found"; exit 2; }
done

IMG="$(mktemp /tmp/ark_aa64.XXXXXX.img)"
BIN="$BOOT/kernel.elf"
cleanup() { rm -f "$IMG"; }
trap cleanup EXIT

FAIL=0
ok()  { echo "  [PASS] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=1; }

# ---- build the smoke kernel (clean, so the -D reaches main.o + ark_bdev.o) -
echo "[build] make EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE (clean rebuild) ..."
make -C "$BOOT" clean >/dev/null 2>&1
if ! make -C "$BOOT" EXTRA_CFLAGS=-DARK_BAREMETAL_SMOKE \
        >/tmp/ark_aa64_build.log 2>&1; then
    echo "FATAL: kernel build failed (see /tmp/ark_aa64_build.log)"; exit 2
fi
[ -f "$BIN" ] || { echo "FATAL: $BIN not produced"; exit 2; }

# ---- a fresh RAW disk (zeroed: ARK sees no superblock -> formats) ---------
dd if=/dev/zero of="$IMG" bs=1M count=8 >/dev/null 2>&1

run_qemu() {  # $1=logfile
    timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a53 -m 256M \
        -kernel "$BIN" -serial stdio -display none -no-reboot \
        -drive file="$IMG",format=raw,if=none,id=ark0 \
        -device virtio-blk-device,drive=ark0 >"$1" 2>&1 || true
}

# ---- BOOT 1: format + write + sync + remount + read ----------------------
echo
echo "==========================================================="
echo " BOOT 1 — fresh disk: format -> write -> sync -> remount -> read"
echo "==========================================================="
L1=/tmp/ark_aa64_boot1.log
run_qemu "$L1"
grep -aE 'ark-smoke|vblk|SMOKE RESULT' "$L1" | sed 's/^/    /'
grep -qa "virtio-blk ready" "$L1" \
    && ok "virtio-blk MMIO driver discovered + brought up the device" \
    || bad "virtio-blk device not brought up"
grep -qa "formatting a fresh ARK image" "$L1" \
    && ok "ARK formatted a fresh image on the physical virtio-blk disk" \
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
L2=/tmp/ark_aa64_boot2.log
run_qemu "$L2"
grep -aE 'ark-smoke|vblk|SMOKE RESULT' "$L2" | sed 's/^/    /'
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
    echo " RESULT: PASS — ARK now mounts and round-trips on AArch64 bare"
    echo " metal. A from-scratch virtio-blk MMIO driver gives the QEMU"
    echo " virt machine durable memory: a file written through"
    echo " ARK_BDEV/virtio-blk survives an in-process remount AND a full"
    echo " QEMU power-cycle, sha256-verified, with ZERO changes to"
    echo " arch/common/arkfs.c. The ARK-3 residual (x86-only) is closed."
    echo "==========================================================="
    exit 0
else
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines and /tmp/ark_aa64_boot{1,2}.log"
    echo "==========================================================="
    exit 1
fi
