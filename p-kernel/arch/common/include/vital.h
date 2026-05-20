/*
 *  vital.h (x86)
 *  生命兆候モニタリング — フェーズ 6: 生存本能
 *
 *  各ノードが K-DDS トピック "vital/N" に自分の健康状態を毎秒発行する。
 *  隣のノードはこれを受信して「隣が生きている」ことをリアルタイムで確認できる。
 *
 *  発行データ (VITAL_DATA):
 *    - uptime_s     : 起動からの秒数
 *    - peers_alive  : 現在 ALIVE と認識しているピア数
 *    - topics       : アクティブなK-DDSトピック数
 *    - replica_sent : 送信した複製パケット数
 *    - replica_recv : 受信した複製パケット数
 *    - recovered    : 復元したトピック数
 *
 *  shell コマンド:
 *    vital stat  — クラスタ全ノードの生命兆候一覧表示
 */

#pragma once
#include "kdds.h"
#include "drpc.h"

/* ------------------------------------------------------------------ */
/* 生命兆候データ構造 (K-DDS トピックのペイロード)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  node_id;        /* このノードの ID                             */
    UB  peers_alive;    /* 現在 ALIVE と認識しているピア数             */
    UB  topics;         /* アクティブな K-DDS トピック数               */
    UB  _pad;
    UW  uptime_s;       /* 起動からの秒数                              */
    UW  replica_sent;   /* 送信した複製パケット数 (生存活動の証拠)     */
    UW  replica_recv;   /* 受信した複製パケット数                      */
    UW  recovered;      /* 復元したトピック数 (記憶回復の証拠)         */
} __attribute__((packed)) VITAL_DATA;   /* 20 bytes */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* 生命兆候タスク (優先度 9, スタック 2048)。 */
void vital_task(INT stacd, void *exinf);

/* クラスタ健康状態一覧表示 (shell `vital stat`)。 */
void vital_stat(void);
