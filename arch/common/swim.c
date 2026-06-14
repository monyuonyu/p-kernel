/*
 *  swim.c (x86)
 *  SWIM membership protocol — indirect probing + gossip dissemination
 *
 *  Uses the same dnode_table[] as drpc.c (shared extern).
 *  drpc_task continues broadcasting DRPC_HEARTBEAT for initial discovery;
 *  swim_task adds explicit PING/ACK probing on top for faster failure
 *  detection with indirect verification (avoids false positives).
 *
 *  Gossip TTL: each membership event is piggybacked GOSSIP_TTL times,
 *  giving O(log N) epidemic spread across the cluster.
 */

#include "swim.h"
#include "heal.h"
#include "replica.h"
#include "degrade.h"
#include "dmn.h"
#include "galaxy.h"     /* galaxy v1: S1 membership emit hook */
#include "netstack.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

static void sw_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void sw_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sw_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sw_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* Gossip queue                                                        */
/*                                                                     */
/* Each entry is piggybacked on outgoing SWIM packets until            */
/* remaining == 0, then retired.  GOSSIP_TTL ≈ 2*log(N) = 6.         */
/* ------------------------------------------------------------------ */

#define GOSSIP_TTL    6
#define GOSSIP_Q_MAX  (SWIM_GOSSIP_MAX * 2)

typedef struct {
    UB  node_id;
    UB  state;
    UB  incarnation;
    UB  remaining;
} GOSSIP_QUEUED;

static GOSSIP_QUEUED gq[GOSSIP_Q_MAX];
static INT           gq_cnt = 0;

/* ------------------------------------------------------------------ */
/* Incarnation numbers (canonical SWIM anti-stale-rumor)               */
/*                                                                     */
/* SWIM-INCARN (external audit 2026-06-13): the `incarnation` field was */
/* on the wire but never assigned a real value and never compared, so   */
/* a node could not refute an OLD DEAD/SUSPECT rumor about itself with a */
/* FRESHER incarnation — the rumor's TTL could re-kill it.              */
/*                                                                     */
/* Fix: each node keeps a monotonic `my_incarnation`, bumped whenever it */
/* must refute a SUSPECT/DEAD rumor about ITSELF (re-asserting ALIVE at  */
/* a strictly higher incarnation). `dnode_incarn[nid]` mirrors the last  */
/* incarnation we accepted for each peer, and gossip_apply does a        */
/* (incarnation, state-severity) lexicographic last-writer-wins:         */
/*   - a strictly HIGHER incarnation always wins (regardless of state);  */
/*   - within the SAME incarnation, more-dead wins (ALIVE<SUSPECT<DEAD); */
/*   - a strictly LOWER incarnation is ignored (the stale rumor).        */
/* incarnation is a UB on the wire (swim.h) so it wraps at 256; bumps    */
/* are a plain ++ and the compare is on equality + a single-step ladder, */
/* so a wrap only loses one anti-stale round (self re-bumps next time it  */
/* is rumored) — sane and never UB. */
static UB my_incarnation        = 0;
static UB dnode_incarn[DNODE_MAX];

/* deterministic peer address (10.1.0.(n+1)); defined below, used by the
 * transitive-discovery path in gossip_apply() to resolve a gossip-introduced
 * peer's IP from its node id alone (no address rides the gossip frame). */
static UW swim_node_ip(UB n);
static UB suspect_count[DNODE_MAX];    /* fwd: cleared on gossip discovery */

/* state severity ladder for same-incarnation tie-break (ALIVE<SUSPECT<DEAD) */
static INT state_rank(UB st)
{
    if (st == DNODE_DEAD)    return 2;
    if (st == DNODE_SUSPECT) return 1;
    return 0;   /* ALIVE / UNKNOWN */
}

/* galaxy v1 (galaxy.md S1): ONE file-static emitter called at each
 * existing SWIM state-change print site, so a star appears / dims / goes
 * dark in the window exactly when SWIM's belief changes — with SWIM's
 * latency, no new gossip (the membership table itself is read directly
 * by /galaxy.json). old==0xFF where the prior state was not in hand. */
static void sw_note(UB nid, UB oldst, UB newst)
{
    galaxy_emit(EV_SWIM, nid, GALAXY_NODE_NONE, (UH)oldst, (UH)newst);
}

static void gossip_add(UB node_id, UB state, UB incarnation)
{
    /* Update existing entry if present */
    for (INT i = 0; i < gq_cnt; i++) {
        if (gq[i].node_id == node_id) {
            gq[i].state       = state;
            gq[i].incarnation = incarnation;
            gq[i].remaining   = GOSSIP_TTL;
            return;
        }
    }
    if (gq_cnt < GOSSIP_Q_MAX) {
        gq[gq_cnt++] = (GOSSIP_QUEUED){ node_id, state, incarnation, GOSSIP_TTL };
        return;
    }
    /* Queue full: evict oldest (lowest remaining) */
    INT min_i = 0;
    for (INT i = 1; i < gq_cnt; i++)
        if (gq[i].remaining < gq[min_i].remaining) min_i = i;
    gq[min_i] = (GOSSIP_QUEUED){ node_id, state, incarnation, GOSSIP_TTL };
}

static void gossip_fill(SWIM_PKT *pkt)
{
    pkt->gossip_cnt = 0;
    for (INT i = 0; i < gq_cnt && pkt->gossip_cnt < SWIM_GOSSIP_MAX; i++) {
        if (gq[i].remaining == 0) continue;
        pkt->gossip[pkt->gossip_cnt].node_id     = gq[i].node_id;
        pkt->gossip[pkt->gossip_cnt].state       = gq[i].state;
        pkt->gossip[pkt->gossip_cnt].incarnation = gq[i].incarnation;
        pkt->gossip[pkt->gossip_cnt]._pad        = 0;
        pkt->gossip_cnt++;
        gq[i].remaining--;
    }
    /* Compact retired entries */
    INT w = 0;
    for (INT i = 0; i < gq_cnt; i++)
        if (gq[i].remaining > 0) gq[w++] = gq[i];
    gq_cnt = w;
}

/* (incarnation, state) lexicographic last-writer-wins: does the incoming
 * rumor (inc_in, st_in) supersede what we currently hold (inc_cur, st_cur)?
 *   - strictly higher incarnation always wins;
 *   - same incarnation: a more-dead state wins (ALIVE<SUSPECT<DEAD);
 *   - strictly lower incarnation never wins (the stale rumor is refuted). */
static INT gossip_supersedes(UB inc_in, UB st_in, UB inc_cur, UB st_cur)
{
    if (inc_in != inc_cur) return inc_in > inc_cur;
    return state_rank(st_in) > state_rank(st_cur);
}

static void gossip_apply(const SWIM_PKT *pkt)
{
    for (UB i = 0; i < pkt->gossip_cnt && i < SWIM_GOSSIP_MAX; i++) {
        UB nid    = pkt->gossip[i].node_id;
        UB st     = pkt->gossip[i].state;
        UB inc_in = pkt->gossip[i].incarnation;

        /* 自分自身が SUSPECT/DEAD と噂されていたら断末魔散布 +
         * incarnation を進めて ALIVE を再表明し、古い噂を論駁する。 */
        if (nid == drpc_my_node) {
            if (st == DNODE_SUSPECT || st == DNODE_DEAD) {
                sw_puts("[swim] *** SELF-SUSPICION *** I'm rumored ");
                sw_puts(st == DNODE_SUSPECT ? "SUSPECT" : "DEAD");
                sw_puts(" — refuting with fresh incarnation\r\n");
                replica_scatter_all();
                /* refute: bump strictly above the rumor's incarnation so the
                 * ALIVE re-assertion supersedes the stale DEAD/SUSPECT
                 * everywhere it has spread (UB ++ wraps sanely at 256). */
                if (inc_in >= my_incarnation) my_incarnation = (UB)(inc_in + 1);
                else                          my_incarnation = (UB)(my_incarnation + 1);
                gossip_add(drpc_my_node, DNODE_ALIVE, my_incarnation);
            }
            continue;
        }

        if (nid >= DNODE_MAX) continue;

        /* NET-DISCOVERY-STAR (wave-discovery-mesh): TRANSITIVE membership.
         * A peer we have never heard of (UNKNOWN) cannot be introduced by the
         * (incarnation,state) LWW alone: state_rank(ALIVE)==state_rank(UNKNOWN)
         * and the rumour's incarnation usually ties ours (both 0 at bring-up),
         * so gossip_supersedes() returns false and the rumour is dropped. That
         * is the defect — standard SWIM disseminates MEMBERSHIP epidemically
         * (B tells A that C exists), but here a node could only ever learn a
         * peer from a DIRECT packet, capping the live mesh hub-centric.
         *
         * Fix: treat the FIRST non-DEAD sighting of an UNKNOWN peer as a
         * discovery — adopt it. The peer's address is deterministic
         * (node_base_ip / swim_node_ip == 10.1.0.(nid+1), pre-seeded into ARP
         * by drpc_init), so node_id alone resolves it; no address needs to ride
         * the gossip frame (no wire change). We seed dnode_table[nid].ip so any
         * later unicast (and `nodes`) has the right address even before our own
         * direct probe RTT lands. We do NOT fabricate an RTT: region_recompute
         * still requires a real swim_rtt_ms(), which the directed PING/ACK probe
         * measures directly within a couple of rounds. A DEAD rumour about
         * an UNKNOWN peer is ignored (we never saw it alive — gossiping it dead
         * would only seed a grave). */
        if (dnode_table[nid].state == DNODE_UNKNOWN &&
            (st == DNODE_ALIVE || st == DNODE_SUSPECT)) {
            dnode_table[nid].node_id = nid;
            dnode_table[nid].ip      = swim_node_ip(nid);
            dnode_table[nid].state   = st;
            dnode_table[nid].missed  = 0;
            dnode_incarn[nid]        = inc_in;
            suspect_count[nid]       = 0;
            sw_note(nid, DNODE_UNKNOWN, st);
            sw_puts("[swim] gossip: node "); sw_putdec(nid);
            sw_puts(st == DNODE_ALIVE ? " discovered (via gossip)\r\n"
                                      : " SUSPECT (via gossip)\r\n");
            gossip_add(nid, st, inc_in);   /* re-propagate: keep the epidemic alive */
            degrade_update();
            dmn_trigger();
            continue;
        }

        /* anti-stale: accept only if (incarnation,state) supersedes our view */
        if (!gossip_supersedes(inc_in, st, dnode_incarn[nid], dnode_table[nid].state))
            continue;

        dnode_incarn[nid] = inc_in;
        if (dnode_table[nid].state == st) continue;   /* incarnation bumped, state same */

        { UB oldst = dnode_table[nid].state; dnode_table[nid].state = st;
          sw_note(nid, oldst, st); }
        sw_puts("[swim] gossip: node "); sw_putdec(nid);
        if      (st == DNODE_ALIVE)   sw_puts(" -> ALIVE\r\n");
        else if (st == DNODE_SUSPECT) sw_puts(" -> SUSPECT\r\n");
        else if (st == DNODE_DEAD)    sw_puts(" -> DEAD\r\n");
        gossip_add(nid, st, inc_in);   /* re-propagate at the same incarnation */
        degrade_update();
        dmn_trigger();   /* ノード状態変化 = 外部刺激 */
    }
}

/* ------------------------------------------------------------------ */
/* Probe state                                                         */
/* ------------------------------------------------------------------ */

static ID probe_sem          = -1;     /* signalled by swim_rx on ACK    */
static UH probe_seq          = 1;
static UB probe_waiting_node = 0xFF;   /* node currently being probed    */
/* suspect_count[] defined above (consecutive no-response rounds) — moved up so
 * the transitive-discovery path in gossip_apply() can clear it on adoption. */

/* ------------------------------------------------------------------ */
/* RTT estimation (R0, regions design — docs/architecture/regions.md)  */
/*                                                                     */
/* 直接プローブ (SWIM_PING→SWIM_ACK) の往復時間を測り、ノードごとに      */
/* EWMA (alpha=1/4) で平滑化する。間接プローブはヘルパー経由で対象との    */
/* 直接 RTT にならないので測定対象外。region 形成と locality-aware MoE の  */
/* ゲーティングがこの値を消費する。                                      */
/* ------------------------------------------------------------------ */
static UW rtt_ewma_ms[DNODE_MAX];      /* 平滑化済み RTT (ms)            */
static UB rtt_valid  [DNODE_MAX];      /* 1 = 一度でも実測した           */

/* --- RTT シミュレーション (テスト専用; 本番では無効) ---------------- *
 * localhost では全ノードの実測 RTT がほぼ同じ (~数 ms) になり region が
 * 分割されない。region 形成ロジックを degenerate でなく検証するため、
 * ノード ID を zone_size でグループ化し (zone = id / zone_size)、異なる
 * zone のピアへ合成ペナルティを上乗せして観測 RTT を水増しする。
 * arch 非依存に保つため env は読まない: linux usermain が
 * swim_set_sim_zone() で注入する。zone_size==0 で無効。 */
static UB sim_zone_size    = 0;
static UW sim_zone_penalty = 0;

void swim_set_sim_zone(UB zone_size, UW penalty_ms)
{
    sim_zone_size    = zone_size;
    sim_zone_penalty = penalty_ms;
}

static void rtt_observe(UB node, UW sample_ms)
{
    if (node >= DNODE_MAX) return;
    if (!rtt_valid[node]) {
        rtt_ewma_ms[node] = sample_ms;
        rtt_valid[node]   = 1;
    } else {
        /* ewma = 3/4 old + 1/4 sample (+2 は四捨五入) */
        rtt_ewma_ms[node] = (rtt_ewma_ms[node] * 3 + sample_ms + 2) / 4;
    }
}

/* 公開: ノードへの平滑化 RTT(ms)。未実測は 0xFFFFFFFF を返す。
 * sim_zone が有効なら異 zone のピアへ合成ペナルティを上乗せする。 */
UW swim_rtt_ms(UB node)
{
    if (node >= DNODE_MAX || !rtt_valid[node]) return 0xFFFFFFFFUL;
    UW r = rtt_ewma_ms[node];
    if (sim_zone_size > 0 && drpc_my_node != 0xFF &&
        node / sim_zone_size != drpc_my_node / sim_zone_size) {
        r += sim_zone_penalty;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static UW swim_node_ip(UB n)
{
    /* 10.1.0.(n+1) — same scheme as drpc */
    return ((UW)(n + 1) << 24) | 0x0000010AUL;
}

static void swim_send(UW dst_ip, SWIM_PKT *pkt)
{
    gossip_fill(pkt);
    udp_send(dst_ip, SWIM_PORT, SWIM_PORT, (const UB *)pkt, (UH)sizeof(*pkt));
}

/* ------------------------------------------------------------------ */
/* UDP receive callback (registered on SWIM_PORT)                     */
/* ------------------------------------------------------------------ */

void swim_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_port;
    if (len < (UH)sizeof(SWIM_PKT)) return;

    const SWIM_PKT *pkt = (const SWIM_PKT *)data;
    if (pkt->magic != SWIM_MAGIC || pkt->version != SWIM_VERSION) return;

    /* Apply piggybacked gossip */
    gossip_apply(pkt);

    /* Mark sender alive */
    UB snid = pkt->src_node;
    if (snid < DNODE_MAX && snid != drpc_my_node) {
        UB old = dnode_table[snid].state;
        dnode_table[snid].ip     = src_ip;
        dnode_table[snid].state  = DNODE_ALIVE;
        if (old != DNODE_ALIVE) sw_note(snid, old, DNODE_ALIVE);
        dnode_table[snid].missed = 0;
        suspect_count[snid]      = 0;
        if (old != DNODE_ALIVE) {
            sw_puts("[swim] node "); sw_putdec(snid);
            sw_puts(old == DNODE_UNKNOWN ? " discovered" : " recovered");
            sw_puts("  (via rx)\r\n");
            gossip_add(snid, DNODE_ALIVE, dnode_incarn[snid]);
            replica_push_to(snid);
            degrade_update();
        }
    }

    switch (pkt->type) {

    case SWIM_PING: {
        /* Reply ACK directly to sender */
        SWIM_PKT ack = { 0 };
        ack.magic        = SWIM_MAGIC;
        ack.version      = SWIM_VERSION;
        ack.type         = SWIM_ACK;
        ack.seq          = pkt->seq;
        ack.src_node     = drpc_my_node;
        ack.probe_target = drpc_my_node;
        swim_send(src_ip, &ack);
        break;
    }

    case SWIM_ACK: {
        /* Wake probing task if seq + target match */
        if (probe_sem >= 0
            && pkt->seq          == probe_seq
            && pkt->probe_target == probe_waiting_node) {
            tk_sig_sem(probe_sem, 1);
        }
        break;
    }

    case SWIM_PING_REQ: {
        /*
         * Helper role: probe probe_target on behalf of src_node.
         * Forward SWIM_PING with original seq so the target's ACK
         * (sent back to us) can be forwarded to the original prober.
         * Simplification: we forward the PING; if the target is alive
         * it will send ACK to us, and we forward it to the requester.
         */
        UB target = pkt->probe_target;
        if (target >= DNODE_MAX || target == drpc_my_node) break;

        /* Ping the target */
        SWIM_PKT fwd = { 0 };
        fwd.magic        = SWIM_MAGIC;
        fwd.version      = SWIM_VERSION;
        fwd.type         = SWIM_PING;
        fwd.seq          = pkt->seq;     /* preserve seq for ACK matching */
        fwd.src_node     = drpc_my_node;
        fwd.probe_target = target;
        swim_send(swim_node_ip(target), &fwd);

        /*
         * Note: target's SWIM_ACK comes back to us (we sent the PING).
         * swim_rx will receive the ACK → if seq matches probe_seq and
         * probe_waiting_node matches, it wakes the original prober's sem.
         * This works because the original prober keeps probe_waiting_node
         * set while waiting on probe_sem.
         */
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Round-robin probe target selector                                   */
/* ------------------------------------------------------------------ */

static UB probe_cursor = 0;

static UB pick_probe_target(void)
{
    /* Try round-robin among known (non-unknown) nodes first */
    for (UB tries = 0; tries < DNODE_MAX; tries++) {
        probe_cursor = (UB)((probe_cursor + 1) % DNODE_MAX);
        if (probe_cursor == drpc_my_node) continue;
        if (dnode_table[probe_cursor].state != DNODE_UNKNOWN) return probe_cursor;
    }
    /* Fallback: any non-self node (trigger initial discovery) */
    for (UB i = 0; i < DNODE_MAX; i++) {
        if (i != drpc_my_node) return i;
    }
    return drpc_my_node;
}

static void pick_helpers(UB exclude, UB helpers[SWIM_K_HELPERS], INT *cnt)
{
    *cnt = 0;
    for (UB i = 0; i < DNODE_MAX && *cnt < SWIM_K_HELPERS; i++) {
        if (i == drpc_my_node || i == exclude) continue;
        if (dnode_table[i].state == DNODE_ALIVE) helpers[(*cnt)++] = i;
    }
}

/* ------------------------------------------------------------------ */
/* SWIM task — one probing round per SWIM_PROBE_INTERVAL_MS           */
/* ------------------------------------------------------------------ */

void swim_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    probe_sem = tk_cre_sem(&cs);

    for (;;) {
        tk_dly_tsk(SWIM_PROBE_INTERVAL_MS);
        if (drpc_my_node == 0xFF) continue;

        /* NET-DISCOVERY-STAR (wave-discovery-mesh): broadcast membership beacon.
         *
         * The defect: a peer left UNKNOWN until a DIRECT packet arrived, and the
         * only direct traffic a non-hub node reliably saw was its own directed
         * probe — whose all-UNKNOWN fallback always targeted the minimum-id node
         * (the hub). So two NON-hub peers never exchanged a direct packet and the
         * live mesh collapsed into a star centred on the lowest id; region_recompute
         * kept the two at size=1, capping the cluster hub-centric and blocking
         * federation.
         *
         * Fix (peer-symmetric, no central node, no wire change): once per round
         * each node emits ONE SWIM packet to the LIMITED BROADCAST address. Over
         * the relay (a broadcast fan-out bus) and over QEMU socket networking this
         * reaches EVERY node; swim_rx() already (a) marks the SENDER ALIVE on any
         * SWIM packet and (b) applies the piggybacked gossip. So a single beacon
         * makes A learn B directly AND carries B's knowledge of C to A
         * (transitive membership / infection-style dissemination). It is typed
         * SWIM_ACK with seq=0 so no receiver treats it as a probe to answer (no
         * ACK storm) and it never matches an outstanding probe's (seq,target).
         * Cost: one extra datagram per node per second — bounded and tiny. The
         * directed PING/ACK probing below is UNTOUCHED, so RTT measurement and
         * SUSPECT/DEAD liveness detection keep their exact prior cadence. */
        {
            SWIM_PKT beacon = { 0 };
            beacon.magic        = SWIM_MAGIC;
            beacon.version      = SWIM_VERSION;
            beacon.type         = SWIM_ACK;   /* seq=0 => not a probe reply to anyone */
            beacon.seq          = 0;
            beacon.src_node     = drpc_my_node;
            beacon.probe_target = drpc_my_node;
            swim_send(IP4(255, 255, 255, 255), &beacon);
        }

        UB target = pick_probe_target();
        if (target == drpc_my_node) continue;

        /* ---- Step 1: direct SWIM_PING ---- */
        probe_waiting_node = target;
        probe_seq++;
        if (probe_seq == 0) probe_seq = 1;

        SWIM_PKT ping = { 0 };
        ping.magic        = SWIM_MAGIC;
        ping.version      = SWIM_VERSION;
        ping.type         = SWIM_PING;
        ping.seq          = probe_seq;
        ping.src_node     = drpc_my_node;
        ping.probe_target = target;
        swim_send(swim_node_ip(target), &ping);
        SYSTIM t_ping; tk_get_otm(&t_ping);   /* RTT 計測開始 */

        ER er = tk_wai_sem(probe_sem, 1, SWIM_PROBE_TMO_MS);
        if (er == E_OK) {
            /* Direct probe succeeded — clean round-trip, record RTT. */
            SYSTIM t_ack; tk_get_otm(&t_ack);
            rtt_observe(target, (UW)(t_ack.lo - t_ping.lo));
            if (dnode_table[target].state != DNODE_ALIVE) {
                UB oldst = dnode_table[target].state;
                dnode_table[target].state = DNODE_ALIVE;
                sw_note(target, oldst, DNODE_ALIVE);
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (direct probe)\r\n");
                gossip_add(target, DNODE_ALIVE, dnode_incarn[target]);
                replica_push_to(target);
            }
            suspect_count[target] = 0;
            continue;
        }

        /* ---- Step 2: indirect probe via K helpers ---- */
        UB helpers[SWIM_K_HELPERS];
        INT hcnt = 0;
        pick_helpers(target, helpers, &hcnt);

        for (INT h = 0; h < hcnt; h++) {
            SWIM_PKT preq = { 0 };
            preq.magic        = SWIM_MAGIC;
            preq.version      = SWIM_VERSION;
            preq.type         = SWIM_PING_REQ;
            preq.seq          = probe_seq;
            preq.src_node     = drpc_my_node;
            preq.probe_target = target;
            swim_send(swim_node_ip(helpers[h]), &preq);
        }

        er = tk_wai_sem(probe_sem, 1, SWIM_IND_TMO_MS);
        if (er == E_OK) {
            if (dnode_table[target].state != DNODE_ALIVE) {
                UB oldst = dnode_table[target].state;
                dnode_table[target].state = DNODE_ALIVE;
                sw_note(target, oldst, DNODE_ALIVE);
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (indirect probe)\r\n");
                gossip_add(target, DNODE_ALIVE, dnode_incarn[target]);
                replica_push_to(target);
            }
            suspect_count[target] = 0;
            continue;
        }

        /* ---- Step 3: no response — escalate state ---- */
        suspect_count[target]++;
        UB st = dnode_table[target].state;

        /* UNKNOWN nodes are not "suspected dead" — we've simply never
         * confirmed them alive yet. Gossiping a never-seen node as SUSPECT
         * makes the peer self-suspect and trigger death throes during the
         * normal discovery race (esp. before ARP/relay routes warm up), so
         * leave UNKNOWN nodes untouched and let an actual rx mark them ALIVE.
         *
         * For ALIVE nodes, require SWIM_SUSPECT_ROUNDS consecutive misses
         * before SUSPECT: with only 2 nodes there are no indirect helpers,
         * so a single dropped UDP datagram must not be a death sentence. */
        if (st == DNODE_ALIVE && suspect_count[target] >= SWIM_SUSPECT_ROUNDS) {
            dnode_table[target].state  = DNODE_SUSPECT;
            sw_note(target, DNODE_ALIVE, DNODE_SUSPECT);
            dnode_table[target].missed = 0;
            suspect_count[target]      = 0;   /* count fresh toward DEAD */
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> SUSPECT (no response)\r\n");
            gossip_add(target, DNODE_SUSPECT, dnode_incarn[target]);
        } else if (st == DNODE_SUSPECT && suspect_count[target] >= SWIM_DEAD_ROUNDS) {
            dnode_table[target].state  = DNODE_DEAD;
            sw_note(target, DNODE_SUSPECT, DNODE_DEAD);
            dnode_table[target].missed = 0;
            suspect_count[target]      = 0;
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> DEAD\r\n");
            gossip_add(target, DNODE_DEAD, dnode_incarn[target]);
            heal_on_node_dead(target);
            degrade_update();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void swim_init(void)
{
    for (INT i = 0; i < DNODE_MAX; i++) { suspect_count[i] = 0; dnode_incarn[i] = 0; }
    my_incarnation = 0;
    udp_bind(SWIM_PORT, swim_rx);
    sw_puts("[swim] SWIM ready  port=7375\r\n");
}

/* ------------------------------------------------------------------ */
/* Cluster display (shell `nodes`)                                     */
/* ------------------------------------------------------------------ */

static const char *state_str(UB st)
{
    switch (st) {
    case DNODE_ALIVE:   return "ALIVE  ";
    case DNODE_SUSPECT: return "SUSPECT";
    case DNODE_DEAD:    return "DEAD   ";
    default:            return "unknown";
    }
}

void swim_nodes_print(void)
{
    sw_puts("[cluster]  ID  State    IP              (SWIM + DRPC)\r\n");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) {
            sw_puts("            "); sw_putdec(n);
            sw_puts("  SELF     "); sw_puts(ip_str(NET_MY_IP)); sw_puts("\r\n");
        } else if (dnode_table[n].state != DNODE_UNKNOWN) {
            sw_puts("            "); sw_putdec(n);
            sw_puts("  "); sw_puts(state_str(dnode_table[n].state));
            sw_puts("  "); sw_puts(ip_str(dnode_table[n].ip));
            {
                UW rtt = swim_rtt_ms(n);   /* sim_zone ペナルティ込み */
                if (rtt != 0xFFFFFFFFUL) {
                    sw_puts("  rtt="); sw_putdec(rtt); sw_puts("ms");
                } else {
                    sw_puts("  rtt=?");
                }
            }
            if (dnode_table[n].state == DNODE_SUSPECT) {
                sw_puts("  (suspect_cnt="); sw_putdec(suspect_count[n]); sw_puts(")");
            }
            sw_puts("\r\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* SWIM-INCARN self-test (external audit 2026-06-13, gap-ledger)       */
/*                                                                     */
/* Drives the REAL gossip_apply()/gossip_supersedes() with crafted     */
/* SWIM_PKTs and asserts the canonical anti-stale-rumor behaviour:     */
/*  (a) a STALE DEAD rumor about SELF is REFUTED — the node is NOT      */
/*      marked dead, my_incarnation is bumped strictly above the rumor, */
/*      and an ALIVE-about-self is queued at the fresh incarnation;     */
/*  (b) a peer's stale LOWER-incarnation DEAD does NOT override the     */
/*      higher-incarnation ALIVE we already hold (the dead mechanism    */
/*      is now alive), while a strictly HIGHER-incarnation rumor does.  */
/* Emits "[swim-incarn] PASS" (returns 0) or "[swim-incarn] FAIL ...". */
/* ------------------------------------------------------------------ */

/* find the incarnation a node_id is currently queued for in gq[] (or -1) */
static INT gq_find_incarn(UB node_id, UB state)
{
    for (INT i = 0; i < gq_cnt; i++)
        if (gq[i].node_id == node_id && gq[i].state == state && gq[i].remaining > 0)
            return (INT)gq[i].incarnation;
    return -1;
}

INT swim_incarn_self_test(void (*emit)(const char *))
{
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* --- save global/file state we will perturb, restore at the end --- */
    UB  saved_my      = drpc_my_node;
    UB  saved_inc     = my_incarnation;
    INT saved_gq_cnt  = gq_cnt;

    const UB SELF = 0;          /* pretend to be node 0 for the test */
    const UB PEER = 1;

    drpc_my_node            = SELF;
    gq_cnt                  = 0;            /* clear the gossip queue */
    my_incarnation          = 0;
    dnode_table[SELF].state = DNODE_ALIVE;
    dnode_incarn[SELF]      = 0;
    dnode_table[PEER].state = DNODE_UNKNOWN;
    dnode_incarn[PEER]      = 0;

    /* (a) a STALE DEAD rumor about SELF at incarnation 1 ------------- */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt           = 1;
        p.gossip[0].node_id    = SELF;
        p.gossip[0].state      = DNODE_DEAD;
        p.gossip[0].incarnation = 1;
        gossip_apply(&p);

        if (dnode_table[SELF].state == DNODE_DEAD) {
            say("[swim-incarn] FAIL self-marked-dead (stale DEAD rumor killed me)\r\n");
            fails++;
        }
        if (my_incarnation <= 1) {
            say("[swim-incarn] FAIL self-incarnation-not-bumped-above-rumor\r\n");
            fails++;
        }
        INT q = gq_find_incarn(SELF, DNODE_ALIVE);
        if (q < 0) {
            say("[swim-incarn] FAIL self-ALIVE-refutation-not-queued\r\n");
            fails++;
        } else if ((UB)q != my_incarnation) {
            say("[swim-incarn] FAIL self-refutation-incarnation-mismatch\r\n");
            fails++;
        }
    }

    /* (b) peer anti-stale LWW: hold PEER ALIVE @ incarnation 5 ------- */
    dnode_table[PEER].state = DNODE_ALIVE;
    dnode_incarn[PEER]      = 5;
    {
        /* stale LOWER-incarnation DEAD must be IGNORED */
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = PEER;
        p.gossip[0].state       = DNODE_DEAD;
        p.gossip[0].incarnation = 3;
        gossip_apply(&p);
        if (dnode_table[PEER].state != DNODE_ALIVE) {
            say("[swim-incarn] FAIL peer-killed-by-stale-rumor (LWW dead)\r\n");
            fails++;
        }
    }
    {
        /* strictly HIGHER-incarnation DEAD MUST be accepted */
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = PEER;
        p.gossip[0].state       = DNODE_DEAD;
        p.gossip[0].incarnation = 6;
        gossip_apply(&p);
        if (dnode_table[PEER].state != DNODE_DEAD) {
            say("[swim-incarn] FAIL fresh-rumor-not-accepted (LWW stuck)\r\n");
            fails++;
        }
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    dnode_table[SELF].state = DNODE_UNKNOWN;
    dnode_table[PEER].state = DNODE_UNKNOWN;
    dnode_incarn[SELF]      = 0;
    dnode_incarn[PEER]      = 0;

    if (fails == 0) say("[swim-incarn] PASS\r\n");
    return fails;
}
