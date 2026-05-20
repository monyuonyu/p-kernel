/*
 *  swim.h (x86)
 *  SWIM — Scalable Weakly-consistent Infection-style Membership
 *
 *  Phase 1: Biotic Mesh Network
 *
 *  既存の drpc ハートビートに以下を追加する:
 *    - 直接プローブ   (SWIM_PING / SWIM_ACK) — 1 ラウンドに 1 ノードを選択
 *    - 間接プローブ   (SWIM_PING_REQ) — K=2 台のヘルパーに代理プローブを依頼
 *    - ゴシップ伝播   — 全 SWIM パケットにメンバーシップ変化をピギーバック
 *
 *  ノードテーブル: drpc と共有 (dnode_table[] / drpc_my_node)
 *  UDP ポート    : SWIM_PORT 7375
 *
 *  プローブラウンド (SWIM_PROBE_INTERVAL_MS ごと):
 *    1. ラウンドロビンで次のピアを選ぶ
 *    2. SWIM_PING を送り、SWIM_PROBE_TMO_MS 以内の SWIM_ACK を待つ
 *    3. ACK なし → K 台のヘルパーに SWIM_PING_REQ を送り、SWIM_IND_TMO_MS 待つ
 *    4. それでも無応答 → 状態を ALIVE→SUSPECT→DEAD に昇格 + ゴシップ
 *    5. 正常応答があれば ALIVE を確認 + 状態変化があればゴシップ
 */

#pragma once
#include "drpc.h"   /* dnode_table[], DNODE_MAX, DNODE_*, drpc_my_node */

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define SWIM_PORT              7375
#define SWIM_GOSSIP_MAX        3        /* 1 パケットあたりのゴシップ数      */
#define SWIM_K_HELPERS         2        /* 間接プローブのヘルパー数          */
#define SWIM_PROBE_INTERVAL_MS 1000     /* プローブラウンド間隔 (ms)         */
#define SWIM_PROBE_TMO_MS      400      /* 直接プローブタイムアウト          */
#define SWIM_IND_TMO_MS        500      /* 間接プローブタイムアウト          */
#define SWIM_DEAD_ROUNDS       3        /* SUSPECT ラウンド数 → DEAD        */

/* ------------------------------------------------------------------ */
/* パケットフォーマット                                                */
/* ------------------------------------------------------------------ */

#define SWIM_MAGIC    0x4D494D53UL   /* "SWIM" little-endian */
#define SWIM_VERSION  1

#define SWIM_PING      0x20   /* 直接プローブ; SWIM_ACK を期待         */
#define SWIM_ACK       0x21   /* 生存確認の返答                         */
#define SWIM_PING_REQ  0x22   /* probe_target への代理プローブを依頼    */

/* ゴシップイベント 1 件 */
typedef struct {
    UB  node_id;
    UB  state;          /* DNODE_ALIVE / DNODE_SUSPECT / DNODE_DEAD */
    UB  incarnation;    /* より新しい incarnation で古い疑惑を上書き */
    UB  _pad;
} __attribute__((packed)) SWIM_GOSSIP_EVT;

/* SWIM パケット本体 (24 bytes) */
typedef struct {
    UW  magic;                               /* SWIM_MAGIC              */
    UB  version;                             /* SWIM_VERSION            */
    UB  type;                                /* SWIM_PING/ACK/PING_REQ  */
    UH  seq;                                 /* PING↔ACK 照合用         */
    UB  src_node;
    UB  probe_target;   /* PING_REQ: プローブ対象; ACK: 確認対象       */
    UB  gossip_cnt;     /* gossip[] の有効エントリ数 (0..SWIM_GOSSIP_MAX) */
    UB  _pad;
    SWIM_GOSSIP_EVT gossip[SWIM_GOSSIP_MAX]; /* ピギーバックイベント    */
} __attribute__((packed)) SWIM_PKT;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* drpc_init() の後に呼ぶ。SWIM_PORT へ UDP バインドする。 */
void swim_init(void);

/* T-Kernel タスク本体 (優先度 6, スタック 4096)。
 * プローブループを実行し dnode_table[] を in-place 更新する。 */
void swim_task(INT stacd, void *exinf);

/* UDP 受信コールバック (swim_init() が SWIM_PORT に登録する)。 */
void swim_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* SWIM 強化版クラスタ表示 (shell `nodes` から呼ぶ)。 */
void swim_nodes_print(void);
