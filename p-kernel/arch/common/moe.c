/*
 *  moe.c (x86)
 *  Phase 10 — Mixture of Experts (MoE) 推論ルーティング
 *
 *  Gate ネットワーク (軽量線形分類):
 *    入力: センサー平均値 (1次元)
 *    出力: クラス確率 (softmax風)
 *    実装: 固定しきい値による区間分類
 *
 *    temp < 20  → class 0 (normal/cold)
 *    temp < 35  → class 1 (alert/warm)
 *    temp >= 35 → class 2 (critical/hot)
 *
 *  ノード選択 (regions R3 — §7 分散ゲーティング):
 *    かつては accuracy[gate_class] のグローバル最大を取っていた (= 全員が
 *    点数を叫び、暗黙のグローバルビューで最大値を拾う = 準・中央集権)。
 *    いまは「局所勾配を下る相互応援」へ置換した:
 *      utility = capability(accuracy) - a·RTT - b·pressure (+ 同 region 加点)
 *    各候補の負荷は world ビーコンでゴシップされた *局所* 逼迫度
 *    (world_peer_pressure) を、距離は swim_rtt_ms を読むだけ。中央/グローバル
 *    なオラクル (broadcast score table のグローバル最大) は決して真実として
 *    扱わない (NO-CENTRAL 不変条件 — world.h と同じ思想)。
 *    §8 ダンピング: 直近に選んだノードへ自己観測の仮想負荷を上乗せして
 *    連続殺到による発振を抑える (recent_pick[]; 詳細は select_expert)。
 *    スコア/ビーコン未取得: ローカル推論にフォールバック (安全側)。
 */

#include "moe.h"
#include "drpc.h"
#include "kdds.h"
#include "swim.h"
#include "world.h"      /* world_note_firing / world_peer_pressure — 局所勾配 */
#include "region.h"     /* region_contains — 同 region 近傍 (反射層 §8)        */
#include "ai_kernel.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void mo_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void mo_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { mo_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    mo_puts(&buf[i]);
}

/* 符号付き10進。utility は負になり得る (acc=0 + pressure/rtt ペナルティ) ので
 * (UW) キャストで 4294967286 のような化け方をしないように。 */
static void mo_putsdec(W v)
{
    if (v < 0) { mo_puts("-"); mo_putdec((UW)(-v)); return; }
    mo_putdec((UW)v);
}

/* ------------------------------------------------------------------ */
/* スコアテーブル                                                      */
/* ------------------------------------------------------------------ */

static MOE_SCORE peer_scores[DNODE_MAX];
static INT       score_valid[DNODE_MAX];

/* 自分のスコア */
static UW  my_total       = 0;
static UW  my_correct[MOE_NUM_CLASSES];
static UB  my_accuracy[MOE_NUM_CLASSES];

/* K-DDS ハンドル (per-source score topics)
 *   h_score_pub     : 自ノード "moe/score/<my>" へ発行
 *   h_score_sub[n]  : ピア "moe/score/<n>" を購読 (n != my) */
static W   h_score_pub = -1;
static W   h_score_sub[DNODE_MAX];

/* "moe/score/<node>" を out に組み立てる (node は 0..DNODE_MAX-1, 1 桁) */
static void score_topic_name(char *out, UB node)
{
    const char *p = MOE_SCORE_TOPIC_PFX;
    INT i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + node);
    out[i]   = '\0';
}

/* ------------------------------------------------------------------ */
/* Gate: 入力からクラス予測                                           */
/* ------------------------------------------------------------------ */

static UB gate_predict(B temp, B hum, B press, B light)
{
    (void)hum; (void)press; (void)light;
    /* 温度を主要特徴として使用 (将来: 多次元Gate) */
    if (temp < 20) return 0;   /* normal */
    if (temp < 35) return 1;   /* alert  */
    return 2;                  /* critical */
}

/* ------------------------------------------------------------------ */
/* 最適ノード選択                                                     */
/* ------------------------------------------------------------------ */

/* ── §8 ダンピング状態: 直近に選んだノードへの自己観測の仮想負荷 ──────
 * recent_pick[n] は「ゴシップがまだ追いつく前に自分がノード n へ投げた
 * 仕事量」の局所推定。select_expert が pressure に加算して見ることで、
 * 同じ空きノードへの連続殺到 (発振) を反射層の速い時定数で抑える。
 * gossip の遅い時定数 (WORLD_BEACON_MS) と分離した、速い局所閉ループ。 */
static UW recent_pick[DNODE_MAX];

/* locality-gradient 効用 (§7): 賢さ(accuracy) から 近さ(RTT) と 余力(pressure)
 * のペナルティを引く。pressure は world ビーコンの *局所* 勾配信号 (§6)。
 * 同 region なら反射層 (§8) を優先してわずかに加点。
 *   eff_pressure = beacon_pressure + recent_pick (§8 ダンピング込み)
 * すべて局所窓口だけを読み、中央/グローバルなオラクルは見ない。 */
static W expert_utility(UB accuracy, UW rtt_ms, INT eff_pressure, int same_region)
{
    if (rtt_ms == 0xFFFFFFFFUL) rtt_ms = MOE_RTT_UNKNOWN_MS;  /* 未実測 */
    if (eff_pressure < 0) eff_pressure = MOE_PRESS_UNKNOWN;   /* 逼迫度未知 */
    W u = (W)accuracy;
    u -= (W)(rtt_ms / MOE_RTT_MS_PER_POINT);
    u -= (W)((eff_pressure * MOE_PRESS_NUM) / MOE_PRESS_DEN);
    if (same_region) u += MOE_SAME_REGION_BONUS;
    return u;
}

static UB select_expert(UB gate_class)
{
    /* §8 ダンピング: 毎決定の冒頭で recent_pick を減衰させる。直近の選択ほど
     * 効き、時間 (= 連続選択数) が経つと薄れる。ゴシップが本物の逼迫度を
     * 運んでくる頃には自己観測の仮想負荷は消えている (二層の時定数分離)。 */
    for (UB n = 0; n < DNODE_MAX; n++)
        recent_pick[n] = recent_pick[n] * MOE_PICK_DECAY_NUM / MOE_PICK_DECAY_DEN;

    /* region のメンバ判定をこの選択の間だけ固定する (ホットパス: 再計算回避)。 */
    region_recompute();

    /* 自分: RTT=0、逼迫度は world の自己観測 (無ければ中庸)。自分も応援先の
     * 一候補にすぎない — ローカルが常に勝つわけではない (相互応援)。 */
    INT self_press = world_peer_pressure(drpc_my_node);
    self_press = (self_press < 0) ? -1
               : self_press + (INT)recent_pick[drpc_my_node];
    UB best_node = drpc_my_node;
    W  best_util = expert_utility(my_accuracy[gate_class], 0, self_press, 1);

    mo_puts("[moe] cand self  acc="); mo_putdec(my_accuracy[gate_class]);
    mo_puts(" rtt=0ms press=");
    mo_putdec(self_press < 0 ? MOE_PRESS_UNKNOWN : (UW)self_press);
    mo_puts(" util="); mo_putsdec(best_util); mo_puts(" [self]\r\n");

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (!score_valid[n]) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;

        UB  acc = peer_scores[n].accuracy[gate_class];
        UW  rtt = swim_rtt_ms(n);
        /* 負荷項は world ビーコン (ゴシップされた局所勾配) だけから読む。
         * broadcast score table はここでは負荷の真実として使わない (§7)。 */
        INT press = world_peer_pressure(n);
        INT eff   = (press < 0) ? -1 : press + (INT)recent_pick[n];
        int same  = region_is_member(n) ? 1 : 0;
        W   u     = expert_utility(acc, rtt, eff, same);

        mo_puts("[moe] cand node"); mo_putdec(n);
        mo_puts(" acc="); mo_putdec(acc);
        mo_puts(" rtt="); mo_putdec(rtt == 0xFFFFFFFFUL ? MOE_RTT_UNKNOWN_MS : rtt);
        mo_puts("ms press=");
        mo_putdec(eff < 0 ? MOE_PRESS_UNKNOWN : (UW)eff);
        mo_puts(same ? " rgn" : "    ");
        mo_puts(" util="); mo_putsdec(u); mo_puts("\r\n");

        if (u > best_util) {
            best_util = u;
            best_node = n;
        }
    }

    /* §8: 選んだノードへ自己観測の仮想負荷を即座に上乗せ (反射層の速い側)。
     * 次の連続選択ではこのノードがわずかに「混んで見える」ので、空き先が
     * 一点に殺到せず近傍へ分散する。ゴシップで本物の逼迫度が届けば自然に
     * 引き継がれる。 */
    if (best_node < DNODE_MAX)
        recent_pick[best_node] += MOE_PICK_LOAD;

    return best_node;
}

/* ------------------------------------------------------------------ */
/* スコア更新                                                         */
/* ------------------------------------------------------------------ */

static void update_my_accuracy(void)
{
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        if (my_total == 0) {
            my_accuracy[c] = 50;   /* デフォルト 50% */
        } else {
            my_accuracy[c] = (UB)((my_correct[c] * 100) / (my_total + 1));
        }
    }
}

/* ------------------------------------------------------------------ */
/* K-DDS でスコアをブロードキャスト                                  */
/* ------------------------------------------------------------------ */

static void broadcast_score(void)
{
    if (h_score_pub < 0) return;

    MOE_SCORE s;
    s.node_id     = drpc_my_node;
    s.total_infer = my_total;
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        s.accuracy[c] = my_accuracy[c];
        s.correct[c]  = my_correct[c];
    }
    kdds_pub(h_score_pub, &s, sizeof(s));
}

/* ------------------------------------------------------------------ */
/* 推論実行 (MoE ルーティング)                                        */
/* ------------------------------------------------------------------ */

UB moe_infer(B temp, B hum, B press, B light)
{
    UB gate_class = gate_predict(temp, hum, press, light);
    UB expert     = select_expert(gate_class);

    /* このノードがこの推論で発火したことを world-table へ通知する。
     * 全網マップ (world.c) の firing インジケータが点灯する。 */
    world_note_firing(gate_class);

    mo_puts("[moe] gate="); mo_putdec(gate_class);
    mo_puts("  expert=node"); mo_putdec(expert);

    UB result_class;

    if (expert == drpc_my_node || drpc_my_node == 0xFF) {
        /* ローカル推論 */
        B input[MLP_IN] = { (B)temp, (B)hum, (B)press, (B)light };
        result_class = mlp_forward(input);
        mo_puts("  [local]\r\n");
    } else {
        /* リモート推論 (DRPC) */
        W packed = SENSOR_PACK(temp, hum, press, light);
        UB cls = 0;
        ER er = dtk_infer(expert, packed, &cls, 800);
        if (er == E_OK) {
            result_class = cls;
            mo_puts("  [remote] class="); mo_putdec(cls); mo_puts("\r\n");
        } else {
            /* フォールバック: ローカル推論 */
            B input[MLP_IN] = { (B)temp, (B)hum, (B)press, (B)light };
            result_class = mlp_forward(input);
            mo_puts("  [fallback] class="); mo_putdec(result_class); mo_puts("\r\n");
        }
    }

    my_total++;
    return result_class;
}

/* ------------------------------------------------------------------ */
/* フィードバック: 正解ラベルで精度更新                               */
/* ------------------------------------------------------------------ */

void moe_feedback(UB pred_class, UB true_class)
{
    if (pred_class >= MOE_NUM_CLASSES) return;
    if (pred_class == true_class)
        my_correct[pred_class]++;
    update_my_accuracy();
}

/* ------------------------------------------------------------------ */
/* ピアスコア更新 (K-DDS サブスクライブ受信時)                       */
/* ------------------------------------------------------------------ */

void moe_update_peer(const MOE_SCORE *score)
{
    if (!score) return;
    UB n = score->node_id;
    if (n >= DNODE_MAX) return;
    peer_scores[n] = *score;
    score_valid[n] = 1;

    mo_puts("[moe] peer score  node="); mo_putdec(n);
    mo_puts("  acc=[");
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        mo_putdec(score->accuracy[c]); mo_puts("%");
        if (c < MOE_NUM_CLASSES - 1) mo_puts(",");
    }
    mo_puts("]\r\n");
}

/* ------------------------------------------------------------------ */
/* MoE タスク: 定期スコアブロードキャスト                            */
/* ------------------------------------------------------------------ */

void moe_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* moe_task は drpc_init の後 (cmd_net) に起動されるので drpc_my_node は確定。
     * 自ノードの per-source スコアトピックへ pub、ピアのトピックを sub する。 */
    if (drpc_my_node != 0xFF) {
        char tn[KDDS_NAME_MAX];
        score_topic_name(tn, drpc_my_node);
        h_score_pub = kdds_open(tn, KDDS_QOS_LATEST_ONLY);
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) { h_score_sub[n] = -1; continue; }
            score_topic_name(tn, n);
            h_score_sub[n] = kdds_open(tn, KDDS_QOS_LATEST_ONLY);
        }
    }

    UW since_bcast = MOE_BROADCAST_MS;   /* 起動直後に1回 broadcast */
    for (;;) {
        tk_dly_tsk(MOE_POLL_MS);
        if (drpc_my_node == 0xFF) continue;

        /* ピアのスコアを取り込む (per-source なので衝突せず全ノード蓄積) */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (h_score_sub[n] < 0) continue;
            MOE_SCORE s;
            W r = kdds_sub(h_score_sub[n], &s, (W)sizeof(s), 0);
            if (r >= (W)sizeof(MOE_SCORE) && s.node_id == n)
                moe_update_peer(&s);
        }

        /* 自分のスコアを定期 broadcast */
        since_bcast += MOE_POLL_MS;
        if (since_bcast >= MOE_BROADCAST_MS) {
            since_bcast = 0;
            update_my_accuracy();
            broadcast_score();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void moe_init(void)
{
    my_total = 0;
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        my_correct[c]  = 0;
        my_accuracy[c] = 50;   /* 初期値 50% */
    }
    for (INT n = 0; n < DNODE_MAX; n++) {
        score_valid[n]  = 0;
        h_score_sub[n]  = -1;
        recent_pick[n]  = 0;   /* §8 ダンピング状態 */
    }
    h_score_pub = -1;
    mo_puts("[moe] initialized  classes="); mo_putdec(MOE_NUM_CLASSES); mo_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void moe_stat(void)
{
    mo_puts("[moe] my node="); mo_putdec(drpc_my_node); mo_puts("\r\n");
    mo_puts("[moe] total_infer="); mo_putdec(my_total); mo_puts("\r\n");
    mo_puts("[moe] accuracy  : ");
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        mo_puts("cls"); mo_putdec((UW)c); mo_puts("=");
        mo_putdec(my_accuracy[c]); mo_puts("% ");
    }
    mo_puts("\r\n");
    mo_puts("[moe] peers     :\r\n");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (!score_valid[n]) continue;
        mo_puts("  node"); mo_putdec(n); mo_puts(": ");
        for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
            mo_putdec(peer_scores[n].accuracy[c]); mo_puts("% ");
        }
        mo_puts("\r\n");
    }
}
