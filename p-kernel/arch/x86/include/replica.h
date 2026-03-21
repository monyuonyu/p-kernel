/*
 *  replica.h (x86)
 *  分散状態複製 — フェーズ 5
 *
 *  K-DDS のトピックデータを gossip で全 ALIVE ノードへ複製する。
 *  ノードが死んでもデータは隣のノードに残り、
 *  再起動したノードは 3 秒以内に全記憶を取り戻す。
 *
 *  動作概要:
 *    1. replica_task が 3 秒ごとに全トピックスナップショットを
 *       全 ALIVE ノードへ UDP ブロードキャストする (REPLICA_DATA)
 *    2. 受信ノードは seq 番号を比較してより新しいデータだけマージする
 *    3. 新規参加 / 復帰ノードを検出したら即座にプッシュする
 *
 *  UDP ポート: REPLICA_PORT 7379
 *
 *  これにより「最後の 1 ノードが生き残れば全記憶が保存される」
 *  というサバイバビリティが実現される。
 */

#pragma once
#include "kdds.h"
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define REPLICA_PORT     7379
#define REPLICA_MAGIC    0x4C504552UL   /* "REPL" little-endian         */
#define REPLICA_VERSION  1

/* パケットタイプ */
#define REPLICA_ANNOUNCE 0x01   /* 参加/復帰通知 — 相手に状態要求      */
#define REPLICA_DATA     0x02   /* トピックテーブル全体を送信           */

/* ------------------------------------------------------------------ */
/* パケットフォーマット                                                */
/* ------------------------------------------------------------------ */

/* 1 トピックのスナップショット (可変長パケットの要素) */
typedef struct {
    char name[KDDS_NAME_MAX];  /* トピック名                           */
    UH   data_len;             /* データバイト数 (0 = なし)            */
    UH   data_seq;             /* 複製判定用シーケンス番号             */
    UB   qos;                  /* KDDS_QOS_*                           */
    UB   _pad[3];
    UB   data[KDDS_DATA_MAX];  /* 最新データ                           */
} __attribute__((packed)) REPLICA_ENTRY;
/* = 32 + 2 + 2 + 1 + 3 + 128 = 168 bytes */

typedef struct {
    UW   magic;                             /* REPLICA_MAGIC            */
    UB   version;                           /* REPLICA_VERSION          */
    UB   type;                              /* REPLICA_ANNOUNCE / DATA  */
    UB   src_node;
    UB   entry_cnt;                         /* entries[] 有効数         */
    REPLICA_ENTRY entries[KDDS_TOPIC_MAX];  /* トピックスナップショット */
} __attribute__((packed)) REPLICA_PKT;
/* header=8 + 8×168=1344 = 1352 bytes (UDP MTU 内) */

/* ------------------------------------------------------------------ */
/* 統計情報                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  sent_pkts;   /* 送信パケット数                                 */
    UW  recv_pkts;   /* 受信パケット数                                 */
    UW  merged;      /* リモートデータで上書きしたトピック更新数       */
    UW  skipped;     /* ローカルが最新だったため無視した数             */
    UW  recovered;   /* 復元した新規トピック数                         */
} REPLICA_STATS;

extern REPLICA_STATS replica_stats;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() 内で kdds_init() の後に呼ぶ。 */
void replica_init(void);

/* 定期複製タスク (優先度 8, スタック 2048)。 */
void replica_task(INT stacd, void *exinf);

/* UDP 受信コールバック (REPLICA_PORT に登録)。 */
void replica_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* 新規/復帰ノードへ即座に全トピックをプッシュする (swim.c から呼ぶ)。 */
void replica_push_to(UB node_id);

/* 起動の叫び: 全ノード IP へ ANNOUNCE を送り「記憶をよこせ」と要求する。
 * replica_task() 起動冒頭から呼ぶ。 */
void replica_boot_cry(void);

/* 断末魔: 自分が SUSPECT と噂されたとき全 ALIVE ノードへ即座にデータを散布。
 * swim.c の gossip_apply() から呼ぶ。 */
void replica_scatter_all(void);

/* 統計表示 (shell `replica stat` から呼ぶ)。 */
void replica_stat(void);
