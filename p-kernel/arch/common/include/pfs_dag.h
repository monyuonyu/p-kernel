/*
 *  pfs_dag.h — p-fs P2: object manifests + append-only version DAG + refs.
 *
 *  Spec: docs/architecture/p-fs.md §2.3 (履歴保存), §3.1 (namespace /
 *  version / object layers), §5 (P2 row).
 *
 *  P2 puts NAMES and HISTORY on top of the P0 block store and the P1
 *  region replication. The essence: saving never destroys the past.
 *
 *    pfs save <name> <bytes>
 *      -> the bytes become a content-addressed block   (content-id)
 *      -> a MANIFEST block is created:
 *           { prev-manifest-id, content-id, seq, name, origin }
 *         and stored via pfs_put — a manifest IS a block, so P1's
 *         announce/want replicates it to the region for free; P2 adds
 *         no new transport.
 *      -> the local REF (name -> head manifest id) moves to the new
 *         manifest. The old manifest is still a live block: walking
 *         prev from the head reaches every version ever saved.
 *
 *  Refs are the ONLY mutable thing (p-fs.md §3.5). They gossip on one
 *  REGION-scoped K-DDS topic "pfs/ref": every node periodically beacons
 *  its whole ref table (state-based, so lost packets self-heal on the
 *  next beacon) and merges received entries last-writer-wins by the
 *  manifest seq.
 *
 *  HONEST CONFLICT CAVEAT (p-fs.md §2.3 / §6): two nodes that save the
 *  same name concurrently both create a manifest with the same seq but
 *  different prev/content — a FORK. The fork is never lost: both head
 *  manifests exist as immutable blocks and both replicate. But the ref
 *  can only point at one of them; the merge breaks the tie
 *  deterministically (larger manifest id wins) so all nodes converge
 *  on the same head, and the losing branch stays reachable only by its
 *  manifest id. Real fork resolution (merge versions, multiple
 *  parents) is deferred, exactly as the spec defers it.
 *
 *  Topic-budget note: a per-source topic per node ("pfs/ref/<n>",
 *  world.c-style) would avoid the shared LATEST_ONLY slot, but dkva +
 *  moe + world already hold ~130 of KDDS_TOPIC_MAX=160 topic slots;
 *  one shared topic + per-source seq dedup + idempotent state merge
 *  gives the same convergence (a beacon overwritten this poll interval
 *  is re-sent next interval) for 1 topic instead of DNODE_MAX.
 *
 *  LP64 / wire discipline (feedback_lp64_typedef_trap): fixed-width
 *  U1/UH/UW fields only; ids are U1[PFS_ID_LEN] byte arrays; every
 *  struct's exact byte size is pinned by _Static_assert in pfs_dag.c.
 *  The manifest is serialized as its packed in-memory image (all
 *  supported targets are little-endian — same assumption as the P1
 *  wire structs), so the same save yields the same manifest-id on
 *  every ABI.
 */

#pragma once
#include "kernel.h"
#include "pfs_block.h"

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

#define PFS_NAME_MAX     16            /* object-name chars (no NUL)   */
#define PFS_REF_MAX      8             /* named objects per node       */
#define PFSD_VERSION     1

#define PFSD_MAN_MAGIC   0x4E4D4650UL  /* "PFMN" LE                    */
#define PFSD_REF_MAGIC   0x46524650UL  /* "PFRF" LE                    */

#define PFSD_TOPIC_REF   "pfs/ref"     /* REGION-scoped ref gossip     */

#define PFSD_REF_PER_PKT 3             /* ref entries per beacon pkt   */
#define PFSD_BEACON_MS   800           /* full-state re-beacon period  */
#define PFSD_POLL_MS     100           /* beacon poll period           */
#define PFSD_LOG_MAX     64            /* max prev-chain walk depth    */

/* ------------------------------------------------------------------ */
/* manifest — one saved version; serialized AS a content-addressed     */
/* block (manifest-id = sha256 of these bytes), so P1 replicates it    */
/* ------------------------------------------------------------------ */

typedef struct {
    UW   magic;                    /* PFSD_MAN_MAGIC                    */
    UW   version;                  /* PFSD_VERSION                      */
    U1   prev[PFS_ID_LEN];         /* previous manifest id; 0 = genesis */
    U1   content[PFS_ID_LEN];      /* block id of this version's bytes  */
    UW   seq;                      /* 1-based version number on chain   */
    UW   content_len;              /* content byte count (for `log`)    */
    U1   origin;                   /* node that made this save          */
    U1   name_len;                 /* valid chars in name[]             */
    UH   _pad;
    char name[PFS_NAME_MAX];       /* object name, NUL-padded           */
} __attribute__((packed)) PFSD_MANIFEST;  /* 4+4+32+32+4+4+1+1+2+16 = 100 B */

/* ------------------------------------------------------------------ */
/* ref gossip wire (K-DDS "pfs/ref", REGION)                           */
/* ------------------------------------------------------------------ */

/* one ref: name -> head manifest. LWW merge key is seq (then a
 * deterministic manifest-id tie-break — see fork caveat above). */
typedef struct {
    char name[PFS_NAME_MAX];       /* NUL-padded                        */
    U1   head[PFS_ID_LEN];         /* head manifest id                  */
    UW   seq;                      /* head manifest's seq               */
    U1   origin;                   /* head manifest's origin            */
    U1   name_len;
    UH   _pad;
} __attribute__((packed)) PFSD_REF_ENT;    /* 16+32+4+1+1+2 = 56 B    */

/* beacon: a rotating window of the sender's ref table. State-based —
 * receiving the same packet twice is harmless (merge is idempotent);
 * seq only short-circuits re-processing of an unchanged slot. */
typedef struct {
    UW   magic;                    /* PFSD_REF_MAGIC                    */
    UW   seq;                      /* per-beaconer counter, starts at 1 */
    U1   src_node;
    U1   n_ent;                    /* valid entries in ent[]            */
    UH   _pad;
    PFSD_REF_ENT ent[PFSD_REF_PER_PKT];
} __attribute__((packed)) PFSD_REF_PKT;    /* 12 + 3*56 = 180 B        */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Init: open the REGION-scoped "pfs/ref" topic, clear the ref table.
 * Call after pmesh_init() + kdds_init() (and after pfs_repl_init() for
 * tidy boot-banner ordering; there is no hard dependency on it). */
void pfs_dag_init(void);

/* Ref gossip task (beacon own refs / merge peers'). Create after
 * drpc_init() — needs drpc_my_node, like pfs_repl_task. */
void pfs_dag_task(INT stacd, void *exinf);

/* Save a new version of <name>: content block + manifest block (both
 * announced to the region by P1) + local ref bump. Returns PFS_OK or a
 * negative PFS_E_* code. Works in SOLO mode too (purely local). */
INT  pfs_dag_save(const UB *name, UW nlen, const void *buf, UW len);

/* Shell dispatcher for the verbs after "pfs ":
 *   save <name> <text> / log <name> / cat <name> [@<seq>]
 * args points at the verb. Prints results/usage via tmonitor. */
void pfs_dag_cmd(const UB *args, UW len);
