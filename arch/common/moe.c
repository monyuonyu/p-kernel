/*
 *  moe.c (x86)
 *  Phase 10 — Mixture of Experts (MoE) 推論ルーティング
 *
 *  Gate ネットワーク (ONE BRAIN — wave 本丸):
 *    入力: 4 センサーチャネル (temp, hum, press, light)
 *    出力: 学習された dtr Transformer の argmax クラス + 実 max-softmax 確信度
 *    実装: dtr_decide() = dtr_forward_probs の argmax。4 トークンすべてを使う。
 *
 *    かつてゲートは温度1チャネルの固定しきい値 if 梯子
 *      (void)hum;(void)press;(void)light; if(temp<20)/<35
 *    で、moe_infer が *返す* 脳 (ai_job.c の手書き MLP 定数) とも *守る* 脳
 *    (dtr) とも別の三本目だった。いまは三つとも *同じ 1 回の dtr forward* から
 *    取る = 一脳。docs/review-2026-06-three-brains.md。
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
#include "reflex.h"     /* G17: 推論完了点 → §8 反射層 (思考→行動の片肺解消)   */
#include "dtr.h"        /* G38: 学習モデルの実 softmax 確信度で反射をゲート     */
#include "gossip_learn.h" /* 本丸: gl_merge — no-central 集合学習で返答精度検証   */
#include "galaxy.h"     /* galaxy v1: S6 moe firing emit hook */
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

/* self-test の精度ループ用に moe_infer/select_expert の冗長出力を抑制する。
 * (本丸 [onebrain-accuracy] が many-input ループを回すため。) */
static int moe_quiet = 0;

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
/* Gate: 入力からクラス予測 — ONE BRAIN (wave 18 本丸)                  */
/* ------------------------------------------------------------------ */

/* ── 三つの脳を一つに (docs/review-2026-06-three-brains.md) ─────────────
 * かつてゲートは温度1チャネルの if 梯子
 *   (void)hum;(void)press;(void)light; if(temp<20).../<35...
 * で、4入力のうち3つを捨てていた = moe_infer が *返す* 脳 (手書き MLP) とも
 * *守る* 脳 (学習 dtr) とも別の、三本目の脳だった。
 *
 * いまゲートは学習された dtr Transformer の argmax を返す。dtr は 4 センサー
 * トークンすべてを使う (DTR_SEQ_LEN==4)。これと *同じ 1 回の forward* の
 * argmax を moe_infer が返し、reflex へ渡す。= ルーティング・返答・守りが
 * 一つの学習脳。G22 で群れが学べば、この一本の forward が良くなり、三つ
 * すべてが同時に良くなる。
 *
 * conf_out!=0 のとき max-softmax×100 (0..100) も返す (反射ゲート用)。 */
static UB dtr_decide(B temp, B hum, B press, B light, INT *conf_out)
{
    B in[DTR_SEQ_LEN] = { (B)temp, (B)hum, (B)press, (B)light };
    float p[DTR_OUT_DIM];
    dtr_forward_probs(in, p);
    UB cls = 0; float mx = p[0];
    for (INT c = 1; c < DTR_OUT_DIM; c++)
        if (p[c] > mx) { mx = p[c]; cls = (UB)c; }
    if (conf_out) {
        INT cf = (INT)(mx * 100.0f + 0.5f);   /* REAL max-softmax, 0..100 */
        if (cf < 0)   cf = 0;
        if (cf > 100) cf = 100;
        *conf_out = cf;
    }
    return cls;
}

static UB gate_predict(B temp, B hum, B press, B light)
{
    return dtr_decide(temp, hum, press, light, (INT *)0);
}

/* 公開ラッパー: 「この入力のクラス帯」を学習脳 (dtr) の argmax で返す。
 * これが live なゲート — DKVA / regions / 反射経路と同じ学習脳を共有する
 * (重複定義を作らない)。注: R3b spec.c は *これを使わない*。spec のゲートは
 * 「訓練バンド (= 入力領域) 判定」であってクラス分類ではなく、クラスを予測
 * する dtr でルーティングすると循環するため、spec.c は自前のバンド分割器を
 * 持つ (spec.c 冒頭の honest note 参照)。 */
UB moe_gate_predict(B temp, B hum, B press, B light)
{
    return gate_predict(temp, hum, press, light);
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
 * のペナルティを引き、脅威(threat) を *加点* する。
 *   - pressure (負荷軸 §6): world ビーコンの局所勾配。高い=混んでいる→引く(避ける)。
 *   - threat   (脅威軸 §2): 候補自身が gossip した「危険/守るべき状態」信号。
 *     高い=守るべき一点→足す(寄る)。load とは逆符号 = 群れが threatened node へ
 *     集束する (G20 修正: かつては threat も pressure へ畳まれて避ける符号だった)。
 * 同 region なら反射層 (§8) を優先してわずかに加点。
 * すべて局所窓口だけを読み、中央/グローバルなオラクルは見ない。 */
static W expert_utility(UB accuracy, UW rtt_ms, INT eff_pressure, INT threat,
                        int same_region)
{
    if (rtt_ms == 0xFFFFFFFFUL) rtt_ms = MOE_RTT_UNKNOWN_MS;  /* 未実測 */
    W u = (W)accuracy;
    u -= (W)(rtt_ms / MOE_RTT_MS_PER_POINT);
    u -= (W)((eff_pressure * MOE_PRESS_NUM) / MOE_PRESS_DEN);   /* 負荷: 避ける */
    u += (W)((threat * MOE_PROTECT_NUM) / MOE_PROTECT_DEN);     /* 脅威: 寄る   */
    if (same_region) u += MOE_SAME_REGION_BONUS;
    return u;
}

/* 公開ラッパー: 閉ループの負帰還を「測れる形」で検証するため、§7 ゲートの
 * 効用関数そのものを reflex.c の自己テストへ開く (重複定義を作らない)。
 * これで closed-loop self-test の「行動→知覚→ゲート」の知覚→ゲート部が
 * 本番と同一の数式で回る (脅威軸は world.c の WORLD_BEACON.threat /
 * world_peer_threat と同じ経路で注入される)。 */
W moe_expert_utility(UB accuracy, UW rtt_ms, INT pressure, INT threat,
                     int same_region)
{
    return expert_utility(accuracy, rtt_ms, pressure, threat, same_region);
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

/* 候補ノードの脅威度 (THREAT 軸 §2)。候補 *自身* が gossip した「危険/守るべき
 * 状態を抱えている」信号 (world_peer_threat = WORLD_BEACON.threat = reflex の
 * CONSERVE が立てる)。これが expert_utility で *加点* され、群れの計算がその
 * 候補へ集束する (rally)。未知/未受信なら 0 (脅威なしと保守的に扱う)。
 * recent_pick のような自己仮想負荷は載せない — 脅威は load と別物。 */
static INT eff_threat(UB n)
{
    if (n >= DNODE_MAX) return MOE_THREAT_UNKNOWN;
    INT thr = world_peer_threat(n);
    return (thr < 0) ? MOE_THREAT_UNKNOWN : thr;
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

/* ── Wave G38.0 testable seam (touchpoint 0) — 純粋な反射層 1 決定 ─────
 * select_expert の「決定の数式」だけをここへ抜き出す (pure code-move):
 *   1. 候補ごとに瞬間 utility (expert_utility) を計り、EWMA (ewma_step) に
 *      畳む (単発ノイズ除去)。EWMA の最大を「挑戦者」とする。
 *   2. 現職 (incumbent) が健在なら、挑戦者が EWMA で MOE_SWITCH_MARGIN 以上
 *      勝らない限り現職を保持 (deadband_pick = ヒステリシス §4.2)。
 *   3. 選んだノードへ §8 の仮想負荷 (recent_pick) を上乗せ。
 *
 * I/O なし: SWIM/drpc/world/printf に一切触れない。候補は select_expert が
 * I/O を伴って列挙し MOE_CAND[] に詰めたものだけを受け取る。本番 select_expert
 * と self-test st_test_seam が *この同一関数* を呼ぶことで、選択ロジックが
 * reconstruction でなく本番そのものだと数で守られる (philosophy-gap-audit
 * trap A2 を閉じる)。数式は select_expert から *バイト等価に* 移しただけ —
 * expert_utility / ewma_step / deadband_pick / recent_pick は不変。新しい
 * 挙動・gacc・学習は一切含まない (Wave G38.1+ で mk_pino の review 待ち)。
 *
 * cand[i].node_id は大域テーブル (util_ewma/ewma_valid/recent_pick) の添字
 * でもある。incumbent_io は gate_class の現職スロット (in/out)。現職の健在
 * 判定は「現職が今回の候補集合に含まれるか」で行う — select_expert の元の
 * inc_ok (inc==me もしくは score_valid && ALIVE) は「候補であること」と同値
 * であり、候補ループが毎回 ewma_valid[node][gate_class]=1 を立てるので、元の
 * ewma_valid 条件込みでバイト等価。返り値は cand[] 内の添字 (picked index)。
 * cand[i].util_out / ewma_out に観測値を書き戻す (本番 printf がそのまま使う)。 */
UB moe_select_step(MOE_CAND *cand, UB ncand, UB gate_class, UB *incumbent_io)
{
    UB chal_i = 0xFF;        /* 挑戦者の cand[] 添字 */
    W  chal_e = 0;
    for (UB i = 0; i < ncand; i++) {
        UB  n = cand[i].node_id;
        W   u = expert_utility(cand[i].acc, cand[i].rtt, cand[i].eff_pressure,
                               cand[i].threat, cand[i].same_region);
        util_ewma[n][gate_class] = ewma_valid[n][gate_class]
                                 ? ewma_step(util_ewma[n][gate_class], u) : u;
        ewma_valid[n][gate_class] = 1;
        W e = util_ewma[n][gate_class];
        cand[i].util_out = u;        /* 観測値を書き戻す (本番 printf 用) */
        cand[i].ewma_out = e;
#ifdef MOE_SEAM_SABOTAGE
        /* falsification hook (cert §1.0 / risk #1 A2): 最高効用→最低効用へ
         * 反転させると [moe-seam] が RED になる。本番ビルドでは未定義。 */
        if (chal_i == 0xFF || e < chal_e) { chal_i = i; chal_e = e; }
#else
        if (chal_i == 0xFF || e > chal_e) { chal_i = i; chal_e = e; }
#endif
    }

    /* デッドバンド (§4.2 ヒステリシス): 現職が健在なら、挑戦者は margin を
     * 超えて勝らない限り選ばれない。現職の健在 = 今回の候補集合に居ること。 */
    UB  inc    = *incumbent_io;
    UB  inc_i  = 0xFF;
    for (UB i = 0; i < ncand; i++)
        if (cand[i].node_id == inc) { inc_i = i; break; }
    int inc_ok = (inc < DNODE_MAX) && (inc_i != 0xFF);
    const char *verdict;
    W   inc_e  = inc_ok ? util_ewma[inc][gate_class] : 0;
    UB  chal_n = (chal_i != 0xFF) ? cand[chal_i].node_id : 0xFF;
    UB  pick_n = deadband_pick(inc, inc_ok, inc_e, chal_n, chal_e, &verdict);
    *incumbent_io = pick_n;

    /* §8: 選んだノードへ自己観測の仮想負荷を即座に上乗せ (反射層の速い側)。 */
    if (pick_n < DNODE_MAX)
        recent_pick[pick_n] += MOE_PICK_LOAD;

    /* picked index を返す (deadband_pick は現職 inc か挑戦者 chal_n のみ返す)。 */
    if (pick_n == inc && inc_i != 0xFF) return inc_i;
    return chal_i;
}

/* 反射層の決定 (reflex-deliberation.md §4.3 / D1):
 *   1. 候補ごとに瞬間 utility を計り、EWMA に畳む (単発ノイズ除去)。
 *   2. EWMA の最大を「挑戦者」とする。
 *   3. 現職 (incumbent) が健在なら、挑戦者が EWMA で MOE_SWITCH_MARGIN
 *      以上勝らない限り現職を保持 (デッドバンド = ヒステリシス)。
 * 入力は accuracy テーブル (熟慮層が遅い帯域で更新) + RTT/pressure/region
 * (局所窓口) のみ。決定そのものは要求駆動 — 反射層の最速の時定数。
 *
 * Wave G38.0: 候補列挙 (I/O) → MOE_CAND[] 充填 → 純粋 moe_select_step、の
 * 三段に整理した。数式は moe_select_step へバイト等価に移しただけ。 */
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
        if (!moe_quiet) mo_puts("[moe] reflex local-only (no node id)\r\n");
        return me;
    }

    /* region のメンバ判定をこの選択の間だけ固定する (ホットパス: 再計算回避)。 */
    region_recompute();

    /* 候補列挙 (I/O 段): 自分も応援先の一候補にすぎない — ローカルが常に勝つ
     * わけではない (相互応援)。SWIM/world/region を読み MOE_CAND[] を詰める。
     * scratch は DNODE_MAX 固定 (no-VLA; 実行時次元には依存しない)。 */
    MOE_CAND cand[DNODE_MAX];
    UB ncand = 0;
    for (UB n = 0; n < DNODE_MAX; n++) {
        int is_self = (n == me);
        if (!is_self) {
            if (!score_valid[n]) continue;
            if (dnode_table[n].state != DNODE_ALIVE) continue;
        }
        cand[ncand].node_id      = n;
        cand[ncand].acc          = is_self ? my_accuracy[gate_class]
                                           : peer_scores[n].accuracy[gate_class];
        cand[ncand].rtt          = is_self ? 0 : swim_rtt_ms(n);
        /* 負荷項は world ビーコン (ゴシップされた局所勾配) + 自己仮想負荷。
         * broadcast score table はここでは負荷の真実として使わない (§7)。 */
        cand[ncand].eff_pressure = eff_pressure(n);
        /* 脅威項 (THREAT 軸 §2): 候補自身の「危険/守るべき」信号。自分なら
         * reflex から直読 (gossip 遅延ゼロ)、ピアなら局所 world-table の gossip。
         * load とは逆符号で *加点* され、群れがその一点へ集束する (rally)。 */
        cand[ncand].threat       = is_self ? (INT)reflex_threat_level()
                                           : eff_threat(n);
        cand[ncand].same_region  = is_self ? 1 : (region_is_member(n) ? 1 : 0);
        ncand++;
    }

    /* 純粋な決定段: 本番と self-test (st_test_seam) が共有する唯一の選択数式。
     * 更新前の現職を控えてから呼ぶ (可観測行の inc/verdict 復元に使う)。 */
    UB inc0 = incumbent[gate_class];
    UB pick_i = moe_select_step(cand, ncand, gate_class, &incumbent[gate_class]);
    UB pick   = (pick_i < ncand) ? cand[pick_i].node_id : me;

    /* 候補ごとの可観測行 (D2): moe_select_step が書き戻した util/ewma を、
     * 元と同じ順序・同じ書式で出す (バイト等価)。 */
    for (UB i = 0; i < ncand; i++) {
        UB n = cand[i].node_id;
        if (n == me) { mo_puts("[moe] cand self "); }
        else         { mo_puts("[moe] cand node"); mo_putdec(n); }
        mo_puts(" acc="); mo_putdec(cand[i].acc);
        mo_puts(" rtt="); mo_putdec(cand[i].rtt == 0xFFFFFFFFUL
                                    ? MOE_RTT_UNKNOWN_MS : cand[i].rtt);
        mo_puts("ms press="); mo_putdec((UW)cand[i].eff_pressure);
        mo_puts(" thr="); mo_putdec((UW)cand[i].threat);
        if (cand[i].threat > 0) mo_puts("(RALLY)");
        mo_puts(cand[i].same_region ? " rgn" : "    ");
        mo_puts(" util="); mo_putsdec(cand[i].util_out);
        mo_puts(" ewma="); mo_putsdec(cand[i].ewma_out);
        mo_puts("\r\n");
    }

    /* 反射 vs 熟慮の可観測行 (D2): 更新前の現職 inc0 / 挑戦者 chal / verdict を
     * moe_select_step と *同一の* deadband_pick で復元する (重複ロジックなし;
     * deadband_pick は副作用なしなので再呼び出しはバイト等価)。 */
    UB chal = 0xFF; W chal_e = 0;
    for (UB i = 0; i < ncand; i++)
        if (chal == 0xFF || cand[i].ewma_out > chal_e) {
            chal = cand[i].node_id; chal_e = cand[i].ewma_out;
        }
    int inc_ok = 0;
    for (UB i = 0; i < ncand; i++)
        if (cand[i].node_id == inc0) { inc_ok = (inc0 < DNODE_MAX); break; }
    const char *verdict;
    W inc_e = inc_ok ? util_ewma[inc0][gate_class] : 0;
    (void)deadband_pick(inc0, inc_ok, inc_e, chal, chal_e, &verdict);

    mo_puts("[moe] reflex cls="); mo_putdec(gate_class);
    mo_puts(" inc=");  if (inc0 < DNODE_MAX) { mo_puts("node"); mo_putdec(inc0); }
                       else mo_puts("none");
    if (inc_ok) { mo_puts(" ewma="); mo_putsdec(util_ewma[inc0][gate_class]); }
    mo_puts(" chal=node"); mo_putdec(chal);
    mo_puts(" ewma="); mo_putsdec(chal_e);
    mo_puts(" margin="); mo_putdec(MOE_SWITCH_MARGIN);
    mo_puts(" -> "); mo_puts(verdict);
    mo_puts("  delib_age=");
    if (delib_count == 0) mo_puts("-");
    else { mo_putdec(moe_uptime_ms - delib_at_ms); mo_puts("ms"); }
    mo_puts("\r\n");

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

/* ── ONE BRAIN observability (wave 18) — 三つの判断が *同じ 1 forward* から
 * 出ていることを self-test が数で検証できるよう、最後の moe_infer の
 *   returned (返した class) / gate (ルーティングした class) /
 *   reflex_cls (守りへ渡した class) / conf
 * を記録する。三者が常に等しい = 三脳が一脳。 */
static UB ob_last_returned   = 0xFF;
static UB ob_last_gate       = 0xFF;
static UB ob_last_reflex_cls = 0xFF;
static UB ob_last_conf       = 0xFF;

void moe_infer_last(UB *returned, UB *gate, UB *reflex_cls, UB *conf)
{
    if (returned)   *returned   = ob_last_returned;
    if (gate)       *gate       = ob_last_gate;
    if (reflex_cls) *reflex_cls = ob_last_reflex_cls;
    if (conf)       *conf       = ob_last_conf;
}

UB moe_infer(B temp, B hum, B press, B light)
{
    /* ── 本丸: ただ 1 回の dtr forward が、ルーティング・返答・守りを駆動する。
     * learned_class = argmax(dtr softmax)、confi = max-softmax×100。
     * gate も returned も reflex もこの learned_class から取る = 一脳。
     * (旧: gate=温度 if 梯子 / returned=手書き MLP / 守り=dtr の三本立て。
     *  docs/review-2026-06-three-brains.md §1。) */
    INT confi = 0;
    UB  learned_class = dtr_decide(temp, hum, press, light, &confi);

    UB gate_class = learned_class;             /* ルーティング = 学習脳 */
    UB expert     = select_expert(gate_class);

    /* S6 (galaxy.md): an inference flash — my star fires toward the
     * chosen expert star. ONE emit at the single select_expert site. */
    galaxy_emit(EV_MOE, drpc_my_node, expert, gate_class, learned_class);

    /* このノードがこの推論で発火したことを world-table へ通知する。
     * 全網マップ (world.c) の firing インジケータが点灯する。 */
    world_note_firing(gate_class);

    if (!moe_quiet) {
        mo_puts("[moe] gate="); mo_putdec(gate_class);
        mo_puts("  expert=node"); mo_putdec(expert);
    }

    UB result_class;

    if (expert == drpc_my_node || drpc_my_node == 0xFF) {
        /* ローカル: 返答は *同じ* dtr forward の argmax (手書き MLP は不使用)。 */
        result_class = learned_class;
        if (!moe_quiet) mo_puts("  [local]\r\n");
    } else {
        /* リモート: expert ノードが *同じ G22-ゴシップ学習脳 (dtr)* を回す
         * (drpc DRPC_CALL_INFER は dtr_classify; 手書き MLP は live path から
         *  排除済み)。タイムアウト時はローカルの learned_class へフォールバック。 */
        W packed = SENSOR_PACK(temp, hum, press, light);
        UB cls = 0;
        ER er = dtk_infer(expert, packed, &cls, 800);
        if (er == E_OK) {
            result_class = cls;
            if (!moe_quiet) { mo_puts("  [remote] class="); mo_putdec(cls); mo_puts("\r\n"); }
        } else {
            result_class = learned_class;
            if (!moe_quiet) { mo_puts("  [fallback] class="); mo_putdec(result_class); mo_puts("\r\n"); }
        }
    }

    my_total++;

    /* ── G17 + G38 + 本丸: 思考→行動を *同じ学習脳の確信* でゲートする ──────
     * learned_class / confi は上の唯一の forward から取った値そのもの。
     * 低確信 (未学習/曖昧) な入力は反射を発火させず、高確信の脅威クラスだけが
     * 決然と発火する。= 群れが学べば守りが良くなる。 */
    if (!moe_quiet) {
        mo_puts("[moe] learned cls="); mo_putdec(learned_class);
        mo_puts(" conf="); mo_putdec((UW)confi);
        mo_puts("% (one brain: gate==returned==guard from one dtr forward)\r\n");
    }
    reflex_on_inference(learned_class, (UB)confi, drpc_my_node);

    /* 三者を記録 (self-test の [onebrain-unified] が等値を検証)。
     * ローカル経路では returned==learned_class==gate_class。 */
    ob_last_returned   = result_class;
    ob_last_gate       = gate_class;
    ob_last_reflex_cls = learned_class;
    ob_last_conf       = (UB)confi;

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
            u[e] = expert_utility(acc, 0, press[obs][e], 0, 0);   /* threat=0: 純負荷軸 */
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
            u[n] = expert_utility(acc, 0, (INT)load[n], 0, 0);   /* threat=0: 純負荷軸 */
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
        out[n] = expert_utility((UB)(n == cls ? 90 : 50), 0, 0, 0, 0);
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

/* ── G20: 符号の分離 — 脅威時は flee でなく rally (survival §2) ──────────
 * 「守る対象へ全網の力を *注ぐ*」を *数で* 守る。脅威ノード P が守るべき仕事を
 * 抱えているとき:
 *   (A) P 自身は仕事を手放す (flee) のではなく保持する (hold)。
 *   (B) 近傍は P を避ける (load 符号) のではなく P へ寄る (rally; aid を送る)。
 *
 * 同一シナリオを 2 つの規則で回し、本番 expert_utility + deadband_pick
 * (st_reflex_step 経由) をそのまま使う:
 *   - naive  "threat==load": 脅威を *負荷軸* へ畳む (= G20 のバグ; 避ける符号)。
 *   - protect (本番)        : 脅威を *脅威軸* へ流す (= 加点; 寄る符号)。
 * 符号が逆 (脅威を引き算) だと protect 側の P は flee し近傍は avoid して
 * 下の assert が落ちる = テストが本当に新コードパスを検査している。 */
#define PT_THREAT   40   /* P の脅威度 (reflex CONSERVE 相当)            */
#define PT_LOAD     30   /* P/N が抱える仕事 (守るべき状態; 両者同一)    */
#define PT_ACC      70   /* 賢さは同一 (純粋に符号で競う)                 */
#define PT_T        8    /* EWMA/デッドバンドを定常へ運ぶ決定回数        */

/* 1 規則ぶんを回す。incumbent0 = この決定の現職 (A は keep=self, B は home)。
 * 返り: keep_or_aid = 「P を選んだ (= hold / rally)」回数。 */
static INT pt_run(int protect, int axisB, UB inc0)
{
    /* 候補0 = (A) P が自分の仕事を保持 / (B) 近傍が P へ寄る。
     * 候補1 = (A) P が隣へ手放す       / (B) 近傍が手元に留まる。   */
    W   ew[2] = { 0, 0 }; UB ev[2] = { 0, 0 }; UB inc = inc0;
    INT chose0 = 0;
    (void)axisB;
    for (INT t = 0; t < PT_T; t++) {
        /* 候補0 = 脅威ノード P。naive は脅威を負荷へ畳む (press += threat,
         * 脅威軸 0)。protect は脅威を脅威軸へ (press は素の負荷, threat=PT)。 */
        W u0 = protect
             ? moe_expert_utility(PT_ACC, 0, PT_LOAD,            PT_THREAT, 0)
             : moe_expert_utility(PT_ACC, 0, PT_LOAD + PT_THREAT, 0,        0);
        /* 候補1 = 脅威でない側 (素の負荷のみ)。 */
        W u1 = moe_expert_utility(PT_ACC, 0, PT_LOAD, 0, 0);
        W inst[2] = { u0, u1 };
        UB pick = st_reflex_step(inst, 2, ew, ev, &inc);
        if (pick == 0) chose0++;
    }
    return chose0;
}

static INT st_test_protect(void)
{
    INT fails = 0;

    /* 符号の生の確認: protect は脅威で +、naive は脅威で − (倒錯)。 */
    W u_protect = moe_expert_utility(PT_ACC, 0, PT_LOAD, PT_THREAT, 0);
    W u_naive   = moe_expert_utility(PT_ACC, 0, PT_LOAD + PT_THREAT, 0, 0);
    W u_safe    = moe_expert_utility(PT_ACC, 0, PT_LOAD, 0, 0);
    mo_puts("[moe-protect] threatened-node utility  protect=");
    mo_putsdec(u_protect); mo_puts(" naive(threat==load)=");
    mo_putsdec(u_naive); mo_puts(" safe-peer="); mo_putsdec(u_safe);
    mo_puts(" (threat="); mo_putdec(PT_THREAT); mo_puts(")\r\n");

    /* (A) P は守るべき仕事を保持するか (hold) / 手放すか (flee)。
     * incumbent = keep(self,0): デッドバンドは現状維持寄り。 */
    INT keep_protect = pt_run(1, 0, 0);   /* protect: hold を期待   */
    INT keep_naive   = pt_run(0, 0, 0);   /* naive  : flee を期待   */
    INT flee_protect = PT_T - keep_protect;
    INT flee_naive   = PT_T - keep_naive;
    mo_puts("[moe-protect] (A) own protected work:  protect keeps ");
    mo_putdec((UW)keep_protect); mo_puts("/"); mo_putdec((UW)PT_T);
    mo_puts(" (flee "); mo_putdec((UW)flee_protect);
    mo_puts(")   naive keeps "); mo_putdec((UW)keep_naive);
    mo_puts("/"); mo_putdec((UW)PT_T);
    mo_puts(" (flee "); mo_putdec((UW)flee_naive); mo_puts(")\r\n");

    /* (B) 近傍は P へ寄るか (rally→aid) / 避けるか (avoid)。
     * incumbent = home(1): 既定は手元に留まる。寄るには margin 超えが要る。 */
    INT aid_protect = pt_run(1, 1, 1);    /* protect: rally を期待  */
    INT aid_naive   = pt_run(0, 1, 1);    /* naive  : avoid を期待  */
    mo_puts("[moe-protect] (B) neighbour routing:   protect rallies ");
    mo_putdec((UW)aid_protect); mo_puts("/"); mo_putdec((UW)PT_T);
    mo_puts(" toward P   naive rallies "); mo_putdec((UW)aid_naive);
    mo_puts("/"); mo_putdec((UW)PT_T); mo_puts("\r\n");

    /* (1) 符号が逆: protect は脅威で utility が上がり、naive は下がる。 */
    if (!(u_protect > u_safe && u_naive < u_safe)) {
        mo_puts("[moe-protect] FAIL sign not separated (threat must add, not subtract)\r\n");
        fails++;
    }
    /* (2) P は守るべき仕事を保持する (rally; flee しない)。 */
    if (!(keep_protect == PT_T)) {
        mo_puts("[moe-protect] FAIL threatened node did not hold its protected work\r\n");
        fails++;
    }
    /* (3) naive 規則では P は仕事を手放す (flee; 対照)。 */
    if (!(flee_naive >= 1)) {
        mo_puts("[moe-protect] FAIL naive rule did not flee (control broken)\r\n");
        fails++;
    }
    /* (4) 近傍は脅威ノードへ寄る (force を一点へ注ぐ §2)。 */
    if (!(aid_protect >= 1 && aid_protect > aid_naive)) {
        mo_puts("[moe-protect] FAIL neighbours did not rally toward threatened node\r\n");
        fails++;
    }
    /* (5) naive 規則では近傍は脅威ノードを避ける (対照)。 */
    if (!(aid_naive == 0)) {
        mo_puts("[moe-protect] FAIL naive rule unexpectedly rallied\r\n");
        fails++;
    }

    if (fails == 0)
        mo_puts("[moe-protect] PASS (threat axis: protected node holds its work &"
                " neighbours rally toward it; naive threat==load flees/avoids)\r\n");
    else
        mo_puts("[moe-protect] FAIL\r\n");
    return fails;
}

/* ── Wave G38.0: [moe-seam] — 純粋選択関数 moe_select_step の直接 cert ──
 * (cert §1.0; philosophy-gap-audit trap A2 を閉じる)
 *
 * 既存の [moe-osc]/[moe-twolayer]/[moe-nocentral] は leaf helper を呼んで
 * 候補ループを *再構成* したテスト (reconstruction) であり、本番 select_expert
 * の選択ロジックそのものは検査していない。本テストは select_expert が実際に
 * 呼ぶ関数 moe_select_step を *逐語的に* (scripted MOE_CAND[] で) 叩き、
 * その選択 (最高効用 / 現職ヒステリシス / same-region 規則) が refactor 前の
 * select_expert 算術と完全に一致することを数で示す。これにより:
 *   - 抽出した moe_select_step が「本番が通る本物の選択ロジック」であること、
 *   - 将来 gacc cert がこの seam を rubber-stamp にできないこと
 * が保証される。MOE_SEAM_SABOTAGE を定義して最高→最低効用へ反転させると
 * 本テストは RED になる (KV/N-2b cert と同形の falsifiability)。
 *
 * 各シナリオは別々の node_id 群を使い (moe_init 後の ewma_valid=0 から開始)、
 * incumbent は呼び出し側ローカルに保持するので相互に汚染しない。
 * 効用 u = acc - rtt/MOE_RTT_MS_PER_POINT - eff_pressure*PRESS_NUM/DEN
 *          + threat*PROTECT_NUM/DEN + (same_region?MOE_SAME_REGION_BONUS:0)。
 * 単発決定では ewma_valid=0 ゆえ EWMA == 瞬間効用 (手計算で picked が決まる)。 */
static void sm_set(MOE_CAND *c, UB node, UB acc, UW rtt, INT press,
                   INT threat, int same_rgn)
{
    c->node_id = node; c->acc = acc; c->rtt = rtt;
    c->eff_pressure = press; c->threat = threat; c->same_region = same_rgn;
    c->util_out = 0; c->ewma_out = 0;
}

static INT st_test_seam(void)
{
    INT fails = 0;
    moe_init();   /* 反射状態 (util_ewma/ewma_valid/incumbent/recent_pick) を 0 へ */
    mo_puts("[moe-seam] direct cert of moe_select_step — THE function"
            " select_expert() calls (production path), not a reconstruction\r\n");

    /* S1: 現職なし (seed) → 最高効用が勝つ。3 候補 acc=70, press={80,10,50}
     *     → u = {30,65,45} → node1 (index 1) が最高。最低が勝つなら index 0/2。 */
    {
        MOE_CAND c[3];
        sm_set(&c[0], 0, 70, 0, 80, 0, 0);   /* u = 70-40        = 30 */
        sm_set(&c[1], 1, 70, 0, 10, 0, 0);   /* u = 70-5         = 65 */
        sm_set(&c[2], 2, 70, 0, 50, 0, 0);   /* u = 70-25        = 45 */
        UB inc = 0xFF;
        UB pick = moe_select_step(c, 3, 0, &inc);
        mo_puts("[moe-seam] S1 seed pick=index"); mo_putdec(pick);
        mo_puts(" node"); mo_putdec(c[pick].node_id);
        mo_puts(" util="); mo_putsdec(c[pick].util_out);
        mo_puts(" (expect index1/node1/util65)\r\n");
        if (!(pick == 1 && c[pick].node_id == 1 && c[1].util_out == 65 &&
              inc == 1)) {
            mo_puts("[moe-seam] FAIL S1 seed did not pick highest utility\r\n");
            fails++;
        }
    }

    /* S2: 現職 node3 (u=60), 挑戦者 node4 (u=65)。差 5 <= MOE_SWITCH_MARGIN(12)
     *     → HOLD 現職 (deadband ヒステリシス)。挑戦者は選ばれない。 */
    {
        MOE_CAND c[2];
        sm_set(&c[0], 3, 70, 0, 20, 0, 0);   /* inc node3: u = 70-10 = 60 */
        sm_set(&c[1], 4, 70, 0, 10, 0, 0);   /* chal node4: u = 70-5 = 65 */
        UB inc = 3;
        UB pick = moe_select_step(c, 2, 0, &inc);
        mo_puts("[moe-seam] S2 inc=node3 chal=node4(+5) pick=node");
        mo_putdec(c[pick].node_id);
        mo_puts(" (expect HOLD node3; margin="); mo_putdec(MOE_SWITCH_MARGIN);
        mo_puts(")\r\n");
        if (!(c[pick].node_id == 3 && inc == 3)) {
            mo_puts("[moe-seam] FAIL S2 deadband did not hold incumbent\r\n");
            fails++;
        }
    }

    /* S3: 現職 node5 (u=45), 挑戦者 node6 (u=70)。差 25 > MOE_SWITCH_MARGIN
     *     → SWITCH to node6。 */
    {
        MOE_CAND c[2];
        sm_set(&c[0], 5, 70, 0, 50, 0, 0);   /* inc node5: u = 70-25 = 45 */
        sm_set(&c[1], 6, 70, 0,  0, 0, 0);   /* chal node6: u = 70     = 70 */
        UB inc = 5;
        UB pick = moe_select_step(c, 2, 0, &inc);
        mo_puts("[moe-seam] S3 inc=node5 chal=node6(+25) pick=node");
        mo_putdec(c[pick].node_id);
        mo_puts(" (expect SWITCH node6)\r\n");
        if (!(c[pick].node_id == 6 && inc == 6)) {
            mo_puts("[moe-seam] FAIL S3 deadband did not switch on margin\r\n");
            fails++;
        }
    }

    /* S4: same-region 加点が pick を決める。node7 acc70 press0 rgn0 → u=70。
     *     node8 acc68 press0 rgn1 → u = 68+MOE_SAME_REGION_BONUS = 73。
     *     region bonus が無ければ node7(70) が勝つはず → node8 勝利が bonus の証拠。 */
    {
        MOE_CAND c[2];
        sm_set(&c[0], 7, 70, 0, 0, 0, 0);    /* u = 70                       */
        sm_set(&c[1], 8, 68, 0, 0, 0, 1);    /* u = 68 + 5 (same_region) = 73 */
        UB inc = 0xFF;
        UB pick = moe_select_step(c, 2, 0, &inc);
        mo_puts("[moe-seam] S4 region-bonus pick=node"); mo_putdec(c[pick].node_id);
        mo_puts(" util="); mo_putsdec(c[pick].util_out);
        mo_puts(" (expect node8/util73 via same_region+");
        mo_putdec(MOE_SAME_REGION_BONUS); mo_puts(")\r\n");
        if (!(c[pick].node_id == 8 && c[pick].util_out == 73)) {
            mo_puts("[moe-seam] FAIL S4 same-region bonus did not enter the pick\r\n");
            fails++;
        }
    }

    /* S5: recent_pick の §8 仮想負荷が picked node へ加算される (本番副作用)。
     *     S4 で node8 (index1) が picked。moe_select_step は recent_pick[8] へ
     *     MOE_PICK_LOAD を足したはず。 */
    if (recent_pick[8] != MOE_PICK_LOAD) {
        mo_puts("[moe-seam] FAIL S5 recent_pick virtual-load not applied to pick\r\n");
        fails++;
    } else {
        mo_puts("[moe-seam] S5 recent_pick[node8]="); mo_putdec(recent_pick[8]);
        mo_puts(" (= MOE_PICK_LOAD, §8 self-load applied by the real function)\r\n");
    }

    moe_init();   /* テストが汚した反射状態を片付ける */
    if (fails == 0)
        mo_puts("[moe-seam] PASS (moe_select_step IS select_expert's selection"
                " logic: highest-utility + hysteresis + region + §8 load)\r\n");
    else
        mo_puts("[moe-seam] FAIL\r\n");
    return fails;
}

/* ================================================================== */
/* 本丸 (wave 18) — ONE BRAIN property tests ([onebrain-*])             */
/*                                                                     */
/* docs/review-2026-06-three-brains.md: moe_infer は三つの違う脳で三つの  */
/* 違うことを言っていた — ルーティング (温度 if 梯子)・返答 (手書き MLP   */
/* 定数)・守り (学習 dtr)。本テスト群は、いまそれらが *同じ 1 回の dtr     */
/* forward* から出ること、4 チャネルすべてが効くこと、live path に手書き   */
/* MLP が居ないこと、そして集合学習 (G22) が *返答* 精度を持ち上げること   */
/* を数で守る。すべて単機 (drpc_my_node==0xFF) の純ローカル計算。dtr 重みは */
/* save/restore して既存 demo を壊さない。                              */
/* ================================================================== */

/* ── 決定的データセット (gl と同族: latent temp -> 3 class, distractor +
 * ラベルノイズ。temp/hum/light が情報を持ち press は純ノイズ)。static で
 * task stack を汚さない。 */
#define OB_N      150
#define OB_TRAIN  120
#define OB_TEST  (OB_N - OB_TRAIN)
#define OB_NCLS     3
#define OB_SEED  0x0BE12018UL

static B  ob_x[OB_N][DTR_SEQ_LEN];
static UB ob_y[OB_N];
static UB ob_ds_ready = 0;
static UW ob_rng;
static UW  ob_rand(void){ ob_rng = ob_rng*1664525UL + 1013904223UL; return (ob_rng>>16)&0x7FFF; }
static INT ob_uni(INT lo, INT hi){ return lo + (INT)(ob_rand() % (UW)(hi-lo+1)); }
static INT ob_noise(INT s){ INT v=0; for(INT i=0;i<4;i++) v+=ob_uni(-s,s); return v/2; }
static B   ob_clamp(INT v){ if(v>127)v=127; if(v<-128)v=-128; return (B)v; }

static void ob_ds_init(void)
{
    if (ob_ds_ready) return;
    ob_rng = OB_SEED;
    for (INT i = 0; i < OB_N; i++) {
        UB c = (UB)(i % OB_NCLS);
        INT latent = (c==0) ? ob_uni(-50,24) : (c==1) ? ob_uni(25,69) : ob_uni(70,120);
        INT temp  = latent + ob_noise(12);
        INT hum   = 60 - latent/2 + ob_noise(20);
        INT press = ob_uni(-30, 90);              /* 純ノイズ (distractor)  */
        INT light = 10 + 30*(INT)c + ob_noise(35);
        ob_x[i][0]=ob_clamp(temp); ob_x[i][1]=ob_clamp(hum);
        ob_x[i][2]=ob_clamp(press); ob_x[i][3]=ob_clamp(light);
        ob_y[i] = c;
    }
    ob_ds_ready = 1;
}

/* RETURNED-class accuracy on held-out under currently-loaded weights.
 * 単機では moe_infer の返り値 == dtr_classify (= [onebrain-unified] が
 * 証明する等値) なので、ここでは副作用のない dtr_classify で測る。 */
static float ob_returned_acc(void)
{
    UW correct = 0;
    for (INT i = OB_TRAIN; i < OB_N; i++)
        if (dtr_classify(ob_x[i]) == ob_y[i]) correct++;
    return (float)correct * 100.0f / (float)OB_TEST;
}

/* ── [onebrain-unified] — 返答 == ルーティング == 守り、すべて 1 forward ── */
static INT st_test_onebrain_unified(void)
{
    INT fails = 0;
    static float saved[DTR_WEIGHT_FLOATS];
    dtr_weights_get(saved);
    dtr_reinit_weights(0x1B0A12FCUL);          /* 決定的モデル */

    static const B probe[4][DTR_SEQ_LEN] = {
        { -40,  50,  10, -50 },
        {  10, -20,   0,  20 },
        {  90, -60,  30,  80 },
        {  60,   0, -20,  40 },
    };
    moe_quiet = 1;
    BOOL rprev = reflex_set_enabled(0);   /* 反射の副作用を止める (経験を汚さない) */
    for (INT i = 0; i < 4; i++) {
        UB ret = moe_infer(probe[i][0], probe[i][1], probe[i][2], probe[i][3]);
        UB r2, g, rx, cf; moe_infer_last(&r2, &g, &rx, &cf);
        UB d = dtr_classify(probe[i]);
        mo_puts("[onebrain-unified] in"); mo_putdec((UW)i);
        mo_puts(" returned="); mo_putdec(ret);
        mo_puts(" routed=");   mo_putdec(g);
        mo_puts(" guarded=");  mo_putdec(rx);
        mo_puts(" dtr=");      mo_putdec(d);
        mo_puts(" conf=");     mo_putdec(cf); mo_puts("%\r\n");
        if (!(ret == r2 && ret == g && g == rx && rx == d)) {
            mo_puts("[onebrain-unified] FAIL three decisions are not one\r\n");
            fails++;
        }
    }
    reflex_set_enabled(rprev);
    moe_quiet = 0;
    dtr_weights_set(saved);
    if (fails == 0)
        mo_puts("[onebrain-unified] PASS (returned==routed==guarded, all one dtr forward)\r\n");
    else
        mo_puts("[onebrain-unified] FAIL\r\n");
    return fails;
}

/* ── [onebrain-channels] — 4 チャネルすべてが出力を動かす ((void) 廃止) ── */
static INT st_test_onebrain_channels(void)
{
    INT fails = 0;
    static float saved[DTR_WEIGHT_FLOATS];
    dtr_weights_get(saved);
    dtr_reinit_weights(0x0C4A11E5UL);          /* 決定的モデル */

    const char *nm[4] = { "temp", "hum", "press", "light" };
    B base[DTR_SEQ_LEN] = { 0, -10, 0, -20 };  /* +90 摂動で範囲内に収まる */
    float p0[DTR_OUT_DIM]; dtr_forward_probs(base, p0);

    INT moved_nontemp = 0;
    for (INT ch = 0; ch < 4; ch++) {
        B x[DTR_SEQ_LEN]; for (INT k = 0; k < 4; k++) x[k] = base[k];
        x[ch] = (B)(base[ch] + 90);
        float p[DTR_OUT_DIM]; dtr_forward_probs(x, p);
        float d = 0.0f;
        for (INT c = 0; c < DTR_OUT_DIM; c++) {
            float e = p[c] - p0[c]; if (e < 0) e = -e; if (e > d) d = e;
        }
        mo_puts("[onebrain-channels] perturb "); mo_puts(nm[ch]);
        mo_puts(" -> max|dprob|x1000="); mo_putdec((UW)(d*1000.0f + 0.5f));
        mo_puts("\r\n");
        if (d <= 0.0f) {
            mo_puts("[onebrain-channels] FAIL channel ignored by gate\r\n"); fails++;
        }
        if (ch != 0 && d > 0.001f) moved_nontemp++;
    }
    dtr_weights_set(saved);
    /* 旧 if 梯子は hum/press/light を (void) で捨てていた。3 つすべてが
     * 出力を動かせば、その破棄は live path から消えている。 */
    if (moved_nontemp < 3) {
        mo_puts("[onebrain-channels] FAIL not all of hum/press/light affect the gate\r\n");
        fails++;
    }
    if (fails == 0)
        mo_puts("[onebrain-channels] PASS (all 4 channels move the gate; the (void) discard is gone)\r\n");
    else
        mo_puts("[onebrain-channels] FAIL\r\n");
    return fails;
}

/* ── [onebrain-nomlp] — live path は学習 dtr に従い、手書き MLP に従わない ── */
static INT st_test_onebrain_nomlp(void)
{
    INT fails = 0;
    ob_ds_init();
    static float saved[DTR_WEIGHT_FLOATS];
    dtr_weights_get(saved);
    dtr_reinit_weights(0x2C0DE777UL);          /* 決定的モデル */

    /* 手書き MLP (ai_job.c の定数) と学習 dtr が *食い違う* 入力を探し、
     * moe_infer がどちらに従うかを見る。live path が dtr なら必ず dtr に従う。 */
    INT found = 0;
    moe_quiet = 1;
    BOOL rprev = reflex_set_enabled(0);   /* 反射の副作用を止める (経験を汚さない) */
    for (INT i = 0; i < OB_N && !found; i++) {
        UB md = mlp_forward(ob_x[i]);
        UB dd = dtr_classify(ob_x[i]);
        if (md != dd) {
            UB ret = moe_infer(ob_x[i][0], ob_x[i][1], ob_x[i][2], ob_x[i][3]);
            mo_puts("[onebrain-nomlp] disagree at i="); mo_putdec((UW)i);
            mo_puts(": handwritten-MLP="); mo_putdec(md);
            mo_puts(" learned-dtr=");      mo_putdec(dd);
            mo_puts(" moe_infer=");        mo_putdec(ret); mo_puts("\r\n");
            if (!(ret == dd && ret != md)) {
                mo_puts("[onebrain-nomlp] FAIL live path followed the handwritten MLP\r\n");
                fails++;
            }
            found = 1;
        }
    }
    reflex_set_enabled(rprev);
    moe_quiet = 0;
    dtr_weights_set(saved);
    if (!found) {
        mo_puts("[onebrain-nomlp] FAIL no MLP/dtr disagreement found to distinguish the brains\r\n");
        fails++;
    }
    if (fails == 0)
        mo_puts("[onebrain-nomlp] PASS (live path follows the learned dtr, not ai_job.c's handwritten-constant MLP)\r\n");
    else
        mo_puts("[onebrain-nomlp] FAIL\r\n");
    return fails;
}

/* ── [onebrain-accuracy] — 集合学習 (G22) が *返答* 精度を持ち上げる ─────
 * これがレビューの核: かつて学習 (G22) が鍛える dtr を moe_infer は *返さ*
 * なかった (返したのは手書き MLP)。いま返答は dtr なので、群れが学べば
 * 返答が良くなる。decentralized・no-central・leave-one-class-out shard の
 * ゴシップ平均 (gl_merge) で「群れ」を作り、未学習と比較する。 */
static B  ob_sh_x[OB_NCLS][OB_TRAIN][DTR_SEQ_LEN];
static UB ob_sh_y[OB_NCLS][OB_TRAIN];
static UW ob_sh_n[OB_NCLS];
static float ob_model[OB_NCLS][DTR_WEIGHT_FLOATS];
static float ob_avg[DTR_WEIGHT_FLOATS];

#define OB_INIT_SEED 0xC0FFEE18UL
#define OB_ROUNDS    40
#define OB_LOCAL      4

static void ob_build_shards(void)
{
    for (UW k = 0; k < OB_NCLS; k++) {
        UW m = 0;
        for (INT i = 0; i < OB_TRAIN; i++) {
            if (ob_y[i] == (UB)(k % OB_NCLS)) continue;   /* leave-one-class-out */
            for (INT t = 0; t < DTR_SEQ_LEN; t++) ob_sh_x[k][m][t] = ob_x[i][t];
            ob_sh_y[k][m] = ob_y[i]; m++;
        }
        ob_sh_n[k] = m;
    }
}

static INT st_test_onebrain_accuracy(void)
{
    INT fails = 0;
    ob_ds_init();
    ob_build_shards();
    static float saved[DTR_WEIGHT_FLOATS];
    dtr_weights_get(saved);

    /* UNLEARNED: 群れが出発する共有 seed そのもの。 */
    dtr_reinit_weights(OB_INIT_SEED);
    float acc_u = ob_returned_acc();

    /* LEARNED: decentralized no-central ゴシップ (各ノードは disjoint shard
     * だけ見る = 単独では全クラスを学べない。gl_merge で peer-symmetric 平均)。 */
    for (UW k = 0; k < OB_NCLS; k++) {
        dtr_reinit_weights(OB_INIT_SEED);
        dtr_weights_get(ob_model[k]);
    }
    UW total = OB_ROUNDS * OB_LOCAL;
    for (UW r = 0; r < OB_ROUNDS; r++) {
        for (UW k = 0; k < OB_NCLS; k++) {
            dtr_weights_set(ob_model[k]);
            for (UW s = 1; s <= OB_LOCAL; s++) {
                UW step = r * OB_LOCAL + s;
                float lr = (step <= total/2) ? 0.10f : 0.05f;
                (void)dtr_train_batch(ob_sh_x[k], ob_sh_y[k], ob_sh_n[k], lr);
            }
            dtr_weights_get(ob_model[k]);
        }
        const float *ptrs[OB_NCLS];
        for (UW k = 0; k < OB_NCLS; k++) ptrs[k] = ob_model[k];
        gl_merge(ob_avg, ptrs, OB_NCLS, DTR_WEIGHT_FLOATS);   /* no-central merge */
        for (UW k = 0; k < OB_NCLS; k++)
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) ob_model[k][i] = ob_avg[i];
    }
    dtr_weights_set(ob_model[0]);
    float acc_l = ob_returned_acc();

    dtr_weights_set(saved);

    mo_puts("[onebrain-accuracy] moe_infer RETURNED-class accuracy on held-out"
            " (returned==dtr argmax; see [onebrain-unified]):\r\n");
    mo_puts("[onebrain-accuracy]   UNLEARNED=");  mo_putdec((UW)(acc_u + 0.5f));
    mo_puts("%   G22-LEARNED=");                  mo_putdec((UW)(acc_l + 0.5f));
    mo_puts("%   (3 nodes, leave-one-class-out shards, no-central gossip)\r\n");
    if (!(acc_l > acc_u + 5.0f)) {
        mo_puts("[onebrain-accuracy] FAIL collective learning did not lift the RETURNED accuracy\r\n");
        fails++;
    }
    if (fails == 0)
        mo_puts("[onebrain-accuracy] PASS (swarm learns -> the RETURNED answer gets better;"
                " was the brain the output never read)\r\n");
    else
        mo_puts("[onebrain-accuracy] FAIL\r\n");
    return fails;
}

/* 公開エントリ: §7/§8/§2 性質テスト + 本丸 ONE BRAIN テストを順に走らせ、
 * 合計 fail 数を返す。shell `moe test` から呼ばれ、CI は各 PASS 行を grep。 */
INT moe_self_test(void)
{
    INT fails = 0;
    mo_puts("[moe-test] ==== §7/§8/§2 property tests (I7/I8/D0/§5/G20) ====\r\n");
    fails += st_test_nocentral();
    fails += st_test_twolayer();
    fails += st_test_oscillation();
    fails += st_test_concurrent();
    fails += st_test_protect();
    fails += st_test_seam();        /* Wave G38.0: direct cert of moe_select_step */
    mo_puts("[moe-test] ==== 本丸 ONE BRAIN tests (wave 18 — three brains -> one) ====\r\n");
    fails += st_test_onebrain_unified();
    fails += st_test_onebrain_channels();
    fails += st_test_onebrain_nomlp();
    fails += st_test_onebrain_accuracy();
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
