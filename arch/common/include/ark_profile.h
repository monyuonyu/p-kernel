/*
 *  ark_profile.h — ark-profile v1: 人類の記憶 (the human chapter of the
 *  autobiography). Spec: docs/architecture/ark-profile.md.
 *
 *  A person opens their node's galaxy page, is shown the manifesto (the
 *  served bytes, content-id-bound), and — if and only if they explicitly
 *  acknowledge — the node records a consent entry bound to the content-id
 *  of the EXACT manifesto bytes they saw. Optionally they declare a handle,
 *  a real name and a free-text 未来への言葉. That declaration becomes a
 *  content-addressed p-fs object ("self/prof"), linked from the node's
 *  "self/lin" autobiography (the LM_SELF v2 human_ref field), replicated by
 *  P1, surviving death via the same durable restore the Self layer certifies.
 *  Every fact taught from a web mouth then carries an ARK_PROV provenance
 *  ref ("self/prov") that resolves to the profile in force.
 *
 *  THE ETHICAL CORE (ark-profile.md §3): consent must be REAL (bound to the
 *  exact words, not a brand); minimal pseudonymous participation is FIRST-
 *  CLASS (consent != disclosure); and the identity claim is honestly
 *  DECLARED-NOT-VERIFIED, BY DESIGN AND FOREVER — there is NO identity
 *  verification anywhere (no email, no key, no uniqueness check; only
 *  max-length bounds). A pen name, an anonymous handle, or a real name are
 *  all equally valid; that mix IS the honest history (歴史地層).
 *
 *  ANTI-FORK (§10): no second lineage chain (a FIELD on LM_SELF_ENTRY +
 *  pfs_dag's existing version manifests); no second consent store (consent
 *  lives ONLY in ARK_PROFILE.consent_ack + manifesto_id); no user DB; no
 *  second hash (pfs_id_compute only); no second teach path (the ONE prov
 *  write site is inside mind_cmd's teach verb); no new gossip wire (T1
 *  replicates as ordinary P1 blocks); no second HTTP server (routes added
 *  to galaxy.c).
 *
 *  arch/common discipline (the lm_self.h rule): fixed-width U1/U2/U4 only
 *  (NEVER UW/W — `long`, LP64-bloating), packed, _Static_assert'd wire size.
 */
#pragma once
#include "kernel.h"
#include "pfs_block.h"     /* PFS_ID_LEN, PFS_BLOCK_MAX */

/* ------------------------------------------------------------------ */
/* the profile object (ark-profile.md §4.1)                            */
/* ------------------------------------------------------------------ */

#define ARK_PROF_MAGIC   0x46525048UL   /* "HPRF" LE                       */
#define ARK_PROF_VER     1
#define ARK_HANDLE_MAX   24
#define ARK_NAME_MAX     48
#define ARK_MSG_MAX      1024           /* 未来への言葉, UTF-8              */

typedef struct {
    U4   magic;                      /* ARK_PROF_MAGIC                     */
    U4   version;                    /* ARK_PROF_VER                       */
    U1   self_id;                    /* drpc_my_node — the node stamp      */
    U1   consent_ack;                /* 1 = manifesto acknowledged         */
    U1   handle_len;                 /* 0 = pseudonymity declined too      */
    U1   name_len;                   /* 0 = real name not disclosed        */
    U4   seq;                        /* profile version, 1-based           */
    U4   age_ms;                     /* node uptime stamp (coarse)         */
    U4   wallclock;                  /* unix secs if the host knows; 0 =   */
                                     /* unknown, shown as unknown (honest) */
    U1   manifesto_id[PFS_ID_LEN];   /* content-id of the EXACT bytes the  */
                                     /* person saw (§7.1). NEVER all-zero  */
                                     /* in a valid profile.                */
    U1   lineage_head[PFS_ID_LEN];   /* self/lin head at declaration —     */
                                     /* anchors the human chapter to the   */
                                     /* machine autobiography              */
    char handle[ARK_HANDLE_MAX];     /* pseudonym, NUL-padded              */
    char name[ARK_NAME_MAX];         /* opt-in                             */
    U2   msg_len;
    U2   _pad;
    char msg[ARK_MSG_MAX];           /* 未来への言葉                        */
} __attribute__((packed)) ARK_PROFILE;   /* 4+4+4+12+32+32+24+48+4+1024 = 1188 B */

_Static_assert(sizeof(ARK_PROFILE) == 1188, "LP64-stable wire image");
_Static_assert(sizeof(ARK_PROFILE) <= PFS_BLOCK_MAX, "one p-fs block");

#define ARK_PROF_REF      "self/prof"   /* 9 chars <= PFS_NAME_MAX=16 */
#define ARK_PROF_REF_LEN  9

/* ------------------------------------------------------------------ */
/* the provenance record (ark-profile.md §5) — who taught this         */
/* ------------------------------------------------------------------ */

#define ARK_PROV_MAGIC   0x564F5250UL   /* "PROV" LE                       */
#define ARK_PROV_SRC_SHELL  0
#define ARK_PROV_SRC_WEB    1

typedef struct {
    U4 magic;                      /* ARK_PROV_MAGIC                        */
    U4 fact_seq;                   /* R3_FACT.seq of the taught fact        */
    U1 key, val;                   /* the binding, as declared              */
    U1 origin_node;                /* drpc_my_node                          */
    U1 src;                        /* 0 = shell verb, 1 = web (galaxy)      */
    U4 age_ms;
    U1 profile_head[PFS_ID_LEN];   /* content-id of the ARK_PROFILE version */
                                   /* in force; ALL-ZERO = anonymous node   */
                                   /* declaration (consent w/o disclosure)  */
} __attribute__((packed)) ARK_PROV;   /* 4+4+1+1+1+1+4+32 = 48 B */

_Static_assert(sizeof(ARK_PROV) == 48, "LP64-stable wire image");

#define ARK_PROV_REF      "self/prov"   /* 9 chars <= PFS_NAME_MAX=16 */
#define ARK_PROV_REF_LEN  9

/* ------------------------------------------------------------------ */
/* API — minimal publics (§10: the helpers are file-static in ark_profile.c) */
/* ------------------------------------------------------------------ */

/* The boot-computed content-id of the served manifesto bytes. Computed
 * once via pfs_id_compute over the embedded manifesto[] image; this id is
 * what every consent ack stores and what POST /profile validates `mid`
 * against. out must hold PFS_ID_LEN bytes. */
void ark_manifesto_id(U1 out[PFS_ID_LEN]);

/* The served manifesto bytes + length (the embedded image). The galaxy
 * GET /manifesto route streams these; ark_manifesto_id() hashes them.
 * These return the CANONICAL (ja) version — the default. */
const U1 *ark_manifesto_bytes(void);
UW         ark_manifesto_len(void);

/* ------------------------------------------------------------------ */
/* i18n (ark-profile.md §7.5) — the manifesto speaks many languages    */
/* ------------------------------------------------------------------ */
/* Each embedded language version is its OWN byte string -> its own
 * pfs_id_compute content-id. The consent ack records the id of the
 * version the person actually READ; ark_consent_ok / ark_manifesto_id_valid
 * accept ANY of the table's ids. The hosted build embeds ~32 languages;
 * the bare-metal build embeds ja+en only (no web UI; lean kernel). */

/* How many language versions are embedded in THIS build (>=1; ja always). */
UW ark_manifesto_count(void);

/* The i-th language's BCP-47 code + endonym (its name in its OWN language),
 * 0-based, i < ark_manifesto_count(). Returns 0 (NUL) on out-of-range. The
 * galaxy /langs route serializes these. */
const char *ark_manifesto_code(UW i);
const char *ark_manifesto_endonym(UW i);

/* The i-th language's bytes/len/content-id (i < ark_manifesto_count()).
 * id_out may be NULL. Returns 1 ok, 0 out-of-range. */
INT ark_manifesto_at(UW i, const U1 **bytes_out, UW *len_out, U1 id_out[PFS_ID_LEN]);

/* Resolve a BCP-47 code (case-insensitive, exact then primary-subtag prefix)
 * to a table index; -1 if no match. "en-US" -> the "en" row; "pt-BR" -> "pt".
 * The galaxy GET /manifesto?lang=xx and Accept-Language matcher use this. */
INT ark_manifesto_find(const char *code);

/* 1 iff mid equals the content-id of ANY embedded language version (the
 * consent-id TABLE). The galaxy POST /profile validates `mid` with this so a
 * person who read the Spanish words may ack the Spanish id. */
INT ark_manifesto_id_valid(const U1 mid[PFS_ID_LEN]);

/* Read the head "self/prof" profile into *out. Returns 1 if a profile
 * exists (out filled), 0 if none yet. Pure read; no side effects. */
INT ark_profile_head(ARK_PROFILE *out);

/* The consent gate predicate (ark-profile.md §7.3): 1 iff a profile head
 * exists with consent_ack==1 AND its manifesto_id matches the served
 * manifesto's boot-computed id (consent is to the exact words). Drives the
 * galaxy POST /teach 403-until-ack gate. consent != disclosure: an ack-only
 * empty profile returns 1. */
INT ark_consent_ok(void);

/* Build + save a new profile version (POST /profile). ack must be 1 and
 * mid must equal the served manifesto id (else the call refuses). handle/
 * name/msg are optional (NULL or empty = not disclosed); they are bounded,
 * never verified. seq = current head seq + 1 (1 for the first). Saves the
 * ARK_PROFILE under "self/prof" then appends ONE LM_SELF v2 entry whose
 * human_ref is its content-id. On success returns 1 and fills id_out (the
 * profile content-id) and *seq_out; returns 0 on a bad ack/mid, -1 on a
 * save failure. NO identity verification is performed (§3.3). */
INT ark_profile_save(U1 ack, const U1 mid[PFS_ID_LEN],
                     const char *handle, UW handle_len,
                     const char *name, UW name_len,
                     const char *msg, UW msg_len,
                     U1 id_out[PFS_ID_LEN], U4 *seq_out);

/* The ONE provenance write site (§5): append one ARK_PROV version of
 * "self/prov" for a just-taught fact. Called from mind_cmd's teach verb
 * (m_teach) immediately after r3_fact_learn returns 0 — NOT from
 * r3_fact_learn itself (certs that call r3_fact_learn directly stay
 * byte-identical). src is ARK_PROV_SRC_SHELL or ARK_PROV_SRC_WEB.
 * profile_head is read from the current "self/prof" head (all-zero when
 * there is no profile — an anonymous-node declaration). No-op safe at any
 * time. Defined as a real appender on every target (pfs_dag exists on bare
 * metal); only the mouth's src differs. */
void ark_prov_record(U4 fact_seq, U1 key, U1 val, U1 src);

/* Which mouth is currently driving mind_cmd's teach verb — set by the
 * caller right before mind_cmd("teach ...") so the single prov site can
 * stamp src. Defaults to shell. The galaxy POST /teach bridge sets WEB. */
void ark_teach_src_set(U1 src);
U1   ark_teach_src_get(void);

