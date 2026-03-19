/*
 *  drpc.c (x86)
 *  Distributed T-Kernel RPC
 *
 *  Transport : UDP port 7374 (point-to-point via QEMU socket networking)
 *  Discovery : HEARTBEAT packet broadcast every 500 ms
 *  RPC       : synchronous REQ/REPLY (3 s timeout, 3 ARP retries)
 *
 *  Remote function table (func_id → task function):
 *    0x0001  hello    — print "Hello from node X" on the target
 *    0x0002  counter  — count 1..5 with 1 s delay on the target
 *
 *  Demo (two QEMU terminals):
 *
 *    [node 0]  p-kernel> dsem new
 *              [dsem] global semaphore: 0x00000003
 *              p-kernel> dsem wai 0x00000003
 *              [dsem] waiting...
 *
 *    [node 1]  p-kernel> nodes
 *              p-kernel> dsem sig 0x00000003   ← wakes node 0
 *
 *    [node 0]  [dsem] woke!
 *
 *    [node 1]  p-kernel> dtask 0 hello         ← runs task on node 0
 *    [node 0]  [remote] Hello from node 1!
 */

#include "drpc.h"
#include "netstack.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Serial output helpers                                               */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void dp_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void dp_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dp_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dp_puts(&buf[i]);
}

static void dp_puthex(UW v)
{
    char buf[11]; buf[0]='0'; buf[1]='x'; buf[10]='\0';
    for (INT i = 9; i >= 2; i--) {
        INT d = (INT)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    dp_puts(buf);
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

UB    drpc_my_node = 0xFF;
DNODE dnode_table[DNODE_MAX];

/* IP for node n: 10.1.0.(n+1)  →  IP4(10,1,0,n+1) = (n+1)<<24 | 0x0000010A */
static UW node_base_ip(UB n)
{
    return ((UW)(n + 1) << 24) | 0x0000010AUL;
}

/* ------------------------------------------------------------------ */
/* Pending RPC call table                                              */
/* ------------------------------------------------------------------ */

#define MAX_PENDING  4

typedef struct {
    UH  seq;
    UB  in_use;
    UB  _pad;
    ID  sem;
    W   result;
} DRPC_PENDING;

static DRPC_PENDING pending[MAX_PENDING];
static UH           drpc_seq_ctr = 1;

/* ------------------------------------------------------------------ */
/* Remote function table                                               */
/* ------------------------------------------------------------------ */

static void remote_hello(INT stacd, void *exinf)
{
    (void)stacd;
    const char *src = exinf ? (const char *)exinf : "?";
    dp_puts("\r\n[remote] Hello from node "); dp_puts(src); dp_puts("!\r\n");
    dp_puts("p-kernel> ");   /* re-print prompt so UX looks clean */
    tk_exd_tsk();
}

static void remote_counter(INT stacd, void *exinf)
{
    (void)exinf;
    dp_puts("\r\n[remote] Counter started (from node ");
    dp_putdec((UW)stacd); dp_puts(")\r\n");
    for (INT i = 1; i <= 5; i++) {
        dp_puts("[remote] count = "); dp_putdec((UW)i); dp_puts("\r\n");
        tk_dly_tsk(1000);
    }
    dp_puts("[remote] Counter done\r\n");
    dp_puts("p-kernel> ");
    tk_exd_tsk();
}

typedef void (*RTASK_FN)(INT, void *);
typedef struct { UH id; RTASK_FN fn; const char *name; } RFUNC_ENTRY;

static const RFUNC_ENTRY rfunc_table[] = {
    { 0x0001, remote_hello,   "hello"   },
    { 0x0002, remote_counter, "counter" },
    { 0,      NULL,            NULL     }
};

/* ------------------------------------------------------------------ */
/* Local task creation (called for DRPC_CALL_CRE_TSK)                 */
/* ------------------------------------------------------------------ */

/* caller_node_str: static per-caller-node string (safe for demo use) */
static char caller_node_strs[DNODE_MAX][4];

static W drpc_local_cre_tsk(UH func_id, INT pri, UB caller_node)
{
    RTASK_FN fn = NULL;
    for (INT i = 0; rfunc_table[i].fn; i++) {
        if (rfunc_table[i].id == func_id) { fn = rfunc_table[i].fn; break; }
    }
    if (!fn) { dp_puts("[drpc] unknown func_id\r\n"); return -1; }

    if (caller_node < DNODE_MAX) {
        caller_node_strs[caller_node][0] = (char)('0' + caller_node);
        caller_node_strs[caller_node][1] = '\0';
    }

    T_CTSK ct = {
        .exinf   = (VP)((caller_node < DNODE_MAX)
                        ? caller_node_strs[caller_node] : "?"),
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)fn,
        .itskpri = (pri >= 1 && pri <= 10) ? pri : 4,
        .stksz   = 4096
    };
    ID tid = tk_cre_tsk(&ct);
    if (tid < E_OK) return (W)tid;
    tk_sta_tsk(tid, (INT)caller_node);
    return (W)tid;
}

/* ------------------------------------------------------------------ */
/* RPC dispatch (called on the receiving end)                          */
/* ------------------------------------------------------------------ */

static W drpc_dispatch(UB src, UH call_id, UW obj_id, W a0, W a1, W a2)
{
    (void)a1; (void)a2;
    switch (call_id) {
    case DRPC_CALL_PING:
        return (W)drpc_my_node;
    case DRPC_CALL_CRE_TSK:
        return drpc_local_cre_tsk((UH)obj_id, (INT)a0, src);
    case DRPC_CALL_SIG_SEM:
        return (W)tk_sig_sem((ID)GOBJ_LOCAL(obj_id), (INT)a0);
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* UDP receive callback (registered on DRPC_PORT)                     */
/* ------------------------------------------------------------------ */

void drpc_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_port;
    if (len < (UH)sizeof(DRPC_PKT)) return;
    const DRPC_PKT *pkt = (const DRPC_PKT *)data;
    if (pkt->magic != DRPC_MAGIC || pkt->version != DRPC_VERSION) return;

    switch (pkt->type) {

    case DRPC_HEARTBEAT: {
        UB nid = pkt->src_node;
        if (nid < DNODE_MAX && nid != drpc_my_node) {
            dnode_table[nid].node_id = nid;
            dnode_table[nid].ip      = src_ip;
            dnode_table[nid].alive   = 1;
            dnode_table[nid].age     = 0;
        }
        break;
    }

    case DRPC_REQ: {
        if (pkt->dst_node != drpc_my_node) return;
        W result = drpc_dispatch(pkt->src_node, pkt->call_id, pkt->obj_id,
                                 pkt->arg[0], pkt->arg[1], pkt->arg[2]);
        DRPC_PKT reply = *pkt;
        reply.type     = DRPC_REPLY;
        reply.src_node = drpc_my_node;
        reply.dst_node = pkt->src_node;
        reply.result   = result;
        udp_send(src_ip, DRPC_PORT, DRPC_PORT,
                 (const UB *)&reply, (UH)sizeof(reply));
        break;
    }

    case DRPC_REPLY: {
        for (INT i = 0; i < MAX_PENDING; i++) {
            if (pending[i].in_use && pending[i].seq == pkt->seq) {
                pending[i].result = pkt->result;
                tk_sig_sem(pending[i].sem, 1);
                break;
            }
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Blocking RPC call                                                   */
/* ------------------------------------------------------------------ */

W drpc_call(UB dst_node, UH call_id, UW obj_id, W a0, W a1, W a2)
{
    /* Find a free pending slot */
    INT slot = -1;
    for (INT i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { dp_puts("[drpc] all slots busy\r\n"); return -1; }

    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    ID sem = tk_cre_sem(&cs);
    if (sem < E_OK) return (W)sem;

    UH seq = drpc_seq_ctr++;
    if (drpc_seq_ctr == 0) drpc_seq_ctr = 1;

    pending[slot] = (DRPC_PENDING){ seq, 1, 0, sem, -1 };

    /* Build request packet */
    DRPC_PKT pkt = { 0 };
    pkt.magic    = DRPC_MAGIC;
    pkt.version  = DRPC_VERSION;
    pkt.type     = DRPC_REQ;
    pkt.seq      = seq;
    pkt.src_node = drpc_my_node;
    pkt.dst_node = dst_node;
    pkt.call_id  = call_id;
    pkt.obj_id   = obj_id;
    pkt.arg[0]   = a0;
    pkt.arg[1]   = a1;
    pkt.arg[2]   = a2;

    UW dst_ip = node_base_ip(dst_node);

    /* Send with ARP retry */
    for (INT r = 0; r < 3; r++) {
        if (udp_send(dst_ip, DRPC_PORT, DRPC_PORT,
                     (const UB *)&pkt, (UH)sizeof(pkt)) == 0) break;
        tk_dly_tsk(200);
    }

    /* Wait up to 3 s for reply */
    ER er = tk_wai_sem(sem, 1, 3000);

    W result = pending[slot].result;
    pending[slot].seq    = 0;
    pending[slot].in_use = 0;
    tk_del_sem(sem);

    if (er != E_OK) { dp_puts("[drpc] RPC timeout\r\n"); return -1; }
    return result;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

void drpc_init(UB my_node_id, UW my_ip)
{
    drpc_my_node = my_node_id;
    net_my_ip    = my_ip;

    dnode_table[my_node_id].node_id = my_node_id;
    dnode_table[my_node_id].ip      = my_ip;
    dnode_table[my_node_id].alive   = 1;
    dnode_table[my_node_id].age     = 0;

    udp_bind(DRPC_PORT, drpc_rx);

    dp_puts("[drpc] node ");  dp_putdec(my_node_id);
    dp_puts("  IP=");         dp_puts(ip_str(my_ip));
    dp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* Heartbeat task (priority 5)                                         */
/* ------------------------------------------------------------------ */

void drpc_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    DRPC_PKT hb = { 0 };
    hb.magic    = DRPC_MAGIC;
    hb.version  = DRPC_VERSION;
    hb.type     = DRPC_HEARTBEAT;
    hb.src_node = drpc_my_node;

    for (;;) {
        tk_dly_tsk(500);

        /* Heartbeat to all potential peers (unresolved ARP fails silently) */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) continue;
            udp_send(node_base_ip(n), DRPC_PORT, DRPC_PORT,
                     (const UB *)&hb, (UH)sizeof(hb));
        }

        /* Age out stale nodes */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node || !dnode_table[n].alive) continue;
            if (++dnode_table[n].age > 6) {
                dnode_table[n].alive = 0;
                dp_puts("[drpc] node "); dp_putdec(n); dp_puts(" timed out\r\n");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Node table display                                                  */
/* ------------------------------------------------------------------ */

void drpc_nodes_list(void)
{
    dp_puts("[nodes]  ID  IP            Status\r\n");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) {
            dp_puts("         "); dp_putdec(n); dp_puts("   ");
            dp_puts(ip_str(net_my_ip)); dp_puts("  (self)\r\n");
        } else if (dnode_table[n].alive) {
            dp_puts("         "); dp_putdec(n); dp_puts("   ");
            dp_puts(ip_str(dnode_table[n].ip));
            dp_puts("  alive  (age="); dp_putdec(dnode_table[n].age);
            dp_puts(")\r\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Distributed T-Kernel API                                            */
/* ------------------------------------------------------------------ */

W dtk_cre_tsk(UB node_id, UH func_id, INT pri)
{
    if (node_id == drpc_my_node)
        return drpc_local_cre_tsk(func_id, pri, drpc_my_node);

    dp_puts("[dtask] -> node "); dp_putdec(node_id);
    dp_puts("  func="); dp_puthex(func_id); dp_puts("\r\n");
    return drpc_call(node_id, DRPC_CALL_CRE_TSK, (UW)func_id, (W)pri, 0, 0);
}

UW dtk_cre_sem(INT isemcnt)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isemcnt, .maxsem = 64 };
    ID sid = tk_cre_sem(&cs);
    if (sid < E_OK) return (UW)-1;
    return GOBJ_MAKE(drpc_my_node, (UW)sid);
}

ER dtk_wai_sem(UW gsemid, INT cnt, TMO tmout)
{
    if (GOBJ_NODE(gsemid) != drpc_my_node) {
        dp_puts("[drpc] dtk_wai_sem: semaphore must be on local node\r\n");
        return E_NOSPT;
    }
    return tk_wai_sem((ID)GOBJ_LOCAL(gsemid), cnt, tmout);
}

ER dtk_sig_sem(UW gsemid, INT cnt)
{
    if (GOBJ_NODE(gsemid) == drpc_my_node)
        return tk_sig_sem((ID)GOBJ_LOCAL(gsemid), cnt);

    dp_puts("[dsem] sig -> node "); dp_putdec(GOBJ_NODE(gsemid)); dp_puts("\r\n");
    W r = drpc_call(GOBJ_NODE(gsemid), DRPC_CALL_SIG_SEM, gsemid, (W)cnt, 0, 0);
    return (r >= 0) ? E_OK : (ER)r;
}
