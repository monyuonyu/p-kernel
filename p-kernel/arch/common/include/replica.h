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
#include "pmesh.h"   /* PMESH_DATA_MAX — wire パケット上限の根拠 */

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define REPLICA_PORT     7379
#define REPLICA_MAGIC    0x4C504552UL   /* "REPL" little-endian         */

/* v2: マルチパケット・スナップショット (wire chunking)。
 * v1 は 1 announce = 1 パケット固定で、KDDS_DATA_MAX が 128→192 に
 * 肥大した時点で REPLICA_PKT が 1864B となり pmesh_send (>1380B 拒否)
 * に黙って弾かれていた — つまり v1 の複製はこのブランチでは実質
 * 死んでいた。v2 はスナップショットを {round_seq, part_idx, part_cnt}
 * 付きのパケット列に分割する。レイアウト変更のため version を上げる。
 *
 * 互換性の決定: 本リポジトリの fleet は単一系統でロールアウト中の
 * 新旧混在は発生しないため、v1↔v2 共存は意図的にサポートしない。
 * (受信側は version 不一致を黙って捨てるので誤マージは起きない) */
#define REPLICA_VERSION  2

/* パケットタイプ */
#define REPLICA_ANNOUNCE 0x01   /* 参加/復帰通知 — 相手に状態要求      */
#define REPLICA_DATA     0x02   /* トピックスナップショット (1 part)   */

/* Tombstone シーケンス番号 — 通常の seq 範囲 (0..0x7FFF) より大きく、
 * 0xFFFF (ラップアラウンドと紛らわしい値) を避けた特別な値 */
#define REPLICA_TOMB_SEQ 0xFFFEU
#define REPLICA_TOMB_MAX 8      /* 同時保持できる tombstone 数          */

/* ------------------------------------------------------------------ */
/* パケットフォーマット (v2)                                           */
/* ------------------------------------------------------------------ */

/* 1 トピックのスナップショット (可変長パケットの要素)。
 * v2 で _pad の 1 バイトを scope に転用: regions R0 の REGION スコープ
 * を複製でも運び、復元トピックが GLOBAL に格下げされるのを防ぐ。 */
typedef struct {
    char name[KDDS_NAME_MAX];  /* トピック名                           */
    UH   data_len;             /* データバイト数 (0 = なし)            */
    UH   data_seq;             /* 複製判定用シーケンス番号             */
    UB   qos;                  /* KDDS_QOS_*                           */
    UB   scope;                /* KDDS_SCOPE_* (新規復元時のみ適用)    */
    UB   _pad[2];
    UB   data[KDDS_DATA_MAX];  /* 最新データ                           */
} __attribute__((packed)) REPLICA_ENTRY;

#define REPLICA_HDR_LEN    12                                  /* v2 ヘッダ */
#define REPLICA_ENTRY_LEN  (KDDS_NAME_MAX + 8 + KDDS_DATA_MAX) /* = 232    */

/* 1 パケットに収められるスナップショット数。
 * 各 part は pmesh_send 経由の 1 UDP パケットなので
 * PMESH_DATA_MAX(1380) を超えてはならない:
 *   12 + 232×N <= 1380 → N <= 5 (現行値)。
 * KDDS_DATA_MAX を動かすとここも自動で再計算される。
 * 全トピック数が REPLICA_ENTRY_MAX を超える分は複数 part に分割される
 * (v1 の「先頭 8 件しか乗らない」負債は v2 で解消)。 */
#define REPLICA_ENTRY_MAX  ((PMESH_DATA_MAX - REPLICA_HDR_LEN) / REPLICA_ENTRY_LEN)

/* part 間に挟む送信間隔 (ms)。タスクコンテキスト (replica_task の定期
 * ラウンド) でのみ適用し、バーストを抑える。コールバックコンテキスト
 * (boot cry 応答 / swim ALIVE 遷移 / 断末魔) では眠れないので連続送信。 */
#define REPLICA_PART_GAP_MS  2

typedef struct {
    UW   magic;                                /* REPLICA_MAGIC            */
    UB   version;                              /* REPLICA_VERSION          */
    UB   type;                                 /* REPLICA_ANNOUNCE / DATA  */
    UB   src_node;
    UB   entry_cnt;                            /* この part の entries 数  */
    UH   round_seq;                            /* スナップショット世代番号 */
    UB   part_idx;                             /* 0 .. part_cnt-1          */
    UB   part_cnt;                             /* このラウンドの総 part 数 */
    REPLICA_ENTRY entries[REPLICA_ENTRY_MAX];  /* トピックスナップショット */
} __attribute__((packed)) REPLICA_PKT;
/* header=12 + 5×232=1160 = 1172 bytes (PMESH_DATA_MAX 内) */

_Static_assert(sizeof(REPLICA_ENTRY) == REPLICA_ENTRY_LEN,
               "REPLICA_ENTRY wire size drifted");
_Static_assert(sizeof(REPLICA_PKT) ==
               REPLICA_HDR_LEN + REPLICA_ENTRY_MAX * REPLICA_ENTRY_LEN,
               "REPLICA_PKT header size drifted");
_Static_assert(sizeof(REPLICA_PKT) <= PMESH_DATA_MAX,
               "REPLICA_PKT must fit one pmesh UDP packet");
_Static_assert(REPLICA_ENTRY_MAX >= 1,
               "KDDS_DATA_MAX too large for a single replica entry");
/* part_idx/part_cnt は UB: 全アイテム (トピック+tombstone) が 255 part
 * に収まることを保証する */
_Static_assert((KDDS_TOPIC_MAX + REPLICA_TOMB_MAX + REPLICA_ENTRY_MAX - 1)
                   / REPLICA_ENTRY_MAX <= 255,
               "part_cnt (UB) would overflow");

/* ------------------------------------------------------------------ */
/* 統計情報                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  sent_pkts;   /* 送信パケット数                                 */
    UW  recv_pkts;   /* 受信パケット数                                 */
    UW  merged;      /* リモートデータで上書きしたトピック更新数       */
    UW  skipped;     /* ローカルが最新だったため無視した数             */
    UW  recovered;   /* 復元した新規トピック数                         */
    /* v2 観測用: 最後に受信したラウンドの part/entry 集計 */
    UW  rnd_entries; /* 当該ラウンドで受信した entry 総数              */
    UB  rnd_parts;   /* 当該ラウンドで受信した part 数                 */
    UB  rnd_total;   /* 送信側が宣言した part_cnt                      */
    UB  rnd_src;     /* 当該ラウンドの送信元ノード                     */
    UH  rnd_seq;     /* 当該ラウンドの round_seq                       */
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
void replica_rx(UB src_node, UH dst_port, const UB *data, UH len);

/* 新規/復帰ノードへ即座に全トピックをプッシュする (swim.c から呼ぶ)。 */
void replica_push_to(UB node_id);

/* 起動の叫び: 全ノード IP へ ANNOUNCE を送り「記憶をよこせ」と要求する。
 * replica_task() 起動冒頭から呼ぶ。 */
void replica_boot_cry(void);

/* 断末魔: 自分が SUSPECT と噂されたとき全 ALIVE ノードへ即座にデータを散布。
 * swim.c の gossip_apply() から呼ぶ。 */
void replica_scatter_all(void);

/* Tombstone: トピックの削除を全 ALIVE ノードへ伝播する。
 * kdds_delete_cluster() から呼ぶ。ローカルトピックは呼び出し前に消す。 */
void replica_tombstone(const char *name);

/* 統計表示 (shell `replica stat` から呼ぶ)。 */
void replica_stat(void);
