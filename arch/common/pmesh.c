/*
 *  pmesh.c (x86)
 *  p-mesh — カーネルネイティブ メッシュルーティング層
 *
 *  Distance Vector ルーティング:
 *    1. pmesh_task が 2 秒ごとに PMESH_BEACON を全隣接ノードへ送信
 *    2. BEACON を受け取ったノードは Bellman-Ford でルーティングテーブルを更新
 *    3. pmesh_send() は宛先ノードへの経路を検索して転送
 *    4. 中間ノードは PMESH_DATA をそのまま next_hop へ横流し
 *    5. PMESH_ROUTE_EXPIRE 周期以上更新のない経路は自動破棄
 */

#include "pmesh.h"
#include "netstack.h"
#include "kernel.h"
#include "pk_nosw.h"

/* ------------------------------------------------------------------ */
/* シリアル出力ヘルパ                                                 */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void pm_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void pm_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { pm_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    pm_puts(&buf[i]);
}

static void pm_puthex1(UB v)
{
    const char *h = "0123456789abcdef";
    char buf[3]; buf[0] = h[v >> 4]; buf[1] = h[v & 0xF]; buf[2] = '\0';
    pm_puts(buf);
}

/* ------------------------------------------------------------------ */
/* グローバル状態                                                      */
/* ------------------------------------------------------------------ */

PMESH_ROUTE  pmesh_routes[DNODE_MAX];
PMESH_STATS  pmesh_stats;

/* Packets rejected as malformed (out-of-range dst_node). Read by the
 * rx-bounds cert (ROADMAP 0-9); not exported in a header. */
UW pmesh_drop_bad = 0;

#if defined(_TK_HOSTED_LIBC_)
#include "swim.h"
PMESH_V2_STATS pmesh_v2_stats;
/* self-test seams (NULL in a live node): the link and the "reads v2" answer */
static INT  (*pm_tx_tap)(UW ip, const UB *d, UH n);
static BOOL (*pm_reads_v2_hook)(pk_node_t id);
static INT pm_link_out(UW ip, const UB *d, UH n)
{
    if (pm_tx_tap) return pm_tx_tap(ip, d, n);
    return udp_send(ip, PMESH_PORT, PMESH_PORT, d, n);
}
static BOOL pm_reads_v2(pk_node_t id)
{
    return pm_reads_v2_hook ? pm_reads_v2_hook(id) : swim_peer_reads_v2(id);
}
#endif

/* ------------------------------------------------------------------ */
/* ローカル配送テーブル (pmesh_bind)                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    UH           port;
    pmesh_recv_fn fn;
    UB           active;
} PMESH_SOCK;

static PMESH_SOCK pmesh_socks[PMESH_BIND_MAX];

W pmesh_bind(UH port, pmesh_recv_fn fn)
{
    for (INT i = 0; i < PMESH_BIND_MAX; i++) {
        if (pmesh_socks[i].active) continue;
        pmesh_socks[i].port   = port;
        pmesh_socks[i].fn     = fn;
        pmesh_socks[i].active = 1;
        return 0;
    }
    return -1;
}

static void local_dispatch(pk_node_t src_node, UH dst_port,
                           const UB *data, UH len)
{
    for (INT i = 0; i < PMESH_BIND_MAX; i++) {
        if (!pmesh_socks[i].active) continue;
        if (pmesh_socks[i].port != dst_port) continue;
        pmesh_socks[i].fn(src_node, dst_port, data, len);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* ユーティリティ                                                      */
/* ------------------------------------------------------------------ */

/* ノード n の IP アドレス: 10.1.0.(n+1) */
static UW node_ip(pk_node_t n)
{
    return pk_node_ip(n);
}

static void pm_memcpy(void *dst, const void *src, UH n)
{
    const UB *s = (const UB *)src;
    UB       *d = (UB *)dst;
    for (UH i = 0; i < n; i++) d[i] = s[i];
}

static void pm_memzero(void *dst, UH n)
{
    UB *d = (UB *)dst;
    for (UH i = 0; i < n; i++) d[i] = 0;
}

/* ------------------------------------------------------------------ */
/* ルーティングテーブル操作                                            */
/* ------------------------------------------------------------------ */

/* 直接隣接ノード（dnode_table で ALIVE なノード）の経路を cost=1 で登録 */
static void update_direct(pk_node_t node_id)
{
    if (node_id >= DNODE_MAX || node_id == drpc_my_node) return;
    if (!pmesh_routes[node_id].active ||
        pmesh_routes[node_id].cost > 1) {
        pmesh_routes[node_id].next_hop = node_id;
        pmesh_routes[node_id].cost     = 1;
        pmesh_routes[node_id].active   = 1;
    }
    pmesh_routes[node_id].age = 0;
}

/* Bellman-Ford: src_node から受け取ったルーティングテーブルで更新 */
static void merge_beacon(pk_node_t src_node,
                         const PMESH_ROUTE_ENTRY *entries, UB cnt)
{
    /* まず src_node 自身を直接ノードとして登録 */
    update_direct(src_node);

    for (UB i = 0; i < cnt; i++) {
        pk_node_t dst  = entries[i].dst_node;
        UB cost = entries[i].cost;

        if (dst >= DNODE_MAX)          continue;
        if (dst == drpc_my_node)       continue;
        if (dst == src_node)           continue;  /* すでに登録済み */
        if (cost == PMESH_COST_INF)    continue;

        /* src_node 経由のコスト = cost + 1 */
        UB new_cost = (UB)(cost + 1);
        if (new_cost == 0) new_cost = PMESH_COST_INF; /* オーバーフロー */

        if (!pmesh_routes[dst].active ||
            pmesh_routes[dst].cost > new_cost) {
            pmesh_routes[dst].next_hop = src_node;
            pmesh_routes[dst].cost     = new_cost;
            pmesh_routes[dst].active   = 1;
            pmesh_routes[dst].age      = 0;

            pm_puts("[pmesh] route learned: node ");
            pm_putdec(dst);
            pm_puts(" via node ");
            pm_putdec(src_node);
            pm_puts("  cost=");
            pm_putdec(new_cost);
            pm_puts("\r\n");
        } else {
            pmesh_routes[dst].age = 0;  /* 既存経路のエージをリセット */
        }
    }
}

/* 古い経路を破棄 */
static void age_routes(void)
{
    for (INT i = 0; i < DNODE_MAX; i++) {
        if (!pmesh_routes[i].active) continue;
        pmesh_routes[i].age++;
        if (pmesh_routes[i].age > PMESH_ROUTE_EXPIRE) {
            pmesh_routes[i].active   = 0;
            pmesh_routes[i].next_hop = 0xFF;
            pmesh_routes[i].cost     = PMESH_COST_INF;
        }
    }
}

/* ------------------------------------------------------------------ */
/* BEACON 送信                                                         */
/* ------------------------------------------------------------------ */

static PMESH_BEACON_PKT beacon_pkt;  /* static: スタック節約 */

static void send_beacon(void)
{
    if (drpc_my_node == 0xFF) return;

    pm_memzero(&beacon_pkt, sizeof(beacon_pkt));
    beacon_pkt.magic    = PMESH_MAGIC;
    beacon_pkt.version  = PMESH_VERSION;
    beacon_pkt.type     = PMESH_BEACON;
    beacon_pkt.src_node = pk_node_v1(drpc_my_node);
    beacon_pkt.entry_cnt = 0;

    /* 自分自身 (cost=0) */
    beacon_pkt.entries[beacon_pkt.entry_cnt].dst_node = pk_node_v1(drpc_my_node);
    beacon_pkt.entries[beacon_pkt.entry_cnt].cost     = 0;
    beacon_pkt.entry_cnt++;

    /* 既知の全経路 */
    for (INT i = 0; i < DNODE_MAX && beacon_pkt.entry_cnt < DNODE_MAX; i++) {
        if (!pmesh_routes[i].active) continue;
        beacon_pkt.entries[beacon_pkt.entry_cnt].dst_node = (UB)i;
        beacon_pkt.entries[beacon_pkt.entry_cnt].cost     = pmesh_routes[i].cost;
        beacon_pkt.entry_cnt++;
    }

    UH pkt_len = (UH)(8 + (UH)beacon_pkt.entry_cnt *
                       (UH)sizeof(PMESH_ROUTE_ENTRY));

    /* 全ノードへ送信（直接届かないノードも含む）*/
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        udp_send(node_ip(n), PMESH_PORT, PMESH_PORT,
                 (const UB *)&beacon_pkt, pkt_len);
    }
    pmesh_stats.beacon_tx++;
}

/* ------------------------------------------------------------------ */
/* UDP 受信コールバック                                                */
/* ------------------------------------------------------------------ */

void pmesh_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_ip; (void)src_port;
    if (len < 8) return;

    /* UDP payload sits at frame offset 42 (eth14 + ip20 + udp8), which is
     * not 4-byte aligned. With MMU off Cortex-A53 treats RAM as
     * Device-nGnRnE and traps unaligned wide reads, so load magic
     * byte-by-byte. */
    UW magic;
    __builtin_memcpy(&magic, data, sizeof magic);
    if (magic != PMESH_MAGIC)        return;
    if (data[4] != PMESH_VERSION)    return;

    UB type     = data[5];
    pk_node_t src_node = data[6];

    /* ---- BEACON ---------------------------------------------------- */
    if (type == PMESH_BEACON) {
        if (src_node >= DNODE_MAX || src_node == drpc_my_node) return;
        pmesh_stats.beacon_rx++;

        UB entry_cnt = data[7];
        if ((UH)(8 + (UH)entry_cnt * 4) > len) return;  /* 不正パケット */

        const PMESH_ROUTE_ENTRY *entries =
            (const PMESH_ROUTE_ENTRY *)(data + 8);
        merge_beacon(src_node, entries, entry_cnt);
        return;
    }

    /* ---- DATA ------------------------------------------------------ */
    if (type == PMESH_DATA) {
        if (len < 12) return;
        pmesh_stats.data_rx++;

        pk_node_t dst_node = data[7];
        UH  dst_port = (UH)(data[8]  | ((UH)data[9]  << 8));
        UH  data_len = (UH)(data[10] | ((UH)data[11] << 8));
        const UB *payload = data + 12;

        if (data_len > PMESH_DATA_MAX) return;
        if ((UH)(12 + data_len) > len) return;

        /* dst_node is a UB off the wire; the relay branch below indexes
         * pmesh_routes[] (DNODE_MAX entries). pmesh_send() range-checks it but
         * the rx relay path did not. Reject out-of-range before the lookup.
         * (The == drpc_my_node local-delivery branch does not index the table,
         * but an out-of-range id can never be this node, so guard first.)
         * (ROADMAP 0-9) */
#ifndef RXB_NO_PMESH_GUARD
        if (dst_node >= DNODE_MAX) { pmesh_drop_bad++; return; }
#endif

        if (dst_node == drpc_my_node) {
            /* ローカル配送 */
            pmesh_stats.data_delivered++;
            local_dispatch(src_node, dst_port, payload, data_len);
            return;
        }

        /* 中継 — 同じバイト列をそのまま next_hop へ転送 */
        pmesh_stats.data_relay++;
        if (!pmesh_routes[dst_node].active) {
            pmesh_stats.no_route++;
            return;
        }
        pk_node_t next = pmesh_routes[dst_node].next_hop;
        udp_send(node_ip(next), PMESH_PORT, PMESH_PORT, data, len);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 送信 API                                                            */
/* ------------------------------------------------------------------ */

static PMESH_DATA_PKT data_pkt;  /* static: スタック節約 */

#if PK_NOSW_ACTIVE
static W pmesh_send_nosw(pk_node_t dst_node, UH dst_port, const UB *data, UH len);

/* pk_nosw.h: data_pkt is shared by every sender, so the whole send (down to
 * the link layer's own static buffers) is one no-switch region */
W pmesh_send(pk_node_t dst_node, UH dst_port, const UB *data, UH len)
{
    PK_NOSW_ENTER();
    W rc = pmesh_send_nosw(dst_node, dst_port, data, len);
    PK_NOSW_LEAVE();
    return rc;
}

static W pmesh_send_nosw(pk_node_t dst_node, UH dst_port, const UB *data, UH len)
#else
W pmesh_send(pk_node_t dst_node, UH dst_port, const UB *data, UH len)
#endif
{
    if (dst_node >= DNODE_MAX) return -1;
    if (len > PMESH_DATA_MAX)  return -1;
    if (drpc_my_node == 0xFF)  return -1;

    /* パケット構築 */
    data_pkt.magic    = PMESH_MAGIC;
    data_pkt.version  = PMESH_VERSION;
    data_pkt.type     = PMESH_DATA;
    data_pkt.src_node = pk_node_v1(drpc_my_node);
    data_pkt.dst_node = pk_node_v1(dst_node);
    data_pkt.dst_port = dst_port;
    data_pkt.data_len = len;
    pm_memcpy(data_pkt.data, data, len);

    UH pkt_len = (UH)(12 + len);
    UW next_ip;

    /* 直接到達可能か確認 */
    if (dnode_table[dst_node].state == DNODE_ALIVE) {
        next_ip = node_ip(dst_node);
    } else if (pmesh_routes[dst_node].active) {
        next_ip = node_ip(pmesh_routes[dst_node].next_hop);
    } else {
        pmesh_stats.no_route++;
        return -1;
    }

#if defined(_TK_HOSTED_LIBC_)
    pm_link_out(next_ip, (const UB *)&data_pkt, pkt_len);
#else
    udp_send(next_ip, PMESH_PORT, PMESH_PORT,
             (const UB *)&data_pkt, pkt_len);
#endif
    pmesh_stats.data_tx++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* タスク                                                              */
/* ------------------------------------------------------------------ */

void pmesh_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    tk_dly_tsk(3000);   /* ネットワーク初期化・ARP 完了を待つ */

    for (;;) {
        age_routes();
        send_beacon();
        tk_dly_tsk(2000);
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void pmesh_init(void)
{
    for (INT i = 0; i < DNODE_MAX; i++) {
        pmesh_routes[i].next_hop = 0xFF;
        pmesh_routes[i].cost     = PMESH_COST_INF;
        pmesh_routes[i].age      = 0;
        pmesh_routes[i].active   = 0;
    }
    /* Do NOT zero pmesh_socks[] here. Callers may have registered
     * inner-port handlers (kdds_rx, dkva, ...) via pmesh_bind() before
     * this init runs, and BSS zero already gave us a clean initial
     * state. Wiping here silently unbound K-DDS on the hosted port
     * because cmd_net()'s pmesh_init() landed after usermain()'s
     * kdds_init() — see moment_2026_05_22_cross_arch_kdds. */
    pm_memzero(&pmesh_stats, sizeof(pmesh_stats));
    udp_bind(PMESH_PORT, pmesh_rx);
    pm_puts("[pmesh] mesh routing ready  port=7380\r\n");
}

/* ------------------------------------------------------------------ */
/* シェル表示                                                          */
/* ------------------------------------------------------------------ */

void pmesh_route_list(void)
{
    if (drpc_my_node == 0xFF) {
        pm_puts("[pmesh] not in distributed mode\r\n");
        return;
    }
    pm_puts("[pmesh] routing table (node ");
    pm_putdec(drpc_my_node);
    pm_puts("):\r\n");
    pm_puts("  dst  next  cost  age\r\n");
    pm_puts("  ---  ----  ----  ---\r\n");

    /* 直接ノード（dnode_table） */
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;
        pm_puts("    ");
        pm_puthex1(n);
        pm_puts("     ");
        pm_puthex1(n);
        pm_puts("     1  direct\r\n");
    }

    /* 中継経路 */
    for (INT i = 0; i < DNODE_MAX; i++) {
        if (!pmesh_routes[i].active) continue;
        if (dnode_table[i].state == DNODE_ALIVE) continue; /* 直接は上で表示済み */
        pm_puts("    ");
        pm_puthex1((UB)i);
        pm_puts("     ");
        pm_puthex1(pk_node_v1(pmesh_routes[i].next_hop));
        pm_puts("     ");
        pm_putdec(pmesh_routes[i].cost);
        pm_puts("  age=");
        pm_putdec(pmesh_routes[i].age);
        pm_puts("\r\n");
    }
}

void pmesh_stat(void)
{
    pm_puts("[pmesh] beacon tx="); pm_putdec(pmesh_stats.beacon_tx);
    pm_puts("  rx=");              pm_putdec(pmesh_stats.beacon_rx);
    pm_puts("  data tx=");         pm_putdec(pmesh_stats.data_tx);
    pm_puts("  rx=");              pm_putdec(pmesh_stats.data_rx);
    pm_puts("  relay=");           pm_putdec(pmesh_stats.data_relay);
    pm_puts("  delivered=");       pm_putdec(pmesh_stats.data_delivered);
    pm_puts("  no_route=");        pm_putdec(pmesh_stats.no_route);
    pm_puts("\r\n");
}
#if defined(_TK_HOSTED_LIBC_)
/* ------------------------------------------------------------------ */
/* N2 (ii)(b) `nodes pmv2`: the one-node DATA frame v2 (n2-wire16.md 3) */
/* with a tap on the link and a stand-in for swim_peer_reads_v2:        */
/*   [pmv2-v1same]  narrow to narrow: v1, 12 + n bytes, as before       */
/*   [pmv2-widedst] narrow to a wide peer that reads v2: v2, 14 + n     */
/*   [pmv2-noread]  wide self to a narrow peer without bit 2: -1, none  */
/*   [pmv2-read]    wide self to a narrow peer with bit 2: v2, src wide */
/*   [pmv2-direct]  v2 goes straight only: a peer not ALIVE gets -1     */
/*   [pmv2-big]     a payload over PMESH_V2_DATA_MAX gets -1            */
/*   [pmv2-rx]      v2 to me is delivered with the wide src; v2 to      */
/*                  another id is dropped and counted; a short or long  */
/*                  length is refused; v1 to me still delivered         */
/* Falsifiers: -DN2_NC_PMV2_NOCHECK (send ignores reads-v2),            */
/* -DN2_NC_PMV2_RXDROP (receive drops v2). Hosted-only.                */
/* ------------------------------------------------------------------ */
static UB  pmt_buf[PMESH_V2_HDR + PMESH_DATA_MAX];
static UH  pmt_len;
static UW  pmt_ip;
static INT pmt_cnt;
static INT pmt_tap(UW ip, const UB *d, UH n)
{
    pmt_ip = ip; pmt_len = n; pmt_cnt++;
    for (UH i = 0; i < n && i < (UH)sizeof pmt_buf; i++) pmt_buf[i] = d[i];
    return 0;
}
static pk_node_t pmt_v2_peer;        /* the one id the stand-in says reads v2 */
static BOOL pmt_reads(pk_node_t id) { return id == pmt_v2_peer ? TRUE : FALSE; }
static pk_node_t pmt_rx_src; static UH pmt_rx_len; static INT pmt_rx_cnt;
static void pmt_rx(pk_node_t src, UH port, const UB *d, UH n)
{
    (void)port; (void)d; pmt_rx_src = src; pmt_rx_len = n; pmt_rx_cnt++;
}
static UH pmt_le16(const UB *p) { return (UH)(p[0] | ((UH)p[1] << 8)); }
static void pmt_put16(UB *p, UH v) { p[0] = (UB)v; p[1] = (UB)(v >> 8); }
static UH pmt_v2frame(UB *f, UH src, UH dst, UH port, const UB *pl, UH n)
{
    UW m = PMESH_MAGIC; __builtin_memcpy(f, &m, 4);
    f[4] = PMESH_VERSION2; f[5] = PMESH_DATA;
    pmt_put16(f + 6, src); pmt_put16(f + 8, dst);
    pmt_put16(f + 10, port); pmt_put16(f + 12, n);
    for (UH i = 0; i < n; i++) f[PMESH_V2_HDR + i] = pl[i];
    return (UH)(PMESH_V2_HDR + n);
}

INT pmesh_v2_self_test(void (*emit)(const char *))
{
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : pm_puts;
    static PMESH_SOCK saved_socks[PMESH_BIND_MAX];
    pk_node_t saved_my = drpc_my_node;
    const pk_node_t N = 2, WIDE = 300, WSELF = 260;
    const UH PORT = 7777;
    UB saved_st = dnode_table[N].state;
    PMESH_STATS saved_stats = pmesh_stats;
    PMESH_V2_STATS saved_v2 = pmesh_v2_stats;
    const UB pl[3] = { 'a', 'b', 'c' };
    static UB big[PMESH_DATA_MAX];
    static UB f[PMESH_V2_HDR + PMESH_DATA_MAX];
    W rc;

    PK_NOSW_ENTER();
    for (INT i = 0; i < PMESH_BIND_MAX; i++) saved_socks[i] = pmesh_socks[i];
    pmesh_socks[0].port = PORT; pmesh_socks[0].fn = pmt_rx; pmesh_socks[0].active = 1;
    pm_tx_tap = pmt_tap; pm_reads_v2_hook = pmt_reads;
    PK_NOSW_LEAVE();
#define PMT_FAIL(m) do { say("[pmv2] FAIL " m "\r\n"); fails++; } while (0)

    /* [pmv2-v1same] */
    drpc_my_node = 1; dnode_table[N].state = DNODE_ALIVE; pmt_v2_peer = 0xFFFF;
    pmt_cnt = 0; rc = pmesh_send(N, PORT, pl, 3);
    if (rc != 0 || pmt_cnt != 1 || pmt_len != 15 || pmt_buf[4] != PMESH_VERSION ||
        pmt_buf[6] != 1 || pmt_buf[7] != N || pmt_ip != pk_node_ip(N))
        PMT_FAIL("v1same: narrow to narrow is not the old 12-byte v1 frame");

    /* [pmv2-widedst] */
    pmt_v2_peer = WIDE; pmt_cnt = 0; rc = pmesh_send(WIDE, PORT, pl, 3);
    if (rc != 0 || pmt_cnt != 1 || pmt_len != PMESH_V2_HDR + 3 || pmt_buf[4] != PMESH_VERSION2 ||
        pmt_buf[5] != PMESH_DATA || pmt_le16(pmt_buf + 6) != 1 || pmt_le16(pmt_buf + 8) != WIDE ||
        pmt_le16(pmt_buf + 10) != PORT || pmt_le16(pmt_buf + 12) != 3 ||
        pmt_buf[PMESH_V2_HDR] != 'a' || pmt_ip != pk_node_ip(WIDE))
        PMT_FAIL("widedst: narrow to a wide peer that reads v2 is not one v2 frame");

    /* [pmv2-noread] */
    drpc_my_node = WSELF; pmt_v2_peer = 0xFFFF; pmt_cnt = 0;
    { UW nr = pmesh_v2_stats.v2_noread;
      rc = pmesh_send(N, PORT, pl, 3);
      if (rc != -1 || pmt_cnt != 0) PMT_FAIL("noread: wide self sent to a narrow peer without bit 2");
      if (pmesh_v2_stats.v2_noread != nr + 1) PMT_FAIL("noread: not counted"); }

    /* [pmv2-read] */
    pmt_v2_peer = N; pmt_cnt = 0; rc = pmesh_send(N, PORT, pl, 3);
    if (rc != 0 || pmt_cnt != 1 || pmt_buf[4] != PMESH_VERSION2 ||
        pmt_le16(pmt_buf + 6) != WSELF || pmt_le16(pmt_buf + 8) != N || pmt_ip != pk_node_ip(N))
        PMT_FAIL("read: wide self to a narrow peer with bit 2 is not one v2 frame from the wide id");

    /* [pmv2-direct] */
    dnode_table[N].state = DNODE_SUSPECT; pmt_cnt = 0; rc = pmesh_send(N, PORT, pl, 3);
    if (rc != -1 || pmt_cnt != 0) PMT_FAIL("direct: v2 went to a narrow peer that is not ALIVE");
    dnode_table[N].state = DNODE_ALIVE;

    /* [pmv2-big] */
    pmt_cnt = 0; rc = pmesh_send(N, PORT, big, (UH)(PMESH_V2_DATA_MAX + 1));
    if (rc != -1 || pmt_cnt != 0) PMT_FAIL("big: a payload over PMESH_V2_DATA_MAX was sent");
    pmt_cnt = 0; rc = pmesh_send(N, PORT, big, (UH)PMESH_V2_DATA_MAX);
    if (rc != 0 || pmt_cnt != 1 || pmt_len != PMESH_V2_HDR + PMESH_V2_DATA_MAX)
        PMT_FAIL("big: the largest v2 payload was not sent whole");

    /* [pmv2-rx] me = WSELF */
    { UH n = pmt_v2frame(f, WIDE, WSELF, PORT, pl, 3);
      pmt_rx_cnt = 0; pmesh_rx(0, PMESH_PORT, f, n);
      if (pmt_rx_cnt != 1 || pmt_rx_src != WIDE || pmt_rx_len != 3)
          PMT_FAIL("rx: v2 to me was not delivered with the wide src");
      UW dr = pmesh_v2_stats.v2_drop;
      n = pmt_v2frame(f, WIDE, (UH)(WSELF + 1), PORT, pl, 3);
      pmt_rx_cnt = 0; pmesh_rx(0, PMESH_PORT, f, n);
      if (pmt_rx_cnt != 0) PMT_FAIL("rx: v2 to another id was delivered");
      if (pmesh_v2_stats.v2_drop != dr + 1) PMT_FAIL("rx: v2 to another id was not counted");
      n = pmt_v2frame(f, WIDE, WSELF, PORT, pl, 3);
      pmt_rx_cnt = 0; pmesh_rx(0, PMESH_PORT, f, (UH)(n - 1));          /* short */
      pmt_put16(f + 12, (UH)(PMESH_V2_DATA_MAX + 1));
      pmesh_rx(0, PMESH_PORT, f, (UH)(PMESH_V2_HDR + PMESH_V2_DATA_MAX + 1));  /* too long */
      pmesh_rx(0, PMESH_PORT, f, 13);                                   /* under the header */
      if (pmt_rx_cnt != 0) PMT_FAIL("rx: a v2 frame with a bad length was delivered"); }
    {   /* v1 to me (narrow) still lands */
        drpc_my_node = 1;
        UW m = PMESH_MAGIC; __builtin_memcpy(f, &m, 4);
        f[4] = PMESH_VERSION; f[5] = PMESH_DATA; f[6] = (UB)N; f[7] = 1;
        pmt_put16(f + 8, PORT); pmt_put16(f + 10, 3); f[12] = 'a'; f[13] = 'b'; f[14] = 'c';
        pmt_rx_cnt = 0; pmesh_rx(0, PMESH_PORT, f, 15);
        if (pmt_rx_cnt != 1 || pmt_rx_src != N) PMT_FAIL("rx: v1 to me was not delivered");
    }
#undef PMT_FAIL

    PK_NOSW_ENTER();
    pm_tx_tap = NULL; pm_reads_v2_hook = NULL;
    for (INT i = 0; i < PMESH_BIND_MAX; i++) pmesh_socks[i] = saved_socks[i];
    PK_NOSW_LEAVE();
    drpc_my_node = saved_my; dnode_table[N].state = saved_st;
    pmesh_stats = saved_stats; pmesh_v2_stats = saved_v2;
    if (fails == 0) say("[pmv2] PASS (v2 only when wide and to a reader, straight; v1 unchanged)\r\n");
    else            say("[pmv2] RED\r\n");
    return fails;
}
#endif /* _TK_HOSTED_LIBC_ */
