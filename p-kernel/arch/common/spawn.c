/*
 *  spawn.c (x86)
 *  Phase 10 — 自己増殖: 新ノード接続時のカーネル自動配布
 *
 *  リーダーが新ノードを検出したとき:
 *    1. SPAWN_OFFER を送信 (クラスタ参加を促す)
 *    2. クラスタ状態 (raft term/leader, 接続ノード一覧) を push
 *    3. SFS boot sync (既存) が残りのファイル状態を同期
 *
 *  新ノード側:
 *    SPAWN_OFFER 受信 → SPAWN_READY で応答
 *    SPAWN_STATE_PUSH 受信 → raft term/leader をローカルに設定
 */

#include "spawn.h"
#include "raft.h"
#include "drpc.h"
#include "netstack.h"
#include "sfs.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void sp_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void sp_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sp_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sp_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

static UW spawn_push_cnt  = 0;   /* push した回数    */
static UW spawn_offer_cnt = 0;   /* offer 受信回数   */
static UB spawned[DNODE_MAX];    /* 配布済みフラグ   */

/* ------------------------------------------------------------------ */
/* ノード IP アドレス                                                  */
/* ------------------------------------------------------------------ */

static UW node_ip(UB n)
{
    return ((UW)(n + 1) << 24) | 0x0000010AUL;
}

/* ------------------------------------------------------------------ */
/* 状態 push: クラスタ情報を新ノードへ送信                            */
/* ------------------------------------------------------------------ */

static void push_state_to(UB dst_node)
{
    SPAWN_PKT pkt = { 0 };
    pkt.magic       = SPAWN_MAGIC;
    pkt.version     = SPAWN_VERSION;
    pkt.type        = SPAWN_STATE_PUSH;
    pkt.src_node    = drpc_my_node;
    pkt.dst_node    = dst_node;
    pkt.raft_term   = (UB)(raft_term() & 0xFF);
    pkt.raft_leader = raft_leader();

    /* data[0..DNODE_MAX-1]: 各ノードの状態を埋め込む */
    for (UB n = 0; n < DNODE_MAX && n < SPAWN_CHUNK_SIZE; n++)
        pkt.data[n] = dnode_table[n].state;

    udp_send(node_ip(dst_node), SPAWN_PORT, SPAWN_PORT,
             (const UB *)&pkt, (UH)sizeof(pkt));

    sp_puts("[spawn] state pushed to node "); sp_putdec(dst_node);
    sp_puts("  term="); sp_putdec(raft_term());
    sp_puts("  leader="); sp_putdec(raft_leader()); sp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* offer 送信                                                          */
/* ------------------------------------------------------------------ */

static void send_offer(UB dst_node)
{
    SPAWN_PKT pkt = { 0 };
    pkt.magic       = SPAWN_MAGIC;
    pkt.version     = SPAWN_VERSION;
    pkt.type        = SPAWN_OFFER;
    pkt.src_node    = drpc_my_node;
    pkt.dst_node    = dst_node;
    pkt.raft_term   = (UB)(raft_term() & 0xFF);
    pkt.raft_leader = raft_leader();

    udp_send(node_ip(dst_node), SPAWN_PORT, SPAWN_PORT,
             (const UB *)&pkt, (UH)sizeof(pkt));

    sp_puts("[spawn] offer -> node "); sp_putdec(dst_node); sp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* UDP 受信コールバック                                                */
/* ------------------------------------------------------------------ */

void spawn_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_ip; (void)src_port;
    if (len < sizeof(SPAWN_PKT) - SPAWN_CHUNK_SIZE) return;

    const SPAWN_PKT *pkt = (const SPAWN_PKT *)data;
    if (pkt->magic != SPAWN_MAGIC || pkt->version != SPAWN_VERSION) return;

    switch (pkt->type) {

    case SPAWN_OFFER: {
        spawn_offer_cnt++;
        sp_puts("[spawn] offer received from node ");
        sp_putdec(pkt->src_node);
        sp_puts("  leader="); sp_putdec(pkt->raft_leader);
        sp_puts("\r\n");

        /* READY 返答 */
        SPAWN_PKT rep = { 0 };
        rep.magic    = SPAWN_MAGIC;
        rep.version  = SPAWN_VERSION;
        rep.type     = SPAWN_READY;
        rep.src_node = drpc_my_node;
        rep.dst_node = pkt->src_node;
        udp_send(node_ip(pkt->src_node), SPAWN_PORT, SPAWN_PORT,
                 (const UB *)&rep, (UH)sizeof(rep) - SPAWN_CHUNK_SIZE);
        break;
    }

    case SPAWN_READY: {
        /* 新ノードから READY → 状態を push */
        sp_puts("[spawn] node "); sp_putdec(pkt->src_node);
        sp_puts(" is ready — pushing cluster state\r\n");
        push_state_to(pkt->src_node);
        spawned[pkt->src_node] = 1;
        spawn_push_cnt++;

        /* SFS boot sync もトリガー (ファイル状態を同期) */
        sfs_boot_sync();
        break;
    }

    case SPAWN_STATE_PUSH: {
        /* リーダーからのクラスタ状態を受信 */
        sp_puts("[spawn] cluster state received  leader=");
        sp_putdec(pkt->raft_leader);
        sp_puts("  term="); sp_putdec(pkt->raft_term);
        sp_puts("\r\n");

        /* dnode_table を状態情報で初期化 (まだ未発見のノードを登録) */
        for (UB n = 0; n < DNODE_MAX && n < SPAWN_CHUNK_SIZE; n++) {
            if (n == drpc_my_node) continue;
            if (dnode_table[n].state == DNODE_UNKNOWN &&
                pkt->data[n] == DNODE_ALIVE) {
                dnode_table[n].ip    = node_ip(n);
                dnode_table[n].state = DNODE_ALIVE;
                sp_puts("[spawn] registered node "); sp_putdec(n);
                sp_puts(" from cluster state\r\n");
            }
        }
        break;
    }

    case SPAWN_DONE: {
        sp_puts("[spawn] transfer done from node ");
        sp_putdec(pkt->src_node); sp_puts("\r\n");
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* リーダー選出後に呼ぶ: 全未参加ノードへ offer                      */
/* ------------------------------------------------------------------ */

void spawn_on_leader(void)
{
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state == DNODE_ALIVE && !spawned[n]) {
            send_offer(n);
            push_state_to(n);   /* offer + state を即時送信 */
            spawned[n] = 1;
            spawn_push_cnt++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 新ノード発見時に呼ぶ (swim.c / drpc.c から)                       */
/* ------------------------------------------------------------------ */

void spawn_on_new_node(UB node_id)
{
    if (node_id >= DNODE_MAX) return;
    if (spawned[node_id]) return;         /* 配布済み */
    if (raft_role() != RAFT_LEADER) return;  /* リーダーのみ配布 */

    sp_puts("[spawn] new node detected: "); sp_putdec(node_id);
    sp_puts(" — initiating spawn\r\n");

    send_offer(node_id);
    /* READY 受信後に push_state_to() が呼ばれる */
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void spawn_init(void)
{
    spawn_push_cnt  = 0;
    spawn_offer_cnt = 0;
    for (INT i = 0; i < DNODE_MAX; i++) spawned[i] = 0;
    udp_bind(SPAWN_PORT, spawn_rx);
    sp_puts("[spawn] initialized  port="); sp_putdec(SPAWN_PORT); sp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void spawn_stat(void)
{
    sp_puts("[spawn] push_cnt  : "); sp_putdec(spawn_push_cnt);  sp_puts("\r\n");
    sp_puts("[spawn] offer_cnt : "); sp_putdec(spawn_offer_cnt); sp_puts("\r\n");
    sp_puts("[spawn] spawned   : ");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (spawned[n]) { sp_putdec(n); sp_puts(" "); }
    }
    sp_puts("\r\n");
}
