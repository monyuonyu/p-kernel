/*
 *  pfs_repl.h — p-fs P1: region-scoped gossip replication of blocks.
 *
 *  Spec: docs/architecture/p-fs.md §2.2 (gossip 複製), §5 (P1 row).
 *
 *  P1 generalizes sfs.c from "/shared files, all-to-all" to "any
 *  content-addressed block, region-scoped":
 *
 *    pfs_put on node A (new block)
 *      -> ANNOUNCE {id, len, origin} on K-DDS topic "pfs/ann"
 *         (KDDS_SCOPE_REGION — only same-region peers hear it)
 *      -> a peer lacking the id publishes WANT {id} on "pfs/want"
 *      -> any holder streams the bytes to the requester over a private
 *         pmesh UDP port (PFSR_PORT) in sfs.c-style 512B chunks
 *         (block payloads are <= PFS_BLOCK_MAX = 4096 B, far over the
 *          KDDS_DATA_MAX = 192 B topic-payload limit — exactly the
 *          "sfs.c 型の独自 UDP を一般化" row of p-fs.md §4)
 *      -> the receiver re-hashes the assembled bytes, verifies the
 *         block-id (content addressing makes transfers self-checking),
 *         and pfs_put_origin()s it — which dedups naturally and
 *         re-announces, so the block gossips on through the region.
 *
 *  Boot sync (sfs_boot_sync generalized): a starting node publishes
 *  SYNC on "pfs/sync"; every holder streams it all of its blocks.
 *  Duplicate streams are harmless — the store dedups by id.
 *
 *  Symmetric by construction: every node runs the same task; there is
 *  no master copy, no central index (p-fs.md §4 invariants).
 *
 *  NOT yet in this minimal P1 (honest scope): tombstones (blocks are
 *  immutable and there is no pfs delete yet) and the consistent-hash
 *  responsibility set (that is P3).
 *
 *  LP64 / wire discipline: every wire field is U1/UH/UW (8/16/32 bit —
 *  include/typedef.h pins UW to unsigned int on all four arches); the
 *  block-id stays U1[PFS_ID_LEN]. _Static_asserts in pfs_repl.c pin
 *  every packet's exact byte size so the wire image is ABI-identical
 *  across aarch64 / x86_64 / i686 (feedback_lp64_typedef_trap).
 */

#pragma once
#include "kernel.h"
#include "pfs_block.h"

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

#define PFSR_PORT        7382          /* pmesh port for chunk transfer  */
#define PFSR_VERSION     1
#define PFSR_CHUNK_SIZE  512           /* sfs.c chunk size, generalized  */

/* control-message magics (K-DDS payloads) */
#define PFSR_ANN_MAGIC   0x4E414650UL  /* "PFAN" LE */
#define PFSR_WANT_MAGIC  0x54574650UL  /* "PFWT" LE */
#define PFSR_SYNC_MAGIC  0x59534650UL  /* "PFSY" LE */

/* chunk-transfer magic (private UDP port, sfs-style) */
#define PFSR_BLK_MAGIC   0x4B424650UL  /* "PFBK" LE */
#define PFSR_BLK_CHUNK   0x01          /* the only type so far           */

/* K-DDS topics (all REGION-scoped) */
#define PFSR_TOPIC_ANN   "pfs/ann"
#define PFSR_TOPIC_WANT  "pfs/want"
#define PFSR_TOPIC_SYNC  "pfs/sync"

/* want-retry policy: a peer that heard an ANNOUNCE but still lacks the
 * block re-publishes WANT until the block arrives (lost control msgs /
 * lost chunks self-heal) or the try budget runs out. */
#define PFSR_PENDING_MAX    8          /* outstanding wanted blocks      */
#define PFSR_WANT_RETRY_MS  600        /* re-WANT interval               */
#define PFSR_WANT_TRIES     10         /* give up after this many WANTs  */

/* ------------------------------------------------------------------ */
/* wire formats — fixed-width fields only, sizes pinned in pfs_repl.c  */
/* ------------------------------------------------------------------ */

/* ANNOUNCE — "I just stored a new block" (K-DDS "pfs/ann", REGION).
 * seq is a per-announcer counter: subscribers poll the LATEST_ONLY
 * topic and act once per (src_node, seq), dkva.c-style. */
typedef struct {
    UW   magic;                    /* PFSR_ANN_MAGIC                    */
    UW   seq;                      /* per-announcer, starts at 1        */
    U1   src_node;                 /* announcer                         */
    U1   origin;                   /* node that first created the block */
    UH   _pad;
    UW   len;                      /* block length in bytes             */
    U1   id[PFS_ID_LEN];           /* block-id = sha256(bytes)          */
} __attribute__((packed)) PFSR_ANN_PKT;     /* 4+4+1+1+2+4+32 = 48 B   */

/* WANT — "send me this block" (K-DDS "pfs/want", REGION). */
typedef struct {
    UW   magic;                    /* PFSR_WANT_MAGIC                   */
    UW   seq;                      /* per-requester, starts at 1        */
    U1   src_node;                 /* requester                         */
    U1   _pad[3];
    U1   id[PFS_ID_LEN];
} __attribute__((packed)) PFSR_WANT_PKT;    /* 4+4+1+3+32 = 44 B       */

/* SYNC — "stream me everything you have" (K-DDS "pfs/sync", REGION).
 * The block-granular sfs_boot_sync(). */
typedef struct {
    UW   magic;                    /* PFSR_SYNC_MAGIC                   */
    UW   seq;                      /* per-requester, starts at 1        */
    U1   src_node;                 /* requester                         */
    U1   _pad[3];
} __attribute__((packed)) PFSR_SYNC_PKT;    /* 4+4+1+3 = 12 B          */

/* CHUNK — block bytes over the private pmesh port. Generalizes
 * sfs.c's FILE_START + FILE_CHUNK pair: because every chunk is
 * self-describing (id + total_len in each packet), chunk_idx == 0
 * doubles as the START that (re)initializes the receiver state. */
typedef struct {
    UW   magic;                    /* PFSR_BLK_MAGIC                    */
    U1   version;                  /* PFSR_VERSION                      */
    U1   type;                     /* PFSR_BLK_CHUNK                    */
    U1   src_node;                 /* sender                            */
    U1   origin;                   /* creator (preserved across hops)   */
    U1   id[PFS_ID_LEN];           /* block-id                          */
    UW   total_len;                /* whole-block byte count            */
    UW   chunk_idx;                /* 0-based, in order (sfs-style)     */
    UH   chunk_len;                /* valid bytes in data[]             */
    UH   _pad;
    U1   data[PFSR_CHUNK_SIZE];
} __attribute__((packed)) PFSR_BLK_PKT;     /* 52 + 512 = 564 B        */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Init: open the three REGION-scoped K-DDS topics, bind PFSR_PORT on
 * pmesh, register the pfs_block put-hook. Call at boot after
 * pmesh_init() + kdds_init() (same ordering rule as kdds itself). */
void pfs_repl_init(void);

/* Replication task (poll the control topics, retry WANTs, one-shot
 * boot sync). Create after drpc_init() — needs drpc_my_node. */
void pfs_repl_task(INT stacd, void *exinf);

/* Store a block tagged with this node as creator (origin =
 * drpc_my_node when distributed, PFS_ORIGIN_SELF otherwise) so every
 * replica's `pfs ls` shows the same origin. The put-hook then
 * announces it to the region. */
INT  pfs_repl_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN]);

/* Shell helpers (`pfs put <text>` / `pfs ls`) — print via tmonitor. */
void pfs_repl_put_cmd(const UB *text, UW len);
void pfs_repl_ls(void);

/* UDP receive callback (bound to PFSR_PORT). */
void pfs_repl_rx(UB src_node, UH dst_port, const UB *data, UH len);
