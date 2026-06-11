/*
 *  lm_self.c -- living-mind Self layer (first slice): a distributed
 *  autobiographical self.
 *
 *  See lm_self.h + docs/architecture/living-mind.md Part III.
 *
 *  THE CLAIM (III.1): a per-node, hash-chained NARRATIVE LINEAGE (versions
 *  of the p-fs object "self/lin") that (1) survives the node's DEATH
 *  (reconstructs from the persisted store, not RAM), (2) is TAMPER-EVIDENT
 *  (any alteration of a committed entry is detectable; the walker fails
 *  closed), (3) reconstructs from a peer subset EXCLUDING the origin
 *  (ownerless), and (4) is continued by a successor (identity persists
 *  THROUGH death).
 *
 *  HONEST BOUND (III.6): tamper-EVIDENT, NOT tamper-PROOF / unforgeable.
 *  There is NO signature primitive in the tree (genome.h states the same
 *  limit verbatim). A malicious node that controls its own store can author
 *  a fresh, internally-consistent FAKE lineage from genesis. The teeth we
 *  DO claim: an already-committed entry cannot be altered or spliced without
 *  the content-address (pfs_id_compute) chain breaking, and reconstruction
 *  needs no owner. Per-node-keypair signatures are deferred to a later slice.
 *
 *  ANTI-FORK (III.7): NO new hash, NO forked merkle/crypto, NO duplicated
 *  gossip loop. Every chain link is pfs_id_compute (THE sha256 content
 *  address); the durable/replicated lineage is pfs_dag_save/read/restore +
 *  pfs_durable_restore; the walk is pfs_get/pfs_has (content-level, III.4
 *  recommended option -> pfs_dag.c is untouched). The self_id stamp is
 *  drpc_my_node; model_ver is the content-id of the "dtr/weights" blob
 *  (GENOME_WEIGHTS_REF); eng_digest summarizes the LM_ENGRAM episode unit.
 */

#include "lm_self.h"
#include "lm_consolidate.h"   /* LM_ENGRAM -- the episodes eng_digest sums   */
#include "pfs_block.h"        /* pfs_id_compute / pfs_get / pfs_has / restore */
#include "pfs_dag.h"          /* pfs_dag_save / pfs_dag_read / pfs_dag_restore */
#include "genome.h"           /* GENOME_WEIGHTS_REF -- the model_ver object   */
#include "drpc.h"             /* drpc_my_node -- the self_id stamp            */
#include "dtr.h"              /* dtr_reinit_weights / dtr_weights_get         */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like lm_consolidate.c)           */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void lp(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}
static void lpd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { lp("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    lp(&buf[i]);
}
static void lpb(const char *name, INT b)   /* print "name=yes/no" */
{
    lp(name); lp(b ? "yes" : "no");
}
/* first 8 bytes of an id as hex, to eyeball the chain */
static void lphex8(const U1 id[PFS_ID_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    char out[2 * 8 + 1]; INT j = 0;
    for (INT i = 0; i < 8; i++) { out[j++] = hexd[(id[i] >> 4) & 0xF]; out[j++] = hexd[id[i] & 0xF]; }
    out[j] = '\0';
    lp(out);
}
/* swallow pfs restore summaries (no-op emit) */
static void self_emit_quiet(const char *s) { (void)s; }

/* ------------------------------------------------------------------ */
/* tiny byte helpers (arch/common rule: no libc here)                  */
/* ------------------------------------------------------------------ */

static void self_memcpy(void *d, const void *s, UW n)
{
    U1 *p = (U1 *)d; const U1 *q = (const U1 *)s;
    while (n--) *p++ = *q++;
}
static void self_memset(void *d, U1 v, UW n)
{
    U1 *p = (U1 *)d; while (n--) *p++ = v;
}
static INT self_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static INT self_id_zero(const U1 a[PFS_ID_LEN])
{
    for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* lineage parameters (III.5: N small, compiled-in)                    */
/* ------------------------------------------------------------------ */

#define LM_SELF_N      8          /* chain length under test          */
#define LM_SELF_WALK_MAX  (LM_SELF_N + 4)   /* walk loop guard         */
#define LM_SELF_SEED   0x5E1F2026UL          /* deterministic model_ver */

/* the node's current "dtr/weights" content-id -- the model_ver carried
 * on every entry (III.3: "the self knows which brain it was running").
 * Computed as pfs_id_compute over the live weight body, which IS the
 * content-id pfs_dag_save would assign the GENOME_WEIGHTS_REF blob. */
static U1 self_model_ver[PFS_ID_LEN];

static void self_compute_model_ver(void)
{
    static float wbuf[DTR_WEIGHT_FLOATS];
    /* Prefer the live "dtr/weights" object (GENOME_WEIGHTS_REF, the genome
     * versioned-object precedent): the content-id of that blob IS our
     * model_ver. If it isn't published yet, fall back to a fixed-seed
     * in-RAM weight body -- the content-address is pfs_id_compute over the
     * weight bytes either way, so no new object is invented. */
    INT n = pfs_dag_read((const UB *)GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN,
                         wbuf, (UW)sizeof wbuf);
    if (n != (INT)sizeof wbuf) {
        dtr_reinit_weights(LM_SELF_SEED);  /* fixed brain -> fixed model_ver */
        dtr_weights_get(wbuf);
    }
    pfs_id_compute(wbuf, (UW)sizeof(wbuf), self_model_ver);
}

/* eng_digest for sequence seq: pfs_id_compute over a small deterministic
 * LM_ENGRAM ring image (reuse Part II's episode unit + the existing
 * content-address path -- NO new hash). Each seq summarizes a distinct
 * (synthetic) period, so entries differ and ids differ. */
#define LM_SELF_ENG  6
static void self_eng_digest(U4 seq, U1 out[PFS_ID_LEN])
{
    LM_ENGRAM ring[LM_SELF_ENG];
    UW r = seq * 2654435761UL + 1013904223UL;     /* per-seq seed       */
    for (INT k = 0; k < LM_SELF_ENG; k++) {
        r = r * 1664525UL + 1013904223UL;
        ring[k].input[0] = (B)(r & 0xFF);
        ring[k].input[1] = (B)((r >> 8) & 0xFF);
        ring[k].input[2] = (B)((r >> 16) & 0xFF);
        ring[k].input[3] = (B)((r >> 24) & 0xFF);
        ring[k].label    = (UB)(r % 3);
        ring[k].task_id  = (UB)(seq & 0xFF);
        ring[k].salience = 1;
        ring[k]._pad     = 0;
    }
    pfs_id_compute(ring, (UW)sizeof(ring), out);
}

/* fill one self entry. prev==NULL => genesis (all-zero prev_entry). */
static void self_fill(LM_SELF_ENTRY *e, U1 self_id, U4 seq,
                      const U1 prev[PFS_ID_LEN])
{
    self_memset(e, 0, sizeof *e);
    e->magic   = LM_SELF_MAGIC;
    e->version = LM_SELF_VER;
    e->self_id = self_id;
    e->seq     = seq;
    e->age_ms  = seq * 1000UL;        /* coarse, deterministic           */
    if (prev) self_memcpy(e->prev_entry, prev, PFS_ID_LEN);
    /* else prev_entry stays all-zero (genesis) */
    self_eng_digest(seq, e->eng_digest);
    self_memcpy(e->model_ver, self_model_ver, PFS_ID_LEN);
}

/* ------------------------------------------------------------------ */
/* the content-level chain WALKER (III.3/III.4). Fail-closed: the       */
/* default verdict on any non-verifying chain is REJECT (ok=0).         */
/*                                                                      */
/* A store is abstracted by a getter so the SAME verified walk runs over */
/* (a) the real p-fs block store (pfs_get/pfs_has) for continuity +      */
/* tamper, and (b) an in-process PEER store for the ownerless test (the  */
/* G22 in-process multi-node pattern). Every link is verified with the   */
/* ONE content address pfs_id_compute -- no forked crypto.               */
/* ------------------------------------------------------------------ */

/* getter: copy the block named id into buf (up to max); return its full
 * length, or -1 on a miss. */
typedef INT (*self_getf)(void *ctx, const U1 id[PFS_ID_LEN], void *buf, UW max);

/* walk head -> genesis. Returns the verified length. *ok=1 only if the
 * chain reaches a genesis (all-zero prev) with every block present and
 * every block's bytes hashing to the id used to fetch it (content-address
 * integrity) and magic intact; otherwise *ok=0 (REJECT). Optionally
 * records each visited id (head-first) into ids_out for hash-for-hash. */
static UW self_walk(self_getf get, void *ctx, const U1 head[PFS_ID_LEN],
                    INT *ok, U1 ids_out[][PFS_ID_LEN])
{
    U1 cur[PFS_ID_LEN];
    self_memcpy(cur, head, PFS_ID_LEN);
    UW len = 0;
    for (;;) {
        if (self_id_zero(cur)) { *ok = 1; return len; }   /* reached genesis */
        LM_SELF_ENTRY e;
        INT g = get(ctx, cur, &e, (UW)sizeof e);
        /* DUAL-WIDTH (ark-profile.md §4.2 / P5): accept a v1 (116 B) OR a
         * v2 (148 B) entry. The getter returns the block's FULL length, so
         * size-check per version; magic+version live at the same offset in
         * both, and prev_entry precedes the v2-only human_ref tail. Any
         * other width REJECTs (fail-closed). */
        if (g != (INT)sizeof e && g != LM_SELF_ENTRY_V1_SIZE) { *ok = 0; return len; }
        U1 rid[PFS_ID_LEN];
        pfs_id_compute(&e, (UW)g, rid);                   /* hash EXACT bytes */
        if (!self_id_eq(rid, cur)) { *ok = 0; return len; } /* bytes!=address */
        if (e.magic != LM_SELF_MAGIC) { *ok = 0; return len; }
        if (g == LM_SELF_ENTRY_V1_SIZE && e.version != 1) { *ok = 0; return len; }
        if (g == (INT)sizeof e && e.version != LM_SELF_VER) { *ok = 0; return len; }
        if (ids_out && len < LM_SELF_WALK_MAX) self_memcpy(ids_out[len], cur, PFS_ID_LEN);
        len++;
        if (len > LM_SELF_WALK_MAX) { *ok = 0; return len; }  /* loop guard */
        self_memcpy(cur, e.prev_entry, PFS_ID_LEN);
    }
}

/* real-p-fs getter (continuity + tamper tests) */
static INT self_get_pfs(void *ctx, const U1 id[PFS_ID_LEN], void *buf, UW max)
{
    (void)ctx;
    if (!pfs_has(id)) return -1;
    return pfs_get(id, buf, max);
}

/* ------------------------------------------------------------------ */
/* in-process block store for the ownerless test (G22 in-process        */
/* pattern: model >=2 nodes' stores as explicit arrays in one process). */
/* store_put content-addresses with the REAL pfs_id_compute -- it is a   */
/* store MODEL, not a new hash.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    U1 id[PFS_ID_LEN];
    U1 bytes[sizeof(LM_SELF_ENTRY)];
    UW len;
    U1 used;
} SELF_BLK;

typedef struct { SELF_BLK b[2 * LM_SELF_N + 4]; UW n; } SELF_STORE;

static void store_clear(SELF_STORE *s) { s->n = 0; for (UW i = 0; i < (UW)(2 * LM_SELF_N + 4); i++) s->b[i].used = 0; }

static void store_put(SELF_STORE *s, const void *buf, UW len, U1 id_out[PFS_ID_LEN])
{
    U1 id[PFS_ID_LEN];
    pfs_id_compute(buf, len, id);               /* THE content address */
    if (id_out) self_memcpy(id_out, id, PFS_ID_LEN);
    for (UW i = 0; i < s->n; i++)               /* dedup, like pfs_put */
        if (s->b[i].used && self_id_eq(s->b[i].id, id)) return;
    if (s->n >= (UW)(2 * LM_SELF_N + 4)) return;
    UW slot = s->n++;
    self_memcpy(s->b[slot].id, id, PFS_ID_LEN);
    self_memcpy(s->b[slot].bytes, buf, len);
    s->b[slot].len  = len;
    s->b[slot].used = 1;
}

static INT store_get(void *ctx, const U1 id[PFS_ID_LEN], void *buf, UW max)
{
    SELF_STORE *s = (SELF_STORE *)ctx;
    for (UW i = 0; i < s->n; i++) {
        if (s->b[i].used && self_id_eq(s->b[i].id, id)) {
            UW cpy = (s->b[i].len < max) ? s->b[i].len : max;
            self_memcpy(buf, s->b[i].bytes, cpy);
            return (INT)s->b[i].len;
        }
    }
    return -1;
}

static void store_copy(SELF_STORE *dst, const SELF_STORE *src)   /* "replicate" */
{
    store_clear(dst);
    for (UW i = 0; i < src->n; i++) if (src->b[i].used) {
        UW slot = dst->n++;
        self_memcpy(dst->b[slot].id, src->b[i].id, PFS_ID_LEN);
        self_memcpy(dst->b[slot].bytes, src->b[i].bytes, src->b[i].len);
        dst->b[slot].len = src->b[i].len; dst->b[slot].used = 1;
    }
}

/* ------------------------------------------------------------------ */
/* shared chain builder: fill N entries with proper prev links and      */
/* record each entry's content-id. Genesis is index 0 (prev all-zero).  */
/* ------------------------------------------------------------------ */

static LM_SELF_ENTRY self_chain[LM_SELF_N];
static U1            self_ids[LM_SELF_N][PFS_ID_LEN];   /* id of each entry */

static void self_build_chain(U1 self_id)
{
    U1 prev[PFS_ID_LEN]; self_memset(prev, 0, PFS_ID_LEN);   /* genesis */
    for (INT k = 0; k < LM_SELF_N; k++) {
        self_fill(&self_chain[k], self_id, (U4)(k + 1),
                  (k == 0) ? (const U1 *)0 : prev);
        pfs_id_compute(&self_chain[k], (UW)sizeof(LM_SELF_ENTRY), self_ids[k]);
        self_memcpy(prev, self_ids[k], PFS_ID_LEN);
    }
}

/* ================================================================== */
/* selfc-ring3 §1.3 — append a unit-lifecycle event to "self/lin"       */
/* ================================================================== */
/* Reuses the EXISTING chain: read the current head LM_SELF_ENTRY (if any)
 * to find the prev content-id + the next seq, build a new entry whose
 * prev_entry is the head's content-id, encode the event into age_ms,
 * summarize the event descriptor into eng_digest via pfs_id_compute, and
 * commit with pfs_dag_save (the SAME path lm_self_test uses; P1 replicates
 * the appended blocks for free). No second chain, no new hash.
 * (ark-profile merge note: head reads accept a v1 116 B or v2 148 B head —
 * the id is computed over the EXACT bytes read, like the dual-width walker.) */

INT lm_self_append_unit_event(UB kind, U4 unit_ver, UB sig)
{
    self_compute_model_ver();               /* stamp the brain we run on   */

    /* find the current head: its content-id becomes the new entry's prev,
     * and its seq+1 is the new seq. If "self/lin" has no head yet, this is
     * a genesis entry (prev all-zero, seq 1). */
    LM_SELF_ENTRY head;
    U1  prev[PFS_ID_LEN];
    self_memset(prev, 0, PFS_ID_LEN);
    U4  next_seq = 1;
    INT rd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                          &head, (UW)sizeof head);
    if ((rd == (INT)sizeof head || rd == LM_SELF_ENTRY_V1_SIZE)
        && head.magic == LM_SELF_MAGIC) {
        pfs_id_compute(&head, (UW)rd, prev);   /* content-id of head (exact bytes) */
        next_seq = head.seq + 1;
    }

    LM_SELF_ENTRY e;
    self_memset(&e, 0, sizeof e);
    e.magic   = LM_SELF_MAGIC;
    e.version = LM_SELF_VER;
    e.self_id = drpc_my_node;
    e.seq     = next_seq;
    e.age_ms  = LM_UNIT_EV_ENCODE(kind, unit_ver, sig);  /* the event payload */
    if (!self_id_zero(prev)) self_memcpy(e.prev_entry, prev, PFS_ID_LEN);
    /* eng_digest summarizes the event descriptor (deterministic image, the
     * same content-address path self_eng_digest uses — NO new hash). */
    {
        U4 desc[3] = { (U4)kind, unit_ver, (U4)sig };
        pfs_id_compute(desc, (UW)sizeof desc, e.eng_digest);
    }
    self_memcpy(e.model_ver, self_model_ver, PFS_ID_LEN);

    return pfs_dag_save((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                        &e, (UW)sizeof e);
}

/* selfc-ring3 §5.3 — walk the live "self/lin" chain head->genesis, verify it
 * hash-chains end-to-end (reusing the wave-22 self_walk verifier), and count
 * the unit-lifecycle events by kind. The walk yields ids head-first; we read
 * each entry's age_ms and decode the event. Returns 1 if the chain verifies,
 * 0 (fail-closed) otherwise; *germ/*reap/*roll receive the event counts and
 * *ok_chain the hash-verify verdict. */
INT lm_self_unit_lineage_check(INT *n_germ, INT *n_reap, INT *n_roll,
                               INT *ok_chain)
{
    if (n_germ) *n_germ = 0;
    if (n_reap) *n_reap = 0;
    if (n_roll) *n_roll = 0;
    if (ok_chain) *ok_chain = 0;

    /* read the head manifest content (an LM_SELF_ENTRY), hash it to get the
     * head content-id, then walk to genesis with the SAME verifier the Self
     * layer uses (content-address integrity + fail-closed). Dual-width head
     * (v1/v2) accepted; the id is over the exact bytes read. */
    LM_SELF_ENTRY head;
    INT rd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                          &head, (UW)sizeof head);
    if ((rd != (INT)sizeof head && rd != LM_SELF_ENTRY_V1_SIZE)
        || head.magic != LM_SELF_MAGIC) return 0;
    U1 head_id[PFS_ID_LEN];
    pfs_id_compute(&head, (UW)rd, head_id);

    INT ok = 0;
    static U1 ids[LM_SELF_WALK_MAX][PFS_ID_LEN];
    UW len = self_walk(self_get_pfs, 0, head_id, &ok, ids);
    if (ok_chain) *ok_chain = ok;
    if (!ok) return 0;                  /* fail-closed: a broken chain REJECTs */

    for (UW i = 0; i < len; i++) {
        LM_SELF_ENTRY e;
        if (self_get_pfs(0, ids[i], &e, sizeof e) != (INT)sizeof e) continue;
        UB kind = (UB)LM_UNIT_EV_KIND(e.age_ms);
        if (kind == LM_UNIT_EV_GERM     && n_germ) (*n_germ)++;
        else if (kind == LM_UNIT_EV_REAP     && n_reap) (*n_reap)++;
        else if (kind == LM_UNIT_EV_ROLLBACK && n_roll) (*n_roll)++;
    }
    return 1;
}

/* ================================================================== */
/* ark-profile v1 (ark-profile.md §4.2): the PRODUCTION human-chapter   */
/* append. Reads the current "self/lin" head as prev_entry, fills a v2  */
/* entry carrying human_ref = the saved ARK_PROFILE content-id, and     */
/* commits it under LM_SELF_REF via pfs_dag_save — the SAME append      */
/* mechanism + the SAME walker the Self certs use. NO parallel chain.   */
/* ================================================================== */

INT lm_self_append_human(const U1 human_ref[PFS_ID_LEN])
{
    self_compute_model_ver();           /* the brain this entry ran on */

    /* prev_entry = content-id of the current head (genesis-safe). */
    U1 prev[PFS_ID_LEN]; self_memset(prev, 0, PFS_ID_LEN);
    U4 next_seq = 1;
    LM_SELF_ENTRY head;
    INT rd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                          &head, (UW)sizeof head);
    /* accept a v1 (116 B) or v2 (148 B) head; compute its id over the
     * EXACT bytes read so prev links by content address either way. */
    if ((rd == (INT)sizeof head || rd == LM_SELF_ENTRY_V1_SIZE)
        && head.magic == LM_SELF_MAGIC) {
        pfs_id_compute(&head, (UW)rd, prev);
        next_seq = head.seq + 1;
    }

    LM_SELF_ENTRY e;
    self_fill(&e, drpc_my_node, next_seq, self_id_zero(prev) ? (const U1 *)0 : prev);
    self_memcpy(e.human_ref, human_ref, PFS_ID_LEN);

    return (pfs_dag_save((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                         &e, (UW)sizeof e) == PFS_OK) ? 1 : 0;
}

/* ================================================================== */
/* the acceptance suite (living-mind.md III.5)                          */
/* ================================================================== */

void lm_self_test(void)
{
    INT fails = 0;
    U1  me = drpc_my_node;                  /* the origin self_id stamp */

    self_compute_model_ver();

    lp("[self-test] ==== living-mind: the Self layer (living-mind.md III) ====\r\n");
    lp("[self-test] hash-chained autobiographical lineage \"self/lin\"; N=");
    lpd(LM_SELF_N); lp(" entries; self_id="); lpd((UW)me);
    lp("; sizeof(LM_SELF_ENTRY)="); lpd((UW)sizeof(LM_SELF_ENTRY));
    lp(" B; content-level walk (pfs_id_compute + pfs_get; pfs_dag.c untouched)\r\n");
    lp("[self-test] model_ver(dtr/weights content-id)="); lphex8(self_model_ver); lp("...\r\n");

    /* ---- 1. [self-continuity] -- survives death AND continues -------- */
    {
        self_build_chain(me);

        /* commit the N entries as versions of "self/lin" via pfs_dag_save
         * (manifest chain + content block; P1 replicates for free). */
        INT save_ok = 1;
        for (INT k = 0; k < LM_SELF_N; k++)
            if (pfs_dag_save((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                             &self_chain[k], (UW)sizeof(LM_SELF_ENTRY)) != PFS_OK)
                save_ok = 0;

        U1 predeath_head[PFS_ID_LEN];
        self_memcpy(predeath_head, self_ids[LM_SELF_N - 1], PFS_ID_LEN);

        /* DROP RAM: forget the in-RAM chain image; force reconstruction
         * from the persisted content-addressed store only. (The engram
         * ring lives in lm_consolidate.c and is not read by the chain;
         * eng_digest is a committed value, independent of the live ring.)
         * Reload via pfs_durable_restore FIRST, then pfs_dag_restore -- the
         * prescribed III.3 calls (no-op without PKERNEL_PFS_DIR, in which
         * case the in-memory block store IS the persisted store, exactly
         * as the DMN survive test treats it). */
        self_memset(self_chain, 0, sizeof self_chain);
        (void)pfs_durable_restore(self_emit_quiet);
        pfs_dag_restore();

        /* reconstruct: read the head version, then walk to genesis. */
        LM_SELF_ENTRY head;
        INT rd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                              &head, (UW)sizeof head);
        U1 head_id[PFS_ID_LEN]; self_memset(head_id, 0, PFS_ID_LEN);
        INT head_ok = (rd == (INT)sizeof head);
        if (head_ok) pfs_id_compute(&head, (UW)sizeof head, head_id);
        INT head_match = head_ok && self_id_eq(head_id, predeath_head);

        INT ok = 0;
        static U1 rec_ids[LM_SELF_WALK_MAX][PFS_ID_LEN];
        UW rlen = head_ok ? self_walk(self_get_pfs, 0, head_id, &ok, rec_ids) : 0;

        /* hash-for-hash: walk yields ids head-first; compare to the
         * pre-death ids (genesis-first), i.e. rec_ids[i] == self_ids[N-1-i]. */
        INT h4h = ok && (rlen == LM_SELF_N);
        for (UW i = 0; h4h && i < rlen; i++)
            if (!self_id_eq(rec_ids[i], self_ids[LM_SELF_N - 1 - i])) h4h = 0;

        /* successor continues the identity THROUGH death: a NEW entry whose
         * prev_entry is the restored head, seq == N+1. */
        LM_SELF_ENTRY succ;
        self_fill(&succ, me, (U4)(LM_SELF_N + 1), head_id);
        INT succ_saved = (pfs_dag_save((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                                       &succ, (UW)sizeof succ) == PFS_OK);
        LM_SELF_ENTRY succ_rd;
        INT srd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                               &succ_rd, (UW)sizeof succ_rd);
        INT seq_ok  = succ_saved && srd == (INT)sizeof succ_rd
                      && succ_rd.seq == (U4)(LM_SELF_N + 1);
        INT prev_ok = seq_ok && self_id_eq(succ_rd.prev_entry, head_id);

        lp("[self-test] continuity: save="); lp(save_ok ? "ok" : "ERR");
        lp(" recovered_len="); lpd(rlen); lp("/"); lpd(LM_SELF_N);
        lp(" head_id="); lphex8(head_id);
        lp("... "); lpb("head_match=", head_match);
        lp(" "); lpb("hashforhash=", h4h);
        lp(" successor_seq="); lpd(srd == (INT)sizeof succ_rd ? succ_rd.seq : 0);
        lp(" "); lpb("seq==N+1=", seq_ok);
        lp(" "); lpb("prev==head=", prev_ok);
        lp("\r\n");

        if (save_ok && rlen == LM_SELF_N && head_match && h4h && seq_ok && prev_ok) {
            lp("[self-continuity] PASS (length-N chain reconstructed hash-for-hash from the persisted store after RAM drop; successor links forward through death)\r\n");
        } else {
            lp("[self-continuity] FAIL\r\n");
            fails++;
        }
    }

    /* ---- 2. [self-tamperevident] -- a forged/edited self is detected - */
    {
        /* rebuild a clean canonical chain in the real store. */
        self_build_chain(me);
        for (INT k = 0; k < LM_SELF_N; k++)
            (void)pfs_put(&self_chain[k], (UW)sizeof(LM_SELF_ENTRY), 0);

        U1 head_id[PFS_ID_LEN];
        self_memcpy(head_id, self_ids[LM_SELF_N - 1], PFS_ID_LEN);

        /* (a) a clean chain still verifies -- no false positive. */
        INT clean_ok = 0;
        UW clean_len = self_walk(self_get_pfs, 0, head_id, &clean_ok, 0);
        INT no_false_pos = clean_ok && clean_len == LM_SELF_N;

        /* (b) byte-level: flip ONE byte in a committed entry -> its
         * content-id MOVES, so it no longer matches the id its successor
         * stored. A committed entry cannot be silently altered. */
        INT k = LM_SELF_N / 2;
        LM_SELF_ENTRY tampered = self_chain[k];
        ((U1 *)&tampered)[40] ^= 0x01;          /* flip one byte           */
        U1 t_id[PFS_ID_LEN];
        pfs_id_compute(&tampered, (UW)sizeof tampered, t_id);
        INT detected_flip = !self_id_eq(t_id, self_ids[k]);  /* stored id != */

        /* (c) walker fail-closed: splice a FORGED entry whose prev_entry is
         * wrong (points at a non-existent block). Walking it must REJECT. */
        LM_SELF_ENTRY forged = self_chain[LM_SELF_N - 1];
        forged.prev_entry[3] ^= 0xFF;           /* corrupt the back-link   */
        U1 forged_id[PFS_ID_LEN];
        (void)pfs_put(&forged, (UW)sizeof forged, forged_id);
        INT forged_ok = 1;
        UW forged_len = self_walk(self_get_pfs, 0, forged_id, &forged_ok, 0);
        INT rejected = (forged_ok == 0);        /* fail-closed REJECT      */

        lp("[self-test] tamperevident: clean_walk_len="); lpd(clean_len);
        lp("/"); lpd(LM_SELF_N); lp(" "); lpb("clean_verifies=", no_false_pos);
        lp("  flip@entry"); lpd((UW)k);
        lp(": tampered_id="); lphex8(t_id);
        lp("... stored_id="); lphex8(self_ids[k]);
        lp("... "); lpb("detected=", detected_flip);
        lp("  forged_splice: walked="); lpd(forged_len);
        lp(" "); lpb("walker_rejects=", rejected);
        lp("\r\n");

        if (no_false_pos && detected_flip && rejected) {
            lp("[self-tamperevident] PASS (committed entry cannot be altered/spliced without the content-address chain breaking; walker fails closed; clean chain still verifies)\r\n");
        } else {
            lp("[self-tamperevident] FAIL\r\n");
            fails++;
        }
        lp("[self-test] NOTE: tamper-EVIDENT, not tamper-PROOF -- no signature primitive exists (III.6); a from-genesis fake is still possible. signatures are deferred.\r\n");
    }

    /* ---- 3. [self-ownerless] -- reconstruct EXCLUDING the origin ----- */
    {
        /* G22 in-process pattern: model two nodes' block stores. The origin
         * builds + content-addresses the chain; P1 replicates the blocks to
         * a peer; we then EMPTY the origin store entirely and reconstruct
         * the full chain, hash-for-hash, from the PEER's blocks ONLY. */
        static SELF_STORE origin, peer;
        store_clear(&origin); store_clear(&peer);

        self_build_chain(me);
        for (INT k = 0; k < LM_SELF_N; k++)
            store_put(&origin, &self_chain[k], (UW)sizeof(LM_SELF_ENTRY), 0);

        U1 head_id[PFS_ID_LEN];
        self_memcpy(head_id, self_ids[LM_SELF_N - 1], PFS_ID_LEN);

        store_copy(&peer, &origin);             /* P1 region replication   */

        /* kill the origin: empty its store entirely. */
        store_clear(&origin);
        LM_SELF_ENTRY tmp;
        INT origin_serves_head = (store_get(&origin, head_id, &tmp, sizeof tmp) >= 0);

        /* reconstruct from the PEER subset only, verified hash-for-hash. */
        INT ok = 0;
        static U1 rec_ids[LM_SELF_WALK_MAX][PFS_ID_LEN];
        UW rlen = self_walk(store_get, &peer, head_id, &ok, rec_ids);
        INT h4h = ok && (rlen == LM_SELF_N);
        for (UW i = 0; h4h && i < rlen; i++)
            if (!self_id_eq(rec_ids[i], self_ids[LM_SELF_N - 1 - i])) h4h = 0;
        /* every served entry carries the origin self_id (whose life). */
        INT self_id_intact = h4h;
        for (UW i = 0; self_id_intact && i < rlen; i++) {
            LM_SELF_ENTRY e;
            if (store_get(&peer, rec_ids[i], &e, sizeof e) != (INT)sizeof e
                || e.self_id != me) self_id_intact = 0;
        }

        lp("[self-test] ownerless: origin_emptied="); lpb("", !origin_serves_head);
        lp(" peer_served_len="); lpd(rlen); lp("/"); lpd(LM_SELF_N);
        lp(" "); lpb("hashforhash=", h4h);
        lp(" "); lpb("self_id_intact=", self_id_intact);
        lp("\r\n");

        if (!origin_serves_head && rlen == LM_SELF_N && h4h && self_id_intact) {
            lp("[self-ownerless] PASS (full length-N chain reconstructed hash-for-hash from a peer subset with the origin store empty -- no central owner)\r\n");
        } else {
            lp("[self-ownerless] FAIL\r\n");
            fails++;
        }
    }

    if (fails == 0) lp("[self-test] ALL PASS\r\n");
    else { lp("[self-test] FAILURES="); lpd((UW)fails); lp("\r\n"); }
}
