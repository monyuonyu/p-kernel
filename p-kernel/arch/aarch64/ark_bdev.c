/*
 *  ark_bdev.c (aarch64)
 *  Bare-metal ARK_BDEV — ARK's storage vtable, bound to the virtio-blk MMIO
 *  driver on the QEMU `virt` machine. See ark_bdev.h.
 *
 *  arkfs.c is touched ZERO times: it speaks only the ARK_BDEV vtable, and here
 *  we fill that vtable from arch/aarch64/virtio_blk.c. This is the aarch64
 *  counterpart of arch/x86/ark_bdev.c (which rides ATA-PIO), closing the
 *  ARK-3 residual: "aarch64 bare metal has no block driver, so ARK runs on
 *  real hardware only on x86."
 */

#include "kernel.h"
#include "arkfs.h"
#include "ark_bdev.h"
#include "virtio_blk.h"
#include "sha256.h"

/* ------------------------------------------------------------------ */
/* ARK_BDEV adapters. ctx is unused (single virtio-blk device).         */
/* virtio_blk_* return 0/<0; ARK wants ARK_OK/ARK_E_IO.                  */
/* ------------------------------------------------------------------ */

static INT ark_bd_read(void *ctx, U4 lba, U4 n, void *buf)
{
    (void)ctx;
    return (virtio_blk_read((UW)lba, (UW)n, buf) == 0) ? ARK_OK : ARK_E_IO;
}

static INT ark_bd_write(void *ctx, U4 lba, U4 n, const void *buf)
{
    (void)ctx;
    return (virtio_blk_write((UW)lba, (UW)n, buf) == 0) ? ARK_OK : ARK_E_IO;
}

static INT ark_bd_sync(void *ctx)
{
    (void)ctx;
    return (virtio_blk_flush() == 0) ? ARK_OK : ARK_E_IO;
}

INT ark_bdev_bind(ARK_BDEV *bd)
{
    if (!bd) return ARK_E_INVAL;
    if (!virtio_blk_present()) return ARK_E_NODEV;

    bd->sector_size   = ARK_SECTOR;
    bd->total_sectors = virtio_blk_sector_count();
    bd->read  = ark_bd_read;
    bd->write = ark_bd_write;
    bd->sync  = ark_bd_sync;
    bd->ctx   = NULL;

    if (bd->total_sectors == 0) return ARK_E_NODEV;
    return ARK_OK;
}

/* ================================================================== */
/* Bare-metal smoke test (compiled only with -DARK_BAREMETAL_SMOKE)     */
/* Mirrors arch/x86/ark_bdev.c so the run.sh grep contract matches.     */
/* ================================================================== */
#ifdef ARK_BAREMETAL_SMOKE

#define ARK_SMOKE_PATH "/ark_smoke.dat"
#define ARK_SMOKE_LEN  600u          /* spans >1 sector, < ARK_BLOCK_MAX */

static INT s_memeq(const void *a, const void *b, U4 n)
{
    const U1 *x = (const U1 *)a, *y = (const U1 *)b;
    for (U4 i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static void emit_int(void (*emit)(const char *), const char *label, INT v)
{
    char tmp[16]; INT t = 0;
    U4 u; INT neg = 0;
    if (v < 0) { neg = 1; u = (U4)(-(long)v); } else u = (U4)v;
    if (u == 0) tmp[t++] = '0';
    while (u) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
    emit(label);
    {
        char num[20]; INT k = 0;
        if (neg) num[k++] = '-';
        while (t > 0) num[k++] = tmp[--t];
        num[k++] = '\r'; num[k++] = '\n'; num[k] = '\0';
        emit(num);
    }
}

static void emit_hex8(void (*emit)(const char *), const char *label,
                      const U1 *d)
{
    static const char *hx = "0123456789abcdef";
    char out[32]; INT k = 0;
    for (INT i = 0; i < 8; i++) {
        out[k++] = hx[(d[i] >> 4) & 0xF];
        out[k++] = hx[d[i] & 0xF];
    }
    out[k++] = '.'; out[k++] = '.'; out[k++] = '\r'; out[k++] = '\n';
    out[k] = '\0';
    emit(label);
    emit(out);
}

static void build_content(U1 *c, U4 n)
{
    const char *hdr = "ARK-baremetal-smoke v1 / survival-fs round-trip / ";
    U4 i = 0;
    while (hdr[i] && i < n) { c[i] = (U1)hdr[i]; i++; }
    for (; i < n; i++) c[i] = (U1)((i * 31u + 7u) & 0xFF);
}

INT ark_baremetal_smoke(void (*emit)(const char *))
{
    static U1 content[ARK_SMOKE_LEN];
    static U1 rd[ARK_BLOCK_MAX];
    U1 want[SHA256_DIGEST_SIZE], got[SHA256_DIGEST_SIZE];
    ARK_BDEV bd;
    INT fails = 0, rc, n;

    emit("[ark-smoke] ============================================\r\n");
    emit("[ark-smoke] ARK on REAL block device (virtio-blk MMIO)\r\n");
    emit("[ark-smoke] probing virtio-blk ...\r\n");
    if (virtio_blk_init() < 0) {
        emit("[ark-smoke] FAIL: no virtio-blk device (add -device "
             "virtio-blk-device,drive=...)\r\n");
        return 1;
    }

    rc = ark_bdev_bind(&bd);
    if (rc != ARK_OK) { emit_int(emit, "[ark-smoke] FAIL bind rc=", rc); return 1; }
    emit_int(emit, "[ark-smoke] device total_sectors = ", (INT)bd.total_sectors);

    build_content(content, ARK_SMOKE_LEN);
    sha256(content, ARK_SMOKE_LEN, want);
    emit_hex8(emit, "[ark-smoke] content sha256 = ", want);

    /* ---- try to mount an EXISTING image (the cross-reboot path) ---- */
    rc = ark_mount(&bd);
    if (rc == ARK_OK) {
        emit("[ark-smoke] mounted an existing ARK image on the disk\r\n");
        n = ark_read_file(ARK_SMOKE_PATH, rd, sizeof rd);
        if (n == (INT)ARK_SMOKE_LEN && s_memeq(rd, content, ARK_SMOKE_LEN)) {
            sha256(rd, (U4)n, got);
            if (s_memeq(got, want, SHA256_DIGEST_SIZE)) {
                emit_hex8(emit, "[ark-smoke] read-back sha256 = ", got);
                emit("[ark-smoke] REBOOT-VERIFY PASS: file persisted across a full "
                     "QEMU reboot, sha256 verified on the real device\r\n");
                return 0;
            }
        }
        emit("[ark-smoke] mounted but marker absent/mismatch -> writing it now\r\n");
    } else {
        emit_int(emit, "[ark-smoke] no ARK fs yet, mount rc=", rc);
        emit("[ark-smoke] formatting a fresh ARK image on the physical disk\r\n");
        if (ark_format(&bd) != ARK_OK) { emit("[ark-smoke] FAIL: format\r\n"); return 1; }
        if (ark_mount(&bd) != ARK_OK)  { emit("[ark-smoke] FAIL: mount after format\r\n"); return 1; }
        emit("[ark-smoke] formatted + mounted\r\n");
    }

    /* ---- WRITE -> commit (fsync) -------------------------------------- */
    rc = ark_write_file(ARK_SMOKE_PATH, content, ARK_SMOKE_LEN);
    if (rc < 0) { emit_int(emit, "[ark-smoke] FAIL write rc=", rc); return 1; }
    bd.sync(bd.ctx);
    emit("[ark-smoke] wrote + committed to disk (content-addressed, fsync'd)\r\n");

    /* ---- IN-PROCESS REMOUNT: wipe RAM state, rebuild from the medium -- */
    ark_unmount();
    emit("[ark-smoke] unmounted (all in-memory index/state discarded)\r\n");
    rc = ark_mount(&bd);
    if (rc != ARK_OK) { emit_int(emit, "[ark-smoke] FAIL remount rc=", rc); return 1; }
    emit("[ark-smoke] remounted by replaying the log from the physical device\r\n");

    /* ---- READ-BACK + sha256-verify ------------------------------------ */
    for (U4 i = 0; i < sizeof rd; i++) rd[i] = 0;
    n = ark_read_file(ARK_SMOKE_PATH, rd, sizeof rd);
    if (n != (INT)ARK_SMOKE_LEN) {
        emit_int(emit, "[ark-smoke] FAIL read-back len=", n);
        return ++fails;
    }
    sha256(rd, (U4)n, got);
    if (!s_memeq(rd, content, ARK_SMOKE_LEN) ||
        !s_memeq(got, want, SHA256_DIGEST_SIZE)) {
        emit("[ark-smoke] FAIL: read-back bytes / sha256 mismatch\r\n");
        emit_hex8(emit, "[ark-smoke]   got sha256 = ", got);
        return ++fails;
    }
    emit_hex8(emit, "[ark-smoke] read-back sha256 = ", got);
    emit("[ark-smoke] WRITE+REMOUNT PASS: write -> sync -> remount -> read "
         "round-trip verified on the REAL device (sha256 match)\r\n");
    emit("[ark-smoke] (reboot QEMU on the same disk image for cross-power-loss "
         "proof -> REBOOT-VERIFY)\r\n");
    return fails;
}

#endif /* ARK_BAREMETAL_SMOKE */
