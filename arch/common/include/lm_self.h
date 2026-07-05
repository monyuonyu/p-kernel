/*
 *  lm_self.h — living-mind Self layer (first slice): a distributed
 *  autobiographical self.
 *
 *  Spec: docs/architecture/30-module/living-mind.md Part III. A per-node, hash-chained
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
#define LM_SELF_VER    2

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
    U1  human_ref [PFS_ID_LEN];    /* ark-profile v2: content-id of the   */
                                   /* ARK_PROFILE in force; all-zero = no  */
                                   /* human chapter (ark-profile.md §4.2)  */
} __attribute__((packed)) LM_SELF_ENTRY;   /* 148 B (v2; v1 was 116) */

/* The v1 entry width — the dual-width walker accepts BOTH (reads
 * magic+version first, then size-checks per version). Old v1 stores still
 * verify; v2 adds the human_ref tail. (ark-profile.md §4.2 / P5.) */
#define LM_SELF_ENTRY_V1_SIZE  116

_Static_assert(sizeof(LM_SELF_ENTRY) == 148,
               "LM_SELF_ENTRY must be 148 bytes (v2 LP64-stable wire image)");
_Static_assert(sizeof(LM_SELF_ENTRY) <= PFS_BLOCK_MAX,
               "a self entry must fit one p-fs block");

/* the lineage object name (8 chars <= PFS_NAME_MAX=16; one PFS_REF_MAX slot) */
#define LM_SELF_REF      "self/lin"
#define LM_SELF_REF_LEN  8

/* ------------------------------------------------------------------ */
/* selfc-ring3 §1.3 — self-built-unit lineage events                   */
/* ------------------------------------------------------------------ */
/* Germination, reap and rollback of a self-built unit are autobiographical
 * events. Each appends ONE LM_SELF_ENTRY to the EXISTING hash-chained
 * "self/lin" lineage — no second chain (anti-fork §6). The event kind +
 * the unit's version are encoded deterministically into the entry's age_ms
 * field (so the same event yields the same content-id on every ABI and the
 * walker still hash-verifies); eng_digest summarizes the event descriptor
 * via the SAME pfs_id_compute content address used everywhere. A node's
 * history of rebuilding itself thus rides the autobiography that already
 * survives death and reconstructs ownerless. */

#define LM_UNIT_EV_GERM      1     /* a unit version germinated (forked)     */
#define LM_UNIT_EV_REAP      2     /* a unit version was reaped (died)       */
#define LM_UNIT_EV_ROLLBACK  3     /* rolled back from one seq to the prev   */
#define LM_SELF_EV_INTROSPECT 4    /* self-access R0: the body was READ      */
#define LM_SELF_EV_REFUSE     5    /* 良心: refused a harmful request (the    */
                                   /* mind remembers it said no; uv=site,     */
                                   /* sig=harm class). conscience.md §1.3.    */

/* age_ms layout for a unit event: [31:28]=kind [27:20]=sig [19:0]=uv|to<<10 */
#define LM_UNIT_EV_ENCODE(kind, uv, sig) \
    (((U4)((kind) & 0xF) << 28) | ((U4)((sig) & 0xFF) << 20) | ((U4)(uv) & 0xFFFFF))
#define LM_UNIT_EV_KIND(age)  (((age) >> 28) & 0xF)
#define LM_UNIT_EV_SIG(age)   (((age) >> 20) & 0xFF)
#define LM_UNIT_EV_UV(age)    ((age) & 0xFFFFF)

/* Append one unit-lifecycle event to the live "self/lin" chain (the same
 * pfs_dag_save path lm_self_test uses). kind is LM_UNIT_EV_*; unit_ver is
 * the unit's pfs_dag seq the event concerns; sig is the reap signal (0 for
 * germ/rollback). Returns PFS_OK or a negative PFS_E_* code. Local-store
 * only; P1 replicates the appended blocks like any other self entry. */
INT lm_self_append_unit_event(UB kind, U4 unit_ver, UB sig);

/* self-access R0 (docs/architecture/30-module/self-access.md): record ONE
 * self-introspection ("body-touch") event onto the live "self/lin" chain.
 * Q3=YES: each explicit READ-ONLY self-examination becomes part of the
 * mind's honest autobiography. Encodes LM_SELF_EV_INTROSPECT as the event
 * kind; `domains` is a bitmask of what was read (bits: 1=stats 2=tasks
 * 4=files 8=devices) carried in the unit_ver slot, so the lineage records
 * WHAT the mind looked at without storing any read CONTENT. ONE entry per
 * explicit invocation (do not spam the lineage). Returns PFS_OK or a
 * negative PFS_E_* code. */
INT lm_self_append_introspect(U4 domains);

/* READ-ONLY: the seq of the current "self/lin" head (0 if the chain is
 * empty / has no readable head). Lets a caller prove an append landed by
 * observing the head seq increment. Reads the head manifest only. */
U4  lm_self_head_seq(void);

/* selfc-ring3 §5.3 [selfc-lineage]: walk the live "self/lin" chain, verify it
 * hash-chains end-to-end (the wave-22 verifier), and count the unit events by
 * kind. Returns 1 iff the chain verifies (fail-closed otherwise). Out params
 * (any may be NULL) receive the germ/reap/rollback counts and the verify
 * verdict. */
INT lm_self_unit_lineage_check(INT *n_germ, INT *n_reap, INT *n_roll,
                               INT *ok_chain);

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

/* ark-profile v1 (ark-profile.md §4.2): append ONE new "self/lin" v2 entry
 * whose human_ref is the content-id of the ARK_PROFILE just saved, linking
 * the human chapter into the ONE autobiographical chain (no parallel chain).
 * Reads the current head version as prev_entry; genesis-safe (all-zero prev
 * when there is no head yet). Returns 1 on success, 0 on a save failure.
 * The PRODUCTION append path (lm_self_test builds its own synthetic chain);
 * called by ark_profile_save(). */
INT lm_self_append_human(const U1 human_ref[PFS_ID_LEN]);
