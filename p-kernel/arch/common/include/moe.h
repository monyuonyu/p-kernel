/*
 *  moe.h (x86)
 *  Phase 10 — Mixture of Experts (MoE) 推論ルーティング
 *
 *  各ノードが「得意なクラス」を持ち、入力センサーデータを
 *  最も得意なノードへ自動ルーティングする。
 *
 *  Expert スコア:
 *    各ノードは class [0,1,2] に対して accuracy[class] を持つ。
 *    ルーター (Gate) は入力特徴量から「どのクラスっぽいか」を予測し、
 *    そのクラスのスコアが最高のノードへ推論を委譲する。
 *
 *  K-DDS トピック "moe/score" でスコアを定期ブロードキャスト。
 *  受信ノードはピアのスコアテーブルを更新する。
 *
 *  ルーティング:
 *    1. Gate: 入力の平均値からクラスを予測 (軽量線形分類)
 *    2. 最も得意なノードを選択
 *    3. DRPC で推論を委譲 (DRPC_CALL_INFER)
 *    4. フォールバック: タイムアウト時はローカル推論
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 定数                                                                */
/* ------------------------------------------------------------------ */

#define MOE_NUM_CLASSES    3    /* クラス数 */
/* スコアはノードごとに別トピック "moe/score/<node>" へ pub する。単一
 * "moe/score" を LATEST_ONLY で共有すると複数ノードの broadcast が1スロットを
 * 上書きし合い、gating 時に全 peer のスコアが揃わない (DKVA と同じ単一スロット
 * fan-in 問題)。per-source topic で各ノードが独立スロットを持つ。 */
#define MOE_SCORE_TOPIC_PFX "moe/score/"   /* + 1 桁ノード ID            */
#define MOE_BROADCAST_MS   5000 /* スコアブロードキャスト間隔 (ms) */
#define MOE_POLL_MS        200  /* peer スコア取り込みのポーリング間隔 */

/* locality-gradient ゲーティング (regions R3 — §7 分散ゲーティング)
 *
 *   utility(node) = accuracy[class]
 *                   - rtt_ms / MOE_RTT_MS_PER_POINT          (近さ: §8 距離)
 *                   - pressure * MOE_PRESS_NUM / MOE_PRESS_DEN (余力: §6 勾配)
 *                   + (同 region なら) MOE_SAME_REGION_BONUS  (反射層を優先)
 *
 * R1 との違い: R1 は「全員が叫んだ accuracy のグローバル最大」を取っていた
 * (= 暗黙のグローバルビュー = 準・中央集権)。R3 は逼迫度 (world ビーコンで
 * ゴシップされた *局所* 勾配信号) を引き、応援可能な近傍ノードへ仕事を流す。
 * 決定は world_peer_pressure()/swim_rtt_ms() という局所窓口だけを読み、
 * 中央/グローバルなオラクルは一切参照しない (NO-CENTRAL 不変条件)。 */
#define MOE_RTT_MS_PER_POINT  20   /* RTT 20ms ごとに effort 1 点減点      */
#define MOE_RTT_UNKNOWN_MS    100  /* RTT 未実測ノードの想定距離 (ms)      */

/* 逼迫度ペナルティ: pressure(0..100) を a 倍して utility から引く。
 * a = NUM/DEN = 0.5。pressure 100 (=満杯) で 50 点ペナルティ ≒ accuracy を
 * 帳消しにする強さ。「余力のある方へ勾配を下る」(§6) を効かせる係数。 */
#define MOE_PRESS_NUM   1
#define MOE_PRESS_DEN   2
#define MOE_PRESS_UNKNOWN  50   /* 逼迫度未知ノードの想定 (中庸) */

/* 同一 region (反射層 §8) の近傍をわずかに優先する加点。光速で間に合う
 * 群れに即応を閉じ込め、大域の遅延に乗せない (§8 反射層/熟慮層)。 */
#define MOE_SAME_REGION_BONUS  5

/* ── §8 ヒステリシス / 減衰 (発振への処方) ───────────────────────────
 * 素朴な勾配は「いま一番空いてるノード」へ全員が殺到し、そのノードが
 * 瞬間的に逼迫 → 全員が逃げる、という速い振動 (発振) を起こす。応援殺到
 * の速い局所ループ (反射層) と、ゴシップで届く遅い逼迫度信号 (熟慮層) の
 * 時定数差がこの振動の正体。
 *
 * 処方 (§8 二層構造): 直近に自分が選んだノードへ、まだビーコンが追いつく
 * 前の短時間だけ「自己観測の仮想負荷」を上乗せして見る。これは自分が今
 * 投げた仕事の分を局所に先取りカウントする = 速い反射層側のダンピング。
 *   - 1 回選ぶごとに recent_pick[node] += MOE_PICK_LOAD
 *   - 毎回の選択時に全ノードの recent_pick を MOE_PICK_DECAY_NUM/DEN で減衰
 * これでゴシップ周期 (WORLD_BEACON_MS=3s) より速い連続選択でも、同じ空き
 * ノードへ一気に殺到せず、選択が近傍へ分散する (発振の抑制)。 */
#define MOE_PICK_LOAD        25   /* 1 回選ぶと +25 の仮想逼迫を自己加算    */
#define MOE_PICK_DECAY_NUM   2    /* 毎決定ごとに recent_pick *= 2/3 で減衰 */
#define MOE_PICK_DECAY_DEN   3

/* ------------------------------------------------------------------ */
/* スコアパケット (K-DDS 経由でブロードキャスト)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  node_id;
    UB  accuracy[MOE_NUM_CLASSES];   /* 各クラスの正答率 (0-100%) */
    UW  total_infer;                 /* 総推論回数                */
    UW  correct[MOE_NUM_CLASSES];    /* クラス別正解数            */
} MOE_SCORE;

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void moe_init(void);
void moe_task(INT stacd, void *exinf);

/* 推論実行: 最適ノードを選んで推論し、クラスを返す */
UB   moe_infer(B temp, B hum, B press, B light);

/* 推論結果をフィードバック (正解ラベルを学習) */
void moe_feedback(UB pred_class, UB true_class);

/* ピアのスコアを更新 (K-DDS 受信コールバックから呼ぶ) */
void moe_update_peer(const MOE_SCORE *score);

/* 統計表示 */
void moe_stat(void);
