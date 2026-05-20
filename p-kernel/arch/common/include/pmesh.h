/*
 *  pmesh.h (x86)
 *  p-mesh — カーネルネイティブ メッシュルーティング層
 *
 *  Phase 10 前準備: 物理層（Wi-Fi / Bluetooth / 有線）に依存しない
 *  メッシュルーティングを提供する。
 *
 *  役割:
 *    - 直接届かないノードへ中継転送する (Distance Vector ルーティング)
 *    - 物理層を抽象化し、上位プロトコル (SWIM/K-DDS/SFS) は
 *      物理層を意識せず pmesh_send() だけで通信できる
 *    - 将来の Wi-Fi Ad-hoc / Bluetooth Mesh への差し替えに備えた設計
 *
 *  現在の物理層: UDP (既存ネットワークスタック)
 *  UDP ポート: PMESH_PORT 7380
 *
 *  パケット種別:
 *    PMESH_BEACON  — 2 秒周期ブロードキャスト、ルーティングテーブルを配布
 *    PMESH_DATA    — ペイロードをラップして宛先ノードまで中継
 *
 *  ルーティング: Distance Vector (Bellman-Ford)
 *    各ノードが隣接ノードのルーティングテーブルを受け取り、
 *    最小ホップ数で到達できる経路を自動的に学習する。
 */

#pragma once
#include "kernel.h"
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define PMESH_PORT          7380
#define PMESH_MAGIC         0x4853454DUL   /* "MESH" little-endian   */
#define PMESH_VERSION       1

/* パケットタイプ */
#define PMESH_BEACON        0x01   /* ルーティングテーブルのブロードキャスト */
#define PMESH_DATA          0x02   /* ペイロード転送（中継あり）              */

#define PMESH_DATA_MAX      1380   /* DATA ペイロード最大バイト数             */
#define PMESH_BIND_MAX      8      /* pmesh_bind() の最大登録数              */
#define PMESH_ROUTE_EXPIRE  5      /* この beacon 周期数を超えたら経路を破棄  */
#define PMESH_COST_INF      255    /* 到達不可能を表すコスト値                */

/* ------------------------------------------------------------------ */
/* パケット構造                                                        */
/* ------------------------------------------------------------------ */

/* BEACON に含まれる 1 エントリ (4 bytes) */
typedef struct {
    UB dst_node;   /* 宛先ノード ID           */
    UB cost;       /* そこへのホップ数        */
    UB _pad[2];
} __attribute__((packed)) PMESH_ROUTE_ENTRY;

/* BEACON パケット: 8 + DNODE_MAX×4 = 40 bytes */
typedef struct {
    UW magic;                              /* PMESH_MAGIC             */
    UB version;                            /* PMESH_VERSION           */
    UB type;                               /* PMESH_BEACON            */
    UB src_node;                           /* 送信ノード ID            */
    UB entry_cnt;                          /* entries[] 有効数        */
    PMESH_ROUTE_ENTRY entries[DNODE_MAX];  /* ルーティングテーブル     */
} __attribute__((packed)) PMESH_BEACON_PKT;

/* DATA パケット: 12 + data[] bytes (最大 1392 bytes < UDP 1400 制限) */
typedef struct {
    UW magic;        /* PMESH_MAGIC                        */
    UB version;      /* PMESH_VERSION                      */
    UB type;         /* PMESH_DATA                         */
    UB src_node;     /* 元の送信ノード ID                   */
    UB dst_node;     /* 最終宛先ノード ID                   */
    UH dst_port;     /* 宛先で配送するポート番号             */
    UH data_len;     /* data[] の有効バイト数               */
    UB data[PMESH_DATA_MAX];
} __attribute__((packed)) PMESH_DATA_PKT;

/* ------------------------------------------------------------------ */
/* ルーティングテーブルエントリ (内部)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UB next_hop;  /* 次に送るべきノード ID (0xFF = 経路なし) */
    UB cost;      /* ホップ数                               */
    UB age;       /* 最終更新からの beacon 周期数           */
    UB active;    /* 1 = 有効                              */
} PMESH_ROUTE;

extern PMESH_ROUTE pmesh_routes[DNODE_MAX];

/* ------------------------------------------------------------------ */
/* 統計                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UW beacon_tx;       /* 送信した BEACON 数              */
    UW beacon_rx;       /* 受信した BEACON 数              */
    UW data_tx;         /* pmesh_send() で送信した数       */
    UW data_rx;         /* 受信した DATA 数                */
    UW data_relay;      /* 中継転送した DATA 数            */
    UW data_delivered;  /* ローカル配送した DATA 数        */
    UW no_route;        /* 経路なしで破棄した数            */
} PMESH_STATS;

extern PMESH_STATS pmesh_stats;

/* ------------------------------------------------------------------ */
/* 受信コールバック型                                                  */
/* ------------------------------------------------------------------ */

typedef void (*pmesh_recv_fn)(UB src_node, UH dst_port,
                              const UB *data, UH len);

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* 初期化 — drpc_init() の後に呼ぶ */
void pmesh_init(void);

/* beacon 送信・経路エージングタスク (priority 8, stack 2048) */
void pmesh_task(INT stacd, void *exinf);

/* UDP 受信コールバック (PMESH_PORT に登録) */
void pmesh_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* ノード ID 指定で送信。直接届かない場合は中継経路を使う。
 * 成功: 0 / 経路なし: -1 */
W pmesh_send(UB dst_node, UH dst_port, const UB *data, UH len);

/* ポートに対する受信コールバックを登録 */
W pmesh_bind(UH port, pmesh_recv_fn fn);

/* ルーティングテーブルを表示 (shell `mesh route`) */
void pmesh_route_list(void);

/* 統計を表示 (shell `mesh stat`) */
void pmesh_stat(void);
