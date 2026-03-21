/*
 *  edf.h (x86)
 *  リアルタイム AI スケジューリング — フェーズ 4
 *
 *  推論 SLA (Service Level Agreement) をカーネルスケジューラが管理する。
 *  「N ミリ秒以内に推論しないと廃棄」という締め切りを宣言すると、
 *  カーネルが最適なノードへ自動的にルーティングする。
 *
 *  動作概要:
 *    1. 各ノードが K-DDS "L/N" トピックへ AI キュー負荷を 500ms ごとに発行
 *    2. edf_infer(sensor, deadline_ms) 呼び出し時にノード選択
 *         - deadline < 5ms  : 負荷 > 10% → 最軽量 ALIVE ノードへオフロード
 *         - deadline < 20ms : 負荷 > 50% → 最軽量 ALIVE ノードへオフロード
 *         - deadline >= 20ms: 常にローカル実行
 *    3. 実際の処理時間を計測し SLA 達成/違反を記録
 *    4. sys_infer_sla syscall (0x230) でユーザー空間から呼べる
 *
 *  syscall API:
 *    SYS_INFER_SLA (0x230) — edf_infer(sensor_packed, deadline_ms)
 */

#pragma once
#include "drpc.h"
#include "ai_kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

/* 負荷トピック名: "L/N" (N = ノード ID 0-7)                         */
#define EDF_LOAD_TOPIC_PREFIX  "L/"

/* オフロード判定しきい値 (負荷スコア 0-100)                         */
#define EDF_STRICT_THR   10    /* deadline < 5ms 用                  */
#define EDF_NORMAL_THR   50    /* deadline < 20ms 用                 */

/* ------------------------------------------------------------------ */
/* 統計情報                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    UW  total;       /* 累計推論要求数                                */
    UW  local;       /* ローカルで実行した推論数                      */
    UW  offloaded;   /* リモートへオフロードした推論数                */
    UW  sla_hit;     /* 期限内に完了した推論数                        */
    UW  sla_miss;    /* 期限を超えた推論数                            */
} EDF_STATS;

extern EDF_STATS edf_stats;
extern UB        peer_load[DNODE_MAX];  /* 各ノードの最新負荷 (0-100) */

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() 内で kdds_init() の後に呼ぶ。 */
void edf_init(void);

/* 負荷発行・受信タスク (優先度 7, スタック 2048)。 */
void edf_load_task(INT stacd, void *exinf);

/* ローカルの AI キュー負荷スコアを返す (0-100)。 */
UB   edf_local_load(void);

/*
 * SLA 付き推論。
 *   sensor_packed : SENSOR_PACK(temp, hum, press, light) で詰めた 4 値
 *   deadline_ms   : 許容最大レイテンシ (ms)
 *   returns       : クラス (0=normal, 1=alert, 2=critical) または負のエラー
 */
W    edf_infer(W sensor_packed, W deadline_ms);

/* 統計表示 (shell `edf stat` から呼ぶ)。 */
void edf_stat(void);
