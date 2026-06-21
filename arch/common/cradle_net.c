/*
 *  cradle_net.c — Thread T lesson-bridge TRANSPORT (T-fix-b / T-1).
 *                 (docs/architecture/thread-t-impl-plan.md §2.1.)
 *
 *  The mesh carrier for a teacher's TEXT lesson to a student node. Mirrors the
 *  LM-7 mind/teach pattern (r3_incontext.c m_publish_teach / mind_net_task):
 *
 *    TEACHER side (cradle_teach_emit): pfs_dag_save the lesson body under a
 *    content ref, then kdds_pub a CRADLE_TEACH_PKT beacon on the region-scoped
 *    LATEST_ONLY topic "cradle/teach". Gated on region_teacher()==drpc_my_node
 *    (only the NOCENTRAL-elected teacher emits; T-fix-a selection). NO-OP-safe
 *    on a non-teacher / solo node.
 *
 *    STUDENT side (cradle_poll_and_pull): the STRONG override of the weak hook
 *    in student_shell.c. Called at the TOP of student_dmn_consolidate() (the DMN
 *    sleep tick) BEFORE windowing. kdds_sub(timeout=0) on cradle/teach; on a
 *    NEWER seq with a matching vocab fingerprint, pfs_dag_read the body and
 *    cradle_lesson_ingest() it into the student's lesson ring. STRICTLY
 *    NO-OP-safe: no relay / no beacon / no body -> the ring stays empty ->
 *    window() reads the fixture (the no-op contract, student_shell.c).
 *
 *  WIRE DISCIPLINE: the body is LENGTH-PREFIXED BINARY (§2.1 risk 10) — never a
 *  C-string; a live lesson is tok_decode output incl. NUL/control bytes. The
 *  body_ref names a p-fs DAG object; body_len bounds the read.
 *
 *  ONE-MIND / NOCENTRAL: the student trains via its OWN distill path (the ring
 *  feeds sleep_rounds -> st_backward); this TU only MOVES BYTES, it never
 *  averages weights. The teacher selection is the SWIM capability bit (T-fix-a),
 *  recomputed locally by every node (region_teacher) — no registrar, no vote.
 *
 *  vocab_fp for the BYTE student is a CONSTANT (ST_VOCAB=256 raw bytes); we
 *  stamp a fixed sentinel and refuse a mismatch, so a future soft-target /
 *  foreign-keying teacher is rejected WITHOUT a wire bump (the MT_WIRE_VER_VOCAB
 *  discipline carried to the byte student).
 *
 *  Tier: kernel-side common TU (KDDS + p-fs + region). Built into the hosted
 *  kernels (boot/linux, boot/linux_x86_64) and Android; weak no-ops in
 *  student_stub.c keep bare-metal / student-less builds linking.
 */
#include "kernel.h"
#include "dtr.h"          /* CRADLE_TEACH_PKT / TOPIC / cradle_* decls          */
#include "kdds.h"
#include "drpc.h"
#include "region.h"
#include "pfs_dag.h"

/* the ingest seam (cradle.c, student tier). Declared plainly so this kernel TU
 * need not pull student.h (which drops every kernel -I). On a build WITHOUT the
 * student (bare metal), the weak student_stub keeps the link green and this TU
 * is simply not driven (no baby -> no DMN student tick -> no pull). */
IMPORT int cradle_lesson_ingest(const UB *body, int len);

/* the fixed byte-student fingerprint: "BYTE" ++ "256." ++ zero pad. Any teacher
 * stamping a different fp (e.g. a soft/foreign-keying one) is REFUSED. */
static const U1 CRADLE_BYTE_FP[MT_VOCAB_FP_LEN] = { 'B','Y','T','E','2','5','6','.' };

/* dtr.h redeclares the p-fs object-name width as CRADLE_REF_LEN (so it need not
 * pull pfs_dag.h). Pin it equal to the real PFS_NAME_MAX here, where pfs_dag.h
 * IS in scope — a future PFS_NAME_MAX change then fails to compile rather than
 * silently truncating the lesson body ref. */
_Static_assert(CRADLE_REF_LEN == PFS_NAME_MAX,
               "CRADLE_REF_LEN must track PFS_NAME_MAX");

/* ------------------------------------------------------------------ */
/* topic reservation (boot)                                            */
/* ------------------------------------------------------------------ */
static W   ct_topic_open = 0;
static W   ct_sub_h = -1;     /* the boot poll handle (reused by the task)    */
static W   ct_pub_h = -1;     /* lazily-opened publish handle                 */

void cradle_net_open(void)
{
    if (ct_topic_open) return;
    W h = kdds_open_poll_scoped(CRADLE_TEACH_TOPIC, KDDS_QOS_LATEST_ONLY,
                                KDDS_SCOPE_REGION);
    if (h >= 0) { ct_topic_open = 1; ct_sub_h = h; }
}

static W ct_pub_handle(void)
{
    if (ct_pub_h < 0)
        ct_pub_h = kdds_open_poll_scoped(CRADLE_TEACH_TOPIC,
                                         KDDS_QOS_LATEST_ONLY,
                                         KDDS_SCOPE_REGION);
    return ct_pub_h;
}

/* ------------------------------------------------------------------ */
/* teacher side — emit ONE lesson                                      */
/* ------------------------------------------------------------------ */

/* build the body ref "ct/<teacher>/<seq8>" (<= PFS_NAME_MAX = 16). seq is
 * the per-teacher monotonic high-water (LATEST_ONLY: a fresh ref per seq lets
 * a late-joining child fetch the newest). */
static UW ct_body_ref(U1 *out, UB teacher, UW seq)
{
    UW k = 0;
    out[k++] = 'c'; out[k++] = 't'; out[k++] = '/';
    if (teacher >= 10) out[k++] = (U1)('0' + (teacher / 10) % 10);
    out[k++] = (U1)('0' + teacher % 10);
    out[k++] = '/';
    /* up-to-8-digit seq, ascending emit, no leading zeros. */
    U1 tmp[10]; INT t = 0;
    if (seq == 0) tmp[t++] = '0';
    while (seq > 0 && t < 10 && k + (UW)t < PFS_NAME_MAX) {
        tmp[t++] = (U1)('0' + seq % 10); seq /= 10;
    }
    while (t > 0 && k < PFS_NAME_MAX) out[k++] = tmp[--t];
    return k;
}

/* per-teacher lesson high-water (this node's own, when it is the teacher). */
static UW ct_emit_seq = 0;
/* file-static scratch — NEVER a task-stack local (the hosted-relay stack
 * lesson). cradle_teach_emit is driven from the teacher's shell/DMN, serialized. */
static CRADLE_TEACH_PKT ct_pub_pkt;

INT cradle_teach_emit(const UB *body, UW len)
{
    if (!body || len == 0 || len > CT_LESSON_MAX) return 0;
    if (drpc_my_node == 0xFF) return 0;             /* solo: no one to teach    */
    /* ONLY the NOCENTRAL-elected region teacher emits (T-fix-a). Every node
     * recomputes region_teacher() locally; a non-teacher stays silent. */
    if (region_teacher() != drpc_my_node) return 0;
    if (ct_pub_handle() < 0) return 0;

    UW seq = ++ct_emit_seq;
    U1 ref[PFS_NAME_MAX]; UW rl = ct_body_ref(ref, drpc_my_node, seq);
    if (pfs_dag_save(ref, rl, body, len) != PFS_OK) { ct_emit_seq--; return 0; }

    ct_pub_pkt.magic        = CRADLE_MAGIC;
    ct_pub_pkt.teacher_node = drpc_my_node;
    ct_pub_pkt.fmt          = LESSON_FMT_BYTE;
    ct_pub_pkt._pad0 = ct_pub_pkt._pad1 = 0;
    ct_pub_pkt.seq          = seq;
    ct_pub_pkt.body_len     = len;
    for (UW i = 0; i < PFS_NAME_MAX; i++) ct_pub_pkt.body_ref[i] = (i < rl) ? ref[i] : 0;
    for (UW i = 0; i < MT_VOCAB_FP_LEN; i++) ct_pub_pkt.vocab_fp[i] = CRADLE_BYTE_FP[i];

    (void)kdds_pub(ct_pub_h, &ct_pub_pkt, (W)sizeof ct_pub_pkt);
    return 1;
}

/* ------------------------------------------------------------------ */
/* student side — poll + pull (the strong cradle_poll_and_pull)        */
/* ------------------------------------------------------------------ */

/* per-teacher seen high-water: ingest a lesson only ONCE per (teacher,seq). */
static UW ct_seen_hw[DNODE_MAX];
static UB ct_hw_init = 0;
/* file-static rx / body scratch — never the task stack. */
static CRADLE_TEACH_PKT ct_rx_pkt;
static UB ct_body_buf[CT_LESSON_MAX];

/* 1 iff fp matches the byte-student fingerprint. */
static INT ct_fp_ok(const U1 fp[MT_VOCAB_FP_LEN])
{
    for (INT i = 0; i < MT_VOCAB_FP_LEN; i++)
        if (fp[i] != CRADLE_BYTE_FP[i]) return 0;
    return 1;
}

/* STRONG override of student_shell.c's weak cradle_poll_and_pull. Pull a newer
 * lesson into the student's ring; NO-OP-safe at every failure (no beacon, own
 * echo, stale seq, fp mismatch, body absent) — the ring is simply left as-is so
 * window() reads the fixture. Returns nothing (the DMN tick narrates). */
void cradle_poll_and_pull(void)
{
    if (!ct_hw_init) {
        for (INT i = 0; i < DNODE_MAX; i++) ct_seen_hw[i] = 0;
        ct_hw_init = 1;
    }
    if (drpc_my_node == 0xFF) return;       /* solo: no mesh to pull from      */
    if (!ct_topic_open) cradle_net_open();
    if (ct_sub_h < 0) return;

    /* LATEST_ONLY poll (timeout=0): get the newest beacon, if any. */
    W r = kdds_sub(ct_sub_h, &ct_rx_pkt, (W)sizeof ct_rx_pkt, 0);
    if (r < (W)sizeof ct_rx_pkt) return;    /* nothing new                     */
    if (ct_rx_pkt.magic != CRADLE_MAGIC) return;

    UB org = ct_rx_pkt.teacher_node;
    if (org == drpc_my_node) return;        /* own echo: don't self-teach      */
    if (org >= DNODE_MAX) return;
    if (ct_rx_pkt.seq <= ct_seen_hw[org]) return;   /* already ingested        */
    if (ct_rx_pkt.fmt != LESSON_FMT_BYTE) return;   /* soft reserved (DEFERRED) */
    if (!ct_fp_ok(ct_rx_pkt.vocab_fp)) return;      /* foreign keying: refuse  */

    UW blen = ct_rx_pkt.body_len;
    if (blen == 0 || blen > CT_LESSON_MAX) return;

    /* pull the body by its content ref. */
    U1 ref[PFS_NAME_MAX];
    for (UW i = 0; i < PFS_NAME_MAX; i++) ref[i] = ct_rx_pkt.body_ref[i];
    UW rl = 0; while (rl < PFS_NAME_MAX && ref[rl]) rl++;
    INT got = pfs_dag_read(ref, rl, ct_body_buf, CT_LESSON_MAX);
    if (got != (INT)blen) return;           /* body not (yet) local: retry later */

    /* feed the lesson into the student's ring (refuses too-small without
     * truncating; the ring then drives the next sleep's windows). */
    if (cradle_lesson_ingest(ct_body_buf, (int)blen) > 0)
        ct_seen_hw[org] = ct_rx_pkt.seq;    /* advance only on a real ingest    */
}

/* ------------------------------------------------------------------ */
/* the student subscriber TASK (created in both hosted usermains)      */
/* ------------------------------------------------------------------ */
#define CRADLE_POLL_MS  500

void cradle_net_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    cradle_net_open();
    for (;;) {
        /* drive the pull on its own task cadence too (belt-and-suspenders with
         * the DMN-tick pull): a node whose baby is asleep between sleeps still
         * stages the newest lesson into the ring. cradle_poll_and_pull is the
         * SAME function the DMN tick calls — one transport, no duplicate. */
        cradle_poll_and_pull();
        tk_dly_tsk(CRADLE_POLL_MS);
    }
}
