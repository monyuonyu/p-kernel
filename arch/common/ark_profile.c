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

#include "manifesto_page.h"   /* GENERATED: manifesto[] + manifesto_len   */

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
    pfs_id_compute(manifesto, (UW)manifesto_len, out);   /* THE content address */
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
    U1 mid[PFS_ID_LEN]; ark_manifesto_id(mid);
    /* consent is to the EXACT words: if the served text changed since the
     * ack, the stored id no longer matches and the gate re-asks (§3.1). */
    return ap_id_eq(ap_head_scratch.manifesto_id, mid) ? 1 : 0;
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
    /* consent must be real and bound to the EXACT served bytes (§3.1). */
    if (ack != 1) return 0;
    U1 cur_mid[PFS_ID_LEN]; ark_manifesto_id(cur_mid);
    if (!mid || !ap_id_eq(mid, cur_mid)) return 0;  /* wrong mid -> 409 caller */

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
    ap_memcpy(p.manifesto_id, cur_mid, PFS_ID_LEN);

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
