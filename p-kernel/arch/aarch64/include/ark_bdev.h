/*
 *  ark_bdev.h (aarch64)
 *  Bare-metal ARK_BDEV — the storage vtable ARK runs on, on real aarch64 HW.
 *
 *  ARK (arch/common/arkfs.c) touches storage ONLY through the ARK_BDEV vtable
 *  (arch/common/include/arkfs.h): { sector_size, total_sectors, read, write,
 *  sync, ctx }. On the Linux ports that vtable is fd-backed; on x86 bare metal
 *  it rides ATA-PIO (arch/x86/ark_bdev.c). This is the aarch64 bridge: it binds
 *  the four ops to the virtio-blk MMIO driver (arch/aarch64/virtio_blk.c) on
 *  the QEMU `virt` machine.
 *
 *  ZERO changes to arkfs.c: the SAME ark_mount()/ark_format() logic runs here,
 *  just over a different vtable. Unlike x86 there is no blk_ssy layer on
 *  aarch64, so the adapters call the virtio-blk driver directly (ctx unused).
 */
#ifndef PKERNEL_AARCH64_ARK_BDEV_H
#define PKERNEL_AARCH64_ARK_BDEV_H

#include "kernel.h"
#include "arkfs.h"

/* Fill *bd with an ARK_BDEV vtable driving the virtio-blk device. The driver
 * must already be initialised (virtio_blk_init). Returns ARK_OK, or
 * ARK_E_NODEV / ARK_E_INVAL on an absent / wrong-geometry device. */
INT ark_bdev_bind(ARK_BDEV *bd);

/* Self-contained bare-metal smoke test (built only with -DARK_BAREMETAL_SMOKE).
 * Probes virtio-blk, then proves a real format/write -> sync -> remount ->
 * read-back -> sha256-verify round-trip on the physical device. On a SECOND
 * boot against the same image it detects the persisted file and reports
 * cross-reboot durability. `emit` prints a NUL-terminated line. Returns 0 on
 * PASS, non-zero failure count on FAIL. */
INT ark_baremetal_smoke(void (*emit)(const char *));

#endif /* PKERNEL_AARCH64_ARK_BDEV_H */
