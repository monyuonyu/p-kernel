/*
 *  gen_succession.h — Evolution layer: generational migration succession.
 *
 *  Spec: docs/architecture/30-module/evolution-migration-design.md. A living
 *  mind crosses an ARCHITECTURE gap (new R_DM / expert count / attention shape
 *  / vocab) without dying, carrying its IDENTITY (the Self-lineage) and its
 *  LEARNED KNOWLEDGE (engram replay), but NEVER its raw weights (rw[] IS the
 *  old architecture; a cross-arch weight load is exactly what the wave-47
 *  dims/vocab guard exists to REFUSE, r3_incontext.c:1183-1190).
 *
 *  This header defines the two content-addressed objects the succession
 *  protocol adds (design §7) plus the arch/common builders + the gate-5
 *  named-predecessor accept-conjunct helper. NO new crypto: every object is an
 *  ordinary p-fs object (pfs_id_compute), signed — where signed — by the
 *  shipped Ed25519 sign.c layer verbatim.
 *
 *  LENS A (byte-identity / crown): the succession machinery is HOSTED-tier
 *  (generation migration is hosted Linux only in v1, design §10). This TU is
 *  compiled ONLY into the hosted Makefiles (boot/linux + boot/linux_x86_64),
 *  NEVER the bare-metal link (boot/aarch64, boot/x86) — so the default
 *  aarch64/x86 .text and the crown are byte-IDENTICAL by construction (the
 *  compat_arkfs_gap.c / compat_ota.c precedent). r_forward is untouched. This
 *  header defines only STRUCTS + prototypes (no code), so including it in a
 *  bare-linked TU (it is not) would still not move .text.
 *
 *  arch/common discipline: fixed-width U1/U4 only (NEVER UW/W — they are
 *  `long` and bloat to 8 B on LP64), packed wire structs, _Static_assert on
 *  every wire size.
 */
#pragma once
#include "kernel.h"
#include "pfs_block.h"     /* PFS_ID_LEN, pfs_id_compute */

/* ------------------------------------------------------------------ */
/* the arch-spec object (design §7, native-student §A.3 reused): the    */
/* canonical, content-addressed description of ONE generation's R3      */
/* architecture. Its content-id (pfs_id_compute over these bytes) is    */
/* the generation's identity — two generations with a genuinely         */
/* different architecture (e.g. R_DM 48 vs 96) have DIFFERENT ids, so   */
/* their raw rw[] blobs are structurally incompatible (different R_NP). */
/* These are EXACTLY the fields the R3_WP durable-weights header pins    */
/* (r3_incontext.c:928-938): version + R_NP + both vocab content-ids.   */
/* ------------------------------------------------------------------ */

#define GEN_ARCHSPEC_MAGIC  0x48435241UL   /* "ARCH" LE                     */
#define GEN_ARCHSPEC_VER    1u

typedef struct {
    U4 magic;                       /* GEN_ARCHSPEC_MAGIC                    */
    U4 version;                     /* GEN_ARCHSPEC_VER                      */
    U4 r_dm;                        /* attention/thinking width (R_DM)       */
    U4 r_nh;                        /* attention heads (R_NH)                */
    U4 r_keyv;                      /* key vocab size (R_KEYV)               */
    U4 r_valv;                      /* answer vocab / output classes (R_VALV)*/
    U4 r_npair;                     /* dictionary entries per episode (R_NPAIR)*/
    U4 r_np;                        /* flat parameter count (the rw[] size)  */
    U1 key_vocab_id[PFS_ID_LEN];    /* r3_vocab_key_id_blob — pins the words */
    U1 val_vocab_id[PFS_ID_LEN];    /* r3_vocab_val_id_blob                  */
} __attribute__((packed)) GEN_ARCHSPEC;    /* 32 + 64 = 96 B */

_Static_assert(sizeof(GEN_ARCHSPEC) == 96,
               "GEN_ARCHSPEC must be 96 bytes (LP64-stable wire image)");
_Static_assert(sizeof(GEN_ARCHSPEC) <= PFS_BLOCK_MAX,
               "an arch-spec must fit one p-fs block");

/* ------------------------------------------------------------------ */
/* the succession bundle MANIFEST (design §7): names the predecessor +  */
/* successor arch-specs (by content-id), the engram flush + probe       */
/* digests the successor re-educates from, the cross-generational       */
/* invariant set (§6, incl. the conscience floor as HARD invariant #5), */
/* and the token-map (0 = superset vocab, no remap needed). Travels      */
/* INSIDE the signed OTA artifact body -> bound by sign gates 1-2, so a  */
/* body-swap of the predecessor name breaks the signature (design §5.1). */
/* ------------------------------------------------------------------ */

#define GEN_SUCC_MAGIC  0x43435553UL       /* "SUCC" LE                     */
#define GEN_SUCC_VER    1u

/* the minimal cross-generational invariant set (design §6). DELIBERATELY
 * small — every addition is bloat a future critique will call dishonest
 * (2026-06-14 external-critique response). Nothing is frozen as a FORMAT; each
 * is frozen as an OBLIGATION carried forward + proven in the succession cert. */
#define GEN_INV_LINEAGE   0   /* committed self/lin entries never rewritten   */
#define GEN_INV_ENVELOPE  1   /* SWIM membership fixed head (any gen sees any) */
#define GEN_INV_GATE      2   /* accept-gate >= the 4+1-gate AND              */
#define GEN_INV_NOHUMAN   3   /* the no-human-verification boundary           */
#define GEN_INV_FLOOR     4   /* 良心/Asimov floor (HARD; no-regress gated)    */
#define GEN_INV_N         5

typedef struct {
    U4 magic;                                   /* GEN_SUCC_MAGIC             */
    U4 version;                                 /* GEN_SUCC_VER              */
    U4 n_invariants;                            /* == GEN_INV_N              */
    U4 reserved;                                /* 0 (8-byte align)          */
    U1 predecessor_archspec_id[PFS_ID_LEN];     /* gate 5 names THIS         */
    U1 successor_archspec_id  [PFS_ID_LEN];     /* the generation we become  */
    U1 successor_pk           [PFS_ID_LEN];     /* 32B; all-zero = no rotation*/
    U1 engram_flush_id        [PFS_ID_LEN];     /* leg-1 exact replay source */
    U1 probe_digest_id        [PFS_ID_LEN];     /* leg-2 distill probe set   */
    U1 token_map_id           [PFS_ID_LEN];     /* 0 = superset vocab        */
    U1 invariant_ids[GEN_INV_N][PFS_ID_LEN];    /* §6 obligations, by id     */
} __attribute__((packed)) GEN_SUCC_MANIFEST;    /* 16 + 6*32 + 5*32 = 368 B  */

_Static_assert(sizeof(GEN_SUCC_MANIFEST) == 16 + 6*PFS_ID_LEN + GEN_INV_N*PFS_ID_LEN,
               "GEN_SUCC_MANIFEST wire size");
_Static_assert(sizeof(GEN_SUCC_MANIFEST) <= PFS_BLOCK_MAX,
               "a succession manifest must fit one p-fs block");

/* ------------------------------------------------------------------ */
/* production builders (extern, arch/common; hosted-only by Makefile     */
/* placement). No host libc; fixed-width; content-address via pfs.       */
/* ------------------------------------------------------------------ */

/* The flat R3 parameter count for an arbitrary generation's dims, mirroring
 * the O_* layout in r3_incontext.c:113-131 EXACTLY (the same math the shipped
 * header pins). Lets the cert compute a SUCCESSOR generation's R_NP (R_DM=96)
 * without a second binary, and prove it differs from the predecessor's — the
 * structural reason a raw rw[] blob cannot cross the gap. */
U4  gen_r_np(U4 r_dm, U4 r_nh, U4 r_keyv, U4 r_valv, U4 r_npair);

/* Fill an arch-spec object canonically + compute its content-id. */
void gen_archspec_fill(GEN_ARCHSPEC *out, U4 r_dm, U4 r_nh, U4 r_keyv,
                       U4 r_valv, U4 r_npair,
                       const U1 key_vocab_id[PFS_ID_LEN],
                       const U1 val_vocab_id[PFS_ID_LEN]);
void gen_archspec_id(const GEN_ARCHSPEC *a, U1 id_out[PFS_ID_LEN]);

/* Build a succession bundle manifest + its content-id. successor_pk NULL =>
 * no key rotation (all-zero). token_map_id NULL => superset vocab (all-zero).
 * invariant_ids is the §6 obligation-id array (GEN_INV_N entries). */
void gen_succ_manifest_build(GEN_SUCC_MANIFEST *out,
                             const U1 predecessor_archspec_id[PFS_ID_LEN],
                             const U1 successor_archspec_id[PFS_ID_LEN],
                             const U1 successor_pk[PFS_ID_LEN],
                             const U1 engram_flush_id[PFS_ID_LEN],
                             const U1 probe_digest_id[PFS_ID_LEN],
                             const U1 token_map_id[PFS_ID_LEN],
                             const U1 invariant_ids[GEN_INV_N][PFS_ID_LEN]);
void gen_succ_manifest_id(const GEN_SUCC_MANIFEST *m, U1 id_out[PFS_ID_LEN]);

/* The dims/vocab guard PREDICATE (the shipped wave-47 guard, r3_incontext.c:
 * 1183-1190, distilled): a persisted weight blob loads into a build ONLY if
 * its r_np AND both vocab content-ids match the build's. Returns 1 = accept,
 * 0 = REFUSE. The cert drives this with a PREDECESSOR blob against a SUCCESSOR
 * build to prove the raw-weight cross-load is refused (F1 anti-theater). */
INT gen_dims_guard_accepts(U4 blob_r_np, const U1 blob_key_vocab[PFS_ID_LEN],
                           const U1 blob_val_vocab[PFS_ID_LEN],
                           U4 build_r_np, const U1 build_key_vocab[PFS_ID_LEN],
                           const U1 build_val_vocab[PFS_ID_LEN]);

/* [generation-survives] cert (design §8). Compiled ONLY under
 * -DGEN_SURVIVE_CERT (and hosted via _TK_HOSTED_LIBC_), so the default kernel
 * + crown are byte-identical. `compat test gen` drives it. Falsifiers:
 *   F1 arch-delta guard (archspec ids differ + raw blob load REFUSED)
 *   F2 -DGEN_SKIP_EDUCATE (skip replay -> facts to chance -> recovery RED)
 *   F3 impostor (forged successor rejected by the pinned-key verify)
 *   F4 illegal successor (wrong predecessor -> gate5; lower ver -> gate4)
 *   F5 -DOTA_SKIP_VERIFY (still RED through the accept step). */
#if defined(GEN_SURVIVE_CERT) && defined(_TK_HOSTED_LIBC_)
void gen_survive_test(void);
#endif
