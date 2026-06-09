/*
 *  lm_self.h — living-mind Self layer (first slice): a distributed
 *  autobiographical self.
 *
 *  Spec: docs/architecture/living-mind.md Part III. A per-node, hash-chained
 *  NARRATIVE LINEAGE (LM_SELF_ENTRY versions of the p-fs object "self/lin")
 *  that (1) survives the node's DEATH (reconstructs from the persisted store,
 *  not RAM), (2) is TAMPER-EVIDENT (any alteration of a committed entry is
 *  detectable; the walker fails closed), and (3) reconstructs from a peer
 *  subset EXCLUDING the origin (ownerless, no central owner) — and is
 *  continued by a successor (the identity persists THROUGH death).
 *
 *  ANTI-FORK (living-mind.md III.7): this module owns NO new hash / merkle /
 *  crypto and NO new gossip loop. It drives the SAME content-address +
 *  durable-DAG substrate everything else uses:
 *    - pfs_block.h: pfs_id_compute / pfs_get / pfs_has  (THE sha256 chain)
 *    - pfs_dag.h:   pfs_dag_save / pfs_dag_read / pfs_dag_restore
 *    - pfs_block.h: pfs_durable_restore                  (reload after death)
 *    - drpc.h:      drpc_my_node                         (the self_id stamp)
 *    - genome.h:    GENOME_WEIGHTS_REF ("dtr/weights")   (the model_ver object)
 *    - lm_consolidate.h: LM_ENGRAM                       (eng_digest summarizes)
 *  The chain link is the CONTENT-LEVEL walk (III.4 recommended option):
 *  prev_entry is a content-id (pfs_id_compute), walked with pfs_get — so
 *  pfs_dag.c is NOT modified (zero change to the shared P2 substrate).
 *
 *  HONEST BOUND (living-mind.md III.6): tamper-EVIDENT, NOT tamper-PROOF.
 *  There is no signature primitive in the tree (genome.h states the same
 *  limit), so a malicious node that controls its own store can author a
 *  fresh, internally-consistent fake lineage FROM GENESIS. The teeth we DO
 *  claim: you cannot ALTER or SPLICE an already-committed entry without the
 *  content-address chain breaking (detected, fail-closed), and the chain
 *  reconstructs with NO owner. Per-manifest signatures are deferred.
 *
 *  arch/common discipline: no host libc, fixed-width types (U1/U2/U4 — the
 *  unconditional widths in typedef.h, NEVER UW/W which are `long` and bloat
 *  on LP64), static (not task-stack) buffers, output via sio_send_frame,
 *  _Static_assert on the wire size.
 */

#pragma once
#include "kernel.h"
#include "pfs_block.h"      /* PFS_ID_LEN */

/* ------------------------------------------------------------------ */
/* one self-narrative version (living-mind.md III.3). Fixed-width /     */
/* packed / _Static_assert'd. genesis: prev_entry = all-zero.          */
/* ------------------------------------------------------------------ */

#define LM_SELF_MAGIC  0x464C4553UL    /* "SELF" LE                     */
#define LM_SELF_VER    1

typedef struct {
    U4  magic;                     /* LM_SELF_MAGIC                      */
    U4  version;                   /* LM_SELF_VER                        */
    U1  self_id;                   /* ORIGIN node identity (drpc_my_node)*/
    U1  _pad0;
    U2  _pad1;
    U4  seq;                       /* 1-based position on the chain      */
    U4  age_ms;                    /* coarse timestamp                   */
    U1  prev_entry[PFS_ID_LEN];    /* content-id of prev entry (chain)   */
    U1  eng_digest[PFS_ID_LEN];    /* pfs_id_compute over LM_ENGRAM ring */
    U1  model_ver [PFS_ID_LEN];    /* content-id of "dtr/weights" blob   */
} __attribute__((packed)) LM_SELF_ENTRY;   /* 116 B */

_Static_assert(sizeof(LM_SELF_ENTRY) == 116,
               "LM_SELF_ENTRY must be 116 bytes (LP64-stable wire image)");
_Static_assert(sizeof(LM_SELF_ENTRY) <= PFS_BLOCK_MAX,
               "a self entry must fit one p-fs block");

/* the lineage object name (8 chars <= PFS_NAME_MAX=16; one PFS_REF_MAX slot) */
#define LM_SELF_REF      "self/lin"
#define LM_SELF_REF_LEN  8

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* The falsifiable acceptance suite (living-mind.md III.5). Emits printed
 * evidence then a canonical "[self-*] PASS/FAIL" line for each of:
 *   [self-continuity]     survives death + continues forward
 *   [self-tamperevident]  a forged/edited self is detected, fail-closed
 *   [self-ownerless]      reconstruct from a peer subset excluding the origin
 * Wired to CI via the `self test` shell verb. */
void lm_self_test(void);
