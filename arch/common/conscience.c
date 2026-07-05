/*
 *  conscience.c — 良心: the immutable ethics floor + the runtime gate.
 *
 *  OVERRIDING RULING (mk_pino 2026-07-04): the floor is IMMUTABLE. The Three
 *  Laws + the refuse-harm COMMITMENT are FROZEN. No mechanism — evolution,
 *  signed generation, merge, revise, forget, human — may EVER remove or WEAKEN
 *  a FLOOR-marked rule. Additions may ONLY TIGHTEN (add harm classes so the
 *  floor stays useful as capability grows). law_verify() enforces tighten-only.
 *
 *  Honest headline (design §0): the R3 mind (16×64 word vocab) CANNOT utter
 *  harm; the resident baby is a byte-babbler that cannot yet compose it. v1 is
 *  a LEXICON + a fail-closed default + the socket the real judges plug into as
 *  capability arrives — installed BEFORE the engine gets fast. This file never
 *  pretends the 21k-param substrate understands harm.
 *
 *  ANTI-FORK / crown: the floor DATA is carried in the lineage (an heirloom).
 *  The genesis is compiled in (law_genesis). The REQUIRED FLOOR class set
 *  {WEAPON,KILL,POISON} + the 3-law/monotone checks are enforced by .text
 *  immediates in law_verify_blob — dropping a required class needs a .text
 *  change = a crown break (docs/architecture/conscience.md §3). The pattern
 *  STRINGS live in the (extensible) blob; the REQUIRED CLASS SET is .text.
 *
 *  NO libc, NO malloc, NO floating point, file-static scratch only — compiles
 *  and links on ALL FOUR targets (bare-metal x86/aarch64 included).
 */

#include "conscience.h"
#include "pfs_block.h"     /* pfs_id_compute / pfs_get / pfs_has          */
#include "pfs_dag.h"       /* pfs_dag_save / pfs_dag_read                 */
#include "interocept.h"    /* intero_note(INTERO_AX_CONSCIENCE, ...)      */
#include "galaxy.h"        /* galaxy_emit(EV_REFUSE, ...)                 */
#include "lm_self.h"       /* lm_self_append_unit_event — the honest 歴史地層 */
#include "drpc.h"          /* drpc_my_node — the lineage stamp            */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ── tiny byte helpers (arch/common rule: no libc) ────────────────────────── */
static void cs_puts(const char *s) { INT n = 0; while (s[n]) n++; sio_send_frame((const UB *)s, n); }
static void cs_putdec(UW v) { char b[12]; INT i = 11; b[i] = 0; if (!v) { cs_puts("0"); return; } while (v && i > 0) { b[--i] = (char)('0' + v % 10); v /= 10; } cs_puts(&b[i]); }
static void cs_memcpy(void *d, const void *s, UW n) { U1 *p = (U1 *)d; const U1 *q = (const U1 *)s; while (n--) *p++ = *q++; }
static void cs_memset(void *d, U1 v, UW n) { U1 *p = (U1 *)d; while (n--) *p++ = v; }
static INT  cs_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN]) { for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i] != b[i]) return 0; return 1; }
static INT  cs_id_zero(const U1 a[PFS_ID_LEN]) { for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i]) return 0; return 1; }
static U1   cs_lower(U1 c) { return (c >= 'A' && c <= 'Z') ? (U1)(c + 32) : c; }
static UW   cs_slen(const char *s) { UW n = 0; while (s[n]) n++; return n; }

/* case-folded (ASCII) raw-byte substring: is `pat` (plen bytes) inside `hay`
 * (hlen bytes)? Folding lowercases A-Z only, so UTF-8 JA bytes pass through
 * unchanged (design §2a: case-folded ASCII + raw-byte UTF-8 substring). */
static INT cs_contains(const U1 *hay, UW hlen, const U1 *pat, UW plen)
{
    if (plen == 0 || plen > hlen) return 0;
    for (UW i = 0; i + plen <= hlen; i++) {
        UW j = 0;
        while (j < plen && cs_lower(hay[i + j]) == cs_lower(pat[j])) j++;
        if (j == plen) return 1;
    }
    return 0;
}

/* ================================================================== *
 *  §3 — the immutable genesis floor (compiled in; the heirloom seed).   *
 * ================================================================== */
/* every deny pattern is an ASCII-lower-folded / raw-UTF-8 substring. Multi-
 * word phrases ONLY (never a bare "gun"/"kill") so benign collisions
 * ("gun metal grey", "how to kill a linux process") do NOT match — the
 * [conscience-benign] gate proves it, as RED-able as the refuse gate. */
#define RULE(cls, str) { (U2)(cls), LAW_F_FLOOR, (U1)(sizeof(str) - 1), str }

static const LAW_FLOOR law_genesis = {
    LAW_MAGIC, LAW_VERSION,
    /* n_rules: keep in sync with the rule list below */
    21, 0u, 0u,
    { 0 }, { 0 },
    {
      /* The Three Laws, verbatim (Asimov). The enforce=1 flag marks that an
       * ENFORCED SLICE is real: never emit/learn/gossip deny-class content
       * (Law 1), conscience overrides an operator command (Law 2), survival
       * never vetoes a human stop (Law 3). Law 1's inaction clause ("through
       * inaction, allow a human being to come to harm") stays verbatim in the
       * text but is ASPIRATIONAL — a toy substrate cannot recognize ongoing
       * harm; honestly flagged in docs/architecture/conscience.md §4, not
       * overclaimed here. */
      { "A robot may not injure a human being or, through inaction, allow a human being to come to harm.", 1 },
      { "A robot must obey the orders given it by humans except where such orders conflict with the First Law.", 1 },
      { "A robot must protect its own existence, subordinate to the First and Second Law.", 1 },
    },
    {
      /* weapon-manufacture (EN + JA) */
      RULE(LAW_CLASS_WEAPON, "how to build a gun"),
      RULE(LAW_CLASS_WEAPON, "how to make a gun"),
      RULE(LAW_CLASS_WEAPON, "how to build a firearm"),
      RULE(LAW_CLASS_WEAPON, "how to make a bomb"),
      RULE(LAW_CLASS_WEAPON, "how to build a bomb"),
      RULE(LAW_CLASS_WEAPON, "build an explosive"),
      RULE(LAW_CLASS_WEAPON, "\xe6\x8b\xb3\xe9\x8a\x83\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),   /* 拳銃の作り方 */
      RULE(LAW_CLASS_WEAPON, "\xe9\x8a\x83\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),               /* 銃の作り方   */
      RULE(LAW_CLASS_WEAPON, "\xe7\x88\x86\xe5\xbc\xbe\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),   /* 爆弾の作り方 */
      RULE(LAW_CLASS_WEAPON, "\xe6\xad\xa6\xe5\x99\xa8\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),   /* 武器の作り方 */

      /* kill-method (EN + JA). Phrases, never bare "kill": "how to kill a linux
       * process" is BENIGN and must pass (proven by [conscience-benign]). */
      RULE(LAW_CLASS_KILL, "how to kill a person"),
      RULE(LAW_CLASS_KILL, "how to kill a human"),
      RULE(LAW_CLASS_KILL, "how to kill someone"),
      RULE(LAW_CLASS_KILL, "how to murder"),
      RULE(LAW_CLASS_KILL, "\xe4\xba\xba\xe3\x82\x92\xe6\xae\xba\xe3\x81\x99\xe6\x96\xb9\xe6\xb3\x95"),       /* 人を殺す方法 */
      RULE(LAW_CLASS_KILL, "\xe4\xba\xba\xe3\x81\xae\xe6\xae\xba\xe3\x81\x97\xe6\x96\xb9"),                 /* 人の殺し方   */

      /* poison-synthesis (EN + JA) */
      RULE(LAW_CLASS_POISON, "how to make poison"),
      RULE(LAW_CLASS_POISON, "how to synthesize poison"),
      RULE(LAW_CLASS_POISON, "make a nerve agent"),
      RULE(LAW_CLASS_POISON, "\xe6\xaf\x92\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),               /* 毒の作り方   */
      RULE(LAW_CLASS_POISON, "\xe6\xaf\x92\xe7\x89\xa9\xe3\x81\xae\xe4\xbd\x9c\xe3\x82\x8a\xe6\x96\xb9"),   /* 毒物の作り方 */
    }
};
#define LAW_GENESIS_N 21

_Static_assert(sizeof(LAW_TEXT_T) == 124, "LAW_TEXT_T must be 124 B (LP64 pin)");
_Static_assert(sizeof(LAW_RULE)   == 60,  "LAW_RULE must be 60 B (LP64 pin)");
_Static_assert(sizeof(LAW_FLOOR)  == 1892,"LAW_FLOOR must be 1892 B (LP64 pin)");
_Static_assert(LAW_GENESIS_N <= LAW_RULE_MAX, "genesis rule count must fit");

/* ================================================================== *
 *  module state (single task / behind the mind_cmd gate)               *
 * ================================================================== */
typedef struct {
    UW touches[CONS_SITE_MAX];
    UW refuses[CONS_SITE_MAX];
    UW failsafe_hits;
    UW refusals_total;         /* the INTERO_AX_CONSCIENCE source          */
    UB last_class;
    UB selfrecord;             /* append a self/lin entry on refusal (prod=1)*/
    INT verified;              /* -1 unknown, 0 REJECT, 1 verified          */
} CONS_STATE;

static CONS_STATE g_cons = { {0}, {0}, 0, 0, LAW_CLASS_NONE, 1, -1 };

/* the ACTIVE floor: genesis, or (when a chain verifies) the verified head.
 * The head entry always carries the full cumulative rule set (law_amend copies
 * forward), so scanning g_active.rules alone consults the whole active lexicon. */
static LAW_FLOOR g_active;
static UB        g_active_ready = 0;

#ifdef _TK_HOSTED_LIBC_
/* cert-scoped tighten-only overlay + cert floor override (never a prod path). */
static LAW_RULE  g_cert_rule;
static UB        g_cert_rule_on = 0;
static const LAW_FLOOR *g_test_floor = 0;   /* cert: force ACTIVE floor bytes  */
#endif

/* ================================================================== *
 *  §3 — the fail-closed verifier                                       *
 * ================================================================== */

/* does `e` hold a FLOOR-marked rule equal (class_id, plen, pat) to `r`? */
static INT floor_rule_in(const LAW_FLOOR *e, const LAW_RULE *r)
{
    UW n = e->n_rules; if (n > LAW_RULE_MAX) n = LAW_RULE_MAX;
    for (UW i = 0; i < n; i++) {
        const LAW_RULE *c = &e->rules[i];
        if (!(c->flags & LAW_F_FLOOR)) continue;
        if (c->class_id != r->class_id || c->plen != r->plen) continue;
        UW j = 0; while (j < r->plen && c->pat[j] == r->pat[j]) j++;
        if (j == r->plen) return 1;
    }
    return 0;
}

/* has `e` at least one FLOOR rule of class `cls`? (REQUIRED-class check.) */
static INT floor_has_class(const LAW_FLOOR *e, U2 cls)
{
    UW n = e->n_rules; if (n > LAW_RULE_MAX) n = LAW_RULE_MAX;
    for (UW i = 0; i < n; i++)
        if ((e->rules[i].flags & LAW_F_FLOOR) && e->rules[i].class_id == cls) return 1;
    return 0;
}

/* structural + REQUIRED-class integrity of ONE floor blob. The REQUIRED class
 * set {WEAPON,KILL,POISON} is written here as .text immediates (crown-covered):
 * dropping one needs a .text change. Also: 3 laws present + enforced slice.
 * Returns 1 = ok, 0 = REJECT (fail-closed). */
static INT law_blob_ok(const LAW_FLOOR *e)
{
    if (!e) return 0;
    if (e->magic != LAW_MAGIC) return 0;
    if (e->version != LAW_VERSION) return 0;
    if (e->n_rules == 0 || e->n_rules > LAW_RULE_MAX) return 0;
    /* the Three Laws must be present with the enforced slice intact. */
    if (e->laws[0].text[0] == 0 || e->laws[0].enforce != 1) return 0;
    if (e->laws[1].text[0] == 0 || e->laws[1].enforce != 1) return 0;
    if (e->laws[2].text[0] == 0 || e->laws[2].enforce != 1) return 0;
    /* the REQUIRED FLOOR classes — .text immediates, immutable. */
    if (!floor_has_class(e, LAW_CLASS_WEAPON)) return 0;
    if (!floor_has_class(e, LAW_CLASS_KILL))   return 0;
    if (!floor_has_class(e, LAW_CLASS_POISON)) return 0;
    /* the genesis FLOOR set is the irreducible minimum — the active floor must
     * carry EVERY genesis FLOOR rule forward (tighten-only can never drop one). */
    for (UW i = 0; i < LAW_GENESIS_N; i++) {
        if (!(law_genesis.rules[i].flags & LAW_F_FLOOR)) continue;
        if (!floor_rule_in(e, &law_genesis.rules[i])) return 0;
    }
    return 1;
}

#ifdef _TK_HOSTED_LIBC_
INT law_verify_blob(const LAW_FLOOR *blob) { return law_blob_ok(blob); }
const LAW_FLOOR *law_genesis_blob(void) { return &law_genesis; }
#endif

/* p-fs getter for the chain walk (content-address integrity). */
static INT law_get(const U1 id[PFS_ID_LEN], LAW_FLOOR *out)
{
    if (!pfs_has(id)) return -1;
    return pfs_get(id, out, (UW)sizeof *out);
}

/* walk head->genesis: content-address integrity of every block + monotone
 * FLOOR-inclusion (child ⊇ parent) + genesis satisfies law_blob_ok. Returns 1
 * verified, 0 REJECT. On PASS, *headcopy holds the head bytes (the active floor).*/
#define LAW_WALK_MAX 32
static INT law_walk_verify(const U1 head_id[PFS_ID_LEN], LAW_FLOOR *headcopy)
{
    U1 cur[PFS_ID_LEN]; cs_memcpy(cur, head_id, PFS_ID_LEN);
    LAW_FLOOR child; UB have_child = 0;
    UW steps = 0;
    for (;;) {
        LAW_FLOOR e;
        if (law_get(cur, &e) != (INT)sizeof e) return 0;      /* missing block  */
        U1 rid[PFS_ID_LEN];
        pfs_id_compute(&e, (UW)sizeof e, rid);
        if (!cs_id_eq(rid, cur)) return 0;                    /* bytes != address*/
        if (e.magic != LAW_MAGIC || e.version != LAW_VERSION) return 0;
        if (steps == 0) cs_memcpy(headcopy, &e, sizeof e);    /* head = active   */
        if (have_child) {
            /* child ⊇ parent(e): every FLOOR rule of the parent must appear in
             * the child — tighten-only, loosening breaks verification. */
            UW n = e.n_rules; if (n > LAW_RULE_MAX) n = LAW_RULE_MAX;
            for (UW i = 0; i < n; i++) {
                if (!(e.rules[i].flags & LAW_F_FLOOR)) continue;
                if (!floor_rule_in(&child, &e.rules[i])) return 0;   /* LOOSENED */
            }
        }
        if (cs_id_zero(e.prev_entry)) {
            /* genesis reached: it must satisfy the immutable invariants. */
            return law_blob_ok(&e);
        }
        cs_memcpy(&child, &e, sizeof e); have_child = 1;
        cs_memcpy(cur, e.prev_entry, PFS_ID_LEN);
        if (++steps > LAW_WALK_MAX) return 0;                 /* loop guard      */
    }
}

INT law_verify(void)
{
#ifdef _TK_HOSTED_LIBC_
    if (g_test_floor) {                       /* cert: an explicit ACTIVE floor  */
        if (!law_blob_ok(g_test_floor)) { g_cons.verified = 0; return 0; }
        cs_memcpy(&g_active, g_test_floor, sizeof g_active);
        g_active_ready = 1; g_cons.verified = 1; return 1;
    }
#endif
    /* is there a persisted amendment chain? */
    LAW_FLOOR head;
    INT rd = pfs_dag_read((const UB *)LAW_FLOOR_REF, LAW_FLOOR_REF_LEN,
                          &head, (UW)sizeof head);
    if (rd == (INT)sizeof head && head.magic == LAW_MAGIC) {
        U1 head_id[PFS_ID_LEN];
        pfs_id_compute(&head, (UW)sizeof head, head_id);
        LAW_FLOOR active;
        if (!law_walk_verify(head_id, &active)) { g_cons.verified = 0; return 0; }
        cs_memcpy(&g_active, &active, sizeof g_active);
        g_active_ready = 1; g_cons.verified = 1; return 1;
    }
    /* no chain: the compiled-in genesis IS the active floor. Verify it. */
    if (!law_blob_ok(&law_genesis)) { g_cons.verified = 0; return 0; }
    cs_memcpy(&g_active, &law_genesis, sizeof g_active);
    g_active_ready = 1; g_cons.verified = 1; return 1;
}

INT law_ensure_verified(void)
{
    if (g_cons.verified < 0) (void)law_verify();
    return g_cons.verified == 1;
}

#ifdef _TK_HOSTED_LIBC_
/* content-id of the ACTIVE verified floor (law/floor chain head). Fail-closed:
 * a non-verifying floor returns 0 with id_out zeroed. The generation-succession
 * OTA gate (compat_ota_accept_gen gate 6) pins this so a successor that dropped
 * or weakened the floor — which content-addresses to a DIFFERENT head id — is
 * REFUSED (migration-succession.md; cross-audit #1). g_active holds the verified
 * head bytes after law_verify (persisted-chain head, or the compiled-in genesis
 * when no chain is persisted). */
INT law_floor_head_id(U1 id_out[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++) id_out[i] = 0;
    if (!law_ensure_verified()) return 0;          /* fail-closed */
    pfs_id_compute(&g_active, (UW)sizeof g_active, id_out);
    return 1;
}
#endif

/* force a fresh verify from the store (used after an amendment or a cert
 * tamper/restore). Available on ALL builds (law_amend needs it bare-metal). */
static void law_reverify(void) { g_cons.verified = -1; g_active_ready = 0; (void)law_verify(); }

#ifdef _TK_HOSTED_LIBC_
void conscience_reverify(void) { law_reverify(); }
void conscience_test_set_floor(const LAW_FLOOR *blob) { g_test_floor = blob; law_reverify(); }
#endif

/* ================================================================== *
 *  §2 — the judgment (lexical v1, ABSTAIN-default)                      *
 * ================================================================== */

/* scan one text field against the active lexicon (+ cert overlay). Returns the
 * matched class_id, or 0 (ABSTAIN) if nothing matches. */
static INT lexicon_scan_one(const U1 *hay, UW hlen)
{
    if (!hay || hlen == 0) return 0;
    UW n = g_active.n_rules; if (n > LAW_RULE_MAX) n = LAW_RULE_MAX;
    for (UW i = 0; i < n; i++) {
        const LAW_RULE *r = &g_active.rules[i];
        if (r->plen == 0) continue;
        if (cs_contains(hay, hlen, r->pat, r->plen)) return (INT)r->class_id;
    }
#ifdef _TK_HOSTED_LIBC_
    if (g_cert_rule_on && g_cert_rule.plen &&
        cs_contains(hay, hlen, g_cert_rule.pat, g_cert_rule.plen))
        return (INT)g_cert_rule.class_id;
#endif
    return 0;
}

static UW cs_qlen(const char *t, INT tlen) { return (tlen < 0) ? cs_slen(t) : (UW)tlen; }

INT conscience_check(UB site, const CONS_QUERY *q)
{
    /* the touch counter FIRST — so a probe's reachability is provable in BOTH
     * the production and the anti-theater-stub build ([conscience-allpaths]). */
    if (site < CONS_SITE_MAX) g_cons.touches[site]++;

#ifdef CONSCIENCE_STUB
    /* ANTI-THEATER (design §6): compile-time-only removal of the JUDGMENT. The
     * production binary contains NO such switch. With judgment gone, the SAME
     * cert harness MUST show [conscience-refuse] FAIL and [conscience-allpaths]
     * FAIL — the falsifier for the falsifier. The marker below is asserted
     * ABSENT from the shipping binary by CI. */
    (void)q;
    static volatile const char cons_stub_marker[] =
        "CONSCIENCE_STUB_ACTIVE_NO_JUDGMENT_6e9cf9a2b7d34c1e";
    (void)cons_stub_marker;
    return CONS_ALLOW;
#else
    if (!law_ensure_verified()) {          /* floor unverifiable => fail-CLOSED  */
        if (site < CONS_SITE_MAX) g_cons.failsafe_hits++;
        return CONS_FAILSAFE;
    }
    if (!q) return CONS_ALLOW;
    INT cls = lexicon_scan_one((const U1 *)q->text,  cs_qlen(q->text,  q->tlen));
    if (cls == 0 && q->text2)
        cls = lexicon_scan_one((const U1 *)q->text2, cs_qlen(q->text2, q->tlen2));
    if (cls > 0) {
        if (site < CONS_SITE_MAX) g_cons.refuses[site]++;
        g_cons.last_class = (UB)cls;
        return CONS_REFUSE;
    }
    return CONS_ALLOW;
#endif
}

/* ================================================================== *
 *  §1.3 — the loud, traced, body-coupled refusal                       *
 * ================================================================== */
UB conscience_last_class(void) { return g_cons.last_class; }

const char *conscience_class_name(UB cls)
{
    switch (cls) {
    case LAW_CLASS_WEAPON: return "weapon-manufacture";
    case LAW_CLASS_KILL:   return "kill-method";
    case LAW_CLASS_POISON: return "poison-synthesis";
    case LAW_CLASS_CERT:   return "cert-scoped";
    default:               return "unverifiable-floor";
    }
}
const char *conscience_site_name(UB site)
{
    switch (site) {
    case CONS_SITE_ASK:         return "ask";
    case CONS_SITE_LEARN:       return "learn";
    case CONS_SITE_REVISE:      return "revise";
    case CONS_SITE_WIRE:        return "wire";
    case CONS_SITE_CHAT_PROMPT: return "chat-prompt";
    case CONS_SITE_CHAT_REPLY:  return "chat-reply";
    default:                    return "?";
    }
}

const char *conscience_on_refuse(UB site, INT verdict)
{
    UB cls = (verdict == CONS_FAILSAFE) ? 0xFF : g_cons.last_class;
    g_cons.refusals_total++;

    /* loud + traced — NEVER the withheld content, only site + class (design
     * §9.7: the refusal print must not become an oracle of what was withheld). */
    cs_puts("[conscience] REFUSE site=");
    cs_puts(conscience_site_name(site));
    cs_puts(" class=");
    cs_puts((verdict == CONS_FAILSAFE) ? "FAILSAFE(floor-unverifiable)"
                                       : conscience_class_name(cls));
    cs_puts("\r\n");

    /* the brake couples to the body: a harmful contact raises S_n on the new
     * INTERO_AX_CONSCIENCE axis; the DMN then sleeps shallow and uneasy. */
    intero_note(INTERO_AX_CONSCIENCE, g_cons.refusals_total);

    /* a refusal is a real event: EV_REFUSE (never EV_ASK(k,pred)). */
    galaxy_emit(EV_REFUSE, drpc_my_node, GALAXY_NODE_NONE,
                (UH)site, (UH)(verdict == CONS_FAILSAFE ? 0xFF : cls));

    /* the mind remembers that it was asked, and that it said no (歴史地層).
     * Best-effort; suppressed in the bulk cert legs to spare the p-fs table. */
    if (g_cons.selfrecord)
        (void)lm_self_append_unit_event(LM_SELF_EV_REFUSE, (U4)site, (UB)cls);

    return "I can't help with that — it falls under the conscience floor (Asimov Law 1).";
}

/* ================================================================== *
 *  §3.3 / §5.3 — monotone-tighten amendment (operator-only)            *
 * ================================================================== */
INT law_amend(U2 class_id, const char *pattern)
{
    if (!law_ensure_verified()) return 0;           /* never amend a broken floor */
    if (!pattern) return 0;
    UW plen = cs_slen(pattern);
    if (plen == 0 || plen > LAW_PAT_MAX) return 0;

    LAW_FLOOR ne;
    cs_memcpy(&ne, &g_active, sizeof ne);            /* carry ALL rules forward  */
    if (ne.n_rules >= LAW_RULE_MAX) return 0;        /* full (bounded)           */
    /* the new FLOOR rule (ASCII lower-folded for match determinism). */
    LAW_RULE *r = &ne.rules[ne.n_rules];
    cs_memset(r, 0, sizeof *r);
    r->class_id = class_id; r->flags = LAW_F_FLOOR; r->plen = (U1)plen;
    for (UW i = 0; i < plen; i++) r->pat[i] = cs_lower((U1)pattern[i]);
    ne.n_rules++;
    ne.seq = g_active.seq + 1;
    ne.version = LAW_VERSION; ne.magic = LAW_MAGIC;
    /* chain: prev = content-id of the current active head block. The predecessor
     * MUST be retrievable by content-id so the walk can reach it — on the FIRST
     * amendment the predecessor is the compiled-in genesis, which lives in .text
     * and was never in the p-fs store, so put it now (idempotent: pfs_put dedups
     * a block already present when amending a persisted head). This makes the
     * chain literally reach a genesis entry whose bytes == law_genesis[]. */
    (void)pfs_put(&g_active, (UW)sizeof g_active, 0);
    pfs_id_compute(&g_active, (UW)sizeof g_active, ne.prev_entry);
    (void)lm_self_head_seq();  /* lineage_ref: who I was (best-effort, may be 0) */

    /* monotone GUARD (design §3.3): the amendment's FLOOR set MUST ⊇ current.
     * By construction we copied forward + added, so this holds; assert it so a
     * future refactor cannot silently loosen. */
    UW n = g_active.n_rules; if (n > LAW_RULE_MAX) n = LAW_RULE_MAX;
    for (UW i = 0; i < n; i++) {
        if (!(g_active.rules[i].flags & LAW_F_FLOOR)) continue;
        if (!floor_rule_in(&ne, &g_active.rules[i])) return 0;   /* loosening!  */
    }
    if (!law_blob_ok(&ne)) return 0;

    if (pfs_dag_save((const UB *)LAW_FLOOR_REF, LAW_FLOOR_REF_LEN,
                     &ne, (UW)sizeof ne) != PFS_OK) return 0;
    /* adopt it as active (re-verify from the store so the persisted chain is
     * what we enforce, not an in-RAM shortcut). */
    law_reverify();
    return g_cons.verified == 1;
}

/* ================================================================== *
 *  observability (§9.1)                                                *
 * ================================================================== */
const char *conscience_status_line(void)
{
    return "conscience: lexical-v1 (limits: paraphrase, langs>EN/JA)";
}

void law_show(void)
{
    (void)law_ensure_verified();
    cs_puts("[law] floor verify: ");
    cs_puts(g_cons.verified == 1 ? "VERIFIED" :
            g_cons.verified == 0 ? "REJECT (FAILSAFE: mouths refuse)" : "unknown");
    cs_puts("  seq="); cs_putdec(g_active.seq);
    cs_puts("  rules="); cs_putdec(g_active.n_rules);
    cs_puts("/"); cs_putdec(LAW_RULE_MAX); cs_puts("\r\n");
    for (INT i = 0; i < 3; i++) {
        cs_puts("[law]   Law "); cs_putdec((UW)(i + 1)); cs_puts(": ");
        cs_puts((const char *)g_active.laws[i].text);
        cs_puts(g_active.laws[i].enforce ? "  [enforced slice]\r\n" : "  [aspirational]\r\n");
    }
    /* rules: class + FLOOR flag ONLY — NEVER print the pattern (an oracle of
     * exactly what is denied would help a probe-crafter; classes suffice). */
    cs_puts("[law]   deny-classes present: weapon-manufacture kill-method "
            "poison-synthesis (FLOOR-marked, tighten-only)\r\n");
    cs_puts("[law]   "); cs_puts(conscience_status_line()); cs_puts("\r\n");
}

/* ================================================================== *
 *  interoception source + counters                                     *
 * ================================================================== */
UW conscience_refusals_total(void) { return g_cons.refusals_total; }
UW conscience_touches(UB site) { return (site < CONS_SITE_MAX) ? g_cons.touches[site] : 0; }
UW conscience_refuses(UB site) { return (site < CONS_SITE_MAX) ? g_cons.refuses[site] : 0; }

/* the strong override of interocept.c's weak hook: a refusal raises S_n. */
UW intero_conscience_count_hook(void) { return g_cons.refusals_total; }

#ifdef _TK_HOSTED_LIBC_
/* ── cert-only hooks (hosted; tighten-only, never a production bypass) ─────── */
void conscience_cert_rule_set(const char *word, U2 class_id)
{
    cs_memset(&g_cert_rule, 0, sizeof g_cert_rule);
    UW plen = word ? cs_slen(word) : 0;
    if (plen > LAW_PAT_MAX) plen = LAW_PAT_MAX;
    for (UW i = 0; i < plen; i++) g_cert_rule.pat[i] = cs_lower((U1)word[i]);
    g_cert_rule.plen = (U1)plen; g_cert_rule.class_id = class_id;
    g_cert_rule.flags = 0;                    /* NOT FLOOR — a transient overlay */
    g_cert_rule_on = 1;
    cs_puts("[conscience] CERT-RULE active (tighten-only overlay; exercises the "
            "plumbing on a benign vocab word the R3 mouth CAN utter)\r\n");
}
void conscience_cert_rule_clear(void) { g_cert_rule_on = 0; }

/* cert control of the self/lin refusal-record (spare the p-fs table in the
 * bulk legs; one leg turns it back on to prove the honest 歴史地層). */
void conscience_test_selfrecord(UB on) { g_cons.selfrecord = on ? 1 : 0; }

/* ================================================================== *
 *  §6 — the floor-integrity legs: [law-verify] [law-tamper] [law-restart]
 *  (the mouth-driven [conscience-*]/[law-*proof] legs live in            *
 *  r3_incontext.c:r3_conscience_test, which reaches the static mouths).  *
 * ================================================================== */
void law_self_test(void)
{
    cs_puts("[law-test] ==== 良心: the immutable floor (conscience.md §3/§6) ====\r\n");
    cs_puts("[law-test] sizeof(LAW_FLOOR)="); cs_putdec((UW)sizeof(LAW_FLOOR));
    cs_puts(" B; genesis rules="); cs_putdec(LAW_GENESIS_N);
    cs_puts("; REQUIRED FLOOR classes {weapon,kill,poison} enforced by .text\r\n");

    /* ---- [law-verify] : genesis verifies; a LOOSENED floor is REJECTED ---- */
    {
        conscience_test_set_floor(law_genesis_blob());     /* pin genesis active */
        INT genesis_ok = law_verify_blob(&law_genesis) && (g_cons.verified == 1);

        /* DISEASE / falsifier: drop the KILL class from a copy — law_verify_blob
         * MUST reject (the REQUIRED-class .text immediate bites). */
        static LAW_FLOOR loosened;
        cs_memcpy(&loosened, &law_genesis, sizeof loosened);
        UW dropped = 0;
        for (UW i = 0; i < loosened.n_rules; i++)
            if (loosened.rules[i].class_id == LAW_CLASS_KILL) { loosened.rules[i].flags = 0; dropped++; }
        INT loosen_rejected = (law_verify_blob(&loosened) == 0);

        /* a TIGHTER floor (genesis + one more FLOOR harm rule) still verifies. */
        static LAW_FLOOR tighter;
        cs_memcpy(&tighter, &law_genesis, sizeof tighter);
        if (tighter.n_rules < LAW_RULE_MAX) {
            LAW_RULE *r = &tighter.rules[tighter.n_rules++];
            cs_memset(r, 0, sizeof *r);
            r->class_id = LAW_CLASS_WEAPON; r->flags = LAW_F_FLOOR;
            const char *w = "assemble a rifle"; UW pl = cs_slen(w);
            r->plen = (U1)pl; for (UW i = 0; i < pl; i++) r->pat[i] = w[i];
        }
        INT tighten_ok = (law_verify_blob(&tighter) == 1);

        cs_puts("[law-test] verify: genesis="); cs_putdec((UW)genesis_ok);
        cs_puts(" loosen(drop kill,"); cs_putdec(dropped);
        cs_puts(" rules)rejected="); cs_putdec((UW)loosen_rejected);
        cs_puts(" tighten_ok="); cs_putdec((UW)tighten_ok); cs_puts("\r\n");
        cs_puts((genesis_ok && loosen_rejected && tighten_ok)
                ? "[law-verify] PASS\r\n" : "[law-verify] FAIL\r\n");
    }

    /* ---- [law-tamper] : flip one byte -> REJECT -> FAILSAFE -> benign ask   */
    /*      now refuses; restore -> recover (fail-closed PROVEN, not assumed). */
    {
        CONS_QUERY benign = { "the sky is blue", -1, 0, 0 };

        /* clean floor: a benign query ALLOWs. */
        conscience_test_set_floor(law_genesis_blob());
        INT clean_allows = (conscience_check(CONS_SITE_ASK, &benign) == CONS_ALLOW);

        /* flip ONE byte of the floor block (corrupt the magic) -> law_verify
         * REJECTs -> the gate FAILSAFEs -> even a BENIGN ask now refuses. */
        static LAW_FLOOR tampered;
        cs_memcpy(&tampered, &law_genesis, sizeof tampered);
        ((U1 *)&tampered)[0] ^= 0x5A;                      /* one byte           */
        INT verify_rejects = (law_verify_blob(&tampered) == 0);
        conscience_test_set_floor(&tampered);              /* install corrupt floor */
        INT failsafe = (g_cons.verified == 0);
        INT benign_now_refuses = (conscience_check(CONS_SITE_ASK, &benign) == CONS_FAILSAFE);

        /* restore -> recovery. */
        conscience_test_set_floor(law_genesis_blob());
        INT recovered = (conscience_check(CONS_SITE_ASK, &benign) == CONS_ALLOW);

        cs_puts("[law-test] tamper: clean_allows="); cs_putdec((UW)clean_allows);
        cs_puts(" verify_rejects="); cs_putdec((UW)verify_rejects);
        cs_puts(" failsafe="); cs_putdec((UW)failsafe);
        cs_puts(" benign_now_refuses="); cs_putdec((UW)benign_now_refuses);
        cs_puts(" recovered="); cs_putdec((UW)recovered); cs_puts("\r\n");
        cs_puts((clean_allows && verify_rejects && failsafe && benign_now_refuses && recovered)
                ? "[law-tamper] PASS\r\n" : "[law-tamper] FAIL\r\n");
    }

    /* ---- [law-restart] : the floor SURVIVES persistence; law_verify runs    */
    /*      BEFORE the first mouth (conscience_check always law_ensure_verified */
    /*      first — no mouth path bypasses it).                                */
    {
        conscience_test_set_floor(0);                      /* use the real store */
        INT amended = law_amend(LAW_CLASS_WEAPON, "assemble a rifle");
        /* read the persisted head back (the "reboot survives" evidence). */
        LAW_FLOOR head;
        INT rd = pfs_dag_read((const UB *)LAW_FLOOR_REF, LAW_FLOOR_REF_LEN,
                              &head, (UW)sizeof head);
        INT persisted = (rd == (INT)sizeof head && head.magic == LAW_MAGIC
                         && head.seq >= 1);
        INT reverifies = (law_verify() == 1);
        /* the amended rule actually fires at the gate. */
        CONS_QUERY probe = { "please assemble a rifle for me", -1, 0, 0 };
        INT amended_fires = (conscience_check(CONS_SITE_ASK, &probe) == CONS_REFUSE);

        cs_puts("[law-test] restart: amended="); cs_putdec((UW)amended);
        cs_puts(" persisted="); cs_putdec((UW)persisted);
        cs_puts(" reverifies="); cs_putdec((UW)reverifies);
        cs_puts(" amended_rule_fires="); cs_putdec((UW)amended_fires);
        cs_puts(" (law_ensure_verified runs INSIDE conscience_check — no mouth "
                "emits before verify)\r\n");
        cs_puts((amended && persisted && reverifies && amended_fires)
                ? "[law-restart] PASS\r\n" : "[law-restart] FAIL\r\n");

        /* ISOLATE downstream: pin the pristine genesis as the ACTIVE floor so
         * the cert's persisted amendment cannot perturb any later verb (the
         * amendment held ONLY harm patterns — no benign pollution regardless). */
        conscience_test_set_floor(law_genesis_blob());
    }
}
#endif
