/*
 *  pfs_repl.c — p-fs P1: region-scoped gossip replication of blocks.
 *
 *  Spec: docs/architecture/p-fs.md §2.2 / §3.2 / §5 (P1 row).
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

/* poll-side dedup: last seq acted on, per source node */
static UW last_ann_seq [DNODE_MAX];
static UW last_want_seq[DNODE_MAX];
static UW last_sync_seq[DNODE_MAX];

/* pending WANTs: announced blocks we still lack (retry until they
 * arrive or the budget runs out — lost packets self-heal here). */
typedef struct {
    U1 id[PFS_ID_LEN];
    UW age_ms;                 /* since last WANT publish              */
    U1 tries;
    U1 active;
} PFSR_PENDING;
static PFSR_PENDING pending[PFSR_PENDING_MAX];

/* single-slot receive assembly (sfs_rx_state generalized to a block).
 * Static — 4KB+ must never live on a task stack in a net path. */
static struct {
    U1 id[PFS_ID_LEN];
    UW total;
    UW received;
    UW next_chunk;
    U1 origin;
    U1 active;
    U1 buf[PFS_BLOCK_MAX];
} rx_state;

/* static per-packet / per-block send scratch (same trap) */
static PFSR_BLK_PKT tx_pkt;
static U1           tx_block[PFS_BLOCK_MAX];

/* stats (printed by events; counters kept for future `pfs stat`) */
static UW stat_ann_tx, stat_ann_rx, stat_want_tx, stat_want_rx;
static UW stat_blocks_tx, stat_blocks_rx, stat_hash_fail;

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

static void publish_announce(const U1 id[PFS_ID_LEN], UW len, U1 origin)
{
    if (drpc_my_node == 0xFF || h_ann_pub < 0) return;

    PFSR_ANN_PKT a;
    pr_memset(&a, 0, (UW)sizeof(a));
    a.magic    = PFSR_ANN_MAGIC;
    a.seq      = ++ann_seq;
    a.src_node = drpc_my_node;
    a.origin   = origin;
    a.len      = len;
    pr_memcpy(a.id, id, PFS_ID_LEN);
    kdds_pub(h_ann_pub, &a, (W)sizeof(a));
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
    w.src_node = drpc_my_node;
    pr_memcpy(w.id, id, PFS_ID_LEN);
    kdds_pub(h_want_pub, &w, (W)sizeof(w));
    stat_want_tx++;
}

static void publish_sync(void)
{
    if (drpc_my_node == 0xFF || h_sync_pub < 0) return;

    PFSR_SYNC_PKT s;
    pr_memset(&s, 0, (UW)sizeof(s));
    s.magic    = PFSR_SYNC_MAGIC;
    s.seq      = ++sync_seq;
    s.src_node = drpc_my_node;
    kdds_pub(h_sync_pub, &s, (W)sizeof(s));

    pr_puts("[pfs] boot sync request (region, fanout=");
    pr_putdec(kdds_pub_fanout());
    pr_puts(")\r\n");
}

/* ------------------------------------------------------------------ */
/* put-hook: a NEW local store becomes a region announce               */
/* (save == publish, p-fs.md §3.2 — symmetric on every node, so a      */
/* replica that just stored a received block re-announces it onward)   */
/* ------------------------------------------------------------------ */

static void on_new_block(const U1 id[PFS_ID_LEN], UW len, U1 origin)
{
    if (drpc_my_node == 0xFF) return;       /* not distributed: local only */
    if (announce_suppress) return;          /* G28: protected quiet put     */
    U1 org = (origin == PFS_ORIGIN_SELF) ? drpc_my_node : origin;
    publish_announce(id, len, org);
}

/* G35: unicast "I durably hold your block" straight to the origin. Point-to-
 * point, so distinct holders never clobber each other (unlike the shared
 * LATEST_ONLY announce slot). Drives the origin's protect holder_count
 * reliably, so plural protection's grounded threat falls promptly. */
static void send_hold_ack(UB origin_node, const U1 id[PFS_ID_LEN])
{
    if (drpc_my_node == 0xFF) return;
    if (origin_node >= DNODE_MAX) return;       /* not a real node (e.g. 0xFF) */
    if (origin_node == drpc_my_node) return;    /* never ack ourselves         */
    PFSR_HOLD_PKT h;
    pr_memset(&h, 0, (UW)sizeof(h));
    h.magic    = PFSR_HOLD_MAGIC;
    h.src_node = drpc_my_node;
    h.origin   = origin_node;
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
        U1 sid[PFS_ID_LEN]; UW slen; U1 sorg;
        if (pfs_slot_info(i, sid, &slen, &sorg) && pr_id_eq(sid, id)) {
            U1 org = (sorg == PFS_ORIGIN_SELF) ? drpc_my_node : sorg;
            publish_announce(id, slen, org);
            return;
        }
    }
}

static void send_block_to(UB dst_node, const U1 id[PFS_ID_LEN]);

/* G35: public direct push — unicast a held block straight to one neighbour
 * (reliable discovery for the protect actuator; see send_block_to below). */
void pfs_repl_push(const U1 id[PFS_ID_LEN], UB dst_node)
{
    if (drpc_my_node == 0xFF) return;
    if (dst_node >= DNODE_MAX || dst_node == drpc_my_node) return;
    if (!pfs_has(id)) return;
    send_block_to(dst_node, id);
}

/* ------------------------------------------------------------------ */
/* chunked block send (sfs_push generalized to a block, unicast)       */
/* ------------------------------------------------------------------ */

static void send_block_to(UB dst_node, const U1 id[PFS_ID_LEN])
{
    INT blen = pfs_get(id, tx_block, PFS_BLOCK_MAX);
    if (blen < 0 || blen > (INT)PFS_BLOCK_MAX) return;

    /* recover the origin tag so it survives the hop */
    U1 origin = PFS_ORIGIN_SELF;
    for (UW i = 0; i < PFS_MAX_BLOCKS; i++) {
        U1 sid[PFS_ID_LEN]; U1 sorg;
        if (pfs_slot_info(i, sid, 0, &sorg) && pr_id_eq(sid, id)) {
            origin = sorg;
            break;
        }
    }

    pr_memset(&tx_pkt, 0, (UW)sizeof(tx_pkt));
    tx_pkt.magic     = PFSR_BLK_MAGIC;
    tx_pkt.version   = PFSR_VERSION;
    tx_pkt.type      = PFSR_BLK_CHUNK;
    tx_pkt.src_node  = drpc_my_node;
    tx_pkt.origin    = origin;
    tx_pkt.total_len = (UW)blen;
    pr_memcpy(tx_pkt.id, id, PFS_ID_LEN);

    UW off = 0, idx = 0;
    do {                                   /* at least 1 chunk (len 0 ok) */
        UW n = (UW)blen - off;
        if (n > PFSR_CHUNK_SIZE) n = PFSR_CHUNK_SIZE;
        tx_pkt.chunk_idx = idx;
        tx_pkt.chunk_len = (UH)n;
        if (n) pr_memcpy(tx_pkt.data, tx_block + off, n);
        pmesh_send(dst_node, PFSR_PORT, (const UB *)&tx_pkt,
                   (UH)sizeof(tx_pkt));
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
    /* table full: drop — the next re-announce will retry us */
}

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
}

/* ------------------------------------------------------------------ */
/* chunk receive (sfs_rx FILE_START/FILE_CHUNK generalized)            */
/* ------------------------------------------------------------------ */

void pfs_repl_rx(UB src_node, UH dst_port, const UB *data, UH len)
{
    (void)src_node; (void)dst_port;

    /* G35: HOLD-ACK (unicast holder confirmation) shares this port — peel it off
     * by magic before the CHUNK path. Reliable per-holder signal to the origin:
     * feed the protect announce-hook so holder_count counts this distinct holder
     * even when the shared K-DDS announce slot lost the broadcast. */
    if (len >= (UH)sizeof(PFSR_HOLD_PKT)) {
        const PFSR_HOLD_PKT *hp = (const PFSR_HOLD_PKT *)data;
        if (hp->magic == PFSR_HOLD_MAGIC) {
            if (hp->src_node < DNODE_MAX && hp->src_node != drpc_my_node &&
                announce_hook)
                announce_hook(hp->src_node, hp->id);
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
        rx_state.origin     = pkt->origin;
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
    if (pfs_put_origin(rx_state.buf, rx_state.total, 0,
                       rx_state.origin) == PFS_OK) {
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
        {
            PFSR_ANN_PKT a;
            W r = kdds_sub(h_ann_sub, &a, (W)sizeof(a), 0);
            if (r >= (W)sizeof(a) && a.magic == PFSR_ANN_MAGIC &&
                a.src_node < DNODE_MAX && a.src_node != drpc_my_node &&
                a.seq != last_ann_seq[a.src_node]) {
                last_ann_seq[a.src_node] = a.seq;
                stat_ann_rx++;
                /* G28: tell the protect layer this peer holds a.id, whether or
                 * not WE hold it (the unit owner hears its replicas this way). */
                if (announce_hook) announce_hook(a.src_node, a.id);
                if (a.len <= PFS_BLOCK_MAX && !pfs_has(a.id)) {
                    pr_puts("[pfs] heard announce id="); pr_put_id(a.id);
                    pr_puts("  from n"); pr_putdec(a.src_node);
                    pr_puts(" -> want\r\n");
                    pending_add(a.id);
                } else if (pfs_has(a.id) && a.src_node == a.origin) {
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
                    send_hold_ack(a.origin, a.id);
                }
            }
        }

        /* WANT: someone lacks a block — any holder streams it over */
        {
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
        }

        /* SYNC: a (re)joining node wants everything we hold */
        {
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
    U1 origin = (drpc_my_node == 0xFF) ? PFS_ORIGIN_SELF : drpc_my_node;
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
        U1 id[PFS_ID_LEN]; UW len; U1 origin;
        if (!pfs_slot_info(i, id, &len, &origin)) continue;
        pr_puts("  "); pr_put_id(id);
        pr_puts("  len="); pr_putdec(len);
        if (origin == PFS_ORIGIN_SELF) pr_puts("  origin=self");
        else { pr_puts("  origin=n"); pr_putdec(origin); }
        pr_puts("\r\n");
        shown++;
    }
    if (!shown) pr_puts("  (none)\r\n");
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
