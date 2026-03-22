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

static void local_dispatch(UB src_node, UH dst_port,
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
static UW node_ip(UB n)
{
    return ((UW)(n + 1) << 24) | 0x0000010AUL;
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
static void update_direct(UB node_id)
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
static void merge_beacon(UB src_node,
                         const PMESH_ROUTE_ENTRY *entries, UB cnt)
{
    /* まず src_node 自身を直接ノードとして登録 */
    update_direct(src_node);

    for (UB i = 0; i < cnt; i++) {
        UB dst  = entries[i].dst_node;
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
    beacon_pkt.src_node = drpc_my_node;
    beacon_pkt.entry_cnt = 0;

    /* 自分自身 (cost=0) */
    beacon_pkt.entries[beacon_pkt.entry_cnt].dst_node = drpc_my_node;
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

    const UW magic = *(const UW *)data;
    if (magic != PMESH_MAGIC)        return;
    if (data[4] != PMESH_VERSION)    return;

    UB type     = data[5];
    UB src_node = data[6];

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

        UB  dst_node = data[7];
        UH  dst_port = (UH)(data[8]  | ((UH)data[9]  << 8));
        UH  data_len = (UH)(data[10] | ((UH)data[11] << 8));
        const UB *payload = data + 12;

        if (data_len > PMESH_DATA_MAX) return;
        if ((UH)(12 + data_len) > len) return;

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
        UB next = pmesh_routes[dst_node].next_hop;
        udp_send(node_ip(next), PMESH_PORT, PMESH_PORT, data, len);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 送信 API                                                            */
/* ------------------------------------------------------------------ */

static PMESH_DATA_PKT data_pkt;  /* static: スタック節約 */

W pmesh_send(UB dst_node, UH dst_port, const UB *data, UH len)
{
    if (dst_node >= DNODE_MAX) return -1;
    if (len > PMESH_DATA_MAX)  return -1;
    if (drpc_my_node == 0xFF)  return -1;

    /* パケット構築 */
    data_pkt.magic    = PMESH_MAGIC;
    data_pkt.version  = PMESH_VERSION;
    data_pkt.type     = PMESH_DATA;
    data_pkt.src_node = drpc_my_node;
    data_pkt.dst_node = dst_node;
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

    udp_send(next_ip, PMESH_PORT, PMESH_PORT,
             (const UB *)&data_pkt, pkt_len);
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
    for (INT i = 0; i < PMESH_BIND_MAX; i++) {
        pmesh_socks[i].active = 0;
    }
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
        pm_puthex1(pmesh_routes[i].next_hop);
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
