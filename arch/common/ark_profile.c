/*
 *  ark_profile.c — ark-profile v1: 人類の記憶 (the human chapter).
 *  See ark_profile.h + docs/architecture/ark-profile.md.
 *
 *  This TU owns the ARK_PROFILE / ARK_PROV objects and their save/read
 *  via pfs_dag ("self/prof" / "self/prov"), the consent record bound to
 *  the served manifesto's content-id, and the ONE provenance appender —
 *  all over the EXISTING content-address + durable-DAG substrate. It links
 *  the human chapter into the ONE "self/lin" autobiography via
 *  lm_self_append_human (the LM_SELF v2 field), never a parallel chain.
 *
 *  HONESTY (§3.3): there is NO identity verification here — no email, no
 *  key, no uniqueness check. The only bounds are max-lengths (the wire
 *  widths). 誰もそれを検証しません — the code agrees with the manifesto.
 *
 *  arch/common discipline: no host libc on the shared path (own byte
 *  helpers); fixed-width types; pfs_id_compute is THE only hash. pfs_dag
 *  exists on bare metal, so this TU links on every target — only the
 *  manifesto BYTES come from a build-embedded header, like galaxy_page.h.
 */

#include "ark_profile.h"
#include "lm_self.h"      /* lm_self_append_human — the ONE chain link    */
#include "pfs_block.h"    /* pfs_id_compute                               */
#include "pfs_dag.h"      /* pfs_dag_save / pfs_dag_read                  */
#include "drpc.h"         /* drpc_my_node — the node stamp                */
#include "kernel.h"

#include "manifesto_page.h"   /* GENERATED: manifesto_table[] + manifesto[] */

/* ------------------------------------------------------------------ */
/* tiny byte helpers (arch/common rule: no libc here)                  */
/* ------------------------------------------------------------------ */

static void ap_memcpy(void *d, const void *s, UW n)
{
    U1 *p = (U1 *)d; const U1 *q = (const U1 *)s;
    while (n--) *p++ = *q++;
}
static void ap_memset(void *d, U1 v, UW n)
{
    U1 *p = (U1 *)d; while (n--) *p++ = v;
}
static INT ap_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* coarse uptime ms (SYSTIM.lo) — same source as the galaxy ring. */
static U4 ap_now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (U4)t.lo;
}

/* wallclock unix secs if the host knows it; 0 = unknown (honest, §9).
 * Hosted only — bare metal has no wall clock, so it stays 0. */
#ifdef _TK_HOSTED_LIBC_
extern long time(long *);
static U4 ap_wallclock(void) { long t = time((long *)0); return t > 0 ? (U4)t : 0; }
#else
static U4 ap_wallclock(void) { return 0; }
#endif

/* ------------------------------------------------------------------ */
/* the served manifesto + its content-id (§7.1)                        */
/* ------------------------------------------------------------------ */

const U1 *ark_manifesto_bytes(void) { return (const U1 *)manifesto; }
UW         ark_manifesto_len(void)  { return (UW)manifesto_len; }

void ark_manifesto_id(U1 out[PFS_ID_LEN])
{
    /* the CANONICAL (ja, table row 0) content address — the default served
     * id. Per-language ids come from ark_manifesto_at(); validity checks go
     * through ark_manifesto_id_valid() (the whole table). */
    pfs_id_compute(manifesto, (UW)manifesto_len, out);   /* THE content address */
}

/* ------------------------------------------------------------------ */
/* i18n (§7.5) — the manifesto table: N languages, N content-ids       */
/* ------------------------------------------------------------------ */

UW ark_manifesto_count(void) { return (UW)manifesto_count; }

const char *ark_manifesto_code(UW i)
{
    return i < (UW)manifesto_count ? manifesto_table[i].code : 0;
}
const char *ark_manifesto_endonym(UW i)
{
    return i < (UW)manifesto_count ? manifesto_table[i].endonym : 0;
}

INT ark_manifesto_at(UW i, const U1 **bytes_out, UW *len_out,
                     U1 id_out[PFS_ID_LEN])
{
    if (i >= (UW)manifesto_count) return 0;
    const MANIFESTO_ROW *r = &manifesto_table[i];
    if (bytes_out) *bytes_out = r->bytes;
    if (len_out)   *len_out   = (UW)r->len;
    if (id_out)    pfs_id_compute(r->bytes, (UW)r->len, id_out);
    return 1;
}

/* lower-case an ASCII byte (BCP-47 codes are ASCII; case-insensitive). */
static char ap_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* case-insensitive ASCII compare up to a NUL or '-'/'_' subtag boundary in a;
 * returns 1 if a's primary subtag equals b exactly (b is a table code). */
static INT ap_code_eq(const char *a, const char *b)
{
    UW i = 0;
    for (; a[i] && b[i]; i++) if (ap_lc(a[i]) != ap_lc(b[i])) return 0;
    return a[i] == 0 && b[i] == 0;
}
/* a's primary subtag (up to '-'/'_'/NUL) equals the whole code b. */
static INT ap_prefix_eq(const char *a, const char *b)
{
    UW i = 0;
    for (; b[i]; i++) {
        char ca = a[i];
        if (ca == 0 || ca == '-' || ca == '_') return 0;
        if (ap_lc(ca) != ap_lc(b[i])) return 0;
    }
    char nx = a[i];
    return nx == 0 || nx == '-' || nx == '_';   /* b consumed a's whole subtag */
}

INT ark_manifesto_find(const char *code)
{
    if (!code || !code[0]) return -1;
    /* 1) exact (case-insensitive) full-code match (e.g. "zh-Hans"). */
    for (UW i = 0; i < (UW)manifesto_count; i++)
        if (ap_code_eq(code, manifesto_table[i].code)) return (INT)i;
    /* 2) primary-subtag prefix: "en-US" -> "en", "pt-BR" -> "pt". Skip table
     * codes that themselves carry a subtag (zh-Hans/zh-Hant) so a bare "zh"
     * does NOT silently map to one script. */
    for (UW i = 0; i < (UW)manifesto_count; i++) {
        const char *tc = manifesto_table[i].code;
        INT has_sub = 0;
        for (UW k = 0; tc[k]; k++) if (tc[k] == '-' || tc[k] == '_') { has_sub = 1; break; }
        if (has_sub) continue;
        if (ap_prefix_eq(code, tc)) return (INT)i;
    }
    return -1;
}

INT ark_manifesto_id_valid(const U1 mid[PFS_ID_LEN])
{
    if (!mid) return 0;
    U1 id[PFS_ID_LEN];
    for (UW i = 0; i < (UW)manifesto_count; i++) {
        pfs_id_compute(manifesto_table[i].bytes, (UW)manifesto_table[i].len, id);
        if (ap_id_eq(mid, id)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* head read + consent gate (§7.3)                                     */
/* ------------------------------------------------------------------ */

/* The ARK_PROFILE struct is 1188 B — too large for the bounded galaxy/shell
 * task stacks (the hosted-relay stack-overflow lesson: per-packet 1.4KB
 * stack locals crashed multi-node runs). All profile reads/builds run
 * SERIALIZED (the galaxy server is a single task; the prov/teach path runs
 * behind the mind_cmd gate), so the big scratch buffers are file-static, not
 * task-stack. */
static ARK_PROFILE ap_head_scratch;     /* head read when caller passes NULL */

INT ark_profile_head(ARK_PROFILE *out)
{
    ARK_PROFILE *p = out ? out : &ap_head_scratch;
    INT rd = pfs_dag_read((const UB *)ARK_PROF_REF, ARK_PROF_REF_LEN,
                          p, (UW)sizeof *p);
    if (rd != (INT)sizeof *p || p->magic != ARK_PROF_MAGIC) return 0;
    return 1;
}

INT ark_consent_ok(void)
{
    if (!ark_profile_head(&ap_head_scratch)) return 0; /* no profile -> closed */
    if (ap_head_scratch.consent_ack != 1) return 0;    /* never acked          */
    /* consent is to the EXACT words the person READ (§3.1 / §7.5): the stored
     * manifesto_id must still match ANY embedded language version's id. We
     * keep the per-language id, so we know which words they agreed to; if
     * that version's bytes changed, its id no longer matches and the gate
     * re-asks — honest per-language. */
    return ark_manifesto_id_valid(ap_head_scratch.manifesto_id);
}

/* ------------------------------------------------------------------ */
/* POST /profile — build + save a new version, link the human chapter   */
/* ------------------------------------------------------------------ */

static UW ap_clip(UW n, UW max) { return n > max ? max : n; }

INT ark_profile_save(U1 ack, const U1 mid[PFS_ID_LEN],
                     const char *handle, UW handle_len,
                     const char *name, UW name_len,
                     const char *msg, UW msg_len,
                     U1 id_out[PFS_ID_LEN], U4 *seq_out)
{
    /* consent must be real and bound to the EXACT bytes the person READ
     * (§3.1 / §7.5): mid must equal the content-id of SOME embedded language
     * version — not only the canonical one. The stored manifesto_id is the
     * per-language id the person actually sent, so the record honestly keeps
     * which words they agreed to. */
    if (ack != 1) return 0;
    if (!mid || !ark_manifesto_id_valid(mid)) return 0;  /* wrong mid -> 409 caller */

    /* seq = head.seq + 1 (edit = append a new version, §4.3). Static head
     * read (1188 B off the bounded task stack). */
    U4 next_seq = 1;
    if (ark_profile_head(&ap_head_scratch)) next_seq = ap_head_scratch.seq + 1;

    /* NO identity verification (§3.3): only bound the lengths. Empty
     * fields are a legitimate, complete profile (consent != disclosure). */
    handle_len = ap_clip(handle_len, ARK_HANDLE_MAX);
    name_len   = ap_clip(name_len,   ARK_NAME_MAX);
    msg_len    = ap_clip(msg_len,    ARK_MSG_MAX);

    /* the build buffer is also file-static (1188 B); serialized writers. */
    static ARK_PROFILE p;
    ap_memset(&p, 0, (UW)sizeof p);
    p.magic       = ARK_PROF_MAGIC;
    p.version     = ARK_PROF_VER;
    p.self_id     = drpc_my_node;
    p.consent_ack = 1;
    p.handle_len  = (U1)handle_len;
    p.name_len    = (U1)name_len;
    p.seq         = next_seq;
    p.age_ms      = ap_now_ms();
    p.wallclock   = ap_wallclock();
    ap_memcpy(p.manifesto_id, mid, PFS_ID_LEN);   /* the per-language id read (§7.5) */

    /* lineage_head = the current "self/lin" head id (anchors the human
     * chapter to the machine autobiography). all-zero if none yet. */
    {
        LM_SELF_ENTRY lh;
        INT rd = pfs_dag_read((const UB *)LM_SELF_REF, LM_SELF_REF_LEN,
                              &lh, (UW)sizeof lh);
        if ((rd == (INT)sizeof lh || rd == LM_SELF_ENTRY_V1_SIZE)
            && lh.magic == LM_SELF_MAGIC)
            pfs_id_compute(&lh, (UW)rd, p.lineage_head);
    }

    if (handle_len) ap_memcpy(p.handle, handle, handle_len);
    if (name_len)   ap_memcpy(p.name,   name,   name_len);
    if (msg_len)    ap_memcpy(p.msg,    msg,    msg_len);
    p.msg_len = (U2)msg_len;

    /* save the ARK_PROFILE block first (named head "self/prof"); its id is
     * the content-id of these exact bytes. */
    if (pfs_dag_save((const UB *)ARK_PROF_REF, ARK_PROF_REF_LEN,
                     &p, (UW)sizeof p) != PFS_OK)
        return -1;

    U1 pid[PFS_ID_LEN];
    pfs_id_compute(&p, (UW)sizeof p, pid);

    /* then append ONE "self/lin" v2 entry whose human_ref is that id — the
     * autobiography records that the human chapter changed (§4.2). A save
     * failure here is reported but the profile block already persists. */
    (void)lm_self_append_human(pid);

    if (id_out)  ap_memcpy(id_out, pid, PFS_ID_LEN);
    if (seq_out) *seq_out = next_seq;
    return 1;
}

/* ------------------------------------------------------------------ */
/* the ONE provenance write site (§5)                                  */
/* ------------------------------------------------------------------ */

/* which mouth is driving mind_cmd's teach verb (set by the caller right
 * before mind_cmd("teach ...")). Default shell (operator trust, §7.3).
 * file-static, single-threaded behind the mind_cmd gate. */
static U1 ap_teach_src = ARK_PROV_SRC_SHELL;

void ark_teach_src_set(U1 src) { ap_teach_src = src; }
U1   ark_teach_src_get(void)   { return ap_teach_src; }

void ark_prov_record(U4 fact_seq, U1 key, U1 val, U1 src)
{
    ARK_PROV r;
    ap_memset(&r, 0, (UW)sizeof r);
    r.magic       = ARK_PROV_MAGIC;
    r.fact_seq    = fact_seq;
    r.key         = key;
    r.val         = val;
    r.origin_node = drpc_my_node;
    r.src         = src;
    r.age_ms      = ap_now_ms();

    /* profile_head = the ARK_PROFILE content-id in force; all-zero when
     * there is no profile (an anonymous-node declaration, §5). Static head
     * read (1188 B off the bounded task stack). */
    if (ark_profile_head(&ap_head_scratch)) {
        U1 pid[PFS_ID_LEN];
        pfs_id_compute(&ap_head_scratch, (UW)sizeof ap_head_scratch, pid);
        ap_memcpy(r.profile_head, pid, PFS_ID_LEN);
    }

    /* one new version of "self/prov" — the P2 manifest chain IS the
     * append-only provenance log (no new log structure, §5). */
    (void)pfs_dag_save((const UB *)ARK_PROV_REF, ARK_PROV_REF_LEN,
                       &r, (UW)sizeof r);
}

void ark_prov_head_id(U1 out[PFS_ID_LEN])
{
    /* LM-7 (VIII.3): the content-id of the head self/prov record — the id a
     * region peer resolves the teacher through. Read the head bytes (static
     * scratch off the bounded task stack) and hash exactly them; all-zero
     * when no prov exists yet. Shell/galaxy-task context (pfs_dag_read). */
    static ARK_PROV ph;
    ap_memset(out, 0, PFS_ID_LEN);
    INT rd = pfs_dag_read((const UB *)ARK_PROV_REF, ARK_PROV_REF_LEN,
                          &ph, (UW)sizeof ph);
    if (rd == (INT)sizeof ph && ph.magic == ARK_PROV_MAGIC)
        pfs_id_compute(&ph, (UW)sizeof ph, out);
}

/* LM-7 (VIII.4): resolve a TEACHER's disclosed handle from the content-id of
 * the teacher's ARK_PROV (carried in MT_TEACH_PKT.prov_head, region-replicated
 * via P1 alongside self/prof). Walks prov_head -> ARK_PROV.profile_head ->
 * ARK_PROFILE.handle, all by content-id via pfs_get (supervisor-safe; no
 * shell man_scratch). Returns 1 and fills *origin/*handle (NUL-terminated,
 * <= ARK_HANDLE_MAX+1) when the teacher's profile is replicated AND discloses
 * a handle; returns 0 (anonymous / not-yet-replicated) leaving handle empty.
 * The chain is end-to-end the teacher's own consented record — B re-authors
 * nothing (tamper-EVIDENT, III.6/VIII.4). */
INT ark_prov_resolve_remote(const U1 prov_head[PFS_ID_LEN],
                            U1 *origin_out, char *handle_out, UW handle_max)
{
    if (handle_out && handle_max) handle_out[0] = 0;
    if (origin_out) *origin_out = 0xFF;
    if (!prov_head) return 0;

    /* prov_head -> the teacher's ARK_PROV (their self/prov head version). */
    static ARK_PROV pv;     /* static: 48 B, but keep the net path stack lean */
    INT n = pfs_get(prov_head, &pv, (UW)sizeof pv);
    if (n != (INT)sizeof pv || pv.magic != ARK_PROV_MAGIC) return 0;
    if (origin_out) *origin_out = pv.origin_node;

    /* ARK_PROV.profile_head -> the teacher's ARK_PROFILE. all-zero = the
     * teacher declared anonymously (consent w/o disclosure) — honest. */
    static const U1 zero[PFS_ID_LEN] = {0};
    if (ap_id_eq(pv.profile_head, zero)) return 0;

    static ARK_PROFILE pr;  /* 1188 B static: NEVER a task-stack local       */
    n = pfs_get(pv.profile_head, &pr, (UW)sizeof pr);
    if (n != (INT)sizeof pr || pr.magic != ARK_PROF_MAGIC) return 0;
    if (pr.handle_len == 0) return 0;       /* pseudonymity declined too      */

    UW hl = pr.handle_len;
    if (hl > ARK_HANDLE_MAX) hl = ARK_HANDLE_MAX;
    if (handle_out && handle_max) {
        UW cpy = (hl < handle_max - 1) ? hl : handle_max - 1;
        ap_memcpy(handle_out, pr.handle, cpy);
        handle_out[cpy] = 0;
    }
    return 1;
}
