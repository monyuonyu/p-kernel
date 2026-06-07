/*
 *  reflex.c
 *  §8 反射層 — 思考→行動の配線。
 *
 *  詳細・思想・NO-CENTRAL 不変条件は reflex.h /
 *  docs/architecture/reflex-action.md を参照。
 *
 *  ── 責任範囲 (この 1 ファイルが本体) ────────────────────────────────
 *    推論完了 (reflex_on_inference) → アクション表 → 実在する局所操作:
 *      SHIELD   = reflex_is_shielded() を立てる (usermain が selfc/genome を拒否)
 *      CONSERVE = reflex_pressure_bias() を立てる (world ビーコン pressure へ)
 *      BEACON   = "reflex/alarm/<node>" へ即時 publish (隣が知る/命令ではない)
 *    受信側 (reflex_task) は他ノードのアラームを取り込み、*減衰した* 反射
 *    (CONSERVE どまり, hop 1 で打ち止め) を自分の判断で行う。
 *  ────────────────────────────────────────────────────────────────────
 */

#include "reflex.h"
#include "drpc.h"
#include "kdds.h"
#include "moe.h"      /* moe_expert_utility — closed-loop self-test の §7 ゲート */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー (world.c / moe.c と同じ作法)                            */
/* ------------------------------------------------------------------ */

static void rf_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void rf_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rf_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rf_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* アクション表 — class → action (データ駆動の小さな静的表)            */
/*                                                                     */
/* これが「思考(class) → 行動(action)」の翻訳表。`reflex table` で閲覧。 */
/* SHIELD は最も重い行動なので、表に載っていても連続 critical            */
/* (REFLEX_SHIELD_STREAK) を追加条件とする (下の reflex_on_inference)。   */
/* ------------------------------------------------------------------ */

static UB act_table[REFLEX_NUM_CLASSES] = {
    /* 0 normal   */ REFLEX_ACT_NONE,
    /* 1 alert    */ REFLEX_ACT_CONSERVE | REFLEX_ACT_BEACON,
    /* 2 critical */ REFLEX_ACT_SHIELD | REFLEX_ACT_CONSERVE | REFLEX_ACT_BEACON,
};

static const char *class_name(UB c)
{
    switch (c) {
    case 0:  return "normal";
    case 1:  return "alert";
    case 2:  return "critical";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* 反射状態 (速い時定数で動く局所閉ループ)                              */
/* ------------------------------------------------------------------ */

static UB enabled = 1;             /* デフォルト有効 (通常 class では何もしない) */

/* エンゲージしたアクションの「保持期限」(operating time ms)。
 * 0 = 非エンゲージ。now < until のあいだアクションが効く = ヒステリシス。 */
static UW shield_until   = 0;
static UW conserve_until = 0;

static UB danger_streak  = 0;      /* 連続 critical 回数 (SHIELD のゲート)   */

/* BEACON 発信シーケンスと、受信アラームの重複検出用 last-seq。 */
static UW my_alarm_seq = 0;
static UW peer_last_seq[DNODE_MAX];

/* 統計 (可観測性) */
static UW st_fires       = 0;      /* 自己観測で反射した回数               */
static UW st_beacons     = 0;      /* BEACON publish 回数                  */
static UW st_alarms_rx   = 0;      /* 受信アラーム数                       */
static UW st_attenuated  = 0;      /* 受信→減衰反射した回数               */
static UW st_releases    = 0;      /* ヒステリシス解除回数                 */
static UW st_shield_deny = 0;      /* SHIELD が拒否した取り込み回数        */

/* ── G18 熟慮 → 学習 → 反射ループの状態 (§9) ─────────────────────────────
 * learned_conserve : 反射の CONSERVE 効きの強さ。reflex_pressure_bias() が
 *                    固定値ではなくこの学習値を返す。熟慮層が dwell 経験から
 *                    nudge する (REFLEX_CONSERVE_MIN..MAX)。
 * threat_run       : いま連続して観測している脅威の長さ (= 進行中の dwell)。
 * win_dwell_sum/   : 熟慮の窓で観測した「脅威エピソードの dwell」の合計と本数。
 *   win_episodes     1 エピソード = 脅威が立ち上がってから normal に戻るまで。 */
static UB learned_conserve = REFLEX_CONSERVE_PRESSURE;
static UW threat_run       = 0;
static UW win_dwell_sum    = 0;
static UW win_episodes     = 0;

/* 学習の可観測性 */
static UW st_delib      = 0;       /* 熟慮 tick 回数                       */
static UW st_learn_up   = 0;       /* 効きを上げた回数 (脅威が滞留した)    */
static UW st_learn_down = 0;       /* 効きを下げた回数 (過剰防御だった)    */

/* K-DDS ハンドル (per-source topic; world.c と同じパターン)
 *   h_pub    : 自ノード "reflex/alarm/<my>" へ発行
 *   h_sub[n] : ピア "reflex/alarm/<n>" を購読 (n != my) */
static W h_pub = -1;
static W h_sub[DNODE_MAX];

static void alarm_topic_name(char *out, UB node)
{
    const char *p = REFLEX_ALARM_TOPIC_PFX;
    INT i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + node);
    out[i]   = '\0';
}

static UW now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return t.lo;
}

/* ------------------------------------------------------------------ */
/* BEACON — 脅威観測を per-source topic へ即時 publish                  */
/* (命令ではなく情報: 受信側も自分の反射表で判断する)                  */
/* ------------------------------------------------------------------ */

static void emit_beacon(UB threat_class, UB confidence, UB hop)
{
    if (h_pub < 0) return;
    REFLEX_ALARM a;
    a.magic        = REFLEX_ALARM_MAGIC;
    a.seq          = (U4)(++my_alarm_seq);
    a.src_node     = drpc_my_node;
    a.threat_class = threat_class;
    a.confidence   = confidence;
    a.hop          = hop;
    kdds_pub(h_pub, &a, (W)sizeof(a));
    st_beacons++;
    rf_puts("[reflex] BEACON class="); rf_putdec(threat_class);
    rf_puts(" ("); rf_puts(class_name(threat_class));
    rf_puts(") seq="); rf_putdec(my_alarm_seq);
    rf_puts(" hop="); rf_putdec(hop);
    rf_puts(" -> reflex/alarm\r\n");
}

/* ------------------------------------------------------------------ */
/* アクションのエンゲージ (速く入る)                                    */
/* ------------------------------------------------------------------ */

static void engage(UB mask, const char *why)
{
    UW until = now_ms() + REFLEX_HOLD_MS;
    rf_puts("[reflex] FIRE ");
    rf_puts(why);
    rf_puts(" ->");
    int any = 0;
    if (mask & REFLEX_ACT_SHIELD)   { shield_until   = until; rf_puts(" SHIELD");   any = 1; }
    if (mask & REFLEX_ACT_CONSERVE) { conserve_until = until; rf_puts(" CONSERVE"); any = 1; }
    if (mask & REFLEX_ACT_BEACON)   {                          rf_puts(" BEACON");   any = 1; }
    if (!any) rf_puts(" (none)");
    rf_puts(" (hold ");
    rf_putdec(REFLEX_HOLD_MS / 1000);
    rf_puts("s)\r\n");
}

/* ------------------------------------------------------------------ */
/* 推論完了フック (思考 → 行動の入口)                                  */
/* ------------------------------------------------------------------ */

void reflex_on_inference(UB threat_class, UB confidence, UB src_node)
{
    (void)src_node;
    if (!enabled) return;
    if (threat_class >= REFLEX_NUM_CLASSES) return;

    /* 連続 critical の追跡 (SHIELD の追加ゲート)。
     *   critical → streak++、normal → streak=0、alert → 据え置き。 */
    if (threat_class == 2)      { if (danger_streak < 255) danger_streak++; }
    else if (threat_class == 0) { danger_streak = 0; }

    /* G18 経験の記録: 脅威エピソードの dwell を測る。脅威 (class>=1) が続く
     * 間 threat_run を伸ばし、normal に戻った瞬間にその長さを 1 エピソードの
     * dwell として熟慮の窓へ積む。これが「経験」= 熟慮層が学習に使う観測。
     * confidence ゲートより前に置く: 行動を起こさなくても脅威は知覚した。 */
    if (threat_class >= 1) {
        threat_run++;
    } else {                      /* threat_class == 0 → エピソード終端 */
        if (threat_run > 0) { win_dwell_sum += threat_run; win_episodes++; }
        threat_run = 0;
    }

    UB mask = act_table[threat_class];
    if (mask == REFLEX_ACT_NONE) return;   /* 通常 class では何もしない */

    /* 確信度ゲート: 低確信の推論では行動しない (0xFF=不明は通す)。 */
    if (confidence != 0xFF && confidence < REFLEX_CONF_MIN) return;

    /* SHIELD は連続 critical が閾値に達するまで保留 (重い行動の慎重化)。 */
    if ((mask & REFLEX_ACT_SHIELD) && danger_streak < REFLEX_SHIELD_STREAK)
        mask = (UB)(mask & ~REFLEX_ACT_SHIELD);

    st_fires++;
    {
        char b[48];
        const char *p = "class=";
        INT i = 0; while (p[i]) { b[i] = p[i]; i++; }
        b[i++] = (char)('0' + (threat_class % 10));
        b[i]   = '\0';
        engage(mask, b);
    }

    if (mask & REFLEX_ACT_BEACON)
        emit_beacon(threat_class, confidence, REFLEX_MAX_HOP);
}

/* ------------------------------------------------------------------ */
/* アラーム受信 → *減衰した* 反射 (隣の叫びは自分で判断する)           */
/*                                                                     */
/* 群れ全体の一斉痙攣を防ぐ二段の減衰:                                  */
/*   1. 強度の減衰: 受信した危険では SHIELD まで行かず CONSERVE どまり  */
/*      (自分で観測した危険だけが完全な遮蔽に値する)。                  */
/*   2. 空間の減衰: hop>0 のときだけ hop-1 で 1 回中継。MAX_HOP=1 なので */
/*      連鎖は隣の隣で止まる。                                          */
/* ------------------------------------------------------------------ */

static void on_alarm(const REFLEX_ALARM *a)
{
    if (!enabled) return;
    if (a->magic != REFLEX_ALARM_MAGIC) return;
    if (a->src_node >= DNODE_MAX) return;
    if (a->src_node == drpc_my_node) return;     /* 自分の反響は無視 */

    /* 重複検出: 同じ seq を二度処理しない (LATEST_ONLY を毎ポール返すため)。 */
    if (peer_last_seq[a->src_node] == (UW)a->seq && peer_last_seq[a->src_node] != 0)
        return;
    peer_last_seq[a->src_node] = (UW)a->seq;
    st_alarms_rx++;

    /* 情報であって命令ではない: 受信側が自分の反射表で「構えるか」を決める。
     * 危険 (class>=1) のときだけ、減衰した CONSERVE を engage する。 */
    if (a->threat_class >= 1) {
        st_attenuated++;
        rf_puts("[reflex] heard alarm from node");
        rf_putdec(a->src_node);
        rf_puts(" class="); rf_putdec(a->threat_class);
        rf_puts(" ("); rf_puts(class_name(a->threat_class));
        rf_puts(") -> attenuated CONSERVE (no SHIELD; my own judgement)\r\n");
        conserve_until = now_ms() + REFLEX_HOLD_MS;

        /* 空間の減衰: hop が残っていれば 1 回だけ中継して打ち止め。 */
        if (a->hop > 0)
            emit_beacon(a->threat_class, a->confidence, (UB)(a->hop - 1));
    }
}

/* ------------------------------------------------------------------ */
/* ヒステリシス解除 (ゆっくり出る)                                      */
/* ------------------------------------------------------------------ */

static void check_release(void)
{
    UW now = now_ms();
    if (shield_until && now >= shield_until) {
        shield_until = 0;
        danger_streak = 0;
        st_releases++;
        rf_puts("[reflex] SHIELD released (hysteresis ");
        rf_putdec(REFLEX_HOLD_MS / 1000);
        rf_puts("s elapsed)\r\n");
    }
    if (conserve_until && now >= conserve_until) {
        conserve_until = 0;
        st_releases++;
        rf_puts("[reflex] CONSERVE released (hysteresis ");
        rf_putdec(REFLEX_HOLD_MS / 1000);
        rf_puts("s elapsed)\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* 公開照会 (SHIELD / CONSERVE の実在効果の窓口)                        */
/* ------------------------------------------------------------------ */

BOOL reflex_is_shielded(void)
{
    if (!enabled || shield_until == 0) return FALSE;
    if (now_ms() >= shield_until) return FALSE;
    st_shield_deny++;
    return TRUE;
}

UB reflex_pressure_bias(void)
{
    if (!enabled || conserve_until == 0) return 0;
    if (now_ms() >= conserve_until) return 0;
    /* 固定値ではなく熟慮層が学習した効きを返す (§9 G18)。CONSERVE→pressure→
     * §7 ゲートの負帰還ループの *ゲイン* がこれで経験から調律される。 */
    return learned_conserve;
}

UB reflex_learned_conserve(void) { return learned_conserve; }

/* ------------------------------------------------------------------ */
/* G18 熟慮 tick: 経験 (脅威 dwell) から learned_conserve を学習で nudge   */
/* (遅い時定数。反射の瞬間スパイクではなく窓で均した経験に反応する §8)    */
/* ------------------------------------------------------------------ */

void reflex_deliberate(void)
{
    st_delib++;
    if (win_episodes == 0) return;        /* この窓に経験なし → 据え置き */

    UW avg = win_dwell_sum / win_episodes;   /* 平均脅威 dwell */

    /* 脅威が目標より長く滞留した = 反射が弱すぎる → 効きを上げる。
     * 目標より短い = 過剰防御 → わずかに下げる (応援に出る余地を残す)。
     * いずれも [MIN,MAX] でクランプした小さな nudge (発振しない外側ループ)。 */
    if (avg > (UW)REFLEX_DWELL_TARGET) {
        UW v = (UW)learned_conserve + REFLEX_LEARN_STEP;
        if (v > REFLEX_CONSERVE_MAX) v = REFLEX_CONSERVE_MAX;
        learned_conserve = (UB)v;
        st_learn_up++;
    } else if (avg < (UW)REFLEX_DWELL_TARGET) {
        UW lo = (UW)REFLEX_CONSERVE_MIN;
        UW v  = (learned_conserve > REFLEX_LEARN_STEP)
                  ? (UW)learned_conserve - REFLEX_LEARN_STEP : lo;
        if (v < lo) v = lo;
        learned_conserve = (UB)v;
        st_learn_down++;
    }

    rf_puts("[reflex] DELIBERATE avg_dwell="); rf_putdec(avg);
    rf_puts(" target="); rf_putdec(REFLEX_DWELL_TARGET);
    rf_puts(" -> learned_conserve="); rf_putdec(learned_conserve);
    rf_puts(" (from "); rf_putdec(win_episodes); rf_puts(" episodes)\r\n");

    win_dwell_sum = 0;
    win_episodes  = 0;
}

/* ------------------------------------------------------------------ */
/* 常駐タスク: アラーム取り込み + 解除チェック (全ノード対称)          */
/* ------------------------------------------------------------------ */

void reflex_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* cmd_net (drpc_init 後) に起動されるので drpc_my_node は確定。
     * per-source topic を poll-only で開く (world.c と同じ; セマフォを
     * 消費せず DNODE_MAX 個開いても枯渇しない)。 */
    if (drpc_my_node != 0xFF) {
        char tn[KDDS_NAME_MAX];
        alarm_topic_name(tn, drpc_my_node);
        h_pub = kdds_open_poll(tn, KDDS_QOS_LATEST_ONLY);
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) { h_sub[n] = -1; continue; }
            alarm_topic_name(tn, n);
            h_sub[n] = kdds_open_poll(tn, KDDS_QOS_LATEST_ONLY);
        }
    }

    /* G18: 熟慮層は反射の速い tick を間引いた遅い時定数で回る (§8 二層)。 */
    UW since_delib = 0;
    for (;;) {
        tk_dly_tsk(REFLEX_POLL_MS);

        /* 進行中の脅威 run も「まだ続いている経験」として時々窓へ反映する
         * ため、解除チェックの度に終端していない run を確定はしない (normal
         * 観測で終端する)。熟慮 tick だけは drpc 確定前でも回す価値はないので
         * net 確定後に限る。 */
        if (drpc_my_node == 0xFF) { check_release(); continue; }

        for (UB n = 0; n < DNODE_MAX; n++) {
            if (h_sub[n] < 0) continue;
            REFLEX_ALARM a;
            W r = kdds_sub(h_sub[n], &a, (W)sizeof(a), 0);
            if (r >= (W)sizeof(REFLEX_ALARM) && a.src_node == n)
                on_alarm(&a);
        }

        check_release();

        /* G18 熟慮 → 学習 → 反射: 遅い時定数 (REFLEX_DELIB_EVERY ポール) で
         * 蓄積した dwell 経験から learned_conserve を nudge する。稼働中の観測
         * だけで反射の効きが書き換わる (外部 train コマンド不要)。 */
        if (++since_delib >= REFLEX_DELIB_EVERY) {
            since_delib = 0;
            reflex_deliberate();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void reflex_init(void)
{
    enabled        = 1;
    shield_until   = 0;
    conserve_until = 0;
    danger_streak  = 0;
    my_alarm_seq   = 0;
    h_pub          = -1;
    /* G18 学習状態 */
    learned_conserve = REFLEX_CONSERVE_PRESSURE;
    threat_run     = 0;
    win_dwell_sum  = 0;
    win_episodes   = 0;
    st_delib = st_learn_up = st_learn_down = 0;
    for (INT n = 0; n < DNODE_MAX; n++) { h_sub[n] = -1; peer_last_seq[n] = 0; }
    rf_puts("[reflex] reflex layer initialized (thought->action; no central)\r\n");
}

/* ------------------------------------------------------------------ */
/* shell `reflex [on|off|table|stat]`                                  */
/* ------------------------------------------------------------------ */

static void print_action_mask(UB mask)
{
    int any = 0;
    if (mask & REFLEX_ACT_SHIELD)   { rf_puts("SHIELD ");   any = 1; }
    if (mask & REFLEX_ACT_CONSERVE) { rf_puts("CONSERVE "); any = 1; }
    if (mask & REFLEX_ACT_BEACON)   { rf_puts("BEACON ");   any = 1; }
    if (!any) rf_puts("(none) ");
}

static void print_table(void)
{
    rf_puts("[reflex] action table (class -> action):\r\n");
    for (UB c = 0; c < REFLEX_NUM_CLASSES; c++) {
        rf_puts("  class "); rf_putdec(c);
        rf_puts(" ("); rf_puts(class_name(c));
        rf_puts(")");
        rf_puts(c == 1 ? "    -> " : (c == 0 ? "      -> " : " -> "));
        print_action_mask(act_table[c]);
        if (act_table[c] & REFLEX_ACT_SHIELD) {
            rf_puts(" [SHIELD needs >="); rf_putdec(REFLEX_SHIELD_STREAK);
            rf_puts(" consecutive]");
        }
        rf_puts("\r\n");
    }
}

static void print_stat(void)
{
    UW now = now_ms();
    rf_puts("[reflex] state: ");
    rf_puts(enabled ? "ENABLED" : "disabled");
    rf_puts("\r\n");
    rf_puts("  SHIELD   : ");
    if (shield_until && now < shield_until) {
        rf_puts("ACTIVE ("); rf_putdec((shield_until - now) / 1000 + 1);
        rf_puts("s left)");
    } else rf_puts("off");
    rf_puts("\r\n  CONSERVE : ");
    if (conserve_until && now < conserve_until) {
        rf_puts("ACTIVE pressure+"); rf_putdec(reflex_pressure_bias());
        rf_puts(" ("); rf_putdec((conserve_until - now) / 1000 + 1);
        rf_puts("s left)");
    } else rf_puts("off");
    rf_puts("\r\n  danger streak : "); rf_putdec(danger_streak); rf_puts("\r\n");
    rf_puts("  fires="); rf_putdec(st_fires);
    rf_puts(" beacons="); rf_putdec(st_beacons);
    rf_puts(" alarms_rx="); rf_putdec(st_alarms_rx);
    rf_puts(" attenuated="); rf_putdec(st_attenuated);
    rf_puts(" releases="); rf_putdec(st_releases);
    rf_puts(" shield_denies="); rf_putdec(st_shield_deny);
    rf_puts("\r\n");
    /* G18 熟慮 → 学習 → 反射 (§9) の可観測性 */
    rf_puts("  learned_conserve="); rf_putdec(learned_conserve);
    rf_puts(" (init="); rf_putdec(REFLEX_CONSERVE_PRESSURE);
    rf_puts(" range "); rf_putdec(REFLEX_CONSERVE_MIN);
    rf_puts(".."); rf_putdec(REFLEX_CONSERVE_MAX);
    rf_puts(" target_dwell="); rf_putdec(REFLEX_DWELL_TARGET); rf_puts(")\r\n");
    rf_puts("  deliberate="); rf_putdec(st_delib);
    rf_puts(" learn_up="); rf_putdec(st_learn_up);
    rf_puts(" learn_down="); rf_putdec(st_learn_down);
    rf_puts(" cur_threat_run="); rf_putdec(threat_run);
    rf_puts("\r\n");
}

/* ================================================================== */
/* §8/§9 閉ループ性質テスト (philosophy-gap-audit-2 G17/G18)            */
/*                                                                     */
/* 「鎖を環にする」を *数で* 守るカーネル内 self-test。shell `reflex     */
/* test` から呼び、CI が PASS 行を grep する (moe test / pfs / hrw と同  */
/* 方式)。                                                              */
/*                                                                     */
/*  B 負帰還の収束: 行動(CONSERVE)が知覚(pressure)を変え、§7 ゲートが    */
/*    負荷を再分配して脅威を鎮める「一周」を回す。ループ有り(bias>0)では */
/*    外乱(脅威 burst)が *減衰して定常へ収束* し、ループ無し=フィード     */
/*    フォワード(bias=0, 行動が知覚に戻らない)では脅威が長く滞留すること */
/*    を、脅威 dwell / 整定時間 / 残留分散 / 再励起回数 で示す。          */
/*  C 熟慮→学習→反射: 蓄積した dwell 経験から本番 reflex_deliberate() が  */
/*    learned_conserve を学習し、指標(dwell)が時間とともに改善すること、  */
/*    学習 off では改善しないことを示す。                                */
/*                                                                     */
/* §7 ゲートの効用は本番 moe_expert_utility をそのまま使う(重複定義なし)。*/
/* CONSERVE→pressure の注入は本番 world.c compute_pressure と同じ        */
/* `pressure += bias` 加法 (LP_NBR_PRESS は隣の余裕、bias は自ノードの    */
/* 収縮)。すべて純ローカル整数計算・小スタックでベアメタルでも走る。     */
/* ================================================================== */

/* 符号付き10進 (iae/utility が負になり得る箇所用)。 */
static void rf_putsdec(W v)
{
    if (v < 0) { rf_puts("-"); rf_putdec((UW)(-v)); return; }
    rf_putdec((UW)v);
}

/* ── 閉ループ プラント (負帰還の最小モデル) ───────────────────────────
 * 状態 L = 自ノードの過負荷 (脅威を誘発する観測量 = 「知覚」)。
 * 一周:  外乱で L↑ → 脅威 class を知覚 → reflex CONSERVE が pressure に
 *        +bias (行動が知覚を変える) → §7 ゲート (本番 moe_expert_utility)
 *        が「隣の方が空いている」と判断し負荷を offload → L↓ → 脅威が鎮まる。
 * bias=0 はフィードフォワード (行動が知覚へ戻らない = 負帰還が開いている)。
 *
 * 出力: *dwell  = 脅威 (L>=ALERT) だった tick 数
 *       *iae    = Σ|L| (定常からの逸脱の積分; 小さいほど良く制御された)
 *       *settle = 最後に脅威だった tick の次 (整定時間; 小さいほど速い)
 *       *reexc  = 一度 ALERT を下回った後の再励起回数 (発振の指標; 0=収束)
 *       *tailvar= 末尾 1/4 区間の L の分散×4 (定常残留; 0=完全収束) */
#define LP_NBR_PRESS  15    /* 隣ノードの余裕 (FULL degrade base 相当) */
#define LP_ACC        70    /* 両 expert 同一の賢さ (純粋に負荷勾配で競う) */
#define LP_THR_ALERT  22    /* L>=これで脅威 (alert)                      */
#define LP_INFLOW      4    /* burst 中に毎 tick 流入する外乱             */
#define LP_SHED       10    /* offload された tick に減る負荷             */
#define LP_DECAY       1    /* 自然完了による緩やかな減少 (背景)         */
#define LP_HOLD        8    /* CONSERVE のヒステリシス保持 (§8 反射の hold) */
#define LP_BURST      10    /* 外乱 burst の長さ (tick)                   */
#define LP_T          40    /* 観測ホライズン (tick)                     */

static void lp_run(W bias, INT *dwell, W *iae, INT *settle,
                   INT *reexc, W *tailvar)
{
    W   L = 0;
    INT hold = 0;             /* CONSERVE 残り保持 tick (§8 ゆっくり出る) */
    INT dw = 0, last_above = -1, re = 0;
    int prev_above = 0, dropped = 0;
    W   iae_acc = 0;
    /* 末尾 1/4 の平均/分散用 (オンライン)。 */
    INT tail_from = LP_T - LP_T / 4;
    W   ts = 0, tss = 0; INT tn = 0;

    for (INT t = 0; t < LP_T; t++) {
        if (t < LP_BURST) L += LP_INFLOW;             /* 外乱 (知覚を乱す) */

        int above = (L >= LP_THR_ALERT);              /* 脅威 class>=1 を知覚 */
        if (above) {
            dw++; last_above = t;
            if (bias > 0) hold = LP_HOLD;             /* 行動: CONSERVE engage */
            if (prev_above == 0 && dropped) re++;     /* 再励起 (発振) */
        }
        prev_above = above;
        if (!above && last_above >= 0) dropped = 1;   /* 一度は鎮まった */

        /* 知覚 = pressure。行動(CONSERVE)が eff(>0)の間だけ +bias で *知覚を
         * 変える* = 負帰還を閉じる。bias=0 ならこの経路が無い=フィードフォワード。 */
        int conserve = (hold > 0);
        W   p_self   = L + (conserve ? bias : 0);
        if (hold > 0) hold--;

        /* §7 ゲート: 本番 moe_expert_utility で「自分 vs 隣」を比較。
         * 隣が MOE_SWITCH_MARGIN を超えて勝れば offload (負荷を隣へ流す)。 */
        W u_self = moe_expert_utility(LP_ACC, 0, (INT)p_self, 0);
        W u_nbr  = moe_expert_utility(LP_ACC, 0, LP_NBR_PRESS, 0);
        if (u_nbr > u_self + MOE_SWITCH_MARGIN) L -= LP_SHED;   /* 行動の結果 */

        L -= LP_DECAY;
        if (L < 0) L = 0;

        iae_acc += L;
        if (t >= tail_from) { ts += L; tss += L * L; tn++; }
    }

    *dwell  = dw;
    *iae    = iae_acc;
    *settle = last_above + 1;          /* 最後の脅威 tick の次 (0=即整定) */
    *reexc  = re;
    if (tn > 0) {
        W mean = ts / tn;
        *tailvar = (tss / tn) - mean * mean;     /* E[L^2]-E[L]^2 */
        if (*tailvar < 0) *tailvar = 0;
    } else *tailvar = 0;
}

/* ── B: 負帰還が *減衰して収束* する vs フィードフォワードでは滞留 ───── */
static INT rf_test_feedback(void)
{
    INT fails = 0;
    INT d_on, d_off, s_on, s_off, r_on, r_off;
    W   i_on, i_off, v_on, v_off;

    lp_run(REFLEX_CONSERVE_PRESSURE, &d_on,  &i_on,  &s_on,  &r_on,  &v_on);  /* loop closed */
    lp_run(0,                        &d_off, &i_off, &s_off, &r_off, &v_off); /* feedforward */

    rf_puts("[reflex-fb] disturbance burst, identical except action->perception path:\r\n");
    rf_puts("[reflex-fb]  loop ON  (CONSERVE feeds pressure): dwell=");
    rf_putdec((UW)d_on); rf_puts(" settle="); rf_putdec((UW)s_on);
    rf_puts(" iae="); rf_putsdec(i_on); rf_puts(" reexc="); rf_putdec((UW)r_on);
    rf_puts(" tailvar="); rf_putsdec(v_on); rf_puts("\r\n");
    rf_puts("[reflex-fb]  loop OFF (feedforward, no fb):       dwell=");
    rf_putdec((UW)d_off); rf_puts(" settle="); rf_putdec((UW)s_off);
    rf_puts(" iae="); rf_putsdec(i_off); rf_puts(" reexc="); rf_putdec((UW)r_off);
    rf_puts(" tailvar="); rf_putsdec(v_off); rf_puts("\r\n");

    /* (1) 負帰還が脅威の滞留を確実に縮める (行動が知覚を変えている)。 */
    if (!(d_off >= 2 * d_on && d_off - d_on >= 4)) {
        rf_puts("[reflex-fb] FAIL feedback did not shorten threat dwell\r\n");
        fails++;
    }
    /* (2) 逸脱の積分 (IAE) も負帰還で大きく減る。 */
    if (!(i_off > i_on + i_on / 2)) {
        rf_puts("[reflex-fb] FAIL feedback did not reduce excursion (IAE)\r\n");
        fails++;
    }
    /* (3) 整定: ループ ON は窓内で整定し、OFF より速い。 */
    if (!(s_on > 0 && s_on < LP_T && s_off > s_on)) {
        rf_puts("[reflex-fb] FAIL loop-on did not settle faster than feedforward\r\n");
        fails++;
    }
    /* (4) 収束 = 発振しない: ループ ON は再励起 0 かつ定常残留 ~0。 */
    if (!(r_on == 0 && v_on <= 2)) {
        rf_puts("[reflex-fb] FAIL loop-on oscillated / left steady-state residual\r\n");
        fails++;
    }

    if (fails == 0)
        rf_puts("[reflex-fb] PASS (closed loop: action->perception->gate damps"
                " disturbance to steady state; feedforward does not)\r\n");
    else
        rf_puts("[reflex-fb] FAIL\r\n");
    return fails;
}

/* ── C: 熟慮層が経験(dwell)から learned_conserve を学習 → 指標が改善 ──── */
static INT rf_test_learning(void)
{
    INT fails = 0;
    const INT E = 6;

    /* 本番の学習状態を退避 (live カーネルを汚さない)。 */
    UB save_lc   = learned_conserve;
    UW save_ds   = win_dwell_sum, save_ep = win_episodes;
    UW save_dl   = st_delib, save_lu = st_learn_up, save_ld = st_learn_down;

    /* 故意に弱い初期効き (offload しきい値未満) から始める。 */
    const UB WEAK = REFLEX_CONSERVE_MIN;     /* =8: p_self=L+8 では殺到を捌けない */

    INT d_first_learn = 0, d_last_learn = 0;
    INT d_first_froz  = 0, d_last_froz  = 0;

    /* (i) 学習 ON: 各エピソード後に本番 reflex_deliberate() で nudge。 */
    rf_puts("[reflex-learn] learning ON  (deliberation nudges learned_conserve):\r\n");
    learned_conserve = WEAK;
    for (INT e = 0; e < E; e++) {
        INT dw, st, rx; W ia, tv;
        lp_run((W)learned_conserve, &dw, &ia, &st, &rx, &tv);
        rf_puts("[reflex-learn]  ep"); rf_putdec((UW)e);
        rf_puts(" bias="); rf_putdec(learned_conserve);
        rf_puts(" dwell="); rf_putdec((UW)dw); rf_puts("\r\n");
        if (e == 0) d_first_learn = dw;
        if (e == E - 1) d_last_learn = dw;
        /* 観測した経験を窓へ積み、本番の熟慮ロジックで学習する。 */
        win_dwell_sum = (UW)dw; win_episodes = 1;
        reflex_deliberate();
    }

    /* (ii) 学習 OFF: 効きを弱いまま凍結 (熟慮を呼ばない)。 */
    rf_puts("[reflex-learn] learning OFF (frozen weak conserve):\r\n");
    UB frozen = WEAK;
    for (INT e = 0; e < E; e++) {
        INT dw, st, rx; W ia, tv;
        lp_run((W)frozen, &dw, &ia, &st, &rx, &tv);
        rf_puts("[reflex-learn]  ep"); rf_putdec((UW)e);
        rf_puts(" bias="); rf_putdec(frozen);
        rf_puts(" dwell="); rf_putdec((UW)dw); rf_puts("\r\n");
        if (e == 0) d_first_froz = dw;
        if (e == E - 1) d_last_froz = dw;
    }

    rf_puts("[reflex-learn] learn: dwell "); rf_putdec((UW)d_first_learn);
    rf_puts("->"); rf_putdec((UW)d_last_learn);
    rf_puts("  frozen: dwell "); rf_putdec((UW)d_first_froz);
    rf_puts("->"); rf_putdec((UW)d_last_froz);
    rf_puts("  final learned_conserve="); rf_putdec(learned_conserve);
    rf_puts("\r\n");

    /* (1) 学習で指標 (dwell) が時間とともに改善する。 */
    if (!(d_last_learn < d_first_learn && d_last_learn * 2 <= d_first_learn)) {
        rf_puts("[reflex-learn] FAIL deliberation did not improve dwell over time\r\n");
        fails++;
    }
    /* (2) 学習 OFF では改善しない (経験が反射を書き換えていないことの対照)。 */
    if (!(d_last_froz >= d_first_froz - 1)) {
        rf_puts("[reflex-learn] FAIL frozen case unexpectedly improved\r\n");
        fails++;
    }
    /* (3) 学習後は凍結より良い (自動適応が効いている)。 */
    if (!(d_last_learn < d_last_froz)) {
        rf_puts("[reflex-learn] FAIL learned policy not better than frozen\r\n");
        fails++;
    }
    /* (4) 学習した効きが範囲内で前進している (本物の重み更新)。 */
    if (!(learned_conserve > WEAK && learned_conserve <= REFLEX_CONSERVE_MAX)) {
        rf_puts("[reflex-learn] FAIL learned_conserve did not advance\r\n");
        fails++;
    }

    /* 本番状態を復元 (self-test は副作用を残さない)。 */
    learned_conserve = save_lc;
    win_dwell_sum = save_ds; win_episodes = save_ep;
    st_delib = save_dl; st_learn_up = save_lu; st_learn_down = save_ld;

    if (fails == 0)
        rf_puts("[reflex-learn] PASS (deliberation learns from experience;"
                " reflex behaviour improves over time; frozen does not)\r\n");
    else
        rf_puts("[reflex-learn] FAIL\r\n");
    return fails;
}

INT reflex_self_test(void)
{
    INT fails = 0;
    rf_puts("[reflex-test] ==== §8/§9 closed-loop tests (G17 wiring / G18) ====\r\n");
    fails += rf_test_feedback();
    fails += rf_test_learning();
    if (fails == 0) rf_puts("[reflex-test] ALL PASS\r\n");
    else { rf_puts("[reflex-test] FAILURES="); rf_putdec((UW)fails);
           rf_puts("\r\n"); }
    return fails;
}

void reflex_cmd(const UB *args, UW len)
{
    const UB *p   = args;
    const UB *end = args + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    UW rem = (UW)(end - p);
    if (rem >= 2 && p[0] == 'o' && p[1] == 'n') {
        enabled = 1;
        rf_puts("[reflex] enabled\r\n");
        return;
    }
    if (rem >= 3 && p[0] == 'o' && p[1] == 'f' && p[2] == 'f') {
        enabled = 0;
        shield_until = conserve_until = 0;
        rf_puts("[reflex] disabled (all actions cleared)\r\n");
        return;
    }
    if (rem >= 5 && p[0] == 't' && p[1] == 'a' && p[2] == 'b') {
        print_table();
        return;
    }
    if (rem >= 4 && p[0] == 't' && p[1] == 'e' && p[2] == 's' && p[3] == 't') {
        reflex_self_test();
        return;
    }
    /* default / "stat" */
    print_stat();
    print_table();
}
