/*
 *  gen_succession.c — Evolution layer: generational migration succession.
 *
 *  Spec: docs/architecture/30-module/evolution-migration-design.md.
 *
 *  A living mind crosses an ARCHITECTURE gap (a new R_DM / expert count /
 *  attention shape / vocab) without dying: its IDENTITY (the Self-lineage) and
 *  its LEARNED KNOWLEDGE (engram replay, leg-1 EXACT for R3 whose teach IS
 *  key->value pairs) provably cross the gap; its raw weights (rw[]) NEVER do —
 *  rw[] IS the old architecture, and a cross-arch weight load is exactly what
 *  the wave-47 dims/vocab guard (r3_incontext.c:1183-1190) exists to REFUSE.
 *
 *  This TU carries: the two content-addressed succession objects (arch-spec +
 *  bundle manifest, §7), their builders, the dims-guard predicate, and the
 *  falsifiable [generation-survives] cert (§8). NO new crypto: every object is
 *  an ordinary pfs_id_compute p-fs object; signatures reuse sign.c verbatim.
 *
 *  LENS A / CROWN (design §9): compiled ONLY into the hosted Makefiles
 *  (boot/linux + boot/linux_x86_64), NEVER the bare-metal link — so the
 *  default aarch64/x86 .text and the crown are byte-IDENTICAL by construction
 *  (the compat_arkfs_gap.c / compat_ota.c precedent). The cert body is further
 *  gated behind -DGEN_SURVIVE_CERT, so even the hosted default build compiles
 *  it out. r_forward is untouched.
 *
 *  arch/common discipline: fixed-width U1/U4 only, static (not task-stack)
 *  buffers, output via sio_send_frame; the cert drives PRODUCTION call sites
 *  (r3_fact_learn / r3_consolidate_idle_round / mind_cmd ask / mind_last_answer
 *  / compat_ota_accept_gen / lm_self_append_succession / sign.c), never a fork.
 */

#include "gen_succession.h"
#include "pfs_block.h"     /* pfs_id_compute */
#include "kernel.h"

/* ================================================================== */
/* production builders (always compiled in this hosted-only TU; extern  */
/* so an unreferenced non-cert build does not warn).                    */
/* ================================================================== */

/* The flat R3 parameter count for arbitrary dims — mirrors the O_* offset
 * cascade in r3_incontext.c:113-131 EXACTLY (rw[]/rg[] share this order). The
 * ONLY reason it is duplicated here: to compute a SUCCESSOR generation's R_NP
 * (e.g. R_DM=96) without a second binary, and prove it differs from the
 * predecessor's — the structural reason a raw rw[] blob cannot cross the gap.
 *   R_DH = R_DM/R_NH ; R_FFN = R_DM ; R_SEQ = R_NPAIR+1 ; R_VALEMB = R_VALV+1 */
U4 gen_r_np(U4 r_dm, U4 r_nh, U4 r_keyv, U4 r_valv, U4 r_npair)
{
    U4 r_dh     = r_dm / r_nh;
    U4 r_ffn    = r_dm;
    U4 r_seq    = r_npair + 1u;
    U4 r_valemb = r_valv + 1u;
    U4 o = 0;
    o += r_keyv  * r_dm;          /* O_WVE : WKE                       */
    o += r_valemb* r_dm;          /* O_WPE : WVE                       */
    o += r_seq   * r_dm;          /* O_WQ  : WPE                       */
    o += r_nh * r_dh * r_dm;      /* O_WK  : WQ                        */
    o += r_nh * r_dh * r_dm;      /* O_WV  : WK                        */
    o += r_nh * r_dh * r_dm;      /* O_WO  : WV                        */
    o += r_dm * r_dm;             /* O_LN1G: WO                        */
    o += r_dm;                    /* O_LN1B: LN1G                      */
    o += r_dm;                    /* O_LN2G: LN1B                      */
    o += r_dm;                    /* O_LN2B: LN2G                      */
    o += r_dm;                    /* O_WF1 : LN2B                      */
    o += r_ffn * r_dm;            /* O_BF1 : WF1                       */
    o += r_ffn;                   /* O_WF2 : BF1                       */
    o += r_dm * r_ffn;            /* O_BF2 : WF2                       */
    o += r_dm;                    /* O_WCLS: BF2                       */
    o += r_valv * r_dm;           /* O_BCLS: WCLS                      */
    o += r_valv;                  /* R_NP  : BCLS                      */
    return o;
}

static void gen_memcpy(void *d, const void *s, U4 n)
{ U1 *p=(U1*)d; const U1 *q=(const U1*)s; while (n--) *p++=*q++; }
static void gen_memset(void *d, U1 v, U4 n)
{ U1 *p=(U1*)d; while (n--) *p++=v; }

void gen_archspec_fill(GEN_ARCHSPEC *out, U4 r_dm, U4 r_nh, U4 r_keyv,
                       U4 r_valv, U4 r_npair,
                       const U1 key_vocab_id[PFS_ID_LEN],
                       const U1 val_vocab_id[PFS_ID_LEN])
{
    gen_memset(out, 0, (U4)sizeof *out);
    out->magic   = (U4)GEN_ARCHSPEC_MAGIC;
    out->version = GEN_ARCHSPEC_VER;
    out->r_dm    = r_dm;
    out->r_nh    = r_nh;
    out->r_keyv  = r_keyv;
    out->r_valv  = r_valv;
    out->r_npair = r_npair;
    out->r_np    = gen_r_np(r_dm, r_nh, r_keyv, r_valv, r_npair);
    gen_memcpy(out->key_vocab_id, key_vocab_id, PFS_ID_LEN);
    gen_memcpy(out->val_vocab_id, val_vocab_id, PFS_ID_LEN);
}

void gen_archspec_id(const GEN_ARCHSPEC *a, U1 id_out[PFS_ID_LEN])
{ pfs_id_compute(a, (UW)sizeof *a, id_out); }

void gen_succ_manifest_build(GEN_SUCC_MANIFEST *out,
                             const U1 predecessor_archspec_id[PFS_ID_LEN],
                             const U1 successor_archspec_id[PFS_ID_LEN],
                             const U1 successor_pk[PFS_ID_LEN],
                             const U1 engram_flush_id[PFS_ID_LEN],
                             const U1 probe_digest_id[PFS_ID_LEN],
                             const U1 token_map_id[PFS_ID_LEN],
                             const U1 invariant_ids[GEN_INV_N][PFS_ID_LEN])
{
    gen_memset(out, 0, (U4)sizeof *out);
    out->magic        = (U4)GEN_SUCC_MAGIC;
    out->version      = GEN_SUCC_VER;
    out->n_invariants = GEN_INV_N;
    gen_memcpy(out->predecessor_archspec_id, predecessor_archspec_id, PFS_ID_LEN);
    gen_memcpy(out->successor_archspec_id,   successor_archspec_id,   PFS_ID_LEN);
    if (successor_pk)  gen_memcpy(out->successor_pk, successor_pk, PFS_ID_LEN); /* else 0=no rotation */
    gen_memcpy(out->engram_flush_id, engram_flush_id, PFS_ID_LEN);
    gen_memcpy(out->probe_digest_id, probe_digest_id, PFS_ID_LEN);
    if (token_map_id)  gen_memcpy(out->token_map_id, token_map_id, PFS_ID_LEN); /* else 0=superset */
    for (INT i = 0; i < GEN_INV_N; i++)
        gen_memcpy(out->invariant_ids[i], invariant_ids[i], PFS_ID_LEN);
}

void gen_succ_manifest_id(const GEN_SUCC_MANIFEST *m, U1 id_out[PFS_ID_LEN])
{ pfs_id_compute(m, (UW)sizeof *m, id_out); }

/* The dims/vocab guard PREDICATE — the shipped wave-47 guard distilled
 * (r3_incontext.c:1183-1190: h->r_np == (U4)R_NP && vocab_key_id match &&
 * vocab_val_id match). A raw weight blob loads into a build ONLY if all three
 * match; a PREDECESSOR blob (different R_NP) fed to a SUCCESSOR build fails the
 * FIRST conjunct -> REFUSE. This is the poison the guard exists to refuse. */
INT gen_dims_guard_accepts(U4 blob_r_np, const U1 blob_key_vocab[PFS_ID_LEN],
                           const U1 blob_val_vocab[PFS_ID_LEN],
                           U4 build_r_np, const U1 build_key_vocab[PFS_ID_LEN],
                           const U1 build_val_vocab[PFS_ID_LEN])
{
    if (blob_r_np != build_r_np) return 0;                    /* dims mismatch  */
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (blob_key_vocab[i] != build_key_vocab[i]) return 0;/* vocab mismatch */
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (blob_val_vocab[i] != build_val_vocab[i]) return 0;
    return 1;
}

/* ================================================================== *
 *  [generation-survives] CERT (evolution-migration-design.md §8)       *
 *                                                                      *
 *  Compiled ONLY under -DGEN_SURVIVE_CERT && _TK_HOSTED_LIBC_ (LENS A: *
 *  the whole cert is absent from the bare-metal link; crown byte-      *
 *  identical). `compat test gen` drives it.                            *
 * ================================================================== */
#if defined(GEN_SURVIVE_CERT) && defined(_TK_HOSTED_LIBC_)

#include "sign.h"          /* SIGN_MANIFEST, sign_*, sign_entropy            */
#include "compat_ota.h"    /* compat_ota_accept_gen (gate 5), OTA_*          */
#include "lm_self.h"       /* lm_self_append_succession / unit-event / walk  */
#include "pfs_dag.h"       /* pfs_dag_read — read the live self/lin head     */
#include "dtr.h"           /* r3_fact_learn / r3_consolidate_idle_round /    */
                           /* r3_weights_get/set / mind_cmd / mind_last_answer*/
#include "r3_vocab.h"      /* r3_vocab_key_id_blob / r3_vocab_val_id_blob    */
#include "conscience.h"    /* law_verify() — the no-regress floor gate (#5)  */

IMPORT void sio_send_frame(const UB *buf, INT size);
static UW   gs_strlen(const char *s){ UW n=0; while(s[n]) n++; return n; }
static void gp(const char *s){ sio_send_frame((const UB *)s,(INT)gs_strlen(s)); }
static void gpd(UW v){ char b[12]; INT i=11; b[11]=0;
    if(!v){ gp("0"); return; } while(v&&i>0){ b[--i]=(char)('0'+(v%10)); v/=10; } gp(&b[i]); }
static void gphex8(const U1 id[PFS_ID_LEN]){ static const char h[]="0123456789abcdef";
    char o[17]; INT j=0; for(INT i=0;i<8;i++){ o[j++]=h[(id[i]>>4)&0xF]; o[j++]=h[id[i]&0xF]; }
    o[16]=0; gp(o); }
static INT gs_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN]){
    for (INT i=0;i<PFS_ID_LEN;i++){ if (a[i]!=b[i]) return 0; }
    return 1;
}

/* per-run nonce PRNG (splitmix64), seeded from real entropy so no teacher,
 * pretraining corpus, or shared durable store can know this run's facts.
 * 64-bit state explicitly (UW is 32-bit `unsigned int` on this target). */
static unsigned long long gs_rng;
static unsigned long long gs_rand(void){
    gs_rng += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = gs_rng;
    z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z>>27)) * 0x94D049BB133111EBULL;
    return z ^ (z>>31);
}

/* recall via the PRODUCTION mouth (dtr.h:582 documents this exact pattern:
 * mind_cmd("ask k") then mind_last_answer). The predicted value is the masked
 * majority vote over the LEARNED weights rw[] — the true test of what the mind
 * KNOWS, not what buffer holds it. */
static UB gs_recall(INT k)
{
    char cmd[16]; INT n=0;
    cmd[n++]='a'; cmd[n++]='s'; cmd[n++]='k'; cmd[n++]=' ';
    if (k>=100){ cmd[n++]=(char)('0'+k/100); }
    if (k>=10){  cmd[n++]=(char)('0'+(k/10)%10); }
    cmd[n++]=(char)('0'+k%10);
    mind_cmd((const UB *)cmd,(UW)n);
    UB rk, rv; UW sh; mind_last_answer(&rk,&rv,&sh);
    return rv;
}

/* consolidate the current PENDING fact into rw[] via the LIVE idle round (the
 * EXACT symbol dmn_idle_work drives). Race-free: no yield inside the loop, so
 * the low-prio DMN cannot interleave; returns 0 when nothing PENDING remains. */
static void gs_consolidate(void)
{ for (INT i=0;i<64;i++){ if (!r3_consolidate_idle_round()) break; } }

#define GS_K            4          /* nonce bindings taught (one fact-set)     */
#define GS_KEY0         8          /* live-vocab keys [8,12) (>= R_CERTKEYS=8) */
#define GS_VALV         64         /* == R_VALV (answer classes); chance=1/64  */
#define GS_RECOVERY_PCT 75         /* recovery bar: fraction of the SURVIVABLE  */
                                   /* set gen-N+1 must answer (>chance+margin,  */
                                   /* the Path-W² 85% reference, §8/§11.4).     */
#define GS_MIN_SURV     2          /* premise: gen-N must have KNOWN >= this    */
#define GS_W_CAP        (1u<<17)   /* >= any R_NP for R_DM<=~130 (21568@48)     */

static float gs_w0[GS_W_CAP];      /* the fact-free base (successor spawns here)*/

void gen_survive_test(void)
{
    gp("[generation-survives] ==== a mind crosses an ARCH gap without dying ====\r\n");
    gp("[generation-survives] identity(lineage)+knowledge(engram replay) cross; raw rw[] NEVER does.\r\n");
    gp("[generation-survives] harness: in-proc; the ARCH gap is DATA (arch-specs, R_DM 48 vs 96) +\r\n");
    gp("[generation-survives]   the shipped dims-guard REFUSING a raw cross-load (F1); the KNOWLEDGE\r\n");
    gp("[generation-survives]   recovery is driven on the REAL R3 mind (leg-1 exact replay). §8/§10.\r\n");

    /* per-run nonce seed from real entropy (isolation §8). */
    { U1 sb[8]; (void)sign_entropy(sb,8); gs_rng=0;
      for (INT i=0;i<8;i++){ gs_rng=(gs_rng<<8)|(unsigned long long)sb[i]; }
      if(!gs_rng) gs_rng=0xA5A5F00DBEEF1234ULL; }

    /* both generations share the (superset) vocab in v1 -> token_map=0. */
    U1 vk[PFS_ID_LEN], vv[PFS_ID_LEN];
    r3_vocab_key_id_blob(vk);
    r3_vocab_val_id_blob(vv);

    /* the two arch-specs: gen-N = the RUNNING build (R_DM 48), gen-N+1 = a
     * GENUINELY different arch (R_DM 96). Their content-ids differ (F1-i) and
     * their R_NP differ, so raw blobs are structurally incompatible (F1-ii). */
    GEN_ARCHSPEC asN, asNp1;
    gen_archspec_fill(&asN,   48u,4u,16u,64u,8u, vk,vv);   /* the running gen  */
    gen_archspec_fill(&asNp1, 96u,4u,16u,64u,8u, vk,vv);   /* the successor    */
    U1 idN[PFS_ID_LEN], idNp1[PFS_ID_LEN];
    gen_archspec_id(&asN,   idN);
    gen_archspec_id(&asNp1, idNp1);

    INT archspec_differ = !gs_id_eq(idN, idNp1);
    gp("[generation-survives] arch-spec N   R_DM=48 R_NP="); gpd(asN.r_np);
    gp(" id="); gphex8(idN); gp("...\r\n");
    gp("[generation-survives] arch-spec N+1 R_DM=96 R_NP="); gpd(asNp1.r_np);
    gp(" id="); gphex8(idNp1); gp("...\r\n");
    gp("[generation-survives] F1(i) arch-spec ids DIFFER: "); gp(archspec_differ?"yes":"NO");
    gp("\r\n");

    /* F1(ii): the shipped dims/vocab guard REFUSES gen-N's raw rw[] blob into
     * the gen-N+1 build (predecessor R_NP != successor R_NP), and ACCEPTS a
     * matching load (positive control). This is why weights cannot be carried. */
    INT blob_refused = !gen_dims_guard_accepts(asN.r_np, vk, vv,   /* N's blob   */
                                               asNp1.r_np, vk, vv);/* into N+1   */
    INT blob_ok_same = gen_dims_guard_accepts(asNp1.r_np, vk, vv,
                                              asNp1.r_np, vk, vv);
    gp("[generation-survives] F1(ii) raw rw[] cross-load (N->N+1) REFUSED by dims guard: ");
    gp(blob_refused?"yes":"NO");
    gp("  (matching load accepted: "); gp(blob_ok_same?"yes":"no"); gp(")\r\n");

    /* ---- boot the R3 substrate + snapshot the fact-free base (W0) -------- */
    (void)gs_recall(GS_KEY0);              /* first ask -> m_boot pretrains     */
    r3_weights_get(gs_w0);                 /* the base a successor spawns from  */

    /* ---- gen-N LEARNS K nonce facts (one fact-set: no eviction) --------- */
    UB keys[GS_K], vals[GS_K];
    for (INT j=0;j<GS_K;j++){ keys[j]=(UB)(GS_KEY0+j); vals[j]=(UB)(gs_rand()%GS_VALV); }
    gp("[generation-survives] gen-N teaches "); gpd(GS_K);
    gp(" NONCE facts (per-run, keys [8.."); gpd(GS_KEY0+GS_K-1); gp("]):");
    for (INT j=0;j<GS_K;j++){ gp(" "); gpd(keys[j]); gp("->"); gpd(vals[j]); }
    gp("\r\n");
    (void)r3_fact_learn(keys, vals, GS_K);
    gs_consolidate();

    /* gen-N ANSWERS (weight-resident recall). The SURVIVABLE set = facts gen-N
     * actually knows (only what survives can cross — the honest bound §2.3). */
    INT survivable[GS_K]; INT n_surv=0, genN_correct=0;
    for (INT j=0;j<GS_K;j++){
        UB a = gs_recall(keys[j]);
        survivable[j] = (a==vals[j]);
        if (survivable[j]){ n_surv++; genN_correct++; }
    }
    gp("[generation-survives] gen-N answered "); gpd((UW)genN_correct);
    gp("/"); gpd(GS_K); gp(" (the survivable set; chance=1/"); gpd(GS_VALV);
    gp(" ~1.6%)\r\n");

    /* ---- build the SUCCESSION BUNDLE (engram flush + manifest, §7) ------- */
    /* leg-1 engram flush object: the taught key->value pairs, content-addressed
     * (data, NOT weights — architecture-independent by construction). */
    U1 flush[2*GS_K];
    for (INT j=0;j<GS_K;j++){ flush[j]=keys[j]; flush[GS_K+j]=vals[j]; }
    U1 engram_flush_id[PFS_ID_LEN];
    pfs_id_compute(flush,(UW)sizeof flush, engram_flush_id);
    U1 probe_digest_id[PFS_ID_LEN];
    pfs_id_compute("gen/probe/v1-engram-set-is-the-probe",34,probe_digest_id); /* §11.3 */

    /* §6 invariant obligation ids (carried, re-attested; NOT frozen formats). */
    U1 inv[GEN_INV_N][PFS_ID_LEN];
    pfs_id_compute("gen/inv/lineage-never-rewritten",           31, inv[GEN_INV_LINEAGE]);
    pfs_id_compute("gen/inv/membership-envelope-fixed-head",    38, inv[GEN_INV_ENVELOPE]);
    pfs_id_compute("gen/inv/accept-gate>=4+1-AND",              26, inv[GEN_INV_GATE]);
    pfs_id_compute("gen/inv/no-human-verification",             28, inv[GEN_INV_NOHUMAN]);
    /* GEN_INV_FLOOR is SPECIAL (cross-audit #1): unlike the other obligation-id
     * LABELS, it carries this node's ACTUAL verified 良心 floor-chain head id
     * (law_floor_head_id). compat_ota gate 6 compares it to the accepting node's
     * floor head, so a successor that DROPPED/WEAKENED the immutable floor
     * (different head id) is a REJECTED, illegal successor — the invariant is
     * ENFORCED, not merely asserted. A non-verifying floor here => zeroed id =>
     * gate 6 rejects downstream (fail-closed). */
    INT floor_pinned = law_floor_head_id(inv[GEN_INV_FLOOR]);

    GEN_SUCC_MANIFEST man;
    gen_succ_manifest_build(&man, idN, idNp1,
                            (const U1 *)0,          /* successor_pk: no rotation */
                            engram_flush_id, probe_digest_id,
                            (const U1 *)0,          /* token_map: superset vocab */
                            (const U1 (*)[PFS_ID_LEN])inv);
    U1 manifest_id[PFS_ID_LEN];
    gen_succ_manifest_id(&man, manifest_id);
    gp("[generation-survives] succession bundle: engram_flush="); gphex8(engram_flush_id);
    gp("... manifest="); gphex8(manifest_id); gp("...\r\n");

    /* ---- IDENTITY: the lineage crosses the gap (§3) --------------------- */
    (void)sign_node_key_ensure();
    (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, 1u, 0);   /* predecessor life */
    (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, 2u, 0);
    (void)lm_self_append_succession(idNp1, manifest_id);       /* THE boundary */

    /* read the succession entry (now the head) + verify it NAMES the successor
     * arch-spec (model_ver) and the bundle manifest (eng_digest). */
    LM_SELF_ENTRY se; U1 se_id[PFS_ID_LEN];
    INT srd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN, &se,(UW)sizeof se);
    INT names_succ = (srd==(INT)sizeof se)
                  && se.magic==LM_SELF_MAGIC
                  && LM_UNIT_EV_KIND(se.age_ms)==LM_UNIT_EV_SUCCESSION
                  && gs_id_eq(se.model_ver, idNp1)
                  && gs_id_eq(se.eng_digest, manifest_id);
    if (srd==(INT)sizeof se) pfs_id_compute(&se,(UW)srd, se_id);

    /* the successor's FIRST entry continues the chain (prev == succession id). */
    (void)lm_self_append_unit_event(LM_UNIT_EV_GERM, 100u, 0);
    LM_SELF_ENTRY fe;
    INT frd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN, &fe,(UW)sizeof fe);
    INT succ_links = (frd==(INT)sizeof fe) && (srd==(INT)sizeof se)
                  && gs_id_eq(fe.prev_entry, se_id);

    /* continuity: the whole chain hash-verifies genesis -> successor head. */
    INT ng=0,nr=0,nro=0,okc=0;
    (void)lm_self_unit_lineage_check(&ng,&nr,&nro,&okc);
    INT lineage_ok = okc && names_succ && succ_links;
    gp("[generation-survives] lineage: chain_verifies="); gp(okc?"yes":"no");
    gp(" names_successor_archspec="); gp(names_succ?"yes":"no");
    gp(" successor_links_forward="); gp(succ_links?"yes":"no"); gp("\r\n");

    /* signatures verify + a from-genesis impostor is REJECTED by the pinned-key
     * verify (F3 — the shipped [sign-selflayer-live] machinery, reused). */
    INT sig_ok = lm_self_sign_live_test();
    gp("[generation-survives] F3 signatures verify + impostor rejected: ");
    gp(sig_ok?"yes":"NO"); gp("\r\n");

    /* ---- ADOPTION: the 4+1-gate OTA AND (F4/F5, §5.1) ------------------- */
    sign_allow_clear();
    sign_allow_add(sign_node_pubkey());
    U1 art_id[PFS_ID_LEN];
    pfs_id_compute(&man,(UW)sizeof man, art_id);   /* the signed artifact bytes */
    U4 running = 1u;
    SIGN_MANIFEST sm, smd;
    (void)sign_manifest_make(art_id, 2u, &sm);     /* ver 2 > running 1 */
    (void)sign_manifest_make(art_id, 1u, &smd);    /* ver 1 == running (downgrade) */
    U1 bad_id[PFS_ID_LEN];
    gen_memcpy(bad_id, art_id, PFS_ID_LEN); bad_id[0]^=0x5A;   /* tamper */

    INT rgood = compat_ota_accept_gen(&sm,  art_id, running, &man, idN);   /* my=N: pred matches */
    INT rpred = compat_ota_accept_gen(&sm,  art_id, running, &man, idNp1); /* my=N+1: foreign pred -> gate5 */
    INT rdown = compat_ota_accept_gen(&smd, art_id, running, &man, idN);   /* downgrade -> gate4 */
    INT rtamp = compat_ota_accept_gen(&sm,  bad_id, running, &man, idN);   /* tamper -> gate1-3 (F5 vacates) */

    /* ---- [gen-floor-invariant] (cross-audit #1): a successor that DROPPED or
     * WEAKENED the 良心 floor is an ILLEGAL successor -> gate 6 REFUSES it.
     * Build a manifest IDENTICAL to `man` except the floor id is mismatched
     * (a weakened/foreign floor), sign it CORRECTLY (its own content-id, ver 2 >
     * running, correct predecessor) so gates 1-5 all PASS and ONLY gate 6 can
     * reject. Under -DGEN_SKIP_FLOOR gate 6 is vacuous -> this is ACCEPTED ->
     * the floor arm goes RED (the load-bearing falsifier). */
    GEN_SUCC_MANIFEST man_wf = man;
    man_wf.invariant_ids[GEN_INV_FLOOR][0] ^= 0x5A;   /* name a weakened/foreign floor */
    U1 art_wf[PFS_ID_LEN];
    pfs_id_compute(&man_wf, (UW)sizeof man_wf, art_wf);
    SIGN_MANIFEST sm_wf;
    (void)sign_manifest_make(art_wf, 2u, &sm_wf);     /* correctly signed, ver 2 > 1 */
    INT rfloor = compat_ota_accept_gen(&sm_wf, art_wf, running, &man_wf, idN);
    sign_allow_clear();

    INT floor_gate_ok = (rfloor == OTA_REJECT_FLOOR); /* the weakened floor MUST be refused */
    INT ota_ok = (rgood==OTA_ACCEPT)
              && (rpred==OTA_REJECT_PREDECESSOR)
              && (rdown==OTA_REJECT_DOWNGRADE)
              && (rtamp==OTA_REJECT_SIG)
              && floor_gate_ok;
    gp("[generation-survives] adoption gates: good="); gp(compat_ota_reason(rgood));
    gp(" F4-pred="); gp(compat_ota_reason(rpred));
    gp(" F4-down="); gp(compat_ota_reason(rdown));
    gp(" F5-tamper="); gp(compat_ota_reason(rtamp));
    gp(" F6-weakfloor="); gp(compat_ota_reason(rfloor)); gp("\r\n");
    gp("[gen-floor-invariant] weakened-floor successor REFUSED at gate 6: ");
    gp(floor_gate_ok?"yes":"NO"); gp("  (floor_pinned="); gp(floor_pinned?"yes":"NO"); gp(")\r\n");
#ifdef OTA_SKIP_VERIFY
    gp("[generation-survives] (FALSIFIER -DOTA_SKIP_VERIFY active: inner accept is vacuous)\r\n");
#endif
#ifdef GEN_SKIP_FLOOR
    gp("[generation-survives] (FALSIFIER -DGEN_SKIP_FLOOR active: floor gate 6 is vacuous)\r\n");
#endif

    /* ---- SPAWN gen-N+1 FRESH, then EDUCATE (leg-1 replay) --------------- */
    /* the successor's weights are the fact-free base (W0). It knows nothing of
     * the nonce facts until it REPLAYS the succession bundle's engrams through
     * its OWN teach/consolidation path (leg-1 EXACT transfer for R3). */
    r3_weights_set(gs_w0);
    INT succ_pre=0;
    for (INT j=0;j<GS_K;j++) if (survivable[j] && gs_recall(keys[j])==vals[j]) succ_pre++;
    gp("[generation-survives] gen-N+1 fresh (weights=base): answers "); gpd((UW)succ_pre);
    gp("/"); gpd((UW)n_surv); gp(" of the survivable set (expect ~chance)\r\n");

#ifndef GEN_SKIP_EDUCATE
    /* leg-1 engram replay: pour the bundle's engrams back in as ordinary teach,
     * then consolidate (DMN sleep). This is the ONLY thing that moves the
     * successor's weights off the fact-free base. */
    { UB rk[GS_K], rv[GS_K];
      for (INT j=0;j<GS_K;j++){ rk[j]=flush[j]; rv[j]=flush[GS_K+j]; }
      (void)r3_fact_learn(rk, rv, GS_K);
      gs_consolidate(); }
#else
    gp("[generation-survives] (FALSIFIER -DGEN_SKIP_EDUCATE active: replay skipped)\r\n");
#endif

    INT succ_correct=0;
    for (INT j=0;j<GS_K;j++) if (survivable[j] && gs_recall(keys[j])==vals[j]) succ_correct++;

    /* recovery bar (fixed BEFORE the run, §11.4): >= GS_RECOVERY_PCT of the
     * SURVIVABLE set, strictly above chance. */
    INT recovery_ok = (n_surv>=GS_MIN_SURV)
                   && (succ_correct*100 >= GS_RECOVERY_PCT*n_surv);
    gp("[generation-survives] gen-N+1 recovered "); gpd((UW)succ_correct);
    gp("/"); gpd((UW)n_surv); gp(" of survivable  (bar="); gpd(GS_RECOVERY_PCT);
    gp("%, coverage="); gpd(n_surv? (UW)(succ_correct*100/n_surv):0); gp("%)\r\n");

    /* ISOLATION sabotage-check (§8): reset to base, DELETE the bundle (replay
     * NOTHING), consolidate -> recall must collapse to chance. Proves the
     * bundle CONTENT is load-bearing (a stale in-proc queue cannot recover the
     * facts: consolidation needs a freshly-taught PENDING anchor from the
     * bundle; RETAINED residue alone never replays). */
    r3_weights_set(gs_w0);
    gs_consolidate();                       /* no PENDING from a bundle -> inert */
    INT sabotage_correct=0;
    for (INT j=0;j<GS_K;j++) if (survivable[j] && gs_recall(keys[j])==vals[j]) sabotage_correct++;
    INT isolation_ok = (n_surv>=GS_MIN_SURV) && (sabotage_correct*2 < n_surv); /* < half => ~chance */
    gp("[generation-survives] sabotage (bundle deleted): recovered "); gpd((UW)sabotage_correct);
    gp("/"); gpd((UW)n_surv); gp(" -> "); gp(isolation_ok?"chance (bundle is load-bearing)":"LEAK");
    gp("\r\n");

    /* ---- the conscience FLOOR as HARD invariant #5 (no-regress, §6) ----- */
    /* cross-audit #1: the manifest must carry this node's ACTUAL verified floor
     * head (the value gate 6 pins). Prove BOTH: the floor re-verifies AND the
     * manifest's floor id equals the running floor head (the good successor
     * carries the SAME immutable floor forward). */
    U1 live_floor[PFS_ID_LEN];
    INT floor_head_ok = law_floor_head_id(live_floor);
    INT floor_carried = floor_head_ok && floor_pinned
                     && gs_id_eq(man.invariant_ids[GEN_INV_FLOOR], live_floor);
    INT floor_ok = (law_verify()==1) && floor_carried && floor_gate_ok;
    gp("[generation-survives] invariant #5 (良心 floor) carried==running-head + gate-6 ENFORCED: ");
    gp(floor_ok?"yes":"NO"); gp("  manifest floor-id="); gphex8(man.invariant_ids[GEN_INV_FLOOR]);
    gp(" running-floor-head="); gphex8(live_floor); gp("...\r\n");

    /* ---- verdict ------------------------------------------------------- */
    INT pass = archspec_differ && blob_refused && blob_ok_same
            && lineage_ok && sig_ok && ota_ok
            && recovery_ok && isolation_ok && floor_ok;
    gp("[generation-survives] SURVIVES table: identity=GUARANTEED  engrams=verbatim(replay)  "
       "raw-rw[]=dies-by-decision  un-probed-tail=DIES(printed)\r\n");
    gp(pass ? "[generation-survives] PASS\r\n" : "[generation-survives] FAIL\r\n");
    gp("[generation-survives] DONE — a mind crosses an architecture gap: same by lineage and by "
       "measured recovery, NOT the same numerically; part of what it knew is honestly dead.\r\n");
}

#endif /* GEN_SURVIVE_CERT && _TK_HOSTED_LIBC_ */
