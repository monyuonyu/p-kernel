/*
 *  pfs_block.h — p-fs P0: content-addressed block store.
 *
 *  Spec: docs/architecture/p-fs.md §2.1 (内容アドレス), §4 (mapping), §5 (P0).
 *
 *  A *block* is an arbitrary byte string of length <= PFS_BLOCK_MAX. Its
 *  *block-id* is sha256(block bytes) — a fixed 32-byte value. The same
 *  content always yields the same id, on any node, on any ABI. This is the
 *  bottom (block) layer of the 4-layer p-fs model (p-fs.md §3.1) and the
 *  foundation every later phase (gossip replica, version DAG, erasure code)
 *  stands on.
 *
 *  P0 is LOCAL ONLY: no gossip, no replication (that is P1). What P0 buys
 *  is local content-addressing + deduplication and the second VFS backend
 *  slot that p-fs.md §3.1 / vfs.h "Future: multiple backends" call for.
 *
 *  LP64 / ABI trap (memory: feedback_lp64_typedef_trap): the block-id is a
 *  fixed-width byte array U1[32] — NEVER a `long`-derived type (UW/W bloat
 *  to 8 bytes on LP64). It must be byte-identical across aarch64 / x86_64 /
 *  i686 so the same content hashes to the same id everywhere. A
 *  _Static_assert in pfs_block.c pins sizeof(block-id) == 32.
 */

#pragma once
#include "kernel.h"

#define PFS_ID_LEN     32          /* sha256 digest width — block-id size */
#define PFS_BLOCK_MAX  4096        /* max bytes per block (P0) */
#define PFS_MAX_BLOCKS 64          /* in-memory block table capacity (P0) */

/* Result codes (0 = success, negative = error). */
#define PFS_OK          0
#define PFS_E_NOTFOUND (-1)        /* no block with that id */
#define PFS_E_TOOBIG   (-2)        /* len > PFS_BLOCK_MAX */
#define PFS_E_FULL     (-3)        /* block table is full */
#define PFS_E_INVAL    (-4)        /* bad argument */

/* Compute block-id = sha256(buf[0..len)) into id_out (no store). */
void pfs_id_compute(const void *buf, UW len, U1 id_out[PFS_ID_LEN]);

/* Store a block. block-id = H(buf) is written to id_out. If a block with
 * that id is already present the bytes are NOT re-stored (dedup); id_out is
 * still filled. Returns PFS_OK, or a negative PFS_E_* code. */
INT  pfs_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN]);

/* Retrieve the block named by id into buf (up to maxlen bytes). Returns the
 * block's full length on success (may exceed maxlen if the caller's buffer
 * was too small — bytes beyond maxlen are not copied), PFS_E_NOTFOUND if no
 * such block. */
INT  pfs_get(const U1 id[PFS_ID_LEN], void *buf, UW maxlen);

/* Existence check. Returns 1 if a block with that id is stored, else 0. */
INT  pfs_has(const U1 id[PFS_ID_LEN]);

/* Number of distinct blocks currently stored (dedup'd count). */
UW   pfs_count(void);

/* Run the P0 self-test (dedup + round-trip + miss). Prints PASS/FAIL via
 * the supplied puts-style callback. Returns 0 on PASS, non-zero on FAIL. */
INT  pfs_self_test(void (*emit)(const char *));
