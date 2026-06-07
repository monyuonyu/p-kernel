/*
 *  virtio_blk.h (aarch64)
 *  Minimal virtio-blk (MMIO transport) driver for the QEMU `virt` machine.
 *
 *  The `virt` board exposes 32 virtio-mmio transport slots starting at
 *  0x0a000000 (stride 0x200). `-device virtio-blk-device,drive=...` binds a
 *  block device onto the first free slot. This driver discovers it, sets up a
 *  single split virtqueue, and does polled (no-IRQ) sector read/write/flush —
 *  enough for ARK to format/mount/round-trip on aarch64 bare metal.
 *
 *  Supports BOTH virtio-mmio Version 1 (legacy: QueuePFN + GuestPageSize) and
 *  Version 2 (modern / VIRTIO 1.0: split Desc/Avail/Used + QueueReady), chosen
 *  at runtime from the transport's Version register, so it works regardless of
 *  how the host QEMU defaults the transport.
 *
 *  All buffers are static and physically addressable (MMU is disabled at EL1,
 *  so VA==PA and there is no DMA cache-coherency window to manage beyond a DSB
 *  barrier). Single device, single in-flight request — this is a boot-time /
 *  durable-store backend, not a high-throughput driver.
 */
#ifndef PKERNEL_AARCH64_VIRTIO_BLK_H
#define PKERNEL_AARCH64_VIRTIO_BLK_H

#include "kernel.h"

/* Discover + initialise the first virtio-blk-device on the MMIO bus.
 * Returns 0 on success, <0 if no device was found or setup failed. */
INT virtio_blk_init(void);

/* Read/write `n` 512-byte sectors at LBA `lba`. Return 0 on success, <0 on
 * error. Handles arbitrary n (the data buffer is pointed at directly). */
INT virtio_blk_read (UW lba, UW n, void *buf);
INT virtio_blk_write(UW lba, UW n, const void *buf);

/* Issue a VIRTIO_BLK_T_FLUSH if the device negotiated F_FLUSH; else no-op.
 * Returns 0 on success / no-op, <0 on error. */
INT virtio_blk_flush(void);

/* Device capacity in 512-byte sectors (0 if not initialised). */
UW  virtio_blk_sector_count(void);

/* TRUE once virtio_blk_init() succeeded. */
INT virtio_blk_present(void);

#endif /* PKERNEL_AARCH64_VIRTIO_BLK_H */
