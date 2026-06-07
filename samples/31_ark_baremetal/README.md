# 31_ark_baremetal — ARK on a REAL block device

The arkfs audit (`docs/architecture/arkfs-audit.md`, finding 🔴3) put it
bluntly: *"today ARK saves a Linux process, not a Pi."* ARK's durable wiring
lived entirely behind `_TK_HOSTED_LIBC_`; on the bare-metal hardware it exists
to protect, it was linked dead code and p-fs stayed memory-only.

This sample closes that gap on x86: ARK now mounts and round-trips on the
bare-metal block device (ATA-PIO `ide0` — the same disk FAT32 rides), under
QEMU, with **zero changes to `arch/common/arkfs.c`**.

## How

ARK touches storage only through the `ARK_BDEV` vtable
(`arch/common/include/arkfs.h`). The bridge is **`arch/x86/ark_bdev.c`**, which
binds `ARK_BDEV{read,write,sync}` to the `blk_ssy` "ide0" device:

| ARK_BDEV op | bound to |
|---|---|
| `read(ctx,lba,n,buf)`  | `ide0` `BLK_OPS.read`  (ATA PIO, `arch/x86/ide.c`) |
| `write(ctx,lba,n,buf)` | `ide0` `BLK_OPS.write` (issues ATA `FLUSH_CACHE`)  |
| `sync(ctx)`            | no-op — `ide_write` already flushes per write       |
| `ctx`                  | the `const BLK_OPS *` of the bound device           |

VFS wiring (`arch/common/include/vfs.h`, `arch/x86/vfs.c`): a new
`VFS_BACKEND_ARK` enum value plus `vfs_ark_mount()` /
`vfs_ark_write_file()` / `vfs_ark_read_file()`, and `vfs_stat_path` /
`vfs_readdir` dispatch on the active backend. FAT32 stays the default and is
untouched.

## What it proves

The kernel is built with `-DARK_BAREMETAL_SMOKE`, which runs a driver in
`boot/x86/main.c` (before T-Kernel; IDE PIO is polled) on a RAW disk image
(not a host file, not FAT32 — ARK lays down its own superblock at sector 0):

- **BOOT 1 (fresh disk):** format ARK → write a 600-byte content-addressed
  file → commit/fsync → `ark_unmount` (discard all in-RAM state) →
  `ark_mount` (replay the log off the physical device) → read back → sha256
  verify ⇒ `WRITE+REMOUNT PASS`.
- **BOOT 2 (same image):** a full QEMU power-cycle. Mount the existing image,
  find the file still there, sha256 verify ⇒ `REBOOT-VERIFY PASS`.

## Run

```sh
./run.sh            # builds the smoke kernel, runs two QEMU boots
```

Exit 0 = both boots PASS. Logs: `/tmp/ark_bm_boot{1,2}.log`.

## Scope / honesty

This sample is x86-only (ATA-PIO `ide0`). The aarch64 counterpart has since
landed in **`samples/33_ark_aarch64`**: a from-scratch virtio-blk MMIO driver
(`arch/aarch64/virtio_blk.c`) binds the same `ARK_BDEV` vtable on the QEMU
`virt` board, so ARK now survives a power-cycle on AArch64 bare metal too — with
the identical `ark_bdev_bind()` pattern and **zero `arch/common/arkfs.c`
changes**, exactly as predicted here.
