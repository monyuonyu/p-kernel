/*
 *  kdds.h (x86)
 *  K-DDS — カーネルネイティブ Data Distribution Service
 *
 *  Phase 2: 「すべてはトピック」
 *
 *  Unix の「すべてはファイル」に対して、p-kernel はトピックを一級市民とする。
 *  アプリケーションは sys_topic_pub/sub を呼ぶだけで、同一ノード内・
 *  複数ノード間を問わず同じ API でデータを共有できる。
 *
 *  syscall API:
 *    SYS_TOPIC_OPEN  (0x220) — トピックを開く / 作成する
 *    SYS_TOPIC_PUB   (0x221) — データを発行する
 *    SYS_TOPIC_SUB   (0x222) — データを受信する (ブロッキング)
 *    SYS_TOPIC_CLOSE (0x223) — ハンドルを閉じる
 *
 *  QoS ポリシー:
 *    KDDS_QOS_BEST_EFFORT  — 順序・信頼性保証なし
 *    KDDS_QOS_RELIABLE     — 遅延参加者にも最新値を配信 (store-and-forward)
 *    KDDS_QOS_LATEST_ONLY  — 最新値のみ保持 (センサーデータ向け)
 *
 *  リモート配送 (分散モード):
 *    kdds_pub() 呼び出し時に drpc_my_node != 0xFF なら、
 *    ALIVE 状態の全ノードへ KDDS_PKT を UDP ブロードキャストする。
 *    各受信ノードはローカルの subscriber に配信する。
 */

#pragma once
#include "kernel.h"
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define KDDS_PORT        7376
#define KDDS_TOPIC_MAX   8      /* カーネルが同時に管理できるトピック数    */
#define KDDS_HANDLE_MAX  16     /* 同時オープンハンドル数 (2 per topic)   */
#define KDDS_NAME_MAX    32     /* トピック名の最大長 (null 含む)          */
#define KDDS_DATA_MAX    128    /* トピックデータの最大バイト数            */
#define KDDS_SUB_MAX     4      /* トピックあたりの最大サブスクライバ数    */

/* QoS ポリシー */
#define KDDS_QOS_BEST_EFFORT  0
#define KDDS_QOS_RELIABLE     1
#define KDDS_QOS_LATEST_ONLY  2

/* ------------------------------------------------------------------ */
/* 内部トピックスロット (カーネル側)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[KDDS_NAME_MAX];  /* トピック名 (例: "sensor/temperature") */
    UB    data[KDDS_DATA_MAX];  /* 最新の発行データ                       */
    UH    data_len;             /* データバイト数 (0 = 未発行)            */
    UB    qos;                  /* KDDS_QOS_*                             */
    UB    open;                 /* 1 = 使用中                             */
} KDDS_TOPIC;

extern KDDS_TOPIC kdds_topics[KDDS_TOPIC_MAX];

/* ------------------------------------------------------------------ */
/* ハンドルテーブル (カーネル側)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    W   topic_idx;  /* kdds_topics[] へのインデックス (-1 = 未使用)  */
    ID  sub_sem;    /* subscriber がブロックするセマフォ (-1 = なし) */
    UB  open;       /* 1 = 使用中                                    */
} KDDS_HANDLE_SLOT;

extern KDDS_HANDLE_SLOT kdds_handles[KDDS_HANDLE_MAX];

/* ------------------------------------------------------------------ */
/* ネットワークパケット                                                */
/* ------------------------------------------------------------------ */

#define KDDS_MAGIC      0x5344444BUL   /* "KDDS" little-endian */
#define KDDS_VERSION    1
#define KDDS_DATA_PKT   0x01           /* データ配信パケット             */

typedef struct {
    UW   magic;                    /* KDDS_MAGIC                         */
    UB   version;                  /* KDDS_VERSION                       */
    UB   type;                     /* KDDS_DATA_PKT                      */
    UB   src_node;
    UB   _pad;
    UH   data_len;                 /* data[] の有効バイト数              */
    UH   name_len;                 /* name[] の有効バイト数 (null 含む)  */
    char name[KDDS_NAME_MAX];
    UB   data[KDDS_DATA_MAX];
} __attribute__((packed)) KDDS_PKT;  /* 4+1+1+1+1+2+2+32+128 = 172 bytes */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* 初期化 — usermain() から drpc_init() の後に呼ぶ */
void kdds_init(void);

/* トピックを開く / 作成する。ハンドル (0..KDDS_HANDLE_MAX-1) を返す。
 * 失敗時は負のエラーコード。 */
W kdds_open(const char *name, W qos);

/* データをトピックへ発行する。
 * ローカルの subscriber を起こし、分散モードなら全 ALIVE ノードへ送信。
 * 成功時は 0、失敗時は負のエラーコード。 */
W kdds_pub(W handle, const void *data, W len);

/* トピックの次の値を受信する (ブロッキング)。
 * buf へデータをコピーし、受信バイト数を返す。
 * timeout_ms: -1=無限待ち, 0=ポーリング。
 * 失敗時は負のエラーコード。 */
W kdds_sub(W handle, void *buf, W buflen, W timeout_ms);

/* ハンドルを閉じる */
void kdds_close(W handle);

/* UDP 受信コールバック (KDDS_PORT に登録) */
void kdds_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* トピックテーブルを表示 (shell `topic list`) */
void kdds_list(void);
