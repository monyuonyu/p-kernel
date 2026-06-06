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
 *  ノード選択:
 *    gate_class に対して accuracy[gate_class] が最高のノードを選択。
 *    タイ: node_id の小さい方を優先 (安定した選択)。
 *    スコア未取得: ローカル推論にフォールバック。
 */

#include "moe.h"
#include "drpc.h"
#include "kdds.h"
#include "swim.h"
#include "world.h"      /* world_note_firing — situational-awareness map */
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

/* locality-aware 効用: 賢さ(accuracy) から近さ(RTT) のペナルティを引く。
 * (regions R1 — docs/architecture/regions.md の gating cost を簡略化:
 *  帯域項 γ·(1/bw) はまだ測れないので省略。) */
static W expert_utility(UB accuracy, UW rtt_ms)
{
    if (rtt_ms == 0xFFFFFFFFUL) rtt_ms = MOE_RTT_UNKNOWN_MS;  /* 未実測 */
    return (W)accuracy - (W)(rtt_ms / MOE_RTT_MS_PER_POINT);
}

static UB select_expert(UB gate_class)
{
    /* 自分: RTT=0 なのでペナルティ無し */
    UB best_node = drpc_my_node;
    W  best_util = (W)my_accuracy[gate_class];

    mo_puts("[moe] cand self  acc="); mo_putdec(my_accuracy[gate_class]);
    mo_puts(" rtt=0ms util="); mo_putdec((UW)best_util); mo_puts("\r\n");

    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (!score_valid[n]) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;

        UB acc = peer_scores[n].accuracy[gate_class];
        UW rtt = swim_rtt_ms(n);
        W  u   = expert_utility(acc, rtt);

        mo_puts("[moe] cand node"); mo_putdec(n);
        mo_puts(" acc="); mo_putdec(acc);
        mo_puts(" rtt="); mo_putdec(rtt == 0xFFFFFFFFUL ? MOE_RTT_UNKNOWN_MS : rtt);
        mo_puts("ms util="); mo_putdec((UW)u); mo_puts("\r\n");

        if (u > best_util) {
            best_util = u;
            best_node = n;
        }
    }
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
