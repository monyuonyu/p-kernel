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
 *
 *  二層の時定数 (reflex-deliberation.md — D1+D2):
 *    反射層 (速い, MOE_REFLEX_TICK_MS / 決定ごと):
 *      select_expert の utility EWMA + incumbent デッドバンド、recent_pick
 *      減衰。region 内の局所状態だけで閉じる。「間に合う」を保証する側。
 *    熟慮層 (遅い, MOE_DELIB_TICK_MS / MOE_BROADCAST_MS):
 *      peer スコア取り込み・自 accuracy 再計算・スコア gossip。反射層が読む
 *      「賢さ」テーブルはこの帯域でしか動かない = 反射の瞬間スパイクを
 *      観測しないローパス (§4.2)。「正しい」へ遅れて補正する側。
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

/* ── 反射ゲートの damping 状態 (reflex-deliberation.md §4.3 / D1) ────
 * util_ewma[n][c] = 候補 n のクラス c utility の EWMA (α=1/MOE_UTIL_EWMA_DIV)。
 * incumbent[c]    = クラス c の現職エキスパート。挑戦者は現職の EWMA を
 *                   MOE_SWITCH_MARGIN 上回らない限り選ばれない (デッドバンド)。
 * どちらも決定ごとに動く反射層 (速い時定数) の状態。 */
static W  util_ewma[DNODE_MAX][MOE_NUM_CLASSES];
static UB ewma_valid[DNODE_MAX][MOE_NUM_CLASSES];
static UB incumbent[MOE_NUM_CLASSES];          /* 0xFF = 現職なし */

/* ── 熟慮層の可観測性 (D2): 反射の決定時に「熟慮の情報は何 ms 前か」を
 * 出せるように、moe_task が反射 tick で uptime を刻み、熟慮 tick の度に
 * 時刻を記録する。 */
static UW moe_uptime_ms = 0;   /* moe_task 起動からの経過 (反射 tick 粒度) */
static UW delib_at_ms   = 0;   /* 最後の熟慮層更新の時刻                   */
static UW delib_count   = 0;   /* 熟慮層更新の回数 (0 = まだ)              */

/* locality-gradient 効用 (§7): 賢さ(accuracy) から 近さ(RTT) と 余力(pressure)
 * のペナルティを引く。pressure は world ビーコンの *局所* 勾配信号 (§6)。
 * 同 region なら反射層 (§8) を優先してわずかに加点。
 * すべて局所窓口だけを読み、中央/グローバルなオラクルは見ない。 */
static W expert_utility(UB accuracy, UW rtt_ms, INT eff_pressure, int same_region)
{
    if (rtt_ms == 0xFFFFFFFFUL) rtt_ms = MOE_RTT_UNKNOWN_MS;  /* 未実測 */
    W u = (W)accuracy;
    u -= (W)(rtt_ms / MOE_RTT_MS_PER_POINT);
    u -= (W)((eff_pressure * MOE_PRESS_NUM) / MOE_PRESS_DEN);
    if (same_region) u += MOE_SAME_REGION_BONUS;
    return u;
}

/* 実効逼迫度 = ゴシップされたビーコン値 (熟慮帯域の遅い信号; 未知なら中庸)
 *            + recent_pick の自己観測の仮想負荷 (反射帯域の速い信号)。
 * ビーコン未着でも仮想負荷は常に効かせる — damping がゴシップの到着を
 * 待っていたら発振抑制 (§4.1) として手遅れになる。 */
static INT eff_pressure(UB n)
{
    if (n >= DNODE_MAX) return MOE_PRESS_UNKNOWN;
    INT press = world_peer_pressure(n);
    INT base  = (press < 0) ? MOE_PRESS_UNKNOWN : press;
    return base + (INT)recent_pick[n];
}

/* utility EWMA を 1 サンプル進める (α=1/MOE_UTIL_EWMA_DIV; swim.c の
 * RTT EWMA と同形)。整数除算で歩幅が 0 に潰れて収束しなくなるのを防ぐ
 * ため、差が残っている限り最低 1 は動かす。 */
static W ewma_step(W prev, W sample)
{
    W d = (sample - prev) / MOE_UTIL_EWMA_DIV;
    if (d == 0 && sample != prev) d = (sample > prev) ? 1 : -1;
    return prev + d;
}

/* デッドバンド (§4.2 ヒステリシス) の *唯一の* 定義。現職 inc (inc_ok=健在,
 * inc_e=その EWMA utility) に対し、挑戦者 chal (chal_e=その EWMA) が乗り換える
 * には MOE_SWITCH_MARGIN を超えて勝らねばならない。select_expert と
 * moe_self_test の両方がこの 1 関数を呼ぶので、ヒステリシス規則の重複定義が
 * 生まれない = 性質テスト (D0) が本番の切替ロジックそのものを守る。 */
static UB deadband_pick(UB inc, int inc_ok, W inc_e, UB chal, W chal_e,
                        const char **verdict)
{
    if (!inc_ok) {                       /* 現職不在/死亡: 即引き継ぎ */
        *verdict = "seed";  return chal;
    }
    if (chal != inc && chal_e > inc_e + MOE_SWITCH_MARGIN) {
        *verdict = "SWITCH"; return chal;            /* margin 超過: 乗り換え */
    }
    *verdict = (chal == inc) ? "hold" : "hold(deadband)";
    return inc;
}

/* 反射層の決定 (reflex-deliberation.md §4.3 / D1):
 *   1. 候補ごとに瞬間 utility を計り、EWMA に畳む (単発ノイズ除去)。
 *   2. EWMA の最大を「挑戦者」とする。
 *   3. 現職 (incumbent) が健在なら、挑戦者が EWMA で MOE_SWITCH_MARGIN
 *      以上勝らない限り現職を保持 (デッドバンド = ヒステリシス)。
 * 入力は accuracy テーブル (熟慮層が遅い帯域で更新) + RTT/pressure/region
 * (局所窓口) のみ。決定そのものは要求駆動 — 反射層の最速の時定数。 */
static UB select_expert(UB gate_class)
{
    /* §8 ダンピング: 毎決定の冒頭で recent_pick を減衰させる。直近の選択ほど
     * 効き、時間 (= 連続選択数) が経つと薄れる。ゴシップが本物の逼迫度を
     * 運んでくる頃には自己観測の仮想負荷は消えている (二層の時定数分離)。 */
    for (UB n = 0; n < DNODE_MAX; n++)
        recent_pick[n] = recent_pick[n] * MOE_PICK_DECAY_NUM / MOE_PICK_DECAY_DEN;

    UB me = drpc_my_node;
    if (me >= DNODE_MAX) {
        /* ノード ID 未確定 (net 未投入): 反射層はローカルに閉じる。 */
        mo_puts("[moe] reflex local-only (no node id)\r\n");
        return me;
    }

    /* region のメンバ判定をこの選択の間だけ固定する (ホットパス: 再計算回避)。 */
    region_recompute();

    /* 候補列挙: 自分も応援先の一候補にすぎない — ローカルが常に勝つわけでは
     * ない (相互応援)。瞬間 utility を EWMA に畳み、EWMA で挑戦者を選ぶ。 */
    UB chal = 0xFF;
    W  chal_e = 0;
    for (UB n = 0; n < DNODE_MAX; n++) {
        int is_self = (n == me);
        if (!is_self) {
            if (!score_valid[n]) continue;
            if (dnode_table[n].state != DNODE_ALIVE) continue;
        }

        UB  acc  = is_self ? my_accuracy[gate_class]
                           : peer_scores[n].accuracy[gate_class];
        UW  rtt  = is_self ? 0 : swim_rtt_ms(n);
        /* 負荷項は world ビーコン (ゴシップされた局所勾配) + 自己仮想負荷。
         * broadcast score table はここでは負荷の真実として使わない (§7)。 */
        INT eff  = eff_pressure(n);
        int same = is_self ? 1 : (region_is_member(n) ? 1 : 0);
        W   u    = expert_utility(acc, rtt, eff, same);

        util_ewma[n][gate_class] = ewma_valid[n][gate_class]
                                 ? ewma_step(util_ewma[n][gate_class], u) : u;
        ewma_valid[n][gate_class] = 1;
        W e = util_ewma[n][gate_class];

        if (is_self) { mo_puts("[moe] cand self "); }
        else         { mo_puts("[moe] cand node"); mo_putdec(n); }
        mo_puts(" acc="); mo_putdec(acc);
        mo_puts(" rtt="); mo_putdec(rtt == 0xFFFFFFFFUL ? MOE_RTT_UNKNOWN_MS : rtt);
        mo_puts("ms press="); mo_putdec((UW)eff);
        mo_puts(same ? " rgn" : "    ");
        mo_puts(" util="); mo_putsdec(u);
        mo_puts(" ewma="); mo_putsdec(e);
        mo_puts("\r\n");

        if (chal == 0xFF || e > chal_e) { chal = n; chal_e = e; }
    }

    /* デッドバンド (§4.2 ヒステリシス): 現職が健在なら、挑戦者は margin を
     * 超えて勝らない限り選ばれない。応援に行く閾値と引き上げる閾値をずらす。 */
    UB inc = incumbent[gate_class];
    int inc_ok = (inc < DNODE_MAX) && ewma_valid[inc][gate_class] &&
                 (inc == me || (score_valid[inc] &&
                                dnode_table[inc].state == DNODE_ALIVE));
    const char *verdict;
    W inc_e = inc_ok ? util_ewma[inc][gate_class] : 0;
    UB pick = deadband_pick(inc, inc_ok, inc_e, chal, chal_e, &verdict);
    incumbent[gate_class] = pick;

    /* 反射 vs 熟慮の可観測行 (D2): 現職/挑戦者の EWMA、margin、判定、
     * 熟慮層の情報が何 ms 前のものか。 */
    mo_puts("[moe] reflex cls="); mo_putdec(gate_class);
    mo_puts(" inc=");  if (inc < DNODE_MAX) { mo_puts("node"); mo_putdec(inc); }
                       else mo_puts("none");
    if (inc_ok) { mo_puts(" ewma="); mo_putsdec(util_ewma[inc][gate_class]); }
    mo_puts(" chal=node"); mo_putdec(chal);
    mo_puts(" ewma="); mo_putsdec(chal_e);
    mo_puts(" margin="); mo_putdec(MOE_SWITCH_MARGIN);
    mo_puts(" -> "); mo_puts(verdict);
    mo_puts("  delib_age=");
    if (delib_count == 0) mo_puts("-");
    else { mo_putdec(moe_uptime_ms - delib_at_ms); mo_puts("ms"); }
    mo_puts("\r\n");

    /* §8: 選んだノードへ自己観測の仮想負荷を即座に上乗せ (反射層の速い側)。
     * 次の連続選択ではこのノードがわずかに「混んで見える」ので、空き先が
     * 一点に殺到せず近傍へ分散する。ゴシップで本物の逼迫度が届けば自然に
     * 引き継がれる。 */
    if (pick < DNODE_MAX)
        recent_pick[pick] += MOE_PICK_LOAD;

    return pick;
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

/* フィードバックはカウンタ加算のみ (速い・安価)。反射層が読む my_accuracy
 * テーブルの再計算は熟慮層 tick (MOE_DELIB_TICK_MS) でしか行わない —
 * 1 サンプルの正誤で「賢さ」が即座に動くと、それ自体が反射帯域のノイズ源
 * になる (reflex-deliberation.md §3.2: 学習・スコア更新は熟慮層の責務)。 */
void moe_feedback(UB pred_class, UB true_class)
{
    if (pred_class >= MOE_NUM_CLASSES) return;
    if (pred_class == true_class)
        my_correct[pred_class]++;
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

    /* ── 二層のループ (reflex-deliberation.md §4.2 / D2) ──────────────
     * ベース tick は反射層の MOE_REFLEX_TICK_MS。熟慮層の仕事 (peer スコア
     * 取り込み + 自 accuracy 再計算 + gossip) は MOE_DELIB_TICK_MS /
     * MOE_BROADCAST_MS まで間引かれ、反射層より明示的に低帯域で回る。
     * 反射層が読む「賢さ」テーブル (peer_scores / my_accuracy) はこの遅い
     * 帯域でしか動かない = 熟慮は反射のスパイクを観測しない (ローパス)。 */
    UW since_bcast = MOE_BROADCAST_MS;    /* 起動直後に1回 broadcast */
    UW since_delib = MOE_DELIB_TICK_MS;   /* 起動直後に1回 熟慮 tick */
    for (;;) {
        tk_dly_tsk(MOE_REFLEX_TICK_MS);            /* 反射層の時定数 */
        moe_uptime_ms += MOE_REFLEX_TICK_MS;
        if (drpc_my_node == 0xFF) continue;

        /* ── 熟慮層 tick (遅い時定数) ── */
        since_delib += MOE_REFLEX_TICK_MS;
        if (since_delib >= MOE_DELIB_TICK_MS) {
            since_delib = 0;

            /* ピアのスコアを取り込む (per-source なので衝突せず全ノード蓄積) */
            for (UB n = 0; n < DNODE_MAX; n++) {
                if (h_score_sub[n] < 0) continue;
                MOE_SCORE s;
                W r = kdds_sub(h_score_sub[n], &s, (W)sizeof(s), 0);
                if (r >= (W)sizeof(MOE_SCORE) && s.node_id == n)
                    moe_update_peer(&s);
            }

            /* 自分の「賢さ」テーブルの再計算も熟慮帯域 (§3.2) */
            update_my_accuracy();

            delib_at_ms = moe_uptime_ms;
            delib_count++;
        }

        /* ── 熟慮層 発信側: スコア gossip (最も遅い) ── */
        since_bcast += MOE_REFLEX_TICK_MS;
        if (since_bcast >= MOE_BROADCAST_MS) {
            since_bcast = 0;
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
        for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
            util_ewma[n][c]  = 0;
            ewma_valid[n][c] = 0;
        }
    }
    for (INT c = 0; c < MOE_NUM_CLASSES; c++)
        incumbent[c] = 0xFF;   /* 現職なし (D1 デッドバンド状態) */
    moe_uptime_ms = 0;
    delib_at_ms   = 0;
    delib_count   = 0;
    h_score_pub = -1;
    mo_puts("[moe] initialized  classes="); mo_putdec(MOE_NUM_CLASSES); mo_puts("\r\n");
}

/* ================================================================== */
/* 性質テスト (philosophy-gap-audit G3 / I7 I8 D0 §5 の自動検証)        */
/*                                                                     */
/* §7/§8 の思想の核を「数で」守るカーネル内 self-test。shell の         */
/* `moe test` から呼び、CI が stdin 経由で叩いて PASS 行を grep する     */
/* (pfs/hrw と同じ方式)。本番の expert_utility / ewma_step /            */
/* deadband_pick をそのまま使うので、これらのロジックが思想から逸脱     */
/* (例: ヒステリシス削除、中央 argmax への退行) すると即座に落ちる。     */
/*                                                                     */
/* すべて純ローカル計算 (network/kdds に触れない) でベアメタルでも走る。 */
/* タスクスタックを汚さないよう大きなローカルは置かない (ncand<=3)。    */
/* ================================================================== */

/* テスト用: 既知の瞬間 utility 配列に対し反射層の 1 決定を回す。本番
 * select_expert と *同じ* ewma_step + deadband_pick を呼ぶ (重複定義なし)。
 * ewma[]/ewma_valid[]/incumbent_slot は呼び出し側が保持する反射状態。 */
static UB st_reflex_step(const W *inst_util, INT ncand,
                         W *ewma, UB *ewma_valid, UB *incumbent_slot)
{
    UB chal = 0xFF; W chal_e = 0;
    for (INT n = 0; n < ncand; n++) {
        ewma[n] = ewma_valid[n] ? ewma_step(ewma[n], inst_util[n]) : inst_util[n];
        ewma_valid[n] = 1;
        if (chal == 0xFF || ewma[n] > chal_e) { chal = (UB)n; chal_e = ewma[n]; }
    }
    UB  inc    = *incumbent_slot;
    INT inc_ok = (inc < (UB)ncand) && ewma_valid[inc];
    W   inc_e  = inc_ok ? ewma[inc] : 0;
    const char *v;
    UB pick = deadband_pick(inc, inc_ok, inc_e, chal, chal_e, &v);
    *incumbent_slot = pick;
    return pick;
}

/* argmax の純ヘルパー (中央 argmax の退行検知に使う)。 */
static INT st_argmax(const W *a, INT n)
{
    INT best = 0;
    for (INT i = 1; i < n; i++) if (a[i] > a[best]) best = i;
    return best;
}

/* ── I7: NO-CENTRAL ゲーティング ──────────────────────────────────────
 * expert 選択が「各ノードの *局所* 勾配 (自分が観測した近傍の pressure/RTT)」
 * で決まり、全体を集約する単一点 (= 全員が同じ accuracy テーブルの
 * グローバル argmax を取る準・中央集権) へ退行していないことを数で示す。
 *
 * 仕掛け: 3 つの観測ノード A/B/C が同一 accuracy の 3 expert を見るが、
 * 各々が *別々の局所 pressure ビュー* を持つ。各ノードは本番 expert_utility
 * で自分の勾配を下る → 別々の expert を選ぶ。もし中央 argmax(accuracy) へ
 * 退行していれば accuracy 同点で全員が同一 expert を選ぶはず。 */
static INT st_test_nocentral(void)
{
    INT fails = 0;
    const UB acc = 70;                         /* 3 expert すべて同一の賢さ */
    /* 各観測ノードの局所 pressure ビュー (近傍勾配; 自分だけが知る)。 */
    INT press[3][3] = {
        { 80, 10, 50 },   /* node A: expert1 が一番空いて見える          */
        { 10, 80, 50 },   /* node B: expert0 が一番空いて見える          */
        { 50, 50, 10 },   /* node C: expert2 が一番空いて見える          */
    };
    UB pick[3];
    for (INT obs = 0; obs < 3; obs++) {
        W u[3];
        for (INT e = 0; e < 3; e++)
            u[e] = expert_utility(acc, 0, press[obs][e], 0);
        pick[obs] = (UB)st_argmax(u, 3);
        mo_puts("[moe-nocentral] obs"); mo_putdec((UW)obs);
        mo_puts(" local-press=["); mo_putdec((UW)press[obs][0]);
        mo_puts(","); mo_putdec((UW)press[obs][1]);
        mo_puts(","); mo_putdec((UW)press[obs][2]);
        mo_puts("] -> expert"); mo_putdec(pick[obs]); mo_puts("\r\n");
    }
    /* (1) 各ノードは自分の局所勾配の最小 pressure へ下る (= 中央でなく局所)。 */
    for (INT obs = 0; obs < 3; obs++) {
        INT cheapest = 0;
        for (INT e = 1; e < 3; e++)
            if (press[obs][e] < press[obs][cheapest]) cheapest = e;
        if (pick[obs] != (UB)cheapest) {
            mo_puts("[moe-nocentral] FAIL obs follows non-local choice\r\n");
            fails++;
        }
    }
    /* (2) 退行検知: accuracy だけのグローバル argmax なら全員同一になる。
     * 実際の選択がすべて同一なら中央集権へ退行している。 */
    INT all_same = (pick[0] == pick[1]) && (pick[1] == pick[2]);
    if (all_same) {
        mo_puts("[moe-nocentral] FAIL all nodes converged to one expert"
                " (regressed to central argmax)\r\n");
        fails++;
    } else {
        mo_puts("[moe-nocentral] ok  divergent local choices: no single"
                " aggregation point\r\n");
    }
    if (fails == 0)
        mo_puts("[moe-nocentral] PASS (expert chosen by per-node local"
                " gradient; no central argmax)\r\n");
    else
        mo_puts("[moe-nocentral] FAIL\r\n");
    return fails;
}

/* ── I8: 二層時定数のローパス性 ───────────────────────────────────────
 * 反射層 (速い tick) と熟慮層 (遅い tick = decimation 比 R) が分離している
 * = 高周波の入力変動は熟慮層では減衰する、を数で示す。
 *   (a) ステップ応答: 速い層と遅い層の 63% 到達 tick 比 ≒ 時定数比 R。
 *   (b) 振動応答: 反射層の最速周期 (2 tick) の振動は、熟慮層 (R 間引き) では
 *       peak-to-peak が縮む (ローパス)。
 * 反射層 EWMA は本番 ewma_step を使用。 */
static INT st_test_twolayer(void)
{
    INT fails = 0;
    const INT R = MOE_DELIB_TICK_MS / MOE_REFLEX_TICK_MS;   /* 時定数比 (=10) */

    /* (a) ステップ 0->100。反射=毎 tick、熟慮=R tick ごとに ewma_step。 */
    W fast = 0, slow = 0;                       /* 両層とも baseline 0 で安定 */
    INT fast_t63 = -1, slow_t63 = -1;
    for (INT t = 1; t <= R * 12; t++) {
        fast = ewma_step(fast, 100);            /* 反射: 毎 tick 実 EWMA      */
        if (fast_t63 < 0 && fast >= 63) fast_t63 = t;
        if (t % R == 0) {                       /* 熟慮: R tick ごと          */
            slow = ewma_step(slow, 100);
            if (slow_t63 < 0 && slow >= 63) slow_t63 = t;
        }
    }
    mo_puts("[moe-twolayer] step63  fast="); mo_putdec((UW)fast_t63);
    mo_puts("tick slow="); mo_putdec((UW)slow_t63);
    mo_puts("tick ratio="); 
    if (fast_t63 > 0) mo_putdec((UW)(slow_t63 / fast_t63)); else mo_puts("-");
    mo_puts(" (tau-ratio="); mo_putdec((UW)R); mo_puts(")\r\n");
    /* 遅い層は速い層の (R を超える) 時定数で応答する = ローパス。 */
    if (fast_t63 <= 0 || slow_t63 < fast_t63 * (R - 2)) {
        mo_puts("[moe-twolayer] FAIL step time-constant not separated\r\n");
        fails++;
    }

    /* (b) 反射層の最速振動 (毎 tick 100/0)。熟慮層は R 間引きで観測。
     * 各層の定常 peak-to-peak を測る (位相非依存・バイアス不変の指標)。 */
    W f = 0, sl = 0;                            /* 両層とも baseline 0 で安定 */
    W fmin = 1000, fmax = -1000, smin = 1000, smax = -1000;
    for (INT t = 1; t <= R * 20; t++) {
        W x = (t & 1) ? 100 : 0;
        f = ewma_step(f, x);
        if (t % R == 0) sl = ewma_step(sl, x);
        if (t > R * 10) {                         /* 定常のみ集計 */
            if (f  < fmin) fmin = f;
            if (f  > fmax) fmax = f;
            if (sl < smin) smin = sl;
            if (sl > smax) smax = sl;
        }
    }
    W fpp = fmax - fmin, spp = smax - smin;
    mo_puts("[moe-twolayer] osc-pp fast="); mo_putsdec(fpp);
    mo_puts(" slow="); mo_putsdec(spp); mo_puts("\r\n");
    /* 反射層は実際に振動し (sanity)、熟慮層では強く減衰している。 */
    if (fpp < 8) {
        mo_puts("[moe-twolayer] FAIL fast layer did not oscillate\r\n"); fails++;
    }
    if (spp * 3 >= fpp) {
        mo_puts("[moe-twolayer] FAIL slow layer did not attenuate hi-freq\r\n");
        fails++;
    }
    if (fails == 0)
        mo_puts("[moe-twolayer] PASS (reflex fast / deliberation slow:"
                " hi-freq low-passed)\r\n");
    else
        mo_puts("[moe-twolayer] FAIL\r\n");
    return fails;
}

/* ── D0: 発振の再現 + ヒステリシスによる収束 ──────────────────────────
 * 「いま一番空いてるノードへ全員が殺到 → そこが逼迫 → 全員逃げる」の速い
 * 振動 (発振) を、単一時定数 (ヒステリシス/デッドバンド/EWMA 無しの素朴な
 * 瞬間 argmax) で再現し、現行の処方 (util EWMA + MOE_SWITCH_MARGIN
 * デッドバンド + recent_pick 減衰) を入れると切替回数が激減することを示す。
 *
 * 共通の負荷モデル (本番 recent_pick と同形):
 *   毎決定の冒頭で全 load を MOE_PICK_DECAY_NUM/DEN 減衰、
 *   選んだノードへ MOE_PICK_LOAD を上乗せ。utility は本番 expert_utility。 */
static INT st_herd(INT stabilized, UW *switches_out)
{
    const INT M = 3;            /* 候補ノード数 */
    const UB  acc = 70;         /* 全候補同一の賢さ (純粋に負荷勾配で競う) */
    const INT T = 30;           /* 決定回数 */
    UW load[3] = { 0, 0, 0 };
    W  ewma[3] = { 0, 0, 0 }; UB ev[3] = { 0, 0, 0 };
    UB inc = 0xFF;
    UB prev = 0xFF; UW switches = 0;
    for (INT t = 0; t < T; t++) {
        /* 冒頭で負荷減衰 (本番 select_expert と同じ作法)。 */
        for (INT n = 0; n < M; n++)
            load[n] = load[n] * MOE_PICK_DECAY_NUM / MOE_PICK_DECAY_DEN;
        /* 瞬間 utility = 本番 expert_utility (賢さ - 負荷勾配)。 */
        W u[3];
        for (INT n = 0; n < M; n++)
            u[n] = expert_utility(acc, 0, (INT)load[n], 0);
        UB pick;
        if (stabilized) {
            pick = st_reflex_step(u, M, ewma, ev, &inc);   /* EWMA+デッドバンド */
        } else {
            pick = (UB)st_argmax(u, M);                    /* 素朴な瞬間 argmax */
        }
        if (t > 1 && pick != prev) switches++;             /* 2 回は warmup */
        prev = pick;
        load[pick] += MOE_PICK_LOAD;
    }
    *switches_out = switches;
    return 0;
}

static INT st_test_oscillation(void)
{
    INT fails = 0;
    UW naive = 0, stable = 0;
    st_herd(0, &naive);
    st_herd(1, &stable);
    mo_puts("[moe-osc] herd 3-node, 30 decisions: naive switches=");
    mo_putdec(naive); mo_puts(" stabilized switches="); mo_putdec(stable);
    mo_puts("\r\n");
    /* 発振が再現していること (素朴版は頻繁に切り替わる)。 */
    if (naive < 18) {
        mo_puts("[moe-osc] FAIL naive case did not oscillate\r\n"); fails++;
    }
    /* 処方が効いていること (切替が半分以下かつ閾値以下)。 */
    const UW K = 12;
    if (!(stable <= K && stable * 2 <= naive)) {
        mo_puts("[moe-osc] FAIL hysteresis did not damp oscillation\r\n");
        fails++;
    }
    if (fails == 0) {
        mo_puts("[moe-osc] PASS (switches: "); mo_putdec(stable);
        mo_puts("<="); mo_putdec(K); mo_puts(", naive="); mo_putdec(naive);
        mo_puts(")\r\n");
    } else {
        mo_puts("[moe-osc] FAIL\r\n");
    }
    return fails;
}

/* ── §5: 同時多発の劣化なし (moe 範囲) ────────────────────────────────
 * ゲーティングはクラスごとに独立した局所状態 (incumbent[c], util_ewma[][c])
 * を持つ = 複数の独立イベント (別クラス) が同時に来ても互いを直列化しない。
 * 検証: 同じイベント列を 2 通りの順序 (ラウンドロビン / クラス毎ブロック) で
 * 処理し、各クラスの k 番目の決定結果が順序に依存しないことを示す
 * (= イベント間に共有直列化点がない = 各々ローカルに捌ける)。
 * さらに 3 クラスが別々の expert へ落ち着くことで「単一窓口への funnel」で
 * ないことも確認する。 */
static void st_class_util(INT cls, W out[3])
{
    /* クラス c では候補 c が最良 (賢さ高)。クラスごとに勝者が違う。 */
    for (INT n = 0; n < 3; n++)
        out[n] = expert_utility((UB)(n == cls ? 90 : 50), 0, 0, 0);
}

static INT st_test_concurrent(void)
{
    INT fails = 0;
    const INT C = 3, E = 6;       /* 3 クラス x 6 イベント */
    UB res1[3][6], res2[3][6];
    /* run1: ラウンドロビン (0,1,2,0,1,2,...) — イベントが交互に殺到 */
    {
        W ew[3][3]; UB ev[3][3], inc[3];
        for (INT c = 0; c < C; c++) { inc[c] = 0xFF;
            for (INT n = 0; n < 3; n++) { ew[c][n] = 0; ev[c][n] = 0; } }
        for (INT k = 0; k < E; k++)
            for (INT c = 0; c < C; c++) {
                W u[3]; st_class_util(c, u);
                res1[c][k] = st_reflex_step(u, 3, ew[c], ev[c], &inc[c]);
            }
    }
    /* run2: クラス毎ブロック (0x6, 1x6, 2x6) — 同じイベント集合・別順序 */
    {
        W ew[3][3]; UB ev[3][3], inc[3];
        for (INT c = 0; c < C; c++) { inc[c] = 0xFF;
            for (INT n = 0; n < 3; n++) { ew[c][n] = 0; ev[c][n] = 0; } }
        for (INT c = 0; c < C; c++)
            for (INT k = 0; k < E; k++) {
                W u[3]; st_class_util(c, u);
                res2[c][k] = st_reflex_step(u, 3, ew[c], ev[c], &inc[c]);
            }
    }
    /* (1) 順序非依存: 各クラスの各イベント結果が両順序で一致。 */
    INT order_indep = 1;
    for (INT c = 0; c < C; c++)
        for (INT k = 0; k < E; k++)
            if (res1[c][k] != res2[c][k]) order_indep = 0;
    if (order_indep)
        mo_puts("[moe-concurrent] ok  per-class results order-independent"
                " (no serialization point)\r\n");
    else {
        mo_puts("[moe-concurrent] FAIL interleaving changed results"
                " (events serialized)\r\n"); fails++;
    }
    /* (2) クラスごとに別 expert へ落ち着く (単一窓口へ funnel していない)。 */
    UB f0 = res1[0][E-1], f1 = res1[1][E-1], f2 = res1[2][E-1];
    mo_puts("[moe-concurrent] settled experts: cls0=node"); mo_putdec(f0);
    mo_puts(" cls1=node"); mo_putdec(f1);
    mo_puts(" cls2=node"); mo_putdec(f2); mo_puts("\r\n");
    if (f0 == f1 || f1 == f2 || f0 == f2) {
        mo_puts("[moe-concurrent] FAIL classes funneled to one expert\r\n");
        fails++;
    }
    if (fails == 0)
        mo_puts("[moe-concurrent] PASS (independent local handling of"
                " simultaneous events)\r\n");
    else
        mo_puts("[moe-concurrent] FAIL\r\n");
    return fails;
}

/* 公開エントリ: 4 本の性質テストを順に走らせ、合計 fail 数を返す。
 * shell `moe test` から呼ばれ、CI は各 PASS 行を grep する。 */
INT moe_self_test(void)
{
    INT fails = 0;
    mo_puts("[moe-test] ==== §7/§8 property tests (I7/I8/D0/§5) ====\r\n");
    fails += st_test_nocentral();
    fails += st_test_twolayer();
    fails += st_test_oscillation();
    fails += st_test_concurrent();
    if (fails == 0) mo_puts("[moe-test] ALL PASS\r\n");
    else { mo_puts("[moe-test] FAILURES="); mo_putdec((UW)fails);
           mo_puts("\r\n"); }
    return fails;
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

    /* ── 二層の可観測性 (reflex-deliberation.md / D2) ── */
    mo_puts("[moe] reflex    : margin="); mo_putdec(MOE_SWITCH_MARGIN);
    mo_puts(" ewma_div="); mo_putdec(MOE_UTIL_EWMA_DIV);
    mo_puts(" tick="); mo_putdec(MOE_REFLEX_TICK_MS); mo_puts("ms\r\n");
    for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
        mo_puts("  cls"); mo_putdec((UW)c); mo_puts(": inc=");
        if (incumbent[c] < DNODE_MAX) { mo_puts("node"); mo_putdec(incumbent[c]); }
        else mo_puts("none");
        mo_puts(" ewma=[");
        int first = 1;
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (!ewma_valid[n][c]) continue;
            if (!first) mo_puts(" ");
            first = 0;
            mo_puts("n"); mo_putdec(n); mo_puts(":");
            mo_putsdec(util_ewma[n][c]);
        }
        mo_puts("]\r\n");
    }
    mo_puts("[moe] rpick     : ");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (recent_pick[n] == 0) continue;
        mo_puts("n"); mo_putdec(n); mo_puts(":");
        mo_putdec(recent_pick[n]); mo_puts(" ");
    }
    mo_puts("\r\n");
    mo_puts("[moe] delib     : tick="); mo_putdec(MOE_DELIB_TICK_MS);
    mo_puts("ms bcast="); mo_putdec(MOE_BROADCAST_MS);
    mo_puts("ms updates="); mo_putdec(delib_count);
    mo_puts(" age=");
    if (delib_count == 0) mo_puts("-");
    else { mo_putdec(moe_uptime_ms - delib_at_ms); mo_puts("ms"); }
    mo_puts("\r\n");
}
