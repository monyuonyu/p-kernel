/*
 *  pfs_repl.c — p-fs P1: region-scoped gossip replication of blocks.
 *
 *  Spec: docs/architecture/20-architecture/p-fs.md §2.2 / §3.2 / §5 (P1 row).
 *  Design notes + wire formats: pfs_repl.h.
 *
 *  What is generalized from sfs.c (the spec's "closest existing asset"):
 *    - 512B in-order chunk transfer over a private pmesh UDP port
 *      (sfs FILE_START/FILE_CHUNK -> self-describing PFSR_BLK_PKT)
 *    - boot sync (sfs SYNC_REQ "send me everything" -> PFSR SYNC)
 *    - single-slot receive state machine (one block in flight; an
 *      interrupted transfer self-heals via the WANT retry)
 *  What is new vs sfs.c:
 *    - scope: ANY block, not just /shared files
 *    - delivery: announce/want control plane on REGION-scoped K-DDS
 *      topics instead of unconditional all-to-all pushes
 *    - integrity: the receiver re-hashes the assembled bytes and
 *      verifies them against the block-id before storing
 *    - dedup: pfs_put_origin() by content, so duplicate responders /
 *      re-deliveries are naturally idempotent
 *  Not generalized yet (honest): tombstones — blocks are immutable and
 *  P1 has no delete; that returns with the ref layer (P2).
 *
 *  Stack discipline (feedback_hosted_relay_stack_overflow): all
 *  per-packet / per-block scratch (564B chunk pkt, 4KB assembly and
 *  send buffers) is static, never a task-stack local.
 */

#include "pfs_repl.h"
#include "pfs_block.h"
#include "kdds.h"
#include "pmesh.h"
#include "drpc.h"
#include "region.h"
#include "pk_node_row.h"
#include "pk_nosw.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* wire-image guards (LP64 / cross-ABI: feedback_lp64_typedef_trap)    */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(UW) == 4 && sizeof(UH) == 2 && sizeof(U1) == 1,
               "pfs_repl wire types must be fixed-width on this ABI");
_Static_assert(sizeof(PFSR_ANN_PKT)  == 48,  "ANN pkt must be 48 bytes");
_Static_assert(sizeof(PFSR_WANT_PKT) == 44,  "WANT pkt must be 44 bytes");
_Static_assert(sizeof(PFSR_SYNC_PKT) == 12,  "SYNC pkt must be 12 bytes");
_Static_assert(sizeof(PFSR_BLK_PKT)  == 52 + PFSR_CHUNK_SIZE,
               "BLK pkt must be 564 bytes");
_Static_assert(sizeof(PFSR_HOLD_PKT) == 40, "HOLD pkt must be 40 bytes");
_Static_assert(sizeof(PFSR_ANN_PKT)  <= KDDS_DATA_MAX,
               "ANN must fit a K-DDS payload");
_Static_assert(sizeof(PFSR_WANT_PKT) <= KDDS_DATA_MAX,
               "WANT must fit a K-DDS payload");
_Static_assert(sizeof(PFSR_SYNC_PKT) <= KDDS_DATA_MAX,
               "SYNC must fit a K-DDS payload");
_Static_assert(sizeof(PFSR_BLK_PKT)  <= PMESH_DATA_MAX,
               "BLK pkt must fit a pmesh DATA payload");
_Static_assert(PFS_BLOCK_MAX % PFSR_CHUNK_SIZE == 0,
               "block max must be a whole number of chunks");

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like sfs.c / kdds.c)             */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void pr_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void pr_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { pr_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    pr_puts(&buf[i]);
}

/* first 8 id bytes as 16 hex chars — enough to eyeball-match across nodes */
static void pr_put_id(const U1 id[PFS_ID_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    char out[2 * 8 + 1];
    INT j = 0;
    for (INT i = 0; i < 8; i++) {
        out[j++] = hexd[(id[i] >> 4) & 0xF];
        out[j++] = hexd[id[i] & 0xF];
    }
    out[j] = '\0';
    pr_puts(out);
}

/* ------------------------------------------------------------------ */
/* tiny libc-free helpers (arch/common rule: no <string.h>)            */
/* ------------------------------------------------------------------ */

static void pr_memcpy(void *dst, const void *src, UW n)
{
    U1 *d = (U1 *)dst; const U1 *s = (const U1 *)src;
    while (n--) *d++ = *s++;
}

static void pr_memset(void *dst, U1 v, UW n)
{
    U1 *d = (U1 *)dst;
    while (n--) *d++ = v;
}

static INT pr_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* module state                                                        */
/* ------------------------------------------------------------------ */

/* K-DDS handles: pub+sub per control topic. -1 = not open. */
static W h_ann_pub  = -1, h_ann_sub  = -1;
static W h_want_pub = -1, h_want_sub = -1;
static W h_sync_pub = -1, h_sync_sub = -1;

/* per-publisher sequence counters (start at 1; 0 == "nothing seen") */
static UW ann_seq, want_seq, sync_seq;

/* poll-side dedup: last seq acted on, per source node (hosted: per row,
 * pk_node_row.h, so a wide sender has one; bare metal: DNODE_ROWS == DNODE_MAX) */
static UW last_ann_seq [DNODE_ROWS];
static UW last_want_seq[DNODE_ROWS];
static UW last_sync_seq[DNODE_ROWS];

#ifdef _TK_HOSTED_LIBC_
/* N2 "まだ" 7, pfs_repl (research/n2-wire16.md, 2026-11-09). A wide id (> 0xFF) does
 * not fit the 1-byte src_node / origin of these packets: the byte says 0xFF and the
 * id rides 2 bytes LE after the packet, as mind/teach and mind/want do. Old nodes
 * and bare metal read the packet only (kdds_sub cuts, pmesh hands the length) and
 * drop 0xFF as before. A new node takes the tail only when the byte is 0xFF and the
 * tail is a wide id. Before, every node dropped a wide sender's ANNOUNCE, WANT and
 * SYNC (src_node 0xFF), so no block crossed between wide and narrow, and a block's
 * wide origin became 0xFF ("self") on its first hop. Hosted only; the HOLD-ACK and
 * the chunks name their sender by pmesh's src (v2 carries the wide id).
 * -DN2_NC_PFS_NO_TAIL: never send a tail (the negative control of [n2-mix] XP). */
typedef struct { PFSR_ANN_PKT  p; U1 src_lo, src_hi, org_lo, org_hi; } __attribute__((packed)) PFSR_ANN_PKT_W;
typedef struct { PFSR_WANT_PKT p; U1 src_lo, src_hi; } __attribute__((packed)) PFSR_WANT_PKT_W;
typedef struct { PFSR_SYNC_PKT p; U1 src_lo, src_hi; } __attribute__((packed)) PFSR_SYNC_PKT_W;
typedef struct { PFSR_BLK_PKT  p; U1 org_lo, org_hi; } __attribute__((packed)) PFSR_BLK_PKT_W;
_Static_assert(sizeof(PFSR_ANN_PKT_W)  == 52 && sizeof(PFSR_ANN_PKT_W)  <= KDDS_DATA_MAX, "ANN + tail");
_Static_assert(sizeof(PFSR_WANT_PKT_W) == 46 && sizeof(PFSR_WANT_PKT_W) <= KDDS_DATA_MAX, "WANT + tail");
_Static_assert(sizeof(PFSR_SYNC_PKT_W) == 14 && sizeof(PFSR_SYNC_PKT_W) <= KDDS_DATA_MAX, "SYNC + tail");
_Static_assert(sizeof(PFSR_BLK_PKT_W)  == 566 && sizeof(PFSR_BLK_PKT_W) <= PMESH_V2_DATA_MAX, "BLK + tail");
#ifndef N2_NC_PFS_NO_TAIL
#define PW_SEND_TAIL 1
#else
#define PW_SEND_TAIL 0
#endif
static void pfsr_want_take(const PFSR_WANT_PKT *w, W r);
static void pw_put(U1 *lo, U1 *hi, pk_node_t id) { *lo = (U1)(id & 0xFF); *hi = (U1)(id >> 8); }
/* the id a received byte b stands for: the tail's wide id when b is 0xFF, else b */
static pk_node_t pw_get(pk_node8_t b, U1 lo, U1 hi, INT have_tail)
{
#ifdef N2_NC_PFS_RX_NARROW
    (void)lo; (void)hi; (void)have_tail;       /* negative control: the tail is never read */
    return (pk_node_t)b;
#endif
    if (b == PK_NODE_NONE && have_tail) {
        pk_node_t t = (pk_node_t)(lo | ((pk_node_t)hi << 8));
        if (PK_NODE_IS_WIDE(t)) return t;
    }
    return (pk_node_t)b;
}
/* the dedup row of a received sender, or -1 (self, not a node, no row left) */
static INT pw_row(pk_node_t src)
{
    if (src == drpc_my_node || src == PK_NODE_NONE) return -1;
    if (src >= DNODE_MAX && !PK_NODE_IS_WIDE(src)) return -1;
    return pk_node_row(src);
}
/* the bytes to send for packet p from sender me (and origin org): the packet, plus
 * the tail when an id in it is wide. Returns the length. */
static W pfsr_ann_frame(const PFSR_ANN_PKT *p, pk_node_t me, pk_node_t org, PFSR_ANN_PKT_W *out)
{
    out->p = *p;
    pw_put(&out->src_lo, &out->src_hi, me); pw_put(&out->org_lo, &out->org_hi, org);
    return (PW_SEND_TAIL && (PK_NODE_IS_WIDE(me) || PK_NODE_IS_WIDE(org))) ? (W)sizeof *out : (W)sizeof *p;
}
static W pfsr_want_frame(const PFSR_WANT_PKT *p, pk_node_t me, PFSR_WANT_PKT_W *out)
{
    out->p = *p; pw_put(&out->src_lo, &out->src_hi, me);
    return (PW_SEND_TAIL && PK_NODE_IS_WIDE(me)) ? (W)sizeof *out : (W)sizeof *p;
}
static W pfsr_sync_frame(const PFSR_SYNC_PKT *p, pk_node_t me, PFSR_SYNC_PKT_W *out)
{
    out->p = *p; pw_put(&out->src_lo, &out->src_hi, me);
    return (PW_SEND_TAIL && PK_NODE_IS_WIDE(me)) ? (W)sizeof *out : (W)sizeof *p;
}
#endif

/* pending WANTs: announced blocks we still lack (retry until they
 * arrive or the budget runs out — lost packets self-heal here). */
typedef struct {
    U1 id[PFS_ID_LEN];
    UW age_ms;                 /* since last WANT publish              */
    U1 tries;
    U1 active;
} PFSR_PENDING;
static PFSR_PENDING pending[PFSR_PENDING_MAX];
#if defined(_TK_HOSTED_LIBC_) && !defined(PFS_WANT_NO_OVF)
#define PFSR_WANT_OVF 256                      /* ids waiting for a free pending slot */
static U1 want_ovf[PFSR_WANT_OVF][PFS_ID_LEN];
static UW want_ovf_head, want_ovf_n;
#endif

/* single-slot receive assembly (sfs_rx_state generalized to a block).
 * Static — 4KB+ must never live on a task stack in a net path. */
static struct {
    U1 id[PFS_ID_LEN];
    UW total;
    UW received;
    UW next_chunk;
    pk_node_t origin;
    U1 active;
    U1 buf[PFS_BLOCK_MAX];
} rx_state;

/* static per-packet / per-block send scratch (same trap) */
#ifdef _TK_HOSTED_LIBC_
static PFSR_BLK_PKT_W tx_pkt_w;                /* + the wide origin's tail (N2) */
#define tx_pkt (tx_pkt_w.p)
#else
static PFSR_BLK_PKT tx_pkt;
#endif
static U1           tx_block[PFS_BLOCK_MAX];

/* stats (printed by events; counters kept for future `pfs stat`) */
static UW stat_ann_tx, stat_ann_rx, stat_want_tx, stat_want_rx;
static UW stat_blocks_tx, stat_blocks_rx, stat_hash_fail;
static UW stat_store_fail;   /* verified blocks the local store refused */

/* G28: protected-object layer hooks (protect.c). announce_hook fires for
 * every heard ANNOUNCE; announce_suppress mutes the ambient put-hook push so
 * the protect actuator can be the sole driver of a protected unit. */
static PFSR_ANN_HOOK announce_hook = 0;
static U1            announce_suppress = 0;
/* G28: gate for the boot-SYNC responder — non-zero return keeps a block out
 * of the stream (a quietly-held protected unit must not leak via ambient sync). */
static PFSR_SYNC_FILTER sync_filter = 0;

void pfs_repl_set_announce_hook(PFSR_ANN_HOOK fn) { announce_hook = fn; }
void pfs_repl_set_announce_suppress(INT on) { announce_suppress = on ? 1 : 0; }
void pfs_repl_set_sync_filter(PFSR_SYNC_FILTER fn) { sync_filter = fn; }

/* ------------------------------------------------------------------ */
/* control-plane publishes (small pkts — fine on stack)                */
/* ------------------------------------------------------------------ */

#if defined(_TK_HOSTED_LIBC_) && !defined(PFSR_NC_ANN_UNPACED)
/* PFS-REPL-WANT-DROP, the 0 ms burst (2026-11-09): a put sent its ANNOUNCE at once,
 * so 60 puts in a row put 60 announces on the wire within a few ms; the receiver's
 * K-DDS kept queue (KDDS_Q_N 32, shared by every topic) is read 8 per 50 ms tick
 * (pfsr_ann_drain) and lost 20 of them (kdds_queue_dropped; B held 40 of 60, no
 * want-table line). Hosted: an announce goes into an outgoing ring and pfs_repl_task
 * sends at most PFSR_ANN_PACE (8) per tick, below the rate a receiver reads. A repeat of
 * a queued id is not queued twice. A full ring sends at once and says so (the old
 * way). Bare metal unchanged (crown). -DPFSR_NC_ANN_UNPACED: the old immediate send
 * (the negative control of run_pfs_burst.sh at PFS_BURST_GAP=0). */
#define PFSR_ANN_PACED 1
#ifndef PFSR_ANN_PACE
#define PFSR_ANN_PACE 8                       /* announces sent per 50 ms tick */
#endif
#define PFSR_ANN_OUT 256
static struct { U1 id[PFS_ID_LEN]; UW len; pk_node_t origin; } ann_out[PFSR_ANN_OUT];
static UW ann_out_head, ann_out_n, ann_out_full;
static void publish_announce_now(const U1 id[PFS_ID_LEN], UW len, pk_node_t origin);
static void publish_announce(const U1 id[PFS_ID_LEN], UW len, pk_node_t origin)
{
    if (drpc_my_node == 0xFF || h_ann_pub < 0) return;
    PK_NOSW_ENTER();
    for (UW i = 0; i < ann_out_n; i++)
        if (pr_id_eq(ann_out[(ann_out_head + i) % PFSR_ANN_OUT].id, id)) { PK_NOSW_LEAVE(); return; }
    if (ann_out_n < PFSR_ANN_OUT) {
        UW k = (ann_out_head + ann_out_n) % PFSR_ANN_OUT;
        pr_memcpy(ann_out[k].id, id, PFS_ID_LEN);
        ann_out[k].len = len; ann_out[k].origin = origin;
        ann_out_n++;
        PK_NOSW_LEAVE();
        return;
    }
    UW nf = ann_out_full++;
    PK_NOSW_LEAVE();
    if ((nf & 31u) == 0) {
        pr_puts("[pfs] announce queue full, sent at once (so far "); pr_putdec(nf + 1); pr_puts(")\r\n");
    }
    publish_announce_now(id, len, origin);
}
/* pfs_repl_task, once per tick: send up to PFSR_ANN_PACE queued announces */
static void pfsr_ann_flush(UW max)
{
    for (UW k = 0; k < max; k++) {
        U1 id[PFS_ID_LEN]; UW len; pk_node_t org;
        PK_NOSW_ENTER();
        if (ann_out_n == 0) { PK_NOSW_LEAVE(); return; }
        pr_memcpy(id, ann_out[ann_out_head].id, PFS_ID_LEN);
        len = ann_out[ann_out_head].len; org = ann_out[ann_out_head].origin;
        ann_out_head = (ann_out_head + 1) % PFSR_ANN_OUT; ann_out_n--;
        PK_NOSW_LEAVE();
        publish_announce_now(id, len, org);
    }
}
#else
#define publish_announce_now publish_announce
#endif

static void publish_announce_now(const U1 id[PFS_ID_LEN], UW len, pk_node_t origin)
{
    if (drpc_my_node == 0xFF || h_ann_pub < 0) return;

    PFSR_ANN_PKT a;
    pr_memset(&a, 0, (UW)sizeof(a));
    a.magic    = PFSR_ANN_MAGIC;
    a.seq      = ++ann_seq;
    a.src_node = pk_node_v1(drpc_my_node);
    a.origin   = pk_node_v1(origin);
    a.len      = len;
    pr_memcpy(a.id, id, PFS_ID_LEN);
#ifdef _TK_HOSTED_LIBC_
    {
        PFSR_ANN_PKT_W aw;
        W n = pfsr_ann_frame(&a, drpc_my_node, origin, &aw);
        kdds_pub(h_ann_pub, &aw, n);
    }
#else
    kdds_pub(h_ann_pub, &a, (W)sizeof(a));
#endif
    stat_ann_tx++;

    pr_puts("[pfs] announce id="); pr_put_id(id);
    pr_puts("  len="); pr_putdec(len);
    pr_puts("  fanout="); pr_putdec(kdds_pub_fanout());
    pr_puts("\r\n");
}

static void publish_want(const U1 id[PFS_ID_LEN])
{
    if (drpc_my_node == 0xFF || h_want_pub < 0) return;

    PFSR_WANT_PKT w;
    pr_memset(&w, 0, (UW)sizeof(w));
    w.magic    = PFSR_WANT_MAGIC;
    w.seq      = ++want_seq;
    w.src_node = pk_node_v1(drpc_my_node);
    pr_memcpy(w.id, id, PFS_ID_LEN);
#ifdef _TK_HOSTED_LIBC_
    {
        PFSR_WANT_PKT_W ww;
        W n = pfsr_want_frame(&w, drpc_my_node, &ww);
        kdds_pub(h_want_pub, &ww, n);
    }
#else
    kdds_pub(h_want_pub, &w, (W)sizeof(w));
#endif
    stat_want_tx++;
}

static void publish_sync(void)
{
    if (drpc_my_node == 0xFF || h_sync_pub < 0) return;

    PFSR_SYNC_PKT s;
    pr_memset(&s, 0, (UW)sizeof(s));
    s.magic    = PFSR_SYNC_MAGIC;
    s.seq      = ++sync_seq;
    s.src_node = pk_node_v1(drpc_my_node);
#ifdef _TK_HOSTED_LIBC_
    {
        PFSR_SYNC_PKT_W sw;
        W n = pfsr_sync_frame(&s, drpc_my_node, &sw);
        kdds_pub(h_sync_pub, &sw, n);
    }
#else
    kdds_pub(h_sync_pub, &s, (W)sizeof(s));
#endif

    pr_puts("[pfs] boot sync request (region, fanout=");
    pr_putdec(kdds_pub_fanout());
    pr_puts(")\r\n");
}

/* ------------------------------------------------------------------ */
/* put-hook: a NEW local store becomes a region announce               */
/* (save == publish, p-fs.md §3.2 — symmetric on every node, so a      */
/* replica that just stored a received block re-announces it onward)   */
/* ------------------------------------------------------------------ */

static void on_new_block(const U1 id[PFS_ID_LEN], UW len, pk_node_t origin)
{
    if (drpc_my_node == 0xFF) return;       /* not distributed: local only */
    if (announce_suppress) return;          /* G28: protected quiet put     */
#if defined(_TK_HOSTED_LIBC_) && !defined(ARK_REC_SHARED)
    {   /* ark-entry.md 2.5: the ark's records stay here (see send_block_to) */
        U1 head[4];
        if (len >= 4 && pfs_get(id, head, 4) >= 4 && head[0] == 'A' && head[1] == 'K' && head[2] == 'R') return;
    }
#endif
    pk_node_t org = (origin == PFS_ORIGIN_SELF) ? drpc_my_node : origin;
    publish_announce(id, len, org);
}

/* G35: unicast "I durably hold your block" straight to the origin. Point-to-
 * point, so distinct holders never clobber each other (unlike the shared
 * LATEST_ONLY announce slot). Drives the origin's protect holder_count
 * reliably, so plural protection's grounded threat falls promptly. */
static void send_hold_ack(pk_node_t origin_node, const U1 id[PFS_ID_LEN])
{
    if (drpc_my_node == 0xFF) return;
#ifdef _TK_HOSTED_LIBC_
    if (origin_node >= DNODE_MAX && !PK_NODE_IS_WIDE(origin_node)) return;   /* N2: a wide origin is real */
#else
    if (origin_node >= DNODE_MAX) return;       /* not a real node (e.g. 0xFF) */
#endif
    if (origin_node == drpc_my_node) return;    /* never ack ourselves         */
    PFSR_HOLD_PKT h;
    pr_memset(&h, 0, (UW)sizeof(h));
    h.magic    = PFSR_HOLD_MAGIC;
    h.src_node = pk_node_v1(drpc_my_node);
    h.origin   = pk_node_v1(origin_node);
    pr_memcpy(h.id, id, PFS_ID_LEN);
    pmesh_send(origin_node, PFSR_PORT, (const UB *)&h, (UH)sizeof(h));
}

/* G28 actuator entry: re-announce a block we hold to drive replication. */
void pfs_repl_reannounce(const U1 id[PFS_ID_LEN])
{
    if (drpc_my_node == 0xFF) return;
    if (!pfs_has(id)) return;
    /* recover the block's length + origin tag from the local table */
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        U1 sid[PFS_ID_LEN]; UW slen; pk_node_t sorg;
        if (pfs_slot_info(i, sid, &slen, &sorg) && pr_id_eq(sid, id)) {
            pk_node_t org = (sorg == PFS_ORIGIN_SELF) ? drpc_my_node : sorg;
            publish_announce(id, slen, org);
            return;
        }
    }
#ifdef _TK_HOSTED_LIBC_
    /* ark-entry.md 5 (2026-11-09): a block held only in the ARK image (evicted from
     * the table) is announced too, as this node's (`ark rec sync`) */
    {
        static U1 reann_buf[PFS_BLOCK_MAX];   /* not tx_block: the repl task sends from it */
        INT n = pfs_get(id, reann_buf, PFS_BLOCK_MAX);
        if (n >= 0) publish_announce(id, (UW)n, drpc_my_node);
    }
#endif
}

static void send_block_to(pk_node_t dst_node, const U1 id[PFS_ID_LEN]);

/* G35: public direct push — unicast a held block straight to one neighbour
 * (reliable discovery for the protect actuator; see send_block_to below). */
void pfs_repl_push(const U1 id[PFS_ID_LEN], pk_node_t dst_node)
{
    if (drpc_my_node == 0xFF) return;
#ifdef _TK_HOSTED_LIBC_
    if ((dst_node >= DNODE_MAX && !PK_NODE_IS_WIDE(dst_node)) || dst_node == drpc_my_node) return;
#else
    if (dst_node >= DNODE_MAX || dst_node == drpc_my_node) return;
#endif
    if (!pfs_has(id)) return;
    send_block_to(dst_node, id);
}

/* ------------------------------------------------------------------ */
/* chunked block send (sfs_push generalized to a block, unicast)       */
/* ------------------------------------------------------------------ */

static void send_block_to(pk_node_t dst_node, const U1 id[PFS_ID_LEN])
{
    INT blen = pfs_get(id, tx_block, PFS_BLOCK_MAX);
    if (blen < 0 || blen > (INT)PFS_BLOCK_MAX) return;
#if defined(_TK_HOSTED_LIBC_) && !defined(ARK_REC_SHARED)
    /* ark-entry.md 2.5 (2026-11-09): the ark's records ("AKR" + R/F/I/D, ark_rec.c)
     * do not leave the node in the clear - not to a WANT, a boot SYNC or a push -
     * until they are sealed first (E3). ark_rec.c also mutes their announce. */
    if (blen >= 4 && tx_block[0] == 'A' && tx_block[1] == 'K' && tx_block[2] == 'R') return;
#endif

    /* recover the origin tag so it survives the hop */
    pk_node_t origin = PFS_ORIGIN_SELF;
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        U1 sid[PFS_ID_LEN]; pk_node_t sorg;
        if (pfs_slot_info(i, sid, 0, &sorg) && pr_id_eq(sid, id)) {
            origin = sorg;
            break;
        }
    }

    pr_memset(&tx_pkt, 0, (UW)sizeof(tx_pkt));
    tx_pkt.magic     = PFSR_BLK_MAGIC;
    tx_pkt.version   = PFSR_VERSION;
    tx_pkt.type      = PFSR_BLK_CHUNK;
    tx_pkt.src_node  = pk_node_v1(drpc_my_node);
    tx_pkt.origin    = pk_node_v1(origin);
    tx_pkt.total_len = (UW)blen;
    pr_memcpy(tx_pkt.id, id, PFS_ID_LEN);
#ifdef _TK_HOSTED_LIBC_
    UH tx_len = (UH)sizeof(tx_pkt);
    if (PW_SEND_TAIL && PK_NODE_IS_WIDE(origin)) {     /* N2: the wide origin survives the hop */
        pw_put(&tx_pkt_w.org_lo, &tx_pkt_w.org_hi, origin);
        tx_len = (UH)sizeof(tx_pkt_w);
    }
#endif

    UW off = 0, idx = 0;
    do {                                   /* at least 1 chunk (len 0 ok) */
        UW n = (UW)blen - off;
        if (n > PFSR_CHUNK_SIZE) n = PFSR_CHUNK_SIZE;
        tx_pkt.chunk_idx = idx;
        tx_pkt.chunk_len = (UH)n;
        if (n) pr_memcpy(tx_pkt.data, tx_block + off, n);
#ifdef _TK_HOSTED_LIBC_
        pmesh_send(dst_node, PFSR_PORT, (const UB *)&tx_pkt_w, tx_len);
#else
        pmesh_send(dst_node, PFSR_PORT, (const UB *)&tx_pkt,
                   (UH)sizeof(tx_pkt));
#endif
        off += n;
        idx++;
    } while (off < (UW)blen);

    stat_blocks_tx++;
    pr_puts("[pfs] sent block id="); pr_put_id(id);
    pr_puts("  len="); pr_putdec((UW)blen);
    pr_puts("  to node "); pr_putdec(dst_node);
    pr_puts("  chunks="); pr_putdec(idx);
    pr_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* pending-WANT table                                                  */
/* ------------------------------------------------------------------ */

static void pending_add(const U1 id[PFS_ID_LEN])
{
    for (INT i = 0; i < PFSR_PENDING_MAX; i++)
        if (pending[i].active && pr_id_eq(pending[i].id, id))
            return;                            /* already chasing it */
    for (INT i = 0; i < PFSR_PENDING_MAX; i++) {
        if (!pending[i].active) {
            pr_memcpy(pending[i].id, id, PFS_ID_LEN);
            pending[i].age_ms = 0;
            pending[i].tries  = 1;
            pending[i].active = 1;
            publish_want(id);                  /* first WANT right away */
            return;
        }
    }
#if defined(_TK_HOSTED_LIBC_) && !defined(PFS_WANT_NO_OVF)
    /* gap-ledger PFS-REPL-WANT-DROP (2026-11-09): nothing re-announces, so a dropped
     * id was lost for good. Hosted: keep it in an overflow ring; pending_tick moves it
     * into the table when a slot frees. When the ring itself is full, say so. */
    for (UW i = 0; i < want_ovf_n; i++)
        if (pr_id_eq(want_ovf[(want_ovf_head + i) % PFSR_WANT_OVF], id)) return;
    if (want_ovf_n < PFSR_WANT_OVF) {
        pr_memcpy(want_ovf[(want_ovf_head + want_ovf_n) % PFSR_WANT_OVF], id, PFS_ID_LEN);
        want_ovf_n++;
        return;
    }
    pr_puts("[pfs] want table full, dropped id="); pr_put_id(id); pr_puts("\r\n");
#else
    /* table full: drop — the next re-announce will retry us */
#endif
}

#if defined(_TK_HOSTED_LIBC_) && !defined(PFS_WANT_NO_OVF)
static void want_ovf_refill(void)
{
    while (want_ovf_n) {
        INT free_slot = -1;
        for (INT i = 0; i < PFSR_PENDING_MAX; i++) if (!pending[i].active) { free_slot = i; break; }
        if (free_slot < 0) return;
        U1 *id = want_ovf[want_ovf_head];
        want_ovf_head = (want_ovf_head + 1) % PFSR_WANT_OVF; want_ovf_n--;
        if (pfs_has(id)) continue;
        pending_add(id);
    }
}
#endif

static void pending_tick(UW elapsed_ms)
{
    for (INT i = 0; i < PFSR_PENDING_MAX; i++) {
        if (!pending[i].active) continue;
        if (pfs_has(pending[i].id)) {          /* arrived — done */
            pending[i].active = 0;
            continue;
        }
        pending[i].age_ms += elapsed_ms;
        if (pending[i].age_ms < PFSR_WANT_RETRY_MS) continue;
        pending[i].age_ms = 0;
        if (pending[i].tries >= PFSR_WANT_TRIES) {
            pr_puts("[pfs] gave up on block id=");
            pr_put_id(pending[i].id); pr_puts("\r\n");
            pending[i].active = 0;
            continue;
        }
        pending[i].tries++;
        publish_want(pending[i].id);
    }
#if defined(_TK_HOSTED_LIBC_) && !defined(PFS_WANT_NO_OVF)
    want_ovf_refill();
#endif
}

/* ------------------------------------------------------------------ */
/* chunk receive (sfs_rx FILE_START/FILE_CHUNK generalized)            */
/* ------------------------------------------------------------------ */

void pfs_repl_rx(pk_node_t src_node, UH dst_port, const UB *data, UH len)
{
    (void)src_node; (void)dst_port;

    /* G35: HOLD-ACK (unicast holder confirmation) shares this port — peel it off
     * by magic before the CHUNK path. Reliable per-holder signal to the origin:
     * feed the protect announce-hook so holder_count counts this distinct holder
     * even when the shared K-DDS announce slot lost the broadcast. */
    if (len >= (UH)sizeof(PFSR_HOLD_PKT)) {
        const PFSR_HOLD_PKT *hp = (const PFSR_HOLD_PKT *)data;
        if (hp->magic == PFSR_HOLD_MAGIC) {
#ifdef _TK_HOSTED_LIBC_
            /* N2: a wide holder says 0xFF; pmesh's src (v2) is its id */
            pk_node_t hs = hp->src_node;
            if (hs == PK_NODE_NONE && PK_NODE_IS_WIDE(src_node)) hs = src_node;
            if ((hs < DNODE_MAX || PK_NODE_IS_WIDE(hs)) && hs != drpc_my_node &&
                announce_hook)
                announce_hook(hs, hp->id);
#else
            if (hp->src_node < DNODE_MAX && hp->src_node != drpc_my_node &&
                announce_hook)
                announce_hook(hp->src_node, hp->id);
#endif
            return;
        }
    }

    if (len < (UH)sizeof(PFSR_BLK_PKT)) return;
    const PFSR_BLK_PKT *pkt = (const PFSR_BLK_PKT *)data;

    if (pkt->magic   != PFSR_BLK_MAGIC) return;
    if (pkt->version != PFSR_VERSION)   return;
    if (pkt->type    != PFSR_BLK_CHUNK) return;
    if (pkt->src_node == drpc_my_node)  return;   /* echo guard */
    if (pkt->total_len > PFS_BLOCK_MAX) return;
    if (pkt->chunk_len > PFSR_CHUNK_SIZE) return;

    if (pkt->chunk_idx == 0) {
        /* self-describing first chunk == sfs FILE_START: (re)init state.
         * Already holding the block? Then this is a duplicate stream
         * from a second responder — ignore it entirely. */
        if (pfs_has(pkt->id)) return;
        pr_memset(&rx_state, 0, (UW)sizeof(rx_state));
        pr_memcpy(rx_state.id, pkt->id, PFS_ID_LEN);
        rx_state.total      = pkt->total_len;
#ifdef _TK_HOSTED_LIBC_
        {   /* N2: a wide origin rides the tail (send_block_to) */
            const PFSR_BLK_PKT_W *pw = (const PFSR_BLK_PKT_W *)data;
            INT tail = len >= (UH)sizeof(PFSR_BLK_PKT_W);
            rx_state.origin = pw_get(pkt->origin, tail ? pw->org_lo : 0, tail ? pw->org_hi : 0, tail);
        }
#else
        rx_state.origin     = pkt->origin;
#endif
        rx_state.received   = 0;
        rx_state.next_chunk = 0;
        rx_state.active     = 1;
    }

    if (!rx_state.active)                          return;
    if (!pr_id_eq(rx_state.id, pkt->id))           return;
    if (pkt->chunk_idx != rx_state.next_chunk)     return;  /* out of order */
    if (rx_state.received + pkt->chunk_len > rx_state.total) return;

    if (pkt->chunk_len)
        pr_memcpy(rx_state.buf + rx_state.received, pkt->data,
                  pkt->chunk_len);
    rx_state.received += pkt->chunk_len;
    rx_state.next_chunk++;

    if (rx_state.received < rx_state.total) return;   /* more to come */

    /* complete: verify content address before trusting the bytes */
    rx_state.active = 0;
    U1 chk[PFS_ID_LEN];
    pfs_id_compute(rx_state.buf, rx_state.total, chk);
    if (!pr_id_eq(chk, rx_state.id)) {
        stat_hash_fail++;
        pr_puts("[pfs] DROP block (hash mismatch) id=");
        pr_put_id(rx_state.id); pr_puts("\r\n");
        return;
    }

    /* store (dedup-safe); the put-hook re-announces it to the region */
    INT prc = pfs_put_origin(rx_state.buf, rx_state.total, 0, rx_state.origin);
    if (prc != PFS_OK) {
        /* PFS-FULL-SILENT-DROP: a verified block we could not keep used to
         * vanish here without a word (the sender kept re-sending, we kept
         * re-WANTing). Count it and say so — first time, then every 32nd. */
        if ((stat_store_fail++ & 31u) == 0) {
            /* PFS_E_DURABLE: kept in RAM only (pinned), not dropped */
            pr_puts(prc == PFS_E_DURABLE ? "[pfs] block kept in RAM only (store rc="
                                         : "[pfs] DROP block (store refused rc=");
            pr_putdec((UW)(-prc));
            pr_puts(prc == PFS_E_FULL ? " table full " : " ");
            pr_putdec(pfs_count()); pr_puts("/"); pr_putdec(PFS_MAX_BLOCKS);
            pr_puts(", refused so far "); pr_putdec(stat_store_fail);
            pr_puts(") id="); pr_put_id(rx_state.id); pr_puts("\r\n");
        }
        return;
    }
    {
        stat_blocks_rx++;
        pr_puts("[pfs] replicated block id="); pr_put_id(rx_state.id);
        pr_puts("  len="); pr_putdec(rx_state.total);
        pr_puts("  origin=n"); pr_putdec(rx_state.origin);
        pr_puts("\r\n");
        /* G35: tell the origin directly that we now hold its block (reliable
         * unicast — the broadcast announce-back can be lost under concurrency).
         * The put-hook above also re-announces it on K-DDS for region discovery;
         * this ack is the dependable per-holder confirmation for protect. */
        send_hold_ack(rx_state.origin, rx_state.id);
    }
}

/* ------------------------------------------------------------------ */
/* replication task — poll the REGION control topics                   */
/* ------------------------------------------------------------------ */

#define PFSR_POLL_MS 50

/* one received ANNOUNCE of r bytes: someone in the region stored a new block.
 * Hosted reads the N2 tail and keys the seq by row; bare metal keeps the old
 * function word for word (crown), after the #else below. */
#ifdef _TK_HOSTED_LIBC_
static void pfsr_ann_take(const PFSR_ANN_PKT *ap, W r)
{
    if (r < (W)sizeof(*ap) || ap->magic != PFSR_ANN_MAGIC) return;
    /* N2: ap points at a PFSR_ANN_PKT_W-sized buffer (pfsr_ann_drain) */
    const PFSR_ANN_PKT_W *aw = (const PFSR_ANN_PKT_W *)ap;
    INT tail = r >= (W)sizeof(*aw);
    pk_node_t src = pw_get(ap->src_node, tail ? aw->src_lo : 0, tail ? aw->src_hi : 0, tail);
    pk_node_t org = pw_get(ap->origin,   tail ? aw->org_lo : 0, tail ? aw->org_hi : 0, tail);
    INT row = pw_row(src);
    if (row >= 0 && ap->seq != last_ann_seq[row]) {
        last_ann_seq[row] = ap->seq;
        stat_ann_rx++;
        /* G28: tell the protect layer this peer holds ap->id, whether or
         * not WE hold it (the unit owner hears its replicas this way). */
        if (announce_hook) announce_hook(src, ap->id);
        if (ap->len <= PFS_BLOCK_MAX && !pfs_has(ap->id)) {
            pr_puts("[pfs] heard announce id="); pr_put_id(ap->id);
            pr_puts("  from n"); pr_putdec(src);
            pr_puts(" -> want\r\n");
            pending_add(ap->id);
        } else if (pfs_has(ap->id) && src == org) {
            /* The block's ORIGIN is re-driving its replication (the
             * protect actuator re-announcing a unit it owns; src==origin,
             * never a holder<->holder message) and we already hold it
             * durably. Confirm via reliable UNICAST hold-ack straight to
             * the origin (G35) instead of a broadcast announce-back: the
             * shared LATEST_ONLY announce slot overwrites all but one
             * holder's reply per tick, so under concurrent multi-point
             * replication the origin's holder_count could lag the real
             * durable replicas by many ticks. The unicast path has no
             * shared slot — each distinct holder confirms independently,
             * so holder_count reaches R promptly. Bounded: only
             * origin-driven announces (src==origin) trigger this, so it
             * does not cascade between holders. */
            send_hold_ack(org, ap->id);
        }
    }
}
#else
static void pfsr_ann_take(const PFSR_ANN_PKT *ap, W r)
{
    if (r >= (W)sizeof(*ap) && ap->magic == PFSR_ANN_MAGIC &&
        ap->src_node < DNODE_MAX && ap->src_node != drpc_my_node &&
        ap->seq != last_ann_seq[ap->src_node]) {
        last_ann_seq[ap->src_node] = ap->seq;
        stat_ann_rx++;
        /* G28: tell the protect layer this peer holds ap->id, whether or
         * not WE hold it (the unit owner hears its replicas this way). */
        if (announce_hook) announce_hook(ap->src_node, ap->id);
        if (ap->len <= PFS_BLOCK_MAX && !pfs_has(ap->id)) {
            pr_puts("[pfs] heard announce id="); pr_put_id(ap->id);
            pr_puts("  from n"); pr_putdec(ap->src_node);
            pr_puts(" -> want\r\n");
            pending_add(ap->id);
        } else if (pfs_has(ap->id) && ap->src_node == ap->origin) {
            /* origin re-driving its replication: unicast hold-ack (G35),
             * see the hosted twin above */
            send_hold_ack(ap->origin, ap->id);
        }
    }
}
#endif

#ifdef _TK_HOSTED_LIBC_
/* one received WANT of r bytes: someone in the region lacks a block — any
 * holder streams it over (w points at a PFSR_WANT_PKT_W-sized buffer). Hosted
 * only (N2 tail, row); bare metal reads it inline in pfs_repl_task as before. */
static void pfsr_want_take(const PFSR_WANT_PKT *w, W r)
{
    if (r < (W)sizeof(*w) || w->magic != PFSR_WANT_MAGIC) return;
    const PFSR_WANT_PKT_W *ww = (const PFSR_WANT_PKT_W *)w;
    INT tail = r >= (W)sizeof(*ww);
    pk_node_t src = pw_get(w->src_node, tail ? ww->src_lo : 0, tail ? ww->src_hi : 0, tail);
    INT row = pw_row(src);
    if (row >= 0 && w->seq != last_want_seq[row]) {
        last_want_seq[row] = w->seq;
        stat_want_rx++;
        if (pfs_has(w->id))
            send_block_to(src, w->id);
    }
}

/* one received SYNC of r bytes: a (re)joining node wants everything we hold */
static void pfsr_sync_take(const PFSR_SYNC_PKT *s, W r)
{
    if (r < (W)sizeof(*s) || s->magic != PFSR_SYNC_MAGIC) return;
    const PFSR_SYNC_PKT_W *sw = (const PFSR_SYNC_PKT_W *)s;
    INT tail = r >= (W)sizeof(*sw);
    pk_node_t src = pw_get(s->src_node, tail ? sw->src_lo : 0, tail ? sw->src_hi : 0, tail);
    INT row = pw_row(src);
    if (row >= 0 && s->seq != last_sync_seq[row]) {
        last_sync_seq[row] = s->seq;
        for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
            U1 sid[PFS_ID_LEN];
            if (!pfs_slot_info(i, sid, 0, 0)) continue;
            /* a quietly-held protected unit is NOT streamed by ambient
             * sync — only the protect actuator spreads it (§2/G28) */
            if (sync_filter && sync_filter(sid)) continue;
            send_block_to(src, sid);
        }
    }
}
#endif

#ifdef _TK_HOSTED_LIBC_
/* R-KDDSQ stage 2 (kdds-queue.md): the ANNOUNCE reader takes every announce
 * that came in since the last tick, oldest first (kdds_sub_next), up to
 * PFSR_ANN_PER_TICK, not only the last one: two holders announcing in one
 * tick overwrote each other's in the topic's one slot. The per-sender seq
 * check stays (seq means nothing across senders). Hosted only (kdds_sub_next
 * is hosted only; bare metal reads the last one as before).
 * -DPFSR_NC_ANN_LATEST: the old kdds_sub (negative control, `kdds ann`). */
#ifndef PFSR_ANN_PER_TICK
#define PFSR_ANN_PER_TICK 32   /* 8 until 2026-11-09: a node also keeps its own announces and WANTs in the shared ring (KDDS_Q_N 32), so reading 8 + 8 a tick fell behind a paced peer (PFSR_ANN_PACE) */
#endif
static INT pfsr_ann_drain(W h, void (*take)(const PFSR_ANN_PKT *, W))
{
    PFSR_ANN_PKT_W a;                    /* + a wide sender's / origin's tail (N2) */
    INT k = 0;
#ifndef PFSR_NC_ANN_LATEST
    for (; k < PFSR_ANN_PER_TICK; k++) {
        W r = kdds_sub_next(h, &a, (W)sizeof(a));
        if (r <= 0) break;
        take(&a.p, r);
    }
#else
    W r = kdds_sub(h, &a, (W)sizeof(a), 0);
    if (r > 0) { take(&a.p, r); k = 1; }
#endif
    return k;
}

/* `kdds ann` (R-KDDSQ stage 2): four announces received in a row on a test
 * topic (n1 seq 5, n2 seq 7, n1 seq 5 again, n1 seq 6), one drain with the
 * real pfsr_ann_take: the protect hook hears n1 n2 n1 (the repeat is dropped
 * by the per-sender seq), stat_ann_rx +3. The old kdds_sub reads n1 seq 6
 * only. The block ids are not held and len > PFS_BLOCK_MAX: no want, no
 * hold-ack. The hook, stat_ann_rx, last_ann_seq of 1 and 2 and drpc_my_node
 * are put back. Limit: it checks pfsr_ann_drain, not that pfs_repl_task
 * calls it (run_relay_failover and the CI's pfs runs do). */
static UB  kq_ann_heard[8];
static INT kq_ann_n;
static void kq_ann_hook(pk_node_t src, const U1 id[PFS_ID_LEN])
{
    (void)id;
    if (kq_ann_n < 8) kq_ann_heard[kq_ann_n] = (UB)src;
    kq_ann_n++;
}
static void kq_ann_rx(const char *name, pk_node8_t src, UW seq)
{
    static KDDS_PKT p;
    PFSR_ANN_PKT a;
    for (INT i = 0; i < (INT)sizeof p; i++) ((UB *)&p)[i] = 0;
    for (INT i = 0; i < (INT)sizeof a; i++) ((UB *)&a)[i] = 0;
    a.magic = PFSR_ANN_MAGIC; a.seq = seq; a.src_node = src; a.origin = src;
    a.len = PFS_BLOCK_MAX + 1;
    for (INT i = 0; i < PFS_ID_LEN; i++) a.id[i] = (U1)(0xA0 + src + i);
    p.magic = KDDS_MAGIC; p.version = KDDS_VERSION; p.type = KDDS_DATA_PKT;
    p.src_node = src; p.data_len = (UH)sizeof a;
    INT n = 0; while (name[n]) n++; n++;
    for (INT i = 0; i < n && i < KDDS_NAME_MAX; i++) p.name[i] = name[i];
    p.name_len = (UH)n;
    for (INT i = 0; i < (INT)sizeof a; i++) p.data[i] = ((const UB *)&a)[i];
    kdds_rx(src, KDDS_PORT, (const UB *)&p, (UH)sizeof p);
}
INT pfs_repl_ann_self_test(void (*emit)(const char *))
{
    void (*say)(const char *) = emit ? emit : pr_puts;
    const char *NM = "test/pfsann";
    W h = kdds_open_poll(NM, KDDS_QOS_LATEST_ONLY);
    if (h < 0) { say("[kdds-ann] FAIL open\r\n"); return 1; }
    pk_node_t saved_my = drpc_my_node;
    PFSR_ANN_HOOK saved_hook = announce_hook;
    UW saved_rx = stat_ann_rx, s1 = last_ann_seq[1], s2 = last_ann_seq[2];
    drpc_my_node = 0xFF; announce_hook = kq_ann_hook; kq_ann_n = 0;
    last_ann_seq[1] = 0; last_ann_seq[2] = 0;
    PFSR_ANN_PKT a;
    (void)kdds_sub_next(h, &a, (W)sizeof a);          /* start reading: nothing yet */
    kq_ann_rx(NM, 1, 5); kq_ann_rx(NM, 2, 7); kq_ann_rx(NM, 1, 5); kq_ann_rx(NM, 1, 6);
    INT got = pfsr_ann_drain(h, pfsr_ann_take);
    UW drx = stat_ann_rx - saved_rx;
    INT ok = (kq_ann_n == 3 && kq_ann_heard[0] == 1 && kq_ann_heard[1] == 2 &&
              kq_ann_heard[2] == 1 && drx == 3 && got == 4);
    INT again = pfsr_ann_drain(h, pfsr_ann_take);   /* nothing new */
    if (again != 0 || kq_ann_n != 3) ok = 0;
    announce_hook = saved_hook; stat_ann_rx = saved_rx;
    last_ann_seq[1] = s1; last_ann_seq[2] = s2; drpc_my_node = saved_my;
    kdds_close(h);
    pr_puts("[kdds-ann] read "); pr_putdec((UW)got); pr_puts(", heard ");
    pr_putdec((UW)kq_ann_n); pr_puts(" (want 4 read, 3 heard: n1 n2 n1)\r\n");
    if (!ok) { say("[kdds-ann] FAIL drain (four announces in one tick did not give n1 n2 n1)\r\n"); return 1; }
    say("[kdds-ann] PASS (the announce reader takes every announce of the tick, oldest first; a repeated seq is dropped)\r\n");
    return 0;
}

/* `nodes pfwide` (N2 "まだ" 7, research/n2-wire16.md): the pfs_repl tails, one node.
 *  F  frames: narrow self, narrow origin -> the old 48/44/12 bytes; wide self 299 ->
 *     52/46/14, the byte 0xFF and 299 in the tail; narrow self announcing wide origin
 *     299's block -> 52 with 299 in the tail
 *  R  reading: 0xFF + wide tail -> the wide id; 0xFF + narrow tail 43 stays 0xFF (a tail
 *     cannot speak for a narrow node); a narrow byte ignores the tail; no tail -> the byte
 *  W  a WANT from 299 (0xFF + tail) is taken once on 299's own row (seq 7 twice -> 1);
 *     row 43 (299 & 0xFF) keeps its seq; 0xFF without a tail and 0xFF + narrow tail 43
 *     are dropped. The asked id is not held: nothing is sent.
 *  State touched (drpc_my_node, stat_want_rx, last_want_seq of rows 43 and 299's) is put
 *  back. Negative controls: -DN2_NC_PFS_NO_TAIL (F), -DN2_NC_PFS_RX_NARROW (R, W). */
INT pfs_repl_wide_self_test(void (*emit)(const char *))
{
    void (*say)(const char *) = emit ? emit : pr_puts;
    INT fails = 0;
#define PFW_FAIL(m) do { say("[pfwide] FAIL " m "\r\n"); fails++; } while (0)
    PFSR_ANN_PKT a; PFSR_WANT_PKT w; PFSR_SYNC_PKT s;
    PFSR_ANN_PKT_W aw; PFSR_WANT_PKT_W ww; PFSR_SYNC_PKT_W sw;
    pr_memset(&a, 0, (UW)sizeof a); pr_memset(&w, 0, (UW)sizeof w); pr_memset(&s, 0, (UW)sizeof s);
    a.src_node = 3; a.origin = 3; w.src_node = 3; s.src_node = 3;
    if (pfsr_ann_frame(&a, 3, 3, &aw) != 48 || pfsr_want_frame(&w, 3, &ww) != 44 ||
        pfsr_sync_frame(&s, 3, &sw) != 12) PFW_FAIL("F narrow (not the old 48/44/12 bytes)");
    a.src_node = pk_node_v1(299); a.origin = pk_node_v1(299); w.src_node = pk_node_v1(299); s.src_node = pk_node_v1(299);
    if (pfsr_ann_frame(&a, 299, 299, &aw) != 52 || aw.p.src_node != 0xFF ||
        aw.src_lo != (299 & 0xFF) || aw.src_hi != 1 || aw.org_lo != (299 & 0xFF) || aw.org_hi != 1)
        PFW_FAIL("F wide ANNOUNCE (not 52 bytes with 299 after 0xFF)");
    if (pfsr_want_frame(&w, 299, &ww) != 46 || ww.src_lo != (299 & 0xFF) || ww.src_hi != 1)
        PFW_FAIL("F wide WANT (not 46 bytes with 299 in the tail)");
    if (pfsr_sync_frame(&s, 299, &sw) != 14 || sw.src_lo != (299 & 0xFF) || sw.src_hi != 1)
        PFW_FAIL("F wide SYNC (not 14 bytes with 299 in the tail)");
    a.src_node = 3;
    if (pfsr_ann_frame(&a, 3, 299, &aw) != 52 || aw.org_lo != (299 & 0xFF) || aw.org_hi != 1)
        PFW_FAIL("F narrow self, wide origin (not 52 bytes with the origin in the tail)");

    if (pw_get(0xFF, 299 & 0xFF, 1, 1) != 299) PFW_FAIL("R 0xFF + tail 299 is not 299");
    if (pw_get(0xFF, 43, 0, 1) != 0xFF)        PFW_FAIL("R 0xFF + narrow tail 43 is not 0xFF");
    if (pw_get(3, 299 & 0xFF, 1, 1) != 3)      PFW_FAIL("R narrow byte 3 took the tail");
    if (pw_get(0xFF, 299 & 0xFF, 1, 0) != 0xFF) PFW_FAIL("R no tail but read one");

    pk_node_t saved_my = drpc_my_node;
    UW saved_rx = stat_want_rx;
    INT r299 = pk_node_row(299);
    if (r299 < DNODE_MAX) PFW_FAIL("W 299 has no row of its own");
    else {
        UW s43 = last_want_seq[43], s299 = last_want_seq[r299];
        drpc_my_node = 5; last_want_seq[43] = 0; last_want_seq[r299] = 0;
        pr_memset(&ww, 0, (UW)sizeof ww);
        ww.p.magic = PFSR_WANT_MAGIC; ww.p.seq = 7; ww.p.src_node = 0xFF;
        for (INT i = 0; i < PFS_ID_LEN; i++) ww.p.id[i] = (U1)(0x5A ^ i);   /* not held */
        ww.src_lo = 299 & 0xFF; ww.src_hi = 1;
        pfsr_want_take(&ww.p, (W)sizeof ww);
        pfsr_want_take(&ww.p, (W)sizeof ww);                        /* same seq again */
        UW d1 = stat_want_rx - saved_rx;
        if (d1 != 1 || last_want_seq[r299] != 7 || last_want_seq[43] != 0)
            PFW_FAIL("W a wide WANT was not taken once on 299's own row");
        ww.p.seq = 8; pfsr_want_take(&ww.p, (W)sizeof ww.p);        /* 0xFF, no tail */
        ww.p.seq = 9; ww.src_lo = 43; ww.src_hi = 0;
        pfsr_want_take(&ww.p, (W)sizeof ww);                        /* 0xFF + narrow tail */
        if (stat_want_rx - saved_rx != d1 || last_want_seq[43] != 0)
            PFW_FAIL("W 0xFF without a wide tail was taken");
        last_want_seq[43] = s43; last_want_seq[r299] = s299;
    }
    drpc_my_node = saved_my; stat_want_rx = saved_rx;
#undef PFW_FAIL
    if (fails == 0) say("[pfwide] PASS (wide ids: 0xFF in the byte, 2 bytes LE after the packet; read only when 0xFF and wide; own dedup row)\r\n");
    else            say("[pfwide] RED\r\n");
    return fails;
}
#endif

void pfs_repl_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* one-shot boot sync after SWIM has had time to find peers
     * (sfs_boot_sync generalized — a joining node pulls every block
     * its region already holds; empty cluster: harmless no-op). */
    tk_dly_tsk(3000);
    publish_sync();

    for (;;) {
        /* ANNOUNCE: someone in the region stored a new block */
#ifdef _TK_HOSTED_LIBC_
#ifdef PFSR_ANN_PACED
        pfsr_ann_flush(PFSR_ANN_PACE);                    /* our own, paced (above) */
#endif
        (void)pfsr_ann_drain(h_ann_sub, pfsr_ann_take);   /* every one of the tick */
#else
        {
            PFSR_ANN_PKT a;
            W r = kdds_sub(h_ann_sub, &a, (W)sizeof(a), 0);
            pfsr_ann_take(&a, r);
        }
#endif

        /* WANT: someone lacks a block — any holder streams it over */
        {
#ifdef _TK_HOSTED_LIBC_
            PFSR_WANT_PKT_W ww;                    /* + a wide asker's tail (N2) */
#ifndef PFSR_NC_WANT_LATEST
            /* PFS-REPL-WANT-DROP, 0 ms burst: an asker's pending WANTs go out in the
             * same tick (same age), and the last one overwrote the others in the
             * topic's one slot, so a holder served 1 of 8 per round and the asker
             * gave up on some (2 of 60). Read every WANT of the tick, oldest first,
             * as the ANNOUNCE reader does (R-KDDSQ). -DPFSR_NC_WANT_LATEST: the old read. */
            for (INT k = 0; k < PFSR_ANN_PER_TICK; k++) {
                W r = kdds_sub_next(h_want_sub, &ww, (W)sizeof(ww));
                if (r <= 0) break;
                pfsr_want_take(&ww.p, r);
            }
#else
            W r = kdds_sub(h_want_sub, &ww, (W)sizeof(ww), 0);
            pfsr_want_take(&ww.p, r);
#endif
#else
            PFSR_WANT_PKT w;
            W r = kdds_sub(h_want_sub, &w, (W)sizeof(w), 0);
            if (r >= (W)sizeof(w) && w.magic == PFSR_WANT_MAGIC &&
                w.src_node < DNODE_MAX && w.src_node != drpc_my_node &&
                w.seq != last_want_seq[w.src_node]) {
                last_want_seq[w.src_node] = w.seq;
                stat_want_rx++;
                if (pfs_has(w.id))
                    send_block_to(w.src_node, w.id);
            }
#endif
        }

        /* SYNC: a (re)joining node wants everything we hold */
        {
#ifdef _TK_HOSTED_LIBC_
            PFSR_SYNC_PKT_W sw;
            W r = kdds_sub(h_sync_sub, &sw, (W)sizeof(sw), 0);
            pfsr_sync_take(&sw.p, r);
#else
            PFSR_SYNC_PKT s;
            W r = kdds_sub(h_sync_sub, &s, (W)sizeof(s), 0);
            if (r >= (W)sizeof(s) && s.magic == PFSR_SYNC_MAGIC &&
                s.src_node < DNODE_MAX && s.src_node != drpc_my_node &&
                s.seq != last_sync_seq[s.src_node]) {
                last_sync_seq[s.src_node] = s.seq;
                for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
                    U1 sid[PFS_ID_LEN];
                    if (!pfs_slot_info(i, sid, 0, 0)) continue;
                    /* a quietly-held protected unit is NOT streamed by ambient
                     * sync — only the protect actuator spreads it (§2/G28) */
                    if (sync_filter && sync_filter(sid)) continue;
                    send_block_to(s.src_node, sid);
                }
            }
#endif
        }

        pending_tick(PFSR_POLL_MS);
        tk_dly_tsk(PFSR_POLL_MS);
    }
}

/* ------------------------------------------------------------------ */
/* shell helpers                                                       */
/* ------------------------------------------------------------------ */

INT pfs_repl_put(const void *buf, UW len, U1 id_out[PFS_ID_LEN])
{
    pk_node_t origin = (drpc_my_node == 0xFF) ? PFS_ORIGIN_SELF : drpc_my_node;
    return pfs_put_origin(buf, len, id_out, origin);
}

/* P2 hook: chase a block we heard about via a ref beacon but never got
 * an ANNOUNCE for (e.g. joined after the save). Rides the existing
 * pending-WANT retry machinery; no-op if we already hold it. */
void pfs_repl_want(const U1 id[PFS_ID_LEN])
{
    if (drpc_my_node == 0xFF) return;
    if (pfs_has(id)) return;
    pending_add(id);
}

void pfs_repl_put_cmd(const UB *text, UW len)
{
    U1 id[PFS_ID_LEN];
    INT r = pfs_repl_put(text, len, id);
    if (r != PFS_OK) {
        pr_puts("[pfs] put failed (");
        pr_putdec((UW)(-r));
        pr_puts(")\r\n");
        return;
    }
    pr_puts("[pfs] put len="); pr_putdec(len);
    pr_puts("  id="); pr_put_id(id);
    pr_puts("\r\n");
}

void pfs_repl_ls(void)
{
    pr_puts("[pfs] blocks (id-prefix / len / origin):\r\n");
    UW shown = 0;
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        U1 id[PFS_ID_LEN]; UW len; pk_node_t origin;
        if (!pfs_slot_info(i, id, &len, &origin)) continue;
        pr_puts("  "); pr_put_id(id);
        pr_puts("  len="); pr_putdec(len);
        if (origin == PFS_ORIGIN_SELF) pr_puts("  origin=self");
        else { pr_puts("  origin=n"); pr_putdec(origin); }
        pr_puts("\r\n");
        shown++;
    }
    if (!shown) pr_puts("  (none)\r\n");
    pr_puts("  table "); pr_putdec(pfs_count()); pr_puts("/");
    pr_putdec(PFS_MAX_BLOCKS);
    pr_puts("  puts refused (full) "); pr_putdec(pfs_full_count());
    pr_puts("  replicas refused "); pr_putdec(stat_store_fail);
    pr_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

void pfs_repl_init(void)
{
    pr_memset(pending,       0, (UW)sizeof(pending));
    pr_memset(&rx_state,     0, (UW)sizeof(rx_state));
    pr_memset(last_ann_seq,  0, (UW)sizeof(last_ann_seq));
    pr_memset(last_want_seq, 0, (UW)sizeof(last_want_seq));
    pr_memset(last_sync_seq, 0, (UW)sizeof(last_sync_seq));
    ann_seq = want_seq = sync_seq = 0;

    /* REGION-scoped control topics: announce / want / sync gossip stays
     * inside the latency cluster (p-fs.md §2.2 locality principle). */
    h_ann_pub  = kdds_open_scoped(PFSR_TOPIC_ANN,  KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_ann_sub  = kdds_open_scoped(PFSR_TOPIC_ANN,  KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_want_pub = kdds_open_scoped(PFSR_TOPIC_WANT, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_want_sub = kdds_open_scoped(PFSR_TOPIC_WANT, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_sync_pub = kdds_open_scoped(PFSR_TOPIC_SYNC, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);
    h_sync_sub = kdds_open_scoped(PFSR_TOPIC_SYNC, KDDS_QOS_LATEST_ONLY,
                                  KDDS_SCOPE_REGION);

    pmesh_bind(PFSR_PORT, pfs_repl_rx);
    pfs_set_put_hook(on_new_block);

    pr_puts("[pfs] P1 replication ready  port=");
    pr_putdec(PFSR_PORT);
    pr_puts("  (region-scoped announce/want)\r\n");
}
