# 33_ark_aarch64 — ARK on AArch64 bare metal (virtio-blk)

Closes the honest residual left by ARK-3 (`samples/31_ark_baremetal`): ARK had
a real bare-metal block backend **only on x86** (ATA-PIO `ide0`). AArch64 bare
metal had no block driver at all — `arch/aarch64/vfs_stub.c` was a stub — so the
survival FS could not become durable on the QEMU `virt` machine (or, by
extension, real ARM hardware with a virtio/MMIO block transport).

This sample proves ARK now formats, writes, syncs, remounts and survives a full
power-cycle on AArch64, through a **from-scratch virtio-blk driver**, with
**zero changes to `arch/common/arkfs.c`**.

## What was built

| Piece | File |
|-------|------|
| virtio-blk MMIO driver (discovery, split virtqueue, polled read/write/flush) | `arch/aarch64/virtio_blk.c` + `.../include/virtio_blk.h` |
| ARK_BDEV bridge (binds ARK's vtable to virtio-blk; bare-metal smoke test) | `arch/aarch64/ark_bdev.c` + `.../include/ark_bdev.h` |
| VFS ARK wiring (`vfs_ark_mount/read/write`, aarch64-scoped) | `arch/aarch64/vfs_stub.c` |
| Boot smoke hook + QEMU disk attach + `run-smoke` target | `boot/aarch64/main.c`, `boot/aarch64/Makefile` |

### virtio-blk transport details

- **Bus**: virtio-mmio on the QEMU `virt` board (32 transport slots at
  `0x0a000000`, stride `0x200`). The driver scans the slots for
  MagicValue `"virt"` + DeviceID 2 (block).
- **Versions**: supports BOTH legacy (Version 1: `GuestPageSize` + `QueuePFN` +
  `QueueAlign`) and modern (Version 2: split `QueueDesc/Avail/Used` +
  `QueueReady`, with `VIRTIO_F_VERSION_1` acknowledged). The version is read at
  runtime from the transport. QEMU 10.x defaults this board to the **legacy**
  interface, which is what the smoke run exercises.
- **Queue**: a single split virtqueue (depth 8), one in-flight request, 3
  descriptors (header / data / status). Data descriptors point directly at the
  caller's buffer — no bounce, no per-call size cap.
- **Mode**: polled (no IRQ). It runs in `boot/aarch64/main.c` before T-Kernel,
  so it needs no scheduler or GIC wiring. MMU is disabled at EL1 (VA==PA), so a
  `dsb sy` barrier is the only DMA-ordering concern.
- **Flush**: negotiates `VIRTIO_BLK_F_FLUSH` when offered and issues a real
  `VIRTIO_BLK_T_FLUSH` on `ARK_BDEV.sync`.

## Run

```sh
./run.sh
```

Exit 0 = both boots PASS. Or, by hand from `boot/aarch64`:

```sh
make run-smoke                 # fresh scratch image at /tmp/ark_aa64_smoke.img
make run DISK=/tmp/ark.img     # boot the normal kernel with a virtio-blk disk
```

## Observed result (QEMU 10.1.0, `-M virt -cpu cortex-a53`)

```
[vblk] virtio-blk ready (legacy mmio)
[ark-smoke] device total_sectors = 16384
[ark-smoke] content sha256 = f616f288076e1382..
BOOT 1: formatted -> wrote -> sync -> unmount -> remount(replay) -> read
        read-back sha256 = f616f288076e1382..   => WRITE+REMOUNT PASS
BOOT 2: mounted existing image -> read-back sha256 = f616f288076e1382..
        => REBOOT-VERIFY PASS
```

ARK now runs on AArch64 bare metal.
