/*
 *  ark_bdev.h (x86)
 *  Bare-metal ARK_BDEV — the storage vtable ARK runs on, on REAL hardware.
 *
 *  ARK (arch/common/arkfs.c) touches storage ONLY through the ARK_BDEV
 *  vtable (arch/common/include/arkfs.h): { sector_size, total_sectors,
 *  read, write, sync, ctx }. On the Linux ports that vtable is fd-backed
 *  (arch/linux/pfs_ark.c). On bare metal it has to ride a real block
 *  driver. This file is that bridge for x86: it binds ARK_BDEV's four ops
 *  to the platform block device registered in blk_ssy ("ide0", the same
 *  ATA-PIO disk FAT32 uses, arch/x86/ide.c).
 *
 *  ZERO changes to arkfs.c: the SAME ark_mount()/ark_format() logic runs
 *  here, just over a different vtable ctx. ctx is the const BLK_OPS* of the
 *  bound device; the adapters translate the (lba,n,buf) calls and the
 *  result convention (BLK_OPS: 0/-1  ->  ARK: 0/ARK_E_IO).
 */
#ifndef PKERNEL_ARK_BDEV_H
#define PKERNEL_ARK_BDEV_H

#include "kernel.h"
#include "blk_ssy.h"
#include "arkfs.h"

/* Fill *bd with an ARK_BDEV vtable driving the registered block device
 * *blk (ctx = blk). Validates blk->sector_size == ARK_SECTOR. Returns
 * ARK_OK, or ARK_E_INVAL / ARK_E_NODEV on a bad/absent device. After this
 * the bd is ready for ark_mount(bd) / ark_format(bd). */
INT ark_bdev_bind(ARK_BDEV *bd, const BLK_OPS *blk);

/* Self-contained bare-metal smoke test (built only with
 * -DARK_BAREMETAL_SMOKE). Probes the IDE disk, then proves a real
 * write -> sync -> remount -> read-back -> sha256-verify round-trip on the
 * physical device (not a host file). On a SECOND boot against the same
 * disk it detects the persisted file and reports cross-reboot durability.
 * `emit` prints a NUL-terminated line. Returns 0 on PASS, non-zero count
 * of failures on FAIL. Declared here for completeness; the boot caller
 * (boot/x86/main.c) uses a local prototype to avoid pulling kernel.h. */
INT ark_baremetal_smoke(void (*emit)(const char *));

#endif /* PKERNEL_ARK_BDEV_H */
