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
 *
 *  v2 wire chunking:
 *  スナップショットは {round_seq, part_idx, part_cnt} 付きの
 *  REPLICA_DATA パケット列として送る (1 part ≤ REPLICA_ENTRY_MAX 件)。
 *  マージはエントリ単位の seq 比較で冪等なので、part は受信側で
 *  再組立てしない — 各パケットは単独でマージ可能であり、
 *  part フィールドは観測用の帳簿に過ぎない。part の欠落/順序逆転は
 *  「そのトピックの更新が次の定期ラウンドまで遅れる」以上の影響を
 *  持たない。
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
/* スナップショット列挙 — トピックテーブル + tombstone をひとつの     */
/* 仮想リストとして part 単位に切り出す                               */
/* ------------------------------------------------------------------ */

/* スナップショットに乗るアイテム総数 (open トピック + active tombstone) */
static W snapshot_count(void)
{
    W total = 0;
    for (W i = 0; i < KDDS_TOPIC_MAX; i++)
        if (kdds_topics[i].open) total++;
    for (INT t = 0; t < REPLICA_TOMB_MAX; t++)
        if (tomb[t].active) total++;
    return total;
}

/* カーソル位置から最大 REPLICA_ENTRY_MAX 件を pkt->entries[] に詰める。
 * tcur: kdds_topics[] の走査位置 / mcur: tomb[] の走査位置 (進行する)。
 * 戻り値: 詰めた件数。 */
static UB fill_part(REPLICA_PKT *pkt, W *tcur, INT *mcur)
{
    UB n = 0;

    /* 通常トピック */
    while (*tcur < KDDS_TOPIC_MAX && n < REPLICA_ENTRY_MAX) {
        W i = (*tcur)++;
        if (!kdds_topics[i].open) continue;
        REPLICA_ENTRY *e = &pkt->entries[n++];
        rp_strcpy(e->name, kdds_topics[i].name, KDDS_NAME_MAX);
        e->data_len = kdds_topics[i].data_len;
        e->data_seq = kdds_topics[i].data_seq;
        e->qos      = kdds_topics[i].qos;
        e->scope    = kdds_topics[i].scope;
        e->_pad[0]  = 0; e->_pad[1] = 0;
        if (e->data_len > 0)
            rp_memcpy(e->data, kdds_topics[i].data, e->data_len);
    }

    /* Tombstone エントリを末尾に追加 (削除の伝播) */
    while (*tcur >= KDDS_TOPIC_MAX &&
           *mcur < REPLICA_TOMB_MAX && n < REPLICA_ENTRY_MAX) {
        INT t = (*mcur)++;
        if (!tomb[t].active) continue;
        REPLICA_ENTRY *e = &pkt->entries[n++];
        rp_strcpy(e->name, tomb[t].name, KDDS_NAME_MAX);
        e->data_len = 0;
        e->data_seq = (UH)REPLICA_TOMB_SEQ;
        e->qos      = 0;
        e->scope    = 0;
        e->_pad[0]  = 0; e->_pad[1] = 0;
    }

    return n;
}

/* ------------------------------------------------------------------ */
/* ノードへスナップショット送信 (v2: part 列)                          */
/* ------------------------------------------------------------------ */

/* ラウンド世代番号 (送信ごとに +1。受信側の観測帳簿が区別に使う) */
static UH round_seq_gen;

/* paced != 0 のときのみ part 間に REPLICA_PART_GAP_MS 眠る。
 * 定期ラウンド (replica_task: タスクコンテキスト) は paced=1 で
 * バーストを抑える。pmesh rx コールバック / swim 経由の呼び出しは
 * 眠れないので paced=0 で連続送信する — これらは join/death の
 * 単発イベントなので瞬間バーストを許容する。
 *
 * NOTE: pkt が static なのは hosted-relay の教訓 (1KB 超のスタック
 * ローカルはタスクスタックを食い破る) のため。複数コンテキストから
 * 同時に呼ばれると 1 パケットが壊れる可能性は v1 から同じだが、
 * 受信側は magic/version/長さ検査で弾き、次ラウンドが修復する。 */
static void send_snapshot_to(UB node_id, W paced)
{
    if (node_id >= DNODE_MAX) return;

    static REPLICA_PKT pkt;   /* 1172B — スタックに置かない */

    W  total    = snapshot_count();
    UB part_cnt = (UB)((total + REPLICA_ENTRY_MAX - 1) / REPLICA_ENTRY_MAX);
    if (part_cnt == 0) part_cnt = 1;   /* 空でもヘッダだけは送る (生存通知) */

    UH  round = ++round_seq_gen;
    W   tcur  = 0;
    INT mcur  = 0;

    for (UB p = 0; p < part_cnt; p++) {
        for (INT z = 0; z < (INT)sizeof(pkt); z++) ((UB *)&pkt)[z] = 0;
        pkt.magic     = REPLICA_MAGIC;
        pkt.version   = REPLICA_VERSION;
        pkt.type      = REPLICA_DATA;
        pkt.src_node  = drpc_my_node;
        pkt.round_seq = round;
        pkt.part_idx  = p;
        pkt.part_cnt  = part_cnt;
        pkt.entry_cnt = fill_part(&pkt, &tcur, &mcur);

        pmesh_send(node_id, REPLICA_PORT, (const UB *)&pkt,
                   (UH)(REPLICA_HDR_LEN + pkt.entry_cnt * REPLICA_ENTRY_LEN));
        replica_stats.sent_pkts++;

        if (paced && (UB)(p + 1) < part_cnt)
            tk_dly_tsk(REPLICA_PART_GAP_MS);
    }
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
 *
 * この比較はエントリ単位で完結するため、part の到着順序・欠落・重複の
 * いずれにも依存しない (= 各 REPLICA_DATA パケットは単独でマージ可能)。
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
        /* リモートが新しい (scope は触らない — REGION 格下げ防止) */
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
        /* v2: scope を継承 (不正値は GLOBAL に落とす) */
        kdds_topics[i].scope    = (e->scope == KDDS_SCOPE_REGION)
                                      ? KDDS_SCOPE_REGION : KDDS_SCOPE_GLOBAL;
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
    /* v2 ヘッダ 12 バイト以上 (ANNOUNCE はヘッダのみ送信) */
    if (len < REPLICA_HDR_LEN) return;

    const REPLICA_PKT *pkt = (const REPLICA_PKT *)data;
    if (pkt->magic   != REPLICA_MAGIC)   return;
    if (pkt->version != REPLICA_VERSION) return;   /* v1 は黙って捨てる */

    replica_stats.recv_pkts++;

    if (pkt->type == REPLICA_ANNOUNCE) {
        /* 復帰ノードからの状態要求 — 全トピックをプッシュ
         * (rx コールバック内なので paced なし) */
        if (pkt->src_node < DNODE_MAX)
            replica_push_to(pkt->src_node);
        return;
    }

    if (pkt->type != REPLICA_DATA) return;

    /* エントリ数の検証: 宣言数とパケット実長の両方でクランプ */
    UB cnt = pkt->entry_cnt;
    if (cnt > REPLICA_ENTRY_MAX) cnt = REPLICA_ENTRY_MAX;
    if ((UW)len < (UW)REPLICA_HDR_LEN + (UW)cnt * REPLICA_ENTRY_LEN) return;

    /* 観測帳簿: (src, round_seq) が変わったらラウンド集計をリセット。
     * マージ自体はラウンドに依存しない — これは `replica stat` で
     * 「1 ラウンドで全トピックが渡っているか」を見るためだけの記録 */
    if (replica_stats.rnd_src != pkt->src_node ||
        replica_stats.rnd_seq != pkt->round_seq) {
        replica_stats.rnd_src     = pkt->src_node;
        replica_stats.rnd_seq     = pkt->round_seq;
        replica_stats.rnd_entries = 0;
        replica_stats.rnd_parts   = 0;
    }
    replica_stats.rnd_total    = pkt->part_cnt;
    replica_stats.rnd_parts++;
    replica_stats.rnd_entries += cnt;

    /* エントリをマージ (各 part は単独でマージ可能) */
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
            send_snapshot_to(n, 1 /* paced */);
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
    /* コールバック/イベント文脈から呼ばれ得るため paced なし */
    send_snapshot_to(node_id, 0);
}

/* ------------------------------------------------------------------ */
/* 起動の叫び — 全ノード IP へ ANNOUNCE を送り記憶プッシュを要求       */
/* ------------------------------------------------------------------ */

void replica_boot_cry(void)
{
    if (drpc_my_node == 0xFF) return;
    rp_puts("[replica] *** BOOT CRY *** requesting memories from all peers\r\n");

    /* ANNOUNCE は v2 ヘッダ (12B) のみ送る (entries[] は不要) */
    static REPLICA_PKT ann;
    for (INT z = 0; z < REPLICA_HDR_LEN; z++) ((UB *)&ann)[z] = 0;
    ann.magic     = REPLICA_MAGIC;
    ann.version   = REPLICA_VERSION;
    ann.type      = REPLICA_ANNOUNCE;
    ann.src_node  = drpc_my_node;
    ann.entry_cnt = 0;
    ann.round_seq = 0;
    ann.part_idx  = 0;
    ann.part_cnt  = 1;

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        pmesh_send(n, REPLICA_PORT, (const UB *)&ann, (UH)REPLICA_HDR_LEN);
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
        /* 死にかけ: 待っている暇はない — paced なしで全 part を吐き出す */
        send_snapshot_to(n, 0);
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
    rp_puts("\r\n[replica] last round: src=");
    rp_putdec(replica_stats.rnd_src);
    rp_puts("  seq=");               rp_putdec(replica_stats.rnd_seq);
    rp_puts("  parts=");             rp_putdec(replica_stats.rnd_parts);
    rp_puts("/");                    rp_putdec(replica_stats.rnd_total);
    rp_puts("  entries=");           rp_putdec(replica_stats.rnd_entries);
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

    /* 今すぐ全 ALIVE ノードへ tombstone を送信 (単独 part 1/1) */
    if (drpc_my_node == 0xFF) return;

    /* static: REPLICA_PKT はスタックには大きすぎる */
    static REPLICA_PKT pkt;
    for (INT _i = 0; _i < (INT)sizeof(pkt); _i++) ((UB *)&pkt)[_i] = 0;
    pkt.magic     = REPLICA_MAGIC;
    pkt.version   = REPLICA_VERSION;
    pkt.type      = REPLICA_DATA;
    pkt.src_node  = drpc_my_node;
    pkt.entry_cnt = 1;
    pkt.round_seq = ++round_seq_gen;
    pkt.part_idx  = 0;
    pkt.part_cnt  = 1;

    REPLICA_ENTRY *e = &pkt.entries[0];
    rp_strcpy(e->name, name, KDDS_NAME_MAX);
    e->data_len = 0;
    e->data_seq = (UH)REPLICA_TOMB_SEQ;
    e->qos      = 0;
    e->scope    = 0;

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        pmesh_send(n, REPLICA_PORT, (const UB *)&pkt,
                   (UH)(REPLICA_HDR_LEN + REPLICA_ENTRY_LEN));
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
    replica_stats.sent_pkts   = 0;
    replica_stats.recv_pkts   = 0;
    replica_stats.merged      = 0;
    replica_stats.skipped     = 0;
    replica_stats.recovered   = 0;
    replica_stats.rnd_entries = 0;
    replica_stats.rnd_parts   = 0;
    replica_stats.rnd_total   = 0;
    replica_stats.rnd_src     = 0xFF;
    replica_stats.rnd_seq     = 0;
    for (INT i = 0; i < REPLICA_TOMB_MAX; i++) tomb[i].active = 0;
    pmesh_bind(REPLICA_PORT, replica_rx);
    rp_puts("[replica] state replication v2 ready  port=7379 chunked\r\n");
}
