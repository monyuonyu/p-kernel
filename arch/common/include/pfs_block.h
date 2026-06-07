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

/* origin tag for blocks created locally outside distributed mode (also
 * the value pfs_put() uses). A real node id (0..DNODE_MAX-1) marks the
 * node that first created the block (p-fs P1 replication metadata). */
#define PFS_ORIGIN_SELF 0xFF

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
 * still filled. Returns PFS_OK, or a negative PFS_E_* code.
 * Equivalent to pfs_put_origin(buf, len, id_out, PFS_ORIGIN_SELF). */
INT  pfs_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN]);

/* As pfs_put, but tags the block with the node that first created it
 * (p-fs P1: replicated blocks keep their creator's id so `pfs ls` shows
 * the same origin on every replica). On a NEW store (not a dedup hit)
 * the registered put-hook fires — pfs_repl.c uses this to announce the
 * block to the region (save == publish, p-fs.md §3.2). */
INT  pfs_put_origin(const void *buf, UW len, U1 id_out[PFS_ID_LEN],
                    U1 origin);

/* Hook called on every NEW block store (never on a dedup hit). Keeps
 * pfs_block.c network-free: the P1 replication layer registers here.
 * The hook runs in the storing task's context — keep it light. */
typedef void (*PFS_PUT_HOOK)(const U1 id[PFS_ID_LEN], UW len, U1 origin);
void pfs_set_put_hook(PFS_PUT_HOOK fn);

/* Enumerate the block table for `pfs ls` / replication sync: fills the
 * out params for slot idx (0..PFS_MAX_BLOCKS-1). Returns 1 if the slot
 * holds a block, 0 if empty / out of range. Any out param may be NULL. */
INT  pfs_slot_info(UW idx, U1 id_out[PFS_ID_LEN], UW *len_out,
                   U1 *origin_out);

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

/* ------------------------------------------------------------------ */
/* P0 durable backend (G24) — make the library non-volatile.           */
/*                                                                     */
/* When $PKERNEL_PFS_DIR names a directory (hosted/Linux only), every   */
/* NEW block is also written there content-addressed (filename = the    */
/* block-id hex, 64 chars) and fsync'd. Same content == same id, so the */
/* write is idempotent. pfs_durable_restore() rescans that directory at */
/* boot, recomputes sha256(content) for each file and rejects any whose */
/* bytes do not match their name (content-addressed self-verification), */
/* then reloads the survivors into the in-memory table — "记忆 returns  */
/* after a reboot". With the env unset, or on bare-metal builds, both   */
/* calls are no-ops and the store stays memory-only (backward compat).  */
/* ------------------------------------------------------------------ */

/* Reload persisted blocks from $PKERNEL_PFS_DIR with sha256 verification.
 * Prints a one-line summary (loaded / rejected) via `emit`. Returns the
 * number of blocks restored into the table (0 when disabled). */
INT  pfs_durable_restore(void (*emit)(const char *));

/* 1 if a durable directory is configured (and writes are persisting),
 * else 0. Useful for demos / status. */
INT  pfs_durable_active(void);
