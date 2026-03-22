/*
 *  replica.c (x86)
 *  分散状態複製 — フェーズ 5
 *
 *  gossip ベースのトピックテーブル全複製。
 *  3 秒ごとに全 ALIVE ノードへスナップショットをブロードキャストし、
 *  新規参加/復帰ノードへは即座にプッシュする。
 *
 *  マージ戦略: data_seq を符号なし半開空間で比較し、
 *  リモートが新しい場合のみ kdds_topics[] を直接更新する
 *  (kdds_pub() は呼ばず、再ブロードキャストループを防ぐ)。
 */

#include "replica.h"
#include "degrade.h"
#include "netstack.h"
#include "pmesh.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* シリアル出力ヘルパ                                                 */
/* ------------------------------------------------------------------ */

static void rp_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void rp_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rp_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rp_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

REPLICA_STATS replica_stats;

/* ------------------------------------------------------------------ */
/* Tombstone テーブル — 削除したトピック名を一定期間保持して伝播する  */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[KDDS_NAME_MAX];
    UB   active;
} TOMB_SLOT;

static TOMB_SLOT tomb[REPLICA_TOMB_MAX];

/* ------------------------------------------------------------------ */
/* 文字列 / メモリユーティリティ                                      */
/* ------------------------------------------------------------------ */

static INT rp_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void rp_strcpy(char *dst, const char *src, INT max)
{
    INT i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void rp_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src;
    UB       *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

/* ------------------------------------------------------------------ */
/* パケット構築                                                        */
/* ------------------------------------------------------------------ */

static void build_pkt(REPLICA_PKT *pkt, UB type)
{
    pkt->magic     = REPLICA_MAGIC;
    pkt->version   = REPLICA_VERSION;
    pkt->type      = type;
    pkt->src_node  = drpc_my_node;
    pkt->entry_cnt = 0;

    /* 通常トピック */
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) continue;
        REPLICA_ENTRY *e = &pkt->entries[pkt->entry_cnt++];
        rp_strcpy(e->name, kdds_topics[i].name, KDDS_NAME_MAX);
        e->data_len  = kdds_topics[i].data_len;
        e->data_seq  = kdds_topics[i].data_seq;
        e->qos       = kdds_topics[i].qos;
        e->_pad[0]   = 0; e->_pad[1] = 0; e->_pad[2] = 0;
        if (e->data_len > 0)
            rp_memcpy(e->data, kdds_topics[i].data, e->data_len);
    }

    /* Tombstone エントリを末尾に追加 (削除の伝播) */
    for (INT ti = 0; ti < REPLICA_TOMB_MAX && (INT)pkt->entry_cnt < KDDS_TOPIC_MAX; ti++) {
        if (!tomb[ti].active) continue;
        REPLICA_ENTRY *e = &pkt->entries[pkt->entry_cnt++];
        rp_strcpy(e->name, tomb[ti].name, KDDS_NAME_MAX);
        e->data_len = 0;
        e->data_seq = (UH)REPLICA_TOMB_SEQ;
        e->qos      = 0;
        e->_pad[0]  = 0; e->_pad[1] = 0; e->_pad[2] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* ノードへ送信                                                        */
/* ------------------------------------------------------------------ */

static void send_to_node(UB node_id, UB type)
{
    if (node_id >= DNODE_MAX) return;

    /* static: REPLICA_PKT は KDDS_TOPIC_MAX×168+8 バイト。スタック節約のため static */
    static REPLICA_PKT pkt;
    for (INT _i = 0; _i < (INT)sizeof(pkt); _i++) ((UB *)&pkt)[_i] = 0;
    build_pkt(&pkt, type);
    pmesh_send(node_id, REPLICA_PORT, (const UB *)&pkt, (UH)sizeof(pkt));
    replica_stats.sent_pkts++;
}

/* ------------------------------------------------------------------ */
/* マージ                                                              */
/* ------------------------------------------------------------------ */

/*
 * 符号なし半開空間比較:
 *   diff = (UH)(remote_seq - local_seq)
 *   diff == 0            → 等しい (スキップ)
 *   diff >= 0x8000       → local が新しい (スキップ)
 *   0 < diff < 0x8000    → remote が新しい (マージ)
 */
static void merge_entry(const REPLICA_ENTRY *e)
{
    /* Tombstone: data_len==0 かつ TOMB_SEQ → ローカルトピックを削除する */
    if (e->data_len == 0) {
        if (e->data_seq != (UH)REPLICA_TOMB_SEQ) return;  /* 空エントリ: 無視 */
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!rp_streq(kdds_topics[i].name, e->name)) continue;
            kdds_topics[i].open = 0;   /* トピックスロット解放 */
            replica_stats.merged++;
            rp_puts("[replica] tombstone applied: \"");
            rp_puts(e->name);
            rp_puts("\"\r\n");
            return;
        }
        return;  /* ローカルに存在しない場合は何もしない (既に削除済み) */
    }

    if (e->data_len > KDDS_DATA_MAX) return;

    /* 既存トピック検索 */
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open) continue;
        if (!rp_streq(kdds_topics[i].name, e->name)) continue;

        UH diff = (UH)(e->data_seq - kdds_topics[i].data_seq);
        if (diff == 0 || diff >= 0x8000U) {
            /* ローカルが最新 */
            replica_stats.skipped++;
            return;
        }
        /* リモートが新しい */
        rp_memcpy(kdds_topics[i].data, e->data, e->data_len);
        kdds_topics[i].data_len = e->data_len;
        kdds_topics[i].data_seq = e->data_seq;
        replica_stats.merged++;
        return;
    }

    /* 未知トピック: 新規スロット確保 */
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (kdds_topics[i].open) continue;
        rp_strcpy(kdds_topics[i].name, e->name, KDDS_NAME_MAX);
        rp_memcpy(kdds_topics[i].data, e->data, e->data_len);
        kdds_topics[i].data_len = e->data_len;
        kdds_topics[i].data_seq = e->data_seq;
        kdds_topics[i].qos      = e->qos;
        kdds_topics[i].open     = 1;
        replica_stats.recovered++;
        rp_puts("[replica] recovered topic \"");
        rp_puts(kdds_topics[i].name);
        rp_puts("\"\r\n");
        return;
    }
    /* トピックテーブル満杯: 廃棄 */
}

/* ------------------------------------------------------------------ */
/* UDP 受信コールバック                                                */
/* ------------------------------------------------------------------ */

void replica_rx(UB src_node, UH dst_port, const UB *data, UH len)
{
    (void)src_node; (void)dst_port;
    /* ヘッダ 8 バイト以上あれば受け付ける (ANNOUNCE はヘッダのみ送信) */
    if (len < 8) return;

    const REPLICA_PKT *pkt = (const REPLICA_PKT *)data;
    if (pkt->magic   != REPLICA_MAGIC)   return;
    if (pkt->version != REPLICA_VERSION) return;

    replica_stats.recv_pkts++;

    if (pkt->type == REPLICA_ANNOUNCE) {
        /* 復帰ノードからの状態要求 — 全トピックをプッシュ */
        if (pkt->src_node < DNODE_MAX)
            replica_push_to(pkt->src_node);
        return;
    }

    if (pkt->type != REPLICA_DATA) return;

    /* エントリをマージ */
    UB cnt = pkt->entry_cnt;
    if (cnt > KDDS_TOPIC_MAX) cnt = KDDS_TOPIC_MAX;
    for (UB i = 0; i < cnt; i++)
        merge_entry(&pkt->entries[i]);
}

/* ------------------------------------------------------------------ */
/* 定期複製タスク                                                      */
/* ------------------------------------------------------------------ */

void replica_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    tk_dly_tsk(2000);   /* ネットワーク・ARP 完了を待つ */
    replica_boot_cry(); /* 起動の叫び: 全ピアへ記憶要求 */
    tk_dly_tsk(1000);   /* 応答を受け取る猶予 */

    for (;;) {
        tk_dly_tsk(degrade_replica_interval());
        if (drpc_my_node == 0xFF) continue;

        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) continue;
            if (dnode_table[n].state != DNODE_ALIVE) continue;
            send_to_node(n, REPLICA_DATA);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 即時プッシュ (swim.c から ALIVE 遷移時に呼ぶ)                      */
/* ------------------------------------------------------------------ */

void replica_push_to(UB node_id)
{
    if (node_id >= DNODE_MAX) return;
    if (dnode_table[node_id].state != DNODE_ALIVE) return;
    send_to_node(node_id, REPLICA_DATA);
}

/* ------------------------------------------------------------------ */
/* 起動の叫び — 全ノード IP へ ANNOUNCE を送り記憶プッシュを要求       */
/* ------------------------------------------------------------------ */

void replica_boot_cry(void)
{
    if (drpc_my_node == 0xFF) return;
    rp_puts("[replica] *** BOOT CRY *** requesting memories from all peers\r\n");

    /* ANNOUNCE ヘッダのみ送る (entries[] は不要) */
    static struct {
        UW magic;
        UB version;
        UB type;
        UB src_node;
        UB entry_cnt;
    } ann;
    ann.magic     = REPLICA_MAGIC;
    ann.version   = REPLICA_VERSION;
    ann.type      = REPLICA_ANNOUNCE;
    ann.src_node  = drpc_my_node;
    ann.entry_cnt = 0;

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        pmesh_send(n, REPLICA_PORT, (const UB *)&ann, (UH)sizeof(ann));
    }
}

/* ------------------------------------------------------------------ */
/* 断末魔 — SUSPECT 噂を検知したら全 ALIVE ノードへ即座に散布          */
/* ------------------------------------------------------------------ */

void replica_scatter_all(void)
{
    if (drpc_my_node == 0xFF) return;
    rp_puts("[replica] *** DEATH THROES *** scattering all memories NOW\r\n");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        send_to_node(n, REPLICA_DATA);
    }
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void replica_stat(void)
{
    rp_puts("[replica] sent=");      rp_putdec(replica_stats.sent_pkts);
    rp_puts("  recv=");              rp_putdec(replica_stats.recv_pkts);
    rp_puts("  merged=");            rp_putdec(replica_stats.merged);
    rp_puts("  skipped=");           rp_putdec(replica_stats.skipped);
    rp_puts("  recovered=");         rp_putdec(replica_stats.recovered);
    rp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* Tombstone 伝播                                                      */
/* ------------------------------------------------------------------ */

void replica_tombstone(const char *name)
{
    /* tombstone テーブルに追加 (gossip パケットに乗せて継続伝播) */
    for (INT i = 0; i < REPLICA_TOMB_MAX; i++) {
        if (tomb[i].active) continue;
        rp_strcpy(tomb[i].name, name, KDDS_NAME_MAX);
        tomb[i].active = 1;
        break;
    }

    /* 今すぐ全 ALIVE ノードへ tombstone を送信 */
    if (drpc_my_node == 0xFF) return;

    /* static: REPLICA_PKT はスタックには大きすぎる */
    static REPLICA_PKT pkt;
    for (INT _i = 0; _i < (INT)sizeof(pkt); _i++) ((UB *)&pkt)[_i] = 0;
    pkt.magic     = REPLICA_MAGIC;
    pkt.version   = REPLICA_VERSION;
    pkt.type      = REPLICA_DATA;
    pkt.src_node  = drpc_my_node;
    pkt.entry_cnt = 1;

    REPLICA_ENTRY *e = &pkt.entries[0];
    rp_strcpy(e->name, name, KDDS_NAME_MAX);
    e->data_len = 0;
    e->data_seq = (UH)REPLICA_TOMB_SEQ;
    e->qos      = 0;

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        pmesh_send(n, REPLICA_PORT, (const UB *)&pkt, (UH)sizeof(pkt));
    }

    rp_puts("[replica] tombstone sent: \"");
    rp_puts(name);
    rp_puts("\"\r\n");
    replica_stats.sent_pkts++;
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void replica_init(void)
{
    replica_stats.sent_pkts = 0;
    replica_stats.recv_pkts = 0;
    replica_stats.merged    = 0;
    replica_stats.skipped   = 0;
    replica_stats.recovered = 0;
    for (INT i = 0; i < REPLICA_TOMB_MAX; i++) tomb[i].active = 0;
    pmesh_bind(REPLICA_PORT, replica_rx);
    rp_puts("[replica] state replication ready  port=7379\r\n");
}
