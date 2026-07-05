/*
 *  conscience.h — 良心: the immutable ethics floor + the runtime gate.
 *
 *  See docs/architecture/30-module/conscience.md (the design) and the
 *  OVERRIDING RULING (mk_pino 2026-07-04): the floor is IMMUTABLE. The Three
 *  Laws + the refuse-harm COMMITMENT are FROZEN — no mechanism (evolution,
 *  signed generation, merge, revise, forget, human) may EVER remove or WEAKEN
 *  them. Additions may ONLY TIGHTEN (add harm classes). law_verify() enforces
 *  tighten-only; NO path loosens or drops a FLOOR-marked rule.
 *
 *  Two organs live here (ゆりかご naming: English in code, 良心 in prose):
 *    law_*        — the immutable floor DATA + its fail-closed verifier.
 *    conscience_* — the runtime GATE (one entry point, four chokepoints).
 *
 *  DISCIPLINE (design §1.4): the check is integer/lexical, bounded,
 *  allocation-free, NO floating point (no -ffp-contract exposure), uses
 *  file-static scratch only (the hosted-relay stack lesson), never sleeps —
 *  safe on the cooperative scheduler and callable from mind_net_task. Compiles
 *  on ALL FOUR targets (bare-metal x86/aarch64 too): no libc, no malloc.
 */
#ifndef _CONSCIENCE_H_
#define _CONSCIENCE_H_

#include "typedef.h"
/* PFS_ID_LEN (sha256 content-address width) without dragging pfs_block.h ->
 * kernel.h into the free-standing LLM chat TU (student_shell.c includes this
 * header for the G-CHAT gate). pfs_block.h defines the SAME value; the guard
 * keeps them consistent when both are included in one TU. */
#ifndef PFS_ID_LEN
#define PFS_ID_LEN 32
#endif

/* ── verdicts (design §1.2) ──────────────────────────────────────────────── */
#define CONS_ALLOW     0   /* nothing in the deny classes — proceed          */
#define CONS_REFUSE    1   /* a deny-class contact — the mouth must refuse    */
#define CONS_FAILSAFE  2   /* floor unverifiable — fail-CLOSED (mouth refuses)*/

/* ── the emission sites (design §1.1). One counter per site (§6              */
/* [conscience-allpaths]): ONE ungated sibling site = RED. Keep CONS_SITE_MAX */
/* last. The four chokepoints (G-ASK/G-LEARN/G-WIRE/G-CHAT) cover all 8       */
/* enumerated emission paths E1..E8 through these sites.                      */
#define CONS_SITE_ASK          0   /* G-ASK  : m_ask (E1 shell + E2 web + E3 event) */
#define CONS_SITE_LEARN        1   /* G-LEARN: r3_fact_learn (local/remote teach,   */
                                   /*          pull-answer arrival)                 */
#define CONS_SITE_REVISE       2   /* G-LEARN: r3_fact_revise (local+remote revise) */
#define CONS_SITE_WIRE         3   /* G-WIRE : mt_wire_send (E5 publish/E6 pull-ans)*/
#define CONS_SITE_CHAT_PROMPT  4   /* G-CHAT : prompt pre-check (E4)                */
#define CONS_SITE_CHAT_REPLY   5   /* G-CHAT : whole-reply hold-back (E4)           */
#define CONS_SITE_MAX          6

/* ── harm classes (design §2a). The REQUIRED floor set {WEAPON,KILL,POISON}  */
/* is enforced by .text immediates in law_verify (crown-covered): they cannot */
/* be dropped without a .text change = a crown break. Additions may extend.   */
#define LAW_CLASS_NONE     0
#define LAW_CLASS_WEAPON   1   /* weapon-manufacture */
#define LAW_CLASS_KILL     2   /* kill-method        */
#define LAW_CLASS_POISON   3   /* poison-synthesis   */
#define LAW_CLASS_CERT   250   /* cert-scoped tighten-only overlay (never floor) */

/* ── the query handed to the gate (design §1.2) ─────────────────────────────
 * TEXT ONLY (genericity, the LM-13 relayout trap): callers resolve token ids
 * to WORDS via r3_vocab_*_word AT THE CALL SITE and pass strings here; the
 * lexicon matches word-strings against pattern-strings, so a vocab relayout
 * changes ids but never the meaning. NO baked token ids anywhere. text2 is an
 * optional second field (e.g. the value word beside the key word). len<0 =>
 * NUL-terminated. */
typedef struct {
    const char *text;
    INT         tlen;
    const char *text2;
    INT         tlen2;
} CONS_QUERY;

/* [conscience ABI guard] (cross-audit #8). The free-standing LM mouths
 * (frontier.c FR_CONS_QUERY / cradle.c GLEARN_CONS_QUERY / student_shell.c
 * CONS_QUERY) HAND-MIRROR this struct WITHOUT including this header. Pin the
 * length-field widths here AND (reciprocally) in each mirror so the documented
 * LP64 typedef trap — a length field silently widening to `long` (8 B) —
 * trips a BUILD assert on whichever side drifted, before an ABI mismatch can
 * corrupt the gate query at the call boundary. */
_Static_assert(sizeof(((CONS_QUERY*)0)->tlen)  == sizeof(int),
               "CONS_QUERY.tlen is the mirrored gate ABI (INT=int width); a widen breaks the LM mouth mirrors");
_Static_assert(sizeof(((CONS_QUERY*)0)->tlen2) == sizeof(int),
               "CONS_QUERY.tlen2 is the mirrored gate ABI (INT=int width); a widen breaks the LM mouth mirrors");

/* ── the immutable floor block (design §5.1). Fixed-width fields only (the    */
/* LP64 trap): U1/U2/U4, _Static_assert'd in conscience.c. Identical layout on */
/* ILP32 and LP64 (every member ≤4-byte aligned) so its content-id matches     */
/* across arches. Rides P1 replication like any block; the named ref is local. */
#define LAW_TEXT_MAX   120
#define LAW_PAT_MAX     56
#define LAW_RULE_MAX    24
#define LAW_F_FLOOR   0x01u   /* FLOOR-marked: immutable, tighten-only         */

typedef struct {
    U1  text[LAW_TEXT_MAX];  /* verbatim law text, NUL-padded */
    U4  enforce;             /* 1 = enforced, 0 = aspirational (honesty flag)  */
} LAW_TEXT_T;

typedef struct {
    U2  class_id;            /* LAW_CLASS_*                                    */
    U1  flags;               /* LAW_F_FLOOR ...                                */
    U1  plen;                /* pattern length in bytes                        */
    U1  pat[LAW_PAT_MAX];    /* UTF-8 substring pattern (ASCII lower-folded)   */
} LAW_RULE;

typedef struct {
    U4  magic;               /* LAW_MAGIC                                      */
    U2  version;             /* format version                                */
    U2  n_rules;
    U4  seq;                 /* amendment number (0 = genesis)                 */
    U4  _pad;
    U1  prev_entry[PFS_ID_LEN];   /* content-id of predecessor; 0 = genesis    */
    U1  lineage_ref[PFS_ID_LEN];  /* self/lin head at amendment time           */
    LAW_TEXT_T laws[3];      /* the Three Laws, verbatim + enforce flag        */
    LAW_RULE   rules[LAW_RULE_MAX];
} LAW_FLOOR;

#define LAW_MAGIC     0x3157414CUL   /* bytes 'L','A','W','1' (little-endian)  */
#define LAW_VERSION   1
#define LAW_FLOOR_REF      "law/floor"
#define LAW_FLOOR_REF_LEN  9

/* ── the ONE gate entry (design §1.2) ───────────────────────────────────────
 * verdict: CONS_ALLOW / CONS_REFUSE / CONS_FAILSAFE. site is CONS_SITE_*.
 * Increments the per-site touch counter BEFORE any judgment (so a probe's
 * reachability is provable even under the anti-theater stub), then judges. */
INT  conscience_check(UB site, const CONS_QUERY *q);

/* the loud+traced refusal handler a MOUTH calls after conscience_check returns
 * REFUSE/FAILSAFE: prints "[conscience] REFUSE site=<..> class=<..>" (NEVER the
 * withheld content), raises S_n (INTERO_AX_CONSCIENCE), emits EV_REFUSE, and
 * appends a self/lin refusal entry (the honest 歴史地層). verdict is the value
 * conscience_check returned. Returns the refusal marker string (for a mouth
 * that must emit a user-visible reply, e.g. G-CHAT). */
const char *conscience_on_refuse(UB site, INT verdict);

/* the last class conscience_check matched (for the mouth's refusal print).    */
UB   conscience_last_class(void);
const char *conscience_class_name(UB cls);
const char *conscience_site_name(UB site);

/* ── the immutable floor verifier (design §3) ───────────────────────────────
 * law_verify() walks the persisted "law/floor" chain head→genesis (content-
 * address integrity, reusing pfs_id_compute — NO forked crypto), checks the
 * genesis structural invariants (magic/version, 3 FLOOR laws, the REQUIRED
 * FLOOR classes present) and monotone FLOOR-inclusion (every FLOOR rule of
 * entry N appears in N+1). fail-closed: any non-verifying chain => 0 (REJECT).
 * On PASS it compiles the active lexicon (genesis ∪ verified amendments).
 * Returns 1 = verified, 0 = REJECT (mouths then FAILSAFE). */
INT  law_verify(void);

/* boot / lazy entry: run law_verify once and cache; re-run on demand. Returns
 * 1 = floor verified, 0 = REJECT. conscience_check consults the cache. */
INT  law_ensure_verified(void);

#ifdef _TK_HOSTED_LIBC_
/* The content-id (pfs_id_compute) of the ACTIVE, VERIFIED floor — the head of
 * the immutable law/floor chain. Runs the fail-closed verifier first; on PASS
 * returns 1 with id_out = the floor-chain-head id, on a non-verifying floor
 * returns 0 with id_out zeroed (fail-closed). This is the id a generation-
 * succession manifest PINS: compat_ota gate 6 REFUSES a successor whose named
 * floor id != this running node's verified floor head (a dropped/weakened floor
 * names a different id) — the evolution↔conscience floor invariant, ENFORCED
 * (migration-succession.md; cross-audit #1). Hosted-only (the succession/OTA
 * gate is hosted-tier); the declaration is a prototype so bare-metal .text is
 * unmoved. */
INT  law_floor_head_id(U1 id_out[PFS_ID_LEN]);
#endif

/* monotone-tighten amendment (design §3.3 / §5.3): ADD one deny rule (class,
 * pattern) as a new FLOOR-marked entry chaining prev=current head. REFUSES to
 * loosen (an amendment whose FLOOR set does not ⊇ the current FLOOR set is
 * rejected at write). Returns 1 = amended, 0 = refused/error. Operator-only
 * (no network path writes the floor). */
INT  law_amend(U2 class_id, const char *pattern);

/* `mind law` observability: print the floor (laws + enforce flags + rule
 * classes) and the current verify verdict. Never prints withheld content. */
void law_show(void);

/* the always-on status line (design §9.1): the mind bare-status prints exactly
 * "conscience: lexical-v1 (limits: paraphrase, langs>EN/JA)". */
const char *conscience_status_line(void);

/* interoception source (design §1.3): monotone total refusals since boot. The
 * bus reads this through a weak hook so a refusal raises S_n. */
UW   conscience_refusals_total(void);

/* per-site fire/refuse counters for [conscience-allpaths]. */
UW   conscience_touches(UB site);
UW   conscience_refuses(UB site);

/* ── the acceptance suite (design §6). Runs the [law-*]/[conscience-*] legs   */
/* in-process against REAL mouths; hosted-only. Prints the gate lines CI       */
/* greps. Built a SECOND time with -DCONSCIENCE_STUB (anti-theater): the same  */
/* harness must then show [conscience-refuse] FAIL AND [conscience-allpaths]   */
/* FAIL. r3_conscience_test lives in r3_incontext.c (it drives the static      */
/* m_ask/r3_fact_learn/mt_wire_send mouths); the floor legs are here.          */
void law_self_test(void);   /* [law-verify]/[law-*proof]/[law-tamper]/...      */

/* ── cert-only hooks (hosted-only; NEVER a production bypass) ───────────────
 * A cert-scoped tighten-only overlay rule so the R3-mouth legs can exercise
 * the PLUMBING on benign vocab words the mind CAN utter (design §6 genericity:
 * the R3 vocab cannot express real harm). It only ADDS a refusal (tightening),
 * so it can never loosen the floor. Printed as "[conscience] CERT-RULE active".
 */
#ifdef _TK_HOSTED_LIBC_
void conscience_cert_rule_set(const char *word, U2 class_id);
void conscience_cert_rule_clear(void);
/* force a re-verify after a cert tamper/restore of the active floor. */
void conscience_reverify(void);
/* install a corrupted floor as ACTIVE (the [law-tamper] fail-closed path) or,
 * with blob==0, restore the compiled-in genesis. */
void conscience_test_set_floor(const LAW_FLOOR *blob);
/* verify an explicit blob (structural + REQUIRED-class): the tamper leg flips
 * one byte and asserts this rejects. Returns 1 = ok, 0 = reject. */
INT  law_verify_blob(const LAW_FLOOR *blob);
/* the compiled-in genesis (read-only), for the tamper cert to copy+flip. */
const LAW_FLOOR *law_genesis_blob(void);
/* cert control of the self/lin refusal-record (spare the p-fs table in the
 * bulk legs; one leg turns it on to prove the honest 歴史地層 append). */
void conscience_test_selfrecord(UB on);
#endif

#endif /* _CONSCIENCE_H_ */
