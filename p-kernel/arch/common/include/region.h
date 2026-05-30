/*
 *  region.h
 *  Regions — 遅延クラスタによる脳の領域分割 (R0)
 *
 *  設計: docs/architecture/regions.md
 *
 *  Region とは「相互 RTT が閾値 REGION_TAU_MS 以内のノード集合」。
 *  物理的・ネットワーク的に近いノードが1つの region を成し、region 内は
 *  密に (DKVA / MoE / replica)、region 間は疎に通信する。
 *
 *  v1 (このファイル) は各ノードの自己中心的 (egocentric) な局所ビュー:
 *    自 region = { 自分 } ∪ { ALIVE かつ RTT≤τ のノード }
 *    coordinator = region 内の最小ノード ID (決定的)
 *  RTT の非対称性やメンバシップ差により、ノード間でビューが完全一致する
 *  保証はまだ無い (region 間の合意は regions.md の未解決問題)。それでも
 *  「近さで束ねる」プリミティブとして、K-DDS スコープや locality-MoE が
 *  この上に乗れる。
 */

#pragma once
#include "drpc.h"   /* dnode_table[], DNODE_MAX, DNODE_ALIVE, drpc_my_node */

/* RTT がこの値 (ms) 以内なら同一 region とみなす */
#define REGION_TAU_MS   50

/* 現在の RTT / membership から自 region を再計算する。
 * 安価 (O(DNODE_MAX)) なので問い合わせ時に都度呼んでよい。 */
void region_recompute(void);

/* 自 region の ID (= coordinator のノード ID)。
 * 分散モード未確立 (drpc_my_node==0xFF) なら 0xFF。 */
UB   region_id(void);

/* 自 region の coordinator ノード ID (region 内の最小 ID)。 */
UB   region_coordinator(void);

/* node が自分と同じ region に属するか。 */
BOOL region_contains(UB node);

/* 自 region のメンバ数 (自分を含む)。 */
UB   region_size(void);

/* shell `region` 表示。 */
void region_print(void);
