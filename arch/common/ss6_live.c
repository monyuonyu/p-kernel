/*
 *  ss6_live.c — SS-6 LIVE remote-expert transport (wave-ss6-live).
 *
 *  Cashes the SS-6 [live] row: a REAL relay-backed remote-expert call. When
 *  student.c's MoE forward fires a WIDE expert (chosen-slot j >= K_min) that
 *  SS-5 placement says a PEER owns, this transport ships the [D] f_in vector to
 *  that owner over the mesh (UDP REQ on SS6L_PORT, mirroring drpc.c's pending-
 *  table + reply-semaphore + hard timeout), the owner runs the EXACT
 *  st_expert_forward_ref on its byte-identical resident model and replies the
 *  [D] output, and student.c sums it in the SAME canonical ascending-slot order.
 *  -> a multi-process cross-node forward == the single-node forward bit-for-bit.
 *  A killed/absent owner times out -> student.c recomputes that expert LOCALLY
 *  (honest degraded). The math (canonical sum, fallback) lives in student.c and
 *  is UNCHANGED; this file is ONLY the wire.
 *
 *  INVARIANTS: canonical reduction order untouched (student.c owns the sum);
 *  rw[]/gl_merge/kv_step untouched; NOCENTRAL (owner = HRW placement, no vote);
 *  no VLA (all packet/scratch are fixed ST_*_MAX); the hook fail-closes off so
 *  the single-node path is byte-unchanged when REMOTE_EXPERTS is not enabled.
 */
#include "ss6_live.h"
#include "student.h"   /* st_model, st_expert_forward_ref, st_set_remote_expert */
#include "placement.h"
#include "drpc.h"
#include "region.h"
#include "degrade.h"
#include "netstack.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helper                                                       */
/* ------------------------------------------------------------------ */
IMPORT void sio_send_frame(const UB *buf, INT size);
static void sl_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}
static void sl_putdec(UW v)
{
    char b[12]; INT i = 11; b[i] = 0;
    if (v == 0) { sl_puts("0"); return; }
    while (v > 0 && i > 0) { b[--i] = (char)('0' + v % 10); v /= 10; }
    sl_puts(&b[i]);
}

/* ------------------------------------------------------------------ */
/* wire packets — carry the [D] float vector (NOT KDDS: > KDDS_DATA_MAX) */
/* ------------------------------------------------------------------ */
/* A request/reply each carry up to ST_D_MAX floats. The on-wire size is fixed
 * (ST_D_MAX) so there is NO VLA and the dedup/pending machine is trivial; only
 * the first `d` entries are meaningful. ST_D_MAX(256)*4 = 1 KB payload + header,
 * comfortably inside a UDP datagram on the mesh. */
typedef struct {
    UW    magic;        /* SS6L_REQ_MAGIC (v2)                             */
    UH    seq;          /* request id (echoed in the reply)                */
    UB    src_node;     /* requester node id                               */
    UB    dst_node;     /* owner node id (SS-5 placement)                  */
    UH    layer;        /* transformer layer                               */
    UH    expert_id;    /* global expert id                                */
    UH    d;            /* model d (== student d_model)                    */
    UH    dff;          /* per-expert SwiGLU hidden (echoed, not used)     */
    /* --- SS6L v2 (DMOE-A §6.3): the version pin + core-epoch + flags --- */
    UW    ver_lo;       /* low 32 bits of the expert version pin (§2.3)    */
    UW    ver_hi;       /* high 32 bits                                    */
    UB    core_epoch;   /* the core-epoch the requester expects            */
    UB    flags;        /* SS6L_FLAG_* (BANK marks a DMOE bank expert)     */
    UH    _rsvd;        /* reserved (keep 4-byte alignment of fin)         */
    float fin[ST_D_MAX];/* the rmsnorm'd [D] input vector                  */
} __attribute__((packed)) SS6L_REQ;

typedef struct {
    UW    magic;        /* SS6L_REP_MAGIC (v2)                             */
    UH    seq;          /* echoes the request seq                          */
    UB    src_node;     /* responder (owner) node id                       */
    UB    status;       /* 0 = OK (eo valid), !=0 = refused                */
    UH    d;            /* number of valid eo entries                      */
    UB    refuse_reason;/* SS6L_REFUSE_* (0 == OK; VERSKEW on pin mismatch) */
    UB    _pad;
    float eo[ST_D_MAX]; /* the [D] expert down-projection output           */
} __attribute__((packed)) SS6L_REP;

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
static st_model *sl_model   = NULL;   /* resident model (responder side)   */
static int       sl_bound   = 0;      /* SS6L_PORT bound                    */
static int       sl_enabled = 0;      /* PKERNEL_REMOTE_EXPERTS=1           */
static unsigned  sl_sent    = 0;      /* remote requests sent (requester)  */
static unsigned  sl_served  = 0;      /* remote requests served (responder)*/

/* pending table (mirror drpc.c) — a forward fires experts one at a time, but
 * make room for a few in flight to be safe. Reply wakes the matching slot. */
#define SL_PENDING  4
typedef struct {
    UH       seq;
    UB       dst_node;
    UB       in_use;    /* set LAST on write, read FIRST on check          */
    UB       got;       /* reply arrived                                   */
    ID       sem;
    SS6L_REP rep;       /* the responder's reply (copied on wake)          */
} SL_PEND;
static SL_PEND sl_pend[SL_PENDING];
static UH      sl_seq_ctr = 1;        /* never 0                            */

/* node n IP — same deterministic addressing drpc.c uses (10.1.0.(n+1)). */
static UW sl_node_ip(UB n)
{
    return ((UW)(n + 1) << 24) | (net_my_ip & 0x00FFFFFFUL);
}

/* ------------------------------------------------------------------ */
/* responder: serve a remote-expert request                           */
/* ------------------------------------------------------------------ */
static void sl_serve(UW src_ip, const SS6L_REQ *rq)
{
    SS6L_REP rep;
    UB *rb = (UB *)&rep;
    for (INT z = 0; z < (INT)sizeof rep; z++) rb[z] = 0;
    rep.magic    = SS6L_REP_MAGIC;
    rep.seq      = rq->seq;
    rep.src_node = drpc_my_node;
    rep.status   = 1;            /* refused unless we fill eo below          */
    rep.refuse_reason = SS6L_REFUSE_ABSENT;   /* SS6L v2: honest refuse code   */
    rep.d        = 0;

    /* SS6L v2: a floor expert (flags & SS6L_FLAG_BANK == 0) is served exactly as
     * SS-6 did (no pin — every node holds an identical whole model). A BANK
     * expert carries the ver pin; the DMOE bank responder (dmoe_bank.c) enforces
     * SS6L_REFUSE_VERSKEW on the requester's ver_lo/ver_hi (§2.3). The wire is
     * v2-ready; the bank-serve install is the production follow-up. */
    if (sl_model && rq->d > 0 && rq->d <= ST_D_MAX && rq->dst_node == drpc_my_node) {
        /* the owner runs the EXACT per-expert SwiGLU (one math) so its [D]
         * output is bit-identical to the requester's local MoE branch. Copy
         * through ALIGNED scratch (the wire structs are packed, so taking the
         * address of their float members would be unaligned on aarch64). */
        static float fin_aln[ST_D_MAX];
        static float eo_aln[ST_D_MAX];
        for (int i = 0; i < (int)rq->d; i++) fin_aln[i] = rq->fin[i];
        if (st_expert_forward_ref(sl_model, (int)rq->layer, (int)rq->expert_id,
                                  fin_aln, eo_aln) == ST_OK) {
            for (int i = 0; i < (int)rq->d; i++) rep.eo[i] = eo_aln[i];
            rep.status        = 0;
            rep.refuse_reason = SS6L_REFUSE_NONE;
            rep.d             = rq->d;
            sl_served++;
        }
    }
    udp_send(src_ip, SS6L_PORT, SS6L_PORT, (const UB *)&rep, (UH)sizeof rep);
}

/* ------------------------------------------------------------------ */
/* UDP rx callback                                                     */
/* ------------------------------------------------------------------ */
void ss6_live_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_port;
    if (len < (UH)sizeof(UW)) return;
    UW magic = *(const UW *)data;

    if (magic == SS6L_REQ_MAGIC) {
        if (len < (UH)sizeof(SS6L_REQ)) return;
        sl_serve(src_ip, (const SS6L_REQ *)data);
        return;
    }
    if (magic == SS6L_REP_MAGIC) {
        if (len < (UH)sizeof(SS6L_REP)) return;
        const SS6L_REP *rp = (const SS6L_REP *)data;
        for (INT i = 0; i < SL_PENDING; i++) {
            if (!sl_pend[i].in_use) continue;       /* in_use read first   */
            if (sl_pend[i].seq != rp->seq) continue;
            sl_pend[i].rep = *rp;
            sl_pend[i].got = 1;
            tk_sig_sem(sl_pend[i].sem, 1);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* requester transport fn (installed as st_remote_expert_fn)           */
/* ------------------------------------------------------------------ */
/* Hard timeout per the SS-6 contract: a few short waits, then give up so
 * student.c recomputes LOCALLY (honest degraded — width lost, not correctness).
 * Bounded so a killed/absent owner NEVER stalls the forward. */
#define SL_WAIT_MS   200
#define SL_RETRIES   3      /* 3 x 200ms = 600ms worst case before fallback */

static int ss6_live_remote_expert(int layer, int expert_id, const float *fin,
                                  int d, int dff, float *out, void *ctx)
{
    (void)ctx;
    if (!sl_bound || d <= 0 || d > ST_D_MAX) return -1;

    /* SS-5 placement: who OWNS this expert (HRW, NOCENTRAL — no vote). */
    INT owner = st_expert_owner((UB)expert_id);
    if (owner < 0 || (UB)owner == drpc_my_node) return -1;   /* local/none */
    if ((UB)owner >= DNODE_MAX) return -1;
    if (dnode_table[owner].state == DNODE_DEAD) return -1;    /* absent     */

    /* claim a pending slot */
    INT slot = -1;
    for (INT i = 0; i < SL_PENDING; i++) if (!sl_pend[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;

    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    ID sem = tk_cre_sem(&cs);
    if (sem < E_OK) return -1;

    UH seq = sl_seq_ctr++;
    if (sl_seq_ctr == 0) sl_seq_ctr = 1;
    sl_pend[slot].seq      = seq;
    sl_pend[slot].dst_node = (UB)owner;
    sl_pend[slot].got      = 0;
    sl_pend[slot].sem      = sem;
    sl_pend[slot].in_use   = 1;          /* visible to ss6_live_rx from here */

    /* build the request (fin padded to ST_D_MAX is fine — only d used). */
    static SS6L_REQ rq;                  /* file-static: 1KB off the stack   */
    UB *qb = (UB *)&rq;
    for (INT z = 0; z < (INT)sizeof rq; z++) qb[z] = 0;
    rq.magic     = SS6L_REQ_MAGIC;
    rq.seq       = seq;
    rq.src_node  = drpc_my_node;
    rq.dst_node  = (UB)owner;
    rq.layer     = (UH)layer;
    rq.expert_id = (UH)expert_id;
    rq.d         = (UH)d;
    rq.dff       = (UH)dff;
    for (int i = 0; i < d; i++) rq.fin[i] = fin[i];

    UW dst_ip = sl_node_ip((UB)owner);
    sl_sent++;

    int ok = 0;
    for (INT retry = 0; retry < SL_RETRIES; retry++) {
        udp_send(dst_ip, SS6L_PORT, SS6L_PORT, (const UB *)&rq, (UH)sizeof rq);
        ER er = tk_wai_sem(sem, 1, SL_WAIT_MS);
        if (er == E_OK && sl_pend[slot].got && sl_pend[slot].rep.status == 0
            && sl_pend[slot].rep.d == (UH)d) {
            for (int i = 0; i < d; i++) out[i] = sl_pend[slot].rep.eo[i];
            ok = 1;
            break;
        }
        if (dnode_table[owner].state == DNODE_DEAD) break;   /* died -> fall back */
    }

    sl_pend[slot].in_use = 0;
    sl_pend[slot].seq    = 0;
    tk_del_sem(sem);
    return ok ? 0 : -1;          /* -1 -> student.c recomputes LOCALLY      */
}

/* ------------------------------------------------------------------ */
/* gate (installed as st_remote_gate_fn)                               */
/* ------------------------------------------------------------------ */
/* Fail-closed predicate. Mirrors the documented production rule:
 *   j >= K_min AND !st_expert_is_local(expert_id) AND degrade==FULL
 *   AND region_size() >= 2 AND PKERNEL_REMOTE_EXPERTS=1.
 * The local K_min experts ALWAYS stay local; only the WIDENED experts a PEER
 * owns are eligible. Off by default -> single-node byte-unchanged. */
static int ss6_live_gate(int layer, int j, int kmin, int expert_id, void *ctx)
{
    (void)layer; (void)ctx;
    if (!sl_enabled) return 0;
    if (j < kmin) return 0;                       /* never the floor experts */
    if (degrade_level() != DEGRADE_FULL) return 0;
    if (region_size() < 2) return 0;
    if (st_expert_is_local((UB)expert_id)) return 0;   /* I own it -> local  */
    return 1;
}

/* ------------------------------------------------------------------ */
/* install / uninstall                                                 */
/* ------------------------------------------------------------------ */
void ss6_live_install(void *mp)
{
    st_model *m = (st_model *)mp;
    sl_model = m;
    /* Bind SS6L_PORT only once the netstack/drpc is up (drpc_my_node set). On a
     * standalone node (no `net`/autonet) udp_bind has no bound socket to attach
     * to; skip it — the gate fail-closes so the forward stays single-node. */
    if (!sl_bound && drpc_my_node != 0xFF) {
        for (INT i = 0; i < SL_PENDING; i++) {
            sl_pend[i].in_use = 0; sl_pend[i].seq = 0;
        }
        udp_bind(SS6L_PORT, ss6_live_rx);
        sl_bound = 1;
        sl_puts("[ss6-live] bound port "); sl_putdec(SS6L_PORT);
        sl_puts("  remote_experts="); sl_puts(sl_enabled ? "ON" : "off");
        sl_puts("\r\n");
    }

    /* install the hook. */
    st_set_remote_expert(ss6_live_remote_expert, ss6_live_gate, NULL);
}

void ss6_live_uninstall(void)
{
    st_set_remote_expert(NULL, NULL, NULL);
}

/* Opt-in (PKERNEL_REMOTE_EXPERTS=1). The env is read on the LLM/host tier
 * (student_shell.c) where getenv is safe to call from the shell task; this
 * setter just records it. Off by default -> gate fail-closes -> single-node
 * byte-unchanged. */
void ss6_live_set_enabled(int on) { sl_enabled = on ? 1 : 0; }

unsigned ss6_live_req_sent(void)   { return sl_sent;   }
unsigned ss6_live_req_served(void) { return sl_served; }
