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

/* ── 二層の時定数 (reflex-deliberation.md §4.2 / D2 層の明示分離) ─────
 * 「近傍は速く、全体は遅く」。この2定数が反射層と熟慮層の帯域差を体現する。
 *
 *   MOE_REFLEX_TICK_MS : 反射層の基本 tick。moe_task のループ周期であり、
 *                        反射状態 (recent_pick / utility EWMA / incumbent)
 *                        が動く速い時定数。決定そのもの (select_expert) は
 *                        要求駆動でさらに速い。
 *   MOE_DELIB_TICK_MS  : 熟慮層の tick。peer スコア取り込みと自 accuracy の
 *                        再計算 (= 反射層が読む「賢さ」テーブルの更新) は
 *                        この周期でしか起きない。反射の瞬間スパイクを観測
 *                        しないローパスとして、意図的に低帯域 (§4.2)。
 *   MOE_BROADCAST_MS   : 熟慮層の発信側 (スコア gossip)。最も遅い。
 *
 * 比は 1 : 10 : 25。比が小さすぎると熟慮が反射のスパイクに反応して発振が
 * 層をまたぎ、大きすぎると学習が鈍る (reflex-deliberation.md §7-5 未解決 —
 * 当面は経験則)。 */
#define MOE_REFLEX_TICK_MS  200   /* 反射層: 速い時定数               */
#define MOE_DELIB_TICK_MS  2000   /* 熟慮層: 遅い時定数 (×10)         */
#define MOE_BROADCAST_MS   5000   /* 熟慮層: スコア gossip 間隔 (×25) */

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

/* ── 反射ゲートの damping (reflex-deliberation.md §4.3 / D1) ─────────
 * recent_pick が「殺到の分散」なら、こちらは「切替そのものの安定化」。
 * select_expert は瞬間 utility の argmax ではなく:
 *
 *   1. utility EWMA — 各候補の utility を α=1/MOE_UTIL_EWMA_DIV で平滑化
 *      (swim.c の RTT EWMA と同じ思想)。単一サンプルのノイズ
 *      (ビーコン 1 個の遅延、瞬間スパイク) でルーティングが反転しない。
 *   2. デッドバンド — 現職 (incumbent) から乗り換えるには、挑戦者の EWMA が
 *      現職の EWMA を MOE_SWITCH_MARGIN 以上 *持続的に* 上回る必要がある。
 *      応援に行く閾値と引き上げる閾値をずらす古典的ヒステリシス (§4.2)。
 *
 * これは最適化ではなく安定化 (§4.3 「振動しない選択」) であり、§7 分散
 * ゲーティングの正しさの前提条件。マージンの目安: 同点ピア間で recent_pick
 * の自己仮想負荷 (連続選択の定常 ≃ 75 → utility -37) が EWMA に浸透する
 * までは現職を保持し、数決定の dwell の後に滑らかに隣へ移る
 * (stable-then-shift)。毎回交互の ping-pong はこのマージンの下で消える。 */
#define MOE_UTIL_EWMA_DIV   4    /* EWMA α = 1/4                            */
#define MOE_SWITCH_MARGIN   12   /* 乗り換えに要する EWMA utility 差        */

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

/* §7 ゲートの公開ラッパー: 入力のクラス帯を返す (0..MOE_NUM_CLASSES-1)。
 * R3b spec.c が専門分化した専門家を疎に発火させるルーティングに使う。 */
UB   moe_gate_predict(B temp, B hum, B press, B light);

/* §7 ゲートの効用関数を公開する (重複定義なし)。reflex.c の closed-loop
 * self-test が「行動→知覚→ゲート」の負帰還を *本番と同一の数式* で測るため
 * に使う。pressure は world ビーコンの局所勾配 (reflex CONSERVE が +bias する)。 */
W    moe_expert_utility(UB accuracy, UW rtt_ms, INT pressure, int same_region);

/* 推論結果をフィードバック (正解ラベルを学習) */
void moe_feedback(UB pred_class, UB true_class);

/* ピアのスコアを更新 (K-DDS 受信コールバックから呼ぶ) */
void moe_update_peer(const MOE_SCORE *score);

/* 統計表示 */
void moe_stat(void);

/* §7/§8 性質テスト (philosophy-gap-audit G3 / I7 I8 D0 §5)。
 * shell `moe test` から呼ぶカーネル内 self-test。0=全 PASS。
 * 純ローカル計算なので net/kdds 不要・ベアメタルでも走る。 */
INT  moe_self_test(void);
