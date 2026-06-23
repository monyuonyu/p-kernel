/*
 *  compat_arkfs_gap.c — the [arkfs-version-gap] cert
 *                       (compat-migration-chain-plan.md §3.3 / §9.2).
 *
 *  THE LAST migration-chain leg. arkfs (arch/common/arkfs.c) is a crash-safe
 *  APPEND-ONLY LOG. The design-harden pass RESOLVED the open question: v1 does
 *  NOT in-place migrate a foreign record stream (transcoding a foreign record
 *  stream would multiply the crash-window surface — forbidden). Instead the
 *  hosted backend (arch/linux/pfs_ark.c) DETECTS a format-version gap and
 *  REJECT-and-REFORMATS: a present image whose superblock is a VALID ARK super-
 *  block of a NON-native format version is NOT replayed; a fresh native image
 *  is laid down so the node still boots with working durable storage.
 *
 *  This cert proves that policy on a RAM bdev (in-proc), driving the EXACT
 *  production helper pfs_ark_mount_or_reformat (arch/linux/pfs_ark.c) so the
 *  version gate it certifies is the shipped one.
 *
 *  HONEST SCOPE (load-bearing — read before trusting a green line):
 *    - Does NOT do in-place log migration (out of scope by decision). Under
 *      reject+reformat the on-disk arkfs BYTES ARE LOST across a version gap;
 *      only SILENT loss is prevented (the honest deathless bound). Survival
 *      across the gap DEPENDS on the Self-lineage being intact
 *      ([selflineage-migrate]) + Path-E re-education — arkfs holds durable
 *      backing bytes only, NOT identity.
 *    - Single-node, in-proc (no [live]); the version gap is SIMULATED by
 *      patching the superblock version int (+ fixing its crc so it is a VALID
 *      foreign-version super, not corrupt). Justified: the gap is exactly the
 *      single compared int at arkfs.c:561 (super_valid: version != FMT).
 *      Not a true two-binary harness.
 *
 *  CROWN / byte-identity (LENS A): this whole TU compiles ONLY under
 *  ARKFS_GAP_CERT && _TK_HOSTED_LIBC_, and is added ONLY to the two hosted
 *  Makefiles' object lists — NEVER to the bare-metal link (boot/aarch64,
 *  boot/x86). So the default aarch64 .text and the [smp-one-mind] crown
 *  755a20fa… are byte-IDENTICAL by construction (this object does not exist
 *  there). The reject+reformat POLICY lives in the caller (pfs_ark.c, hosted);
 *  arkfs.c gains only a hosted-gated read-only peek helper.
 *
 *  FALSIFIER (-DARKFS_GAP_SKIP_VERCHECK): force the version check to be
 *  IGNORED (accept the foreign-version superblock as native and replay its
 *  foreign log) -> the v-new mount is fed a foreign record stream -> the live
 *  state is wrong / inconsistent -> the cert FAILs. Proves the version gate is
 *  load-bearing (flip the define, watch PASS -> FAIL).
 */

#include "kernel.h"
#include "arkfs.h"

#if defined(ARKFS_GAP_CERT) && defined(_TK_HOSTED_LIBC_)

IMPORT void sio_send_frame(const UB *buf, INT size);
static UW   ag_strlen(const char *s){ UW n=0; while(s[n]) n++; return n; }
static void ag_puts(const char *s){ sio_send_frame((const UB *)s,(INT)ag_strlen(s)); }
static void ag_putdec(UW v){ char b[12]; INT i=11; b[11]=0;
    if(!v){ ag_puts("0"); return; } while(v&&i>0){ b[--i]=(char)('0'+(v%10)); v/=10; } ag_puts(&b[i]); }

/* The production reject-or-reformat policy (arch/linux/pfs_ark.c). Plain extern
 * (the arch/linux contract is kept out of the bare-metal include chain). */
IMPORT INT pfs_ark_mount_or_reformat(ARK_BDEV *bd, int *reformatted,
                                     void (*emit)(const char *));

/* ------------------------------------------------------------------ */
/* RAM block device (in-proc, like fb_read/fb_write but RAM-backed).   */
/* ------------------------------------------------------------------ */
#define AG_NSECT 64u                          /* >= 4 */
static U1 g_disk[AG_NSECT * ARK_SECTOR];

static INT ag_read(void *c, U4 lba, U4 n, void *buf)
{
    (void)c;
    if (lba > AG_NSECT || n > AG_NSECT - lba) return ARK_E_IO;
    for (U4 i = 0; i < (U4)(n * ARK_SECTOR); i++)
        ((U1 *)buf)[i] = g_disk[lba * ARK_SECTOR + i];
    return ARK_OK;
}
static INT ag_write(void *c, U4 lba, U4 n, const void *buf)
{
    (void)c;
    if (lba > AG_NSECT || n > AG_NSECT - lba) return ARK_E_IO;
    for (U4 i = 0; i < (U4)(n * ARK_SECTOR); i++)
        g_disk[lba * ARK_SECTOR + i] = ((const U1 *)buf)[i];
    return ARK_OK;
}
static INT ag_sync(void *c) { (void)c; return ARK_OK; }

static ARK_BDEV ag_bdev(void)
{
    ARK_BDEV bd;
    bd.sector_size   = ARK_SECTOR;
    bd.total_sectors = AG_NSECT;
    bd.read  = ag_read;
    bd.write = ag_write;
    bd.sync  = ag_sync;
    bd.ctx   = 0;
    return bd;
}

/* ------------------------------------------------------------------ */
/* Superblock surgery — simulate a foreign-version image by patching   */
/* the version int in BOTH copies (sector 0 + last sector) and fixing  */
/* the crc so each is a VALID superblock of a FOREIGN version. This     */
/* layout MUST match ark_super in arkfs.c (ABI-pinned, _Static_assert   */
/* sizeof==32 there); we replicate the leading fields + crc32 to write  */
/* raw sector bytes, exactly as a foreign-version binary would have.    */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    U1 magic[8];
    U4 version;
    U4 sector_size;
    U4 log_start;
    U4 total_sectors;
    U4 epoch;
    U4 crc;
} ag_super;

static U4 ag_crc32(const void *data, U4 len)
{
    const U1 *p = (const U1 *)data;
    U4 crc = 0xFFFFFFFFu;
    for (U4 i = 0; i < len; i++) {
        crc ^= p[i];
        for (INT k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88420u & (~(crc & 1u) + 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Patch the superblock at sector `sec` (in g_disk) to `newver`, recomputing
 * the crc so it stays a VALID ARK superblock — just of a foreign version. */
static void ag_patch_super_version(U4 sec, U4 newver)
{
    ag_super *sb = (ag_super *)&g_disk[sec * ARK_SECTOR];
    sb->version = newver;
    sb->crc     = ag_crc32(sb, (U4)((U1 *)&sb->crc - (U1 *)sb));
}
static U4 ag_super_version(U4 sec)
{ return ((ag_super *)&g_disk[sec * ARK_SECTOR])->version; }

/* ------------------------------------------------------------------ */
/* one sub-case: format native, store a known block, patch BOTH supers */
/* to `foreign_ver`, run the production mount-or-reformat, assert the   */
/* gap was handled by reject+reformat (NOT replay, NOT silent mis-mount)*/
/* Returns 1 on PASS, 0 on FAIL. emits its trace.                       */
/* ------------------------------------------------------------------ */
static INT ag_subcase(U4 foreign_ver, const char *label)
{
    ARK_BDEV bd = ag_bdev();
    INT ok = 1;

    ag_puts("  [sub "); ag_puts(label); ag_puts(" v=");
    ag_putdec(foreign_ver); ag_puts("] ");

    /* CURE: lay down a native image + a known block; assert read-back. */
    if (ark_format(&bd) != ARK_OK) { ag_puts("FAIL(format) "); return 0; }
    if (ark_mount(&bd)  != ARK_OK) { ag_puts("FAIL(mount) ");  return 0; }
    static const char payload[] = "ARKFS-GAP-OLD-PAYLOAD-do-not-survive";
    U1 oid[ARK_ID_LEN];
    if (ark_block_put(payload, (U4)sizeof(payload), oid) != ARK_OK)
        { ag_puts("FAIL(put) "); return 0; }
    if (ark_checkpoint() != ARK_OK) { ag_puts("FAIL(ckpt) "); return 0; }
    static U1 rb[256];
    if (ark_block_get(oid, rb, sizeof rb) != (INT)sizeof(payload))
        { ag_puts("FAIL(readback) "); return 0; }
    if (!ark_block_has(oid)) { ag_puts("FAIL(has) "); return 0; }
    ark_unmount();

    /* Simulate a foreign-version image: patch BOTH superblock copies
     * (primary @0, replica @last) to `foreign_ver` and fix their crcs so
     * each is a VALID superblock — of a FOREIGN version (the distinction is
     * load-bearing: super_valid would already reject a corrupt one). */
    ag_patch_super_version(0, foreign_ver);
    ag_patch_super_version(AG_NSECT - 1u, foreign_ver);

    /* Confirm the peek sees the foreign version (and != native). */
    U4 peeked = ark_super_version_peek(&bd);
    U4 native = ark_fmt_version();
    if (peeked != foreign_ver) { ag_puts("FAIL(peek!=foreign) "); return 0; }
    if (peeked == native)      { ag_puts("FAIL(peek==native?) "); return 0; }

    /* Run the PRODUCTION mount-or-reformat policy. */
    int reformatted = -1;
    INT r = pfs_ark_mount_or_reformat(&bd, &reformatted, 0);

    /* (a) version-mismatch detected -> reformat ran (NOT a plain mount). */
    if (r != ARK_OK)      { ag_puts("FAIL(policy!=OK) "); return 0; }
    if (reformatted != 1) { ag_puts("FAIL(no reformat) "); ok = 0; }

    /* (b) fresh ark_mount succeeded AND super_valid() now true at the NATIVE
     *     version: re-peek the on-disk image, it must be native again. */
    if (ark_super_version_peek(&bd) != native)
        { ag_puts("FAIL(disk not native) "); ok = 0; }
    if (ag_super_version(0) != native || ag_super_version(AG_NSECT - 1u) != native)
        { ag_puts("FAIL(super not native) "); ok = 0; }

    /* (c) block put/get works on the fresh image. */
    static const char fresh[] = "fresh-after-reformat";
    U1 fid[ARK_ID_LEN];
    if (ark_block_put(fresh, (U4)sizeof(fresh), fid) != ARK_OK)
        { ag_puts("FAIL(fresh put) "); ok = 0; }
    if (ark_checkpoint() != ARK_OK) { ag_puts("FAIL(fresh ckpt) "); ok = 0; }
    if (ark_block_get(fid, rb, sizeof rb) != (INT)sizeof(fresh))
        { ag_puts("FAIL(fresh get) "); ok = 0; }

    /* (d) NEGATIVE SPACE: the OLD foreign payload is GONE (honest reformat,
     *     never a silent mis-mount that secretly kept the old log). */
    if (ark_block_has(oid))      { ag_puts("FAIL(old payload survived!) "); ok = 0; }
    if (ark_block_get(oid, rb, sizeof rb) != ARK_E_NOTFOUND)
        { ag_puts("FAIL(old still readable!) "); ok = 0; }

    ark_unmount();
    ag_puts(ok ? "ok\r\n" : "BAD\r\n");
    return ok;
}

void compat_arkfs_gap_test(void)
{
    ag_puts("[arkfs-version-gap] ==== reject+reformat on a format-version gap ====\r\n");
    ag_puts("[arkfs-version-gap] arkfs is an append-only log: a foreign-version "
            "image is NOT replayed; it is reformatted (durable bytes reset).\r\n");
    ag_puts("[arkfs-version-gap] survival across the gap = Self-lineage intact "
            "+ Path-E re-education; arkfs carries backing BYTES only.\r\n");

    U4 native = ark_fmt_version();
    ag_puts("[arkfs-version-gap] native format version = v");
    ag_putdec(native); ag_puts("\r\n");

    INT ok = 1;
    /* cover-all-paths: a NEWER foreign version (+1) AND an OLD fossil (-1)
     * must both reject+reformat IDENTICALLY. */
    if (!ag_subcase(native + 1u, "newer +1")) ok = 0;
    if (native >= 1u) { if (!ag_subcase(native - 1u, "older -1")) ok = 0; }

#ifdef ARKFS_GAP_SKIP_VERCHECK
    ag_puts("[arkfs-version-gap] (FALSIFIER -DARKFS_GAP_SKIP_VERCHECK active: "
            "the version gate is bypassed; the foreign log is replayed)\r\n");
#endif
    ag_puts(ok ? "[arkfs-version-gap] PASS\r\n" : "[arkfs-version-gap] FAIL\r\n");
    ag_puts("[arkfs-version-gap] DONE — a foreign-version image is rejected and "
            "reformatted, never replayed and never silently mis-mounted.\r\n");
}

#endif /* ARKFS_GAP_CERT && _TK_HOSTED_LIBC_ */
