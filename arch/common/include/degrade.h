/*
 *  degrade.h (x86)
 *  縮退モード管理 — クラスタのノード数に応じて動作モードを自動切換え
 *
 *  レベル定義:
 *    DEGRADE_FULL    (≥3 ノード): 完全分散モード — DTR 分散推論・3 秒レプリカ
 *    DEGRADE_REDUCED (2  ノード): 縮退モード     — DTR パイプライン・2 秒レプリカ
 *    DEGRADE_SOLO    (1  ノード): 単独モード     — DTR ローカルのみ・1 秒レプリカ
 *                                                  SOLO 遷移時に全記憶を即時散布
 *
 *  統合方法:
 *    swim.c  → ALIVE/DEAD 遷移後に degrade_update() を呼ぶ
 *    replica.c → degrade_replica_interval() でスリープ時間を取得
 *    dtr.c   → SOLO なら分散パスをスキップ
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define DEGRADE_FULL     0   /* 3 ノード以上: 完全分散           */
#define DEGRADE_REDUCED  1   /* 2 ノード    : 縮退               */
#define DEGRADE_SOLO     2   /* 1 ノード    : 単独 (孤立)        */

/* ------------------------------------------------------------------ */
/* capacity(N) — 連続容量 (regions.md §3.2)                            */
/*                                                                     */
/* 3 段の degrade レベルは「人間が読む粗いバンド」に降格し、分散戦略の   */
/* 内部判断は以下の連続関数の数値で行う。容量は3軸の積で表現する:        */
/*   breadth (expert 数) × depth (層数) × KV-context (文脈の広さ)        */
/* width (d_model) は重みの再形成=学習を要するので capacity(N) には含め  */
/* ない (R3 の課題)。                                                   */
/* ------------------------------------------------------------------ */

/* breadth の上限。expert ≒ ノードだが、現ノード数を超える expert を     */
/* 抱える余地 (provisioning headroom) を表すため DNODE_MAX とは独立。     */
#define CAP_E_MAX        16

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

/* usermain() の分散モードブロック内で drpc_init() の後に呼ぶ。 */
void degrade_init(void);

/*
 * ノードの ALIVE/DEAD/SUSPECT 状態が変わるたびに swim.c から呼ぶ。
 * 生存ノード数を数え直してレベルを更新する。
 * SOLO への遷移時は replica_scatter_all() を自動呼び出し。
 */
void degrade_update(void);

/* 現在のレベル (DEGRADE_FULL / REDUCED / SOLO) を返す。 */
UB   degrade_level(void);

/* replica_task のスリープ時間 (ms) を返す。 */
TMO  degrade_replica_interval(void);

/* shell `degrade` コマンド用: 現在のレベルと統計を表示。 */
void degrade_stat(void);

/* ------------------------------------------------------------------ */
/* capacity(N) 公開 API                                                */
/* ------------------------------------------------------------------ */

/* breadth: experts_active(N) = clamp(生存ノード数, 1, CAP_E_MAX)。
 * ノード ≒ エキスパート。join で MoE の expert が増える。 */
UW capacity_experts(void);

/* depth: pipeline_depth(N) = 1 + floor(log2(region_size))。
 * 台数の対数で pipeline 並列の段数を深くする。region 内 (密) で測る。 */
UW capacity_depth(void);

/* KV-context: 直近の DKVA 集約で実際に畳み込んだ KV エントリ総数。
 * region 内 → region 間の階層集約で見た「集合記憶」の広さ。
 * 推論前は region_size × DKVA_CACHE_SIZE の見積りを返す。 */
UW capacity_kv(void);

/* 容量の粗い 1 数値 = experts × depth × kv。台数 N とともに増える。 */
UW capacity_score(void);

/* dkva.c が階層集約のたびに、実測した KV エントリ総数を通知する。
 * capacity_kv() がこの実測値を返すようになる。 */
void capacity_note_kv(UW entries);

/* ------------------------------------------------------------------ */
/* capacity(N) 純粋関数 (テスト可能) — 宣言資源のみから計算する。      */
/* live なゲッターはこれらに module/extern 状態を渡す薄いラッパー。    */
/* ------------------------------------------------------------------ */
UW cap_experts_of(UW n);              /* clamp(n, 1, CAP_E_MAX)        */
UW cap_depth_of(UW rs);               /* 1 + floor(log2(rs))           */
UW cap_kv_of(UW rs, UW measured);     /* measured>0 ? measured : rs*KV */
UW cap_score_of(UW n, UW rs, UW measured);  /* experts*depth*kv        */

/* capacity(N) cert: 積同一性・単調性・境界クランプを純粋関数の sweep  */
/* で主張する。`[capacity-score] PASS/FAIL` を印字し fail 数を返す。   */
/* shell `capacity test` / CI が叩く。決定論的 (クラスタ状態に非依存)。*/
INT capacity_self_test(void);
