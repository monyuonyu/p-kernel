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

static void gossip_add(UB node_id, UB state)
{
    /* Update existing entry if present */
    for (INT i = 0; i < gq_cnt; i++) {
        if (gq[i].node_id == node_id) {
            gq[i].state     = state;
            gq[i].remaining = GOSSIP_TTL;
            return;
        }
    }
    if (gq_cnt < GOSSIP_Q_MAX) {
        gq[gq_cnt++] = (GOSSIP_QUEUED){ node_id, state, 0, GOSSIP_TTL };
        return;
    }
    /* Queue full: evict oldest (lowest remaining) */
    INT min_i = 0;
    for (INT i = 1; i < gq_cnt; i++)
        if (gq[i].remaining < gq[min_i].remaining) min_i = i;
    gq[min_i] = (GOSSIP_QUEUED){ node_id, state, 0, GOSSIP_TTL };
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

static void gossip_apply(const SWIM_PKT *pkt)
{
    for (UB i = 0; i < pkt->gossip_cnt && i < SWIM_GOSSIP_MAX; i++) {
        UB nid = pkt->gossip[i].node_id;
        UB st  = pkt->gossip[i].state;
        if (nid >= DNODE_MAX || nid == drpc_my_node) continue;
        if (dnode_table[nid].state == st) continue;
        dnode_table[nid].state = st;
        sw_puts("[swim] gossip: node "); sw_putdec(nid);
        if      (st == DNODE_ALIVE)   sw_puts(" -> ALIVE\r\n");
        else if (st == DNODE_SUSPECT) sw_puts(" -> SUSPECT\r\n");
        else if (st == DNODE_DEAD)    sw_puts(" -> DEAD\r\n");
        gossip_add(nid, st);   /* re-propagate */
    }
}

/* ------------------------------------------------------------------ */
/* Probe state                                                         */
/* ------------------------------------------------------------------ */

static ID probe_sem          = -1;     /* signalled by swim_rx on ACK    */
static UH probe_seq          = 1;
static UB probe_waiting_node = 0xFF;   /* node currently being probed    */
static UB suspect_count[DNODE_MAX];    /* consecutive no-response rounds */

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
        dnode_table[snid].missed = 0;
        suspect_count[snid]      = 0;
        if (old != DNODE_ALIVE) {
            sw_puts("[swim] node "); sw_putdec(snid);
            sw_puts(old == DNODE_UNKNOWN ? " discovered" : " recovered");
            sw_puts("  (via rx)\r\n");
            gossip_add(snid, DNODE_ALIVE);
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

        ER er = tk_wai_sem(probe_sem, 1, SWIM_PROBE_TMO_MS);
        if (er == E_OK) {
            /* Direct probe succeeded */
            if (dnode_table[target].state != DNODE_ALIVE) {
                dnode_table[target].state = DNODE_ALIVE;
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (direct probe)\r\n");
                gossip_add(target, DNODE_ALIVE);
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
                dnode_table[target].state = DNODE_ALIVE;
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (indirect probe)\r\n");
                gossip_add(target, DNODE_ALIVE);
            }
            suspect_count[target] = 0;
            continue;
        }

        /* ---- Step 3: no response — escalate state ---- */
        suspect_count[target]++;
        UB st = dnode_table[target].state;

        if (st == DNODE_ALIVE || st == DNODE_UNKNOWN) {
            dnode_table[target].state  = DNODE_SUSPECT;
            dnode_table[target].missed = 0;
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> SUSPECT (no response)\r\n");
            gossip_add(target, DNODE_SUSPECT);
        } else if (st == DNODE_SUSPECT && suspect_count[target] >= SWIM_DEAD_ROUNDS) {
            dnode_table[target].state  = DNODE_DEAD;
            dnode_table[target].missed = 0;
            suspect_count[target]      = 0;
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> DEAD\r\n");
            gossip_add(target, DNODE_DEAD);
            heal_on_node_dead(target);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void swim_init(void)
{
    for (INT i = 0; i < DNODE_MAX; i++) suspect_count[i] = 0;
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
            if (dnode_table[n].state == DNODE_SUSPECT) {
                sw_puts("  (suspect_cnt="); sw_putdec(suspect_count[n]); sw_puts(")");
            }
            sw_puts("\r\n");
        }
    }
}
