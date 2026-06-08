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
 *      CONSERVE = reflex_threat_level() を立てる (world ビーコンの *脅威軸* へ;
 *                 moe ゲートで加点 = 群れが当ノードへ寄る rally; G20 修正)
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

/* SHIELD の「保持期限」(operating time ms)。0 = 非エンゲージ。
 * now < until のあいだ遮蔽が効く = 重い行動のヒステリシス。 */
static UW shield_until   = 0;

/* ── G33: CONSERVE 脅威レベルは「観測された危険量」で駆動する (タイマではない) ─
 * danger_active : いま危険が観測されているか (発火した推論 / 受信アラームで立ち、
 *                 SAFE (normal) 観測で *即座に* 落ちる = 制御量が安全へ戻った)。
 * last_danger_ms: 最後に危険を観測した時刻。SAFETY CAP (スタック・ラッチ保険)
 *                 だけが読む。正常な解除には一切使わない。 */
static UB danger_active   = 0;
static UW last_danger_ms  = 0;

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
 * learned_conserve : 反射の CONSERVE 効きの強さ (= 脅威軸へ載せる強度)。
 *                    reflex_threat_level() が固定値ではなくこの学習値を返す。
 *                    熟慮層が dwell 経験から nudge する (REFLEX_CONSERVE_MIN..MAX):
 *                    脅威が長く滞留した = 応援が足りない → 効きを上げて rally を
 *                    強める。これは flee を速める方向ではなく rally を強める方向。
 * threat_run       : いま連続して観測している脅威の長さ (= 進行中の dwell)。
 * win_dwell_sum/   : 熟慮の窓で観測した「脅威エピソードの dwell」の合計と本数。
 *   win_episodes     1 エピソード = 脅威が立ち上がってから normal に戻るまで。 */
static UB learned_conserve = REFLEX_CONSERVE_PRESSURE;
static UW threat_run       = 0;
static UW win_dwell_sum    = 0;
static UW win_episodes     = 0;

/* ── G38 第二アロー: 守る → 学習 への観測供給 ─────────────────────────────
 * guard_class_exp[c] = 反射が「危険」として実際に発火したクラス c の経験回数。
 * 「近傍が今 守った経験」(どのクラスが脅威だったか) を、協調学習 (gossip_learn)
 * が *優先度信号* として読む (reflex_threat_experience)。これが §8 の二本目の
 * 配線: 守った経験が全体の未来の学習を形作る。反射 tick では使わない (供給のみ)。 */
static UW guard_class_exp[REFLEX_NUM_CLASSES];

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
    UW now = now_ms();
    rf_puts("[reflex] FIRE ");
    rf_puts(why);
    rf_puts(" ->");
    int any = 0;
    if (mask & REFLEX_ACT_SHIELD)   { shield_until = now + REFLEX_HOLD_MS; rf_puts(" SHIELD"); any = 1; }
    /* G33: CONSERVE は脅威レベルを *観測された危険* に縛る。タイマ期限は
     * 持たせない: danger_active を立て、観測時刻を記録するだけ。レベルは危険が
     * SAFE へ戻った瞬間に落ちる (タイマ満了ではない)。 */
    if (mask & REFLEX_ACT_CONSERVE) { danger_active = 1; last_danger_ms = now; rf_puts(" CONSERVE"); any = 1; }
    if (mask & REFLEX_ACT_BEACON)   {                                          rf_puts(" BEACON");   any = 1; }
    if (!any) rf_puts(" (none)");
    rf_puts(" (threat tracks observed danger; SHIELD hold ");
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
        /* G33: SAFE (normal) 観測 = 危険信号が安全へ戻った → CONSERVE 脅威
         * レベルを *即座に* 落とす (HOLD タイマの満了を待たない)。これが
         * 「制御量が下がったから落ちる」= タイマ解除との決定的な違い。 */
        if (danger_active) {
            danger_active = 0;
            st_releases++;
            rf_puts("[reflex] CONSERVE released (danger signal returned to SAFE;"
                    " driven by the observed quantity, NOT a timer)\r\n");
        }
    }

    /* G38: アクション表ゲート + 確信度フロアを 1 述語に集約 (reflex_would_fire)。
     * 学習モデルが低確信 (未学習/曖昧) なら反射は発火しない = 思考が守りを変える。
     * かつて moe 経路は confidence=0xFF 固定で「常に発火」だった (G34)。いまは
     * moe_infer が学習モデルの実 max-softmax を渡すので、ここが本当のゲートになる。 */
    if (!reflex_would_fire(threat_class, confidence)) return;
    UB mask = act_table[threat_class];

    /* G38 第二アロー: 「いまこのクラスを守った」経験を蓄積する。協調学習が
     * 優先度として読む (reflex_threat_experience)。確信度ゲートを通った =
     * モデルが本当に危険と判断した発火だけを数える。 */
    if (threat_class >= 1 && threat_class < REFLEX_NUM_CLASSES)
        guard_class_exp[threat_class]++;

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
        /* G33: heard danger sets the observed-danger latch (not a timer); it
         * falls again when this node next observes SAFE or the cap guards it. */
        danger_active  = 1;
        last_danger_ms = now_ms();

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
    /* G33: the threat level's NORMAL release is a SAFE observation (handled in
     * reflex_on_inference). The only time-based release here is the SAFETY CAP:
     * danger was observed but NO further observation (danger or safe) has arrived
     * for REFLEX_THREAT_CAP_MS — the inference stream went silent and the latch
     * would otherwise stick. This is a guard, NOT the loop ([g33-controlled]
     * proves the normal release is the quantity, not this cap). */
    if (danger_active && (now - last_danger_ms) >= REFLEX_THREAT_CAP_MS) {
        danger_active = 0;
        st_releases++;
        rf_puts("[reflex] CONSERVE released (SAFETY CAP ");
        rf_putdec(REFLEX_THREAT_CAP_MS / 1000);
        rf_puts("s: observation stream went silent — guard, not normal release)\r\n");
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

/* G33: pure release formula shared by the live accessor and [g33-controlled]
 * (no duplicate; mirrors protect_threat_for). The level is a function of the
 * OBSERVED DANGER QUANTITY, not a wall clock — see reflex.h. */
UB reflex_threat_for(BOOL danger, UW ms_since_danger, UB level)
{
    if (!danger) return 0;                                   /* SAFE -> 0 NOW */
    if (ms_since_danger >= REFLEX_THREAT_CAP_MS) return 0;   /* safety cap only */
    return level;
}

UB reflex_threat_level(void)
{
    if (!enabled) return 0;
    /* The strength returned is the deliberation-learned gain (§9 G18); WHETHER
     * it is returned at all is the controlled quantity (danger_active), released
     * promptly by a SAFE observation, not by a HOLD timer (G33). The cap only
     * guards a stuck latch when observations stop entirely. */
    return reflex_threat_for((BOOL)danger_active,
                             now_ms() - last_danger_ms,
                             learned_conserve);
}

UB reflex_learned_conserve(void) { return learned_conserve; }

/* ── G38 主アロー: 反射の発火ゲートの純述語 (状態なし) ───────────────────
 * reflex_on_inference の判定そのもの (アクション表 + 確信度フロア) を 1 箇所に。
 * self-test (gossip_learn の [g38-*]) が本番と同じゲートを叩けるよう公開する。 */
BOOL reflex_would_fire(UB threat_class, UB confidence)
{
    if (threat_class >= REFLEX_NUM_CLASSES) return FALSE;
    if (act_table[threat_class] == REFLEX_ACT_NONE) return FALSE;   /* normal は黙る */
    if (confidence != 0xFF && confidence < REFLEX_CONF_MIN) return FALSE; /* 低確信 */
    return TRUE;
}

/* ── G38 第二アロー: 守った経験 (クラス別発火回数) を学習へ供給する窓口 ──── */
UW reflex_threat_experience(UB cls)
{
    if (cls >= REFLEX_NUM_CLASSES) return 0;
    return guard_class_exp[cls];
}

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

/* テスト隔離用 (wave 18): reflex の有効/無効を切り替え、旧値を返す。
 * moe.c の [onebrain-*] が live な moe_infer を回す際、reflex の副作用
 * (guard_class_exp 等; g38-guard-feeds が読む経験) を汚さないよう一時的に
 * 無効化してから復元するために使う。moe_infer は無効でも learned_class を
 * reflex_on_inference へ *渡す* (= ob 観測値) ので、ONE BRAIN の等値検証は
 * 損なわれない。 */
BOOL reflex_set_enabled(BOOL on)
{
    BOOL prev = (BOOL)enabled;
    enabled = on ? 1 : 0;
    return prev;
}

void reflex_init(void)
{
    enabled        = 1;
    shield_until   = 0;
    danger_active  = 0;
    last_danger_ms = 0;
    danger_streak  = 0;
    my_alarm_seq   = 0;
    h_pub          = -1;
    /* G18 学習状態 */
    learned_conserve = REFLEX_CONSERVE_PRESSURE;
    threat_run     = 0;
    win_dwell_sum  = 0;
    win_episodes   = 0;
    for (INT c = 0; c < REFLEX_NUM_CLASSES; c++) guard_class_exp[c] = 0;
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
    if (reflex_threat_level() > 0) {
        rf_puts("ACTIVE threat+"); rf_putdec(reflex_threat_level());
        rf_puts(" (rally; driven by observed danger, releases on SAFE not a timer)");
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
    /* G38 第二アロー: 守った経験 (クラス別) — 協調学習が優先度として読む。 */
    rf_puts("  guard_exp (class->fires for learning): ");
    for (UB c = 0; c < REFLEX_NUM_CLASSES; c++) {
        rf_puts("cls"); rf_putdec(c); rf_puts("=");
        rf_putdec(guard_class_exp[c]); rf_puts(" ");
    }
    rf_puts("\r\n");
}

/* ================================================================== */
/* §8/§9 閉ループ性質テスト (philosophy-gap-audit-2 G17/G18)            */
/*                                                                     */
/* 「鎖を環にする」を *数で* 守るカーネル内 self-test。shell `reflex     */
/* test` から呼び、CI が PASS 行を grep する (moe test / pfs / hrw と同  */
/* 方式)。                                                              */
/*                                                                     */
/*  B 負帰還の収束 (rally): 行動(CONSERVE)が脅威軸(threat)を立て、§7 ゲート */
/*    が群れを脅威ノードへ集束させ(rally aid)脅威を鎮める「一周」を回す。  */
/*    ループ有り(gain>0)では外乱(脅威 burst)が *減衰して定常へ収束* し、    */
/*    ループ無し=フィードフォワード(gain=0, 脅威信号を出さない=応援を呼ば   */
/*    ない)では脅威が長く滞留することを、脅威 dwell / 整定時間 / 残留分散 /  */
/*    再励起回数 で示す。【G20】鎮静は P が仕事を手放す(flee)のではなく、    */
/*    近傍 aid が外から backlog を削る(rally)結果である(lp_run 参照)。       */
/*  C 熟慮→学習→反射: 蓄積した dwell 経験から本番 reflex_deliberate() が  */
/*    learned_conserve を学習し、指標(dwell)が時間とともに改善すること、  */
/*    学習 off では改善しないことを示す。                                */
/*                                                                     */
/* §7 ゲートの効用は本番 moe_expert_utility をそのまま使う(重複定義なし)。*/
/* threat 軸の注入は本番 world.c (WORLD_BEACON.threat = reflex_threat_level)*/
/* と同じ経路 (gain は自ノードの脅威強度、LP_HOME は近傍が留まる対抗負荷)。 */
/* すべて純ローカル整数計算・小スタックでベアメタルでも走る。            */
/* ================================================================== */

/* 符号付き10進 (iae/utility が負になり得る箇所用)。 */
static void rf_putsdec(W v)
{
    if (v < 0) { rf_puts("-"); rf_putdec((UW)(-v)); return; }
    rf_putdec((UW)v);
}

/* ── 閉ループ プラント (負帰還の最小モデル) — G20 後は *rally* ループ ──────
 * 状態 L = 脅威ノード P が抱える「守るべき仕事 (backlog)」= 制御量。
 * 一周:  外乱で L↑ → 脅威 class を知覚 → reflex CONSERVE が *脅威軸* (threat)
 *        を立てる (行動) → §7 ゲート (本番 moe_expert_utility) で P の utility
 *        が threat ぶん *加点* される → 近傍が「あそこへ寄ろう」と判断し
 *        rally → 近傍の aid が P の backlog を *外から* 削る → L↓ → 脅威鎮静。
 *
 *  ── G20 修正の核心 (符号の向き) ─────────────────────────────────────
 *    旧プラントは CONSERVE→自 pressure↑→「隣の方が空く」→`L -= SHED`
 *    だった = P が *自分の仕事を手放して* dwell を縮める = flee (§2 の真逆)。
 *    新プラントは threat→P の utility↑→「あそこへ寄ろう」→近傍 aid が入って
 *    `L -= rally` = P は守るべき仕事を *保持したまま* 外から助けられる = rally。
 *    符号が逆 (脅威を引き算) だと u_protected が下がり近傍は寄らず、aid が
 *    入らず dwell が縮まない → loop ON/OFF が区別できず下の assert が落ちる
 *    = テストが本当に「寄る符号」を検査している。
 *
 * gain=0 はフィードフォワード (脅威信号を出さない = 応援を呼ばない = 環が開く)。
 *
 * 出力: *dwell  = 脅威 (L>=ALERT) だった tick 数
 *       *iae    = Σ|L| (定常からの逸脱の積分; 小さいほど良く制御された)
 *       *settle = 最後に脅威だった tick の次 (整定時間; 小さいほど速い)
 *       *reexc  = 一度 ALERT を下回った後の再励起回数 (発振の指標; 0=収束)
 *       *tailvar= 末尾 1/4 区間の L の分散×4 (定常残留; 0=完全収束) */
#define LP_HOME       30    /* 近傍が「手元に留まる」ときの自己負荷 (rally の対抗) */
#define LP_ACC        70    /* 両候補同一の賢さ (純粋に符号で競う)        */
#define LP_THR_ALERT  22    /* L>=これで脅威 (alert)                      */
#define LP_INFLOW      4    /* burst 中に毎 tick 流入する外乱             */
#define LP_AID_MAX     3    /* 1 tick に近傍 aid が削れる backlog の上限   */
#define LP_DECAY       1    /* 自然完了による緩やかな減少 (背景)         */
#define LP_HOLD        8    /* CONSERVE のヒステリシス保持 (§8 反射の hold) */
#define LP_BURST      10    /* 外乱 burst の長さ (tick)                   */
#define LP_T          40    /* 観測ホライズン (tick)                     */

static void lp_run(W gain, INT *dwell, W *iae, INT *settle,
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
        if (t < LP_BURST) L += LP_INFLOW;             /* 外乱 (backlog を積む) */

        int above = (L >= LP_THR_ALERT);              /* 脅威 class>=1 を知覚 */
        if (above) {
            dw++; last_above = t;
            if (gain > 0) hold = LP_HOLD;             /* 行動: CONSERVE engage */
            if (prev_above == 0 && dropped) re++;     /* 再励起 (発振) */
        }
        prev_above = above;
        if (!above && last_above >= 0) dropped = 1;   /* 一度は鎮まった */

        /* 行動 = 脅威軸を立てる。CONSERVE が engage している間だけ threat=gain。
         * gain=0 はフィードフォワード (脅威信号が出ない = 応援を呼ばない)。 */
        int conserve = (hold > 0);
        W   thr      = conserve ? gain : 0;
        if (hold > 0) hold--;

        /* §7 ゲート (本番 moe_expert_utility): 「脅威ノード P (threat 加点)」と
         * 「手元に留まる近傍 (LP_HOME)」を比較。threat が強いほど P が勝ち、
         * 近傍が rally → その差ぶん (上限 LP_AID_MAX) の aid が P の backlog を
         * 外から削る。P は仕事を手放さない (flee ではなく rally)。 */
        W u_protected = moe_expert_utility(LP_ACC, 0, (INT)L,   (INT)thr, 0);
        W u_home      = moe_expert_utility(LP_ACC, 0, LP_HOME,  0,        0);
        if (thr > 0) {
            W rally = u_protected - (u_home + MOE_SWITCH_MARGIN);  /* 寄る余地 */
            if (rally > 0) {
                if (rally > LP_AID_MAX) rally = LP_AID_MAX;
                L -= rally;                  /* 近傍 aid が backlog を外から削る */
            }
        }

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
    rf_puts("[reflex-fb]  loop ON  (CONSERVE->threat->rally aid):  dwell=");
    rf_putdec((UW)d_on); rf_puts(" settle="); rf_putdec((UW)s_on);
    rf_puts(" iae="); rf_putsdec(i_on); rf_puts(" reexc="); rf_putdec((UW)r_on);
    rf_puts(" tailvar="); rf_putsdec(v_on); rf_puts("\r\n");
    rf_puts("[reflex-fb]  loop OFF (feedforward, no rally):     dwell=");
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

/* ── G33: 脅威レベルは「観測された危険量」で上下する (タイマ解除ではない) ──────
 * gap-ledger G33: 旧実装は CONSERVE 脅威レベルを 5s 壁時計タイマ (conserve_until
 * = now + HOLD) で落としていた。レベルは「タイマが切れたから」下がるのであって
 * 「危険が去ったから」ではなかった。この self-test は両方向 + persist-vs-clear の
 * 決定的区別を *数で* 示す (信念ではなく印字):
 *   (A) 危険 HIGH               -> レベルが立つ。
 *   (B) 危険 CLEAR (時計を進めず) -> レベルが *即座に* 0 (= 量で落ちる)。
 *   (C) 危険 PERSIST + 時計を HOLD 超へ進める -> レベルは *落ちない*
 *       (旧タイマ解除なら誤って 0 にしていた = 決定的区別)。
 *   (D) SAFETY CAP (沈黙が CAP 超) でだけ時間で落ちる (= 正常経路ではない保険)。
 * 純粋式 reflex_threat_for() を本番アクセサと共有して検査し、さらに本番の観測
 * フック (reflex_on_inference) でラッチを駆動して live アクセサでも両方向を示す。
 * すべて純ローカル整数計算で、live 部は触れた静的状態を退避/復元する。 */
static INT rf_test_controlled(void)
{
    INT fails = 0;
    const UB LV = REFLEX_CONSERVE_PRESSURE;     /* representative learned level */

    rf_puts("[g33-controlled] threat level is driven by the OBSERVED DANGER"
            " quantity, not a wall-clock timer:\r\n");

    /* (A) danger observed HIGH (clock at 0) -> level rises to LV. */
    UB t_high     = reflex_threat_for(TRUE,  0, LV);
    /* (B) danger CLEARS, clock NOT advanced -> level drops PROMPTLY (no timer). */
    UB t_clear    = reflex_threat_for(FALSE, 0, LV);
    /* (C) danger PERSISTS, clock advanced PAST HOLD -> level must NOT drop
     *     (the old HOLD timer-release would wrongly give 0 here). */
    UB t_persist  = reflex_threat_for(TRUE,  REFLEX_HOLD_MS + 1000, LV);
    /* (D) SAFETY CAP: a SILENT latch only clears once past the (much larger)
     *     cap — this is the guard, never the normal release. */
    UB t_cap      = reflex_threat_for(TRUE,  REFLEX_THREAT_CAP_MS, LV);

    rf_puts("[g33-controlled]  (A) danger HIGH (since=0):              level=");
    rf_putdec(t_high); rf_puts("\r\n");
    rf_puts("[g33-controlled]  (B) danger CLEARED (since=0):           level=");
    rf_putdec(t_clear); rf_puts("   (prompt: dropped by the SAFE signal, no timer)\r\n");
    rf_puts("[g33-controlled]  (C) danger PERSISTS, clock +");
    rf_putdec((REFLEX_HOLD_MS + 1000) / 1000);
    rf_puts("s (>HOLD): level=");
    rf_putdec(t_persist); rf_puts("   (HOLD expiring did NOT drop it)\r\n");
    rf_puts("[g33-controlled]  (D) SILENT latch past SAFETY CAP ");
    rf_putdec(REFLEX_THREAT_CAP_MS / 1000);
    rf_puts("s: level="); rf_putdec(t_cap);
    rf_puts("   (stuck-latch guard only)\r\n");

    /* ── live path: real observations drive the real latch + accessor ──────── */
    BOOL sv_en = (BOOL)enabled;
    UB   sv_da = danger_active;  UW sv_ld = last_danger_ms;
    UB   sv_ds = danger_streak;  UW sv_tr = threat_run;
    UW   sv_wd = win_dwell_sum,  sv_we = win_episodes;
    UW   sv_sf = st_fires, sv_sb = st_beacons, sv_sr = st_releases;
    UW   sv_ge[REFLEX_NUM_CLASSES];
    for (INT c = 0; c < REFLEX_NUM_CLASSES; c++) sv_ge[c] = guard_class_exp[c];

    enabled = 1; danger_active = 0; danger_streak = 0; last_danger_ms = now_ms();
    reflex_on_inference(2, 100, drpc_my_node);   /* observe CRITICAL danger    */
    UB live_hi   = reflex_threat_level();
    reflex_on_inference(0, 100, drpc_my_node);   /* observe SAFE (normal)      */
    UB live_safe = reflex_threat_level();        /* clock NOT advanced         */

    rf_puts("[g33-controlled]  live: observe danger -> level=");
    rf_putdec(live_hi); rf_puts("; observe SAFE -> level=");
    rf_putdec(live_safe); rf_puts("   (live accessor dropped on SAFE, clock unchanged)\r\n");

    /* restore live state (self-test leaves no side effects) */
    enabled = sv_en ? 1 : 0;
    danger_active = sv_da; last_danger_ms = sv_ld;
    danger_streak = sv_ds; threat_run = sv_tr;
    win_dwell_sum = sv_wd; win_episodes = sv_we;
    st_fires = sv_sf; st_beacons = sv_sb; st_releases = sv_sr;
    for (INT c = 0; c < REFLEX_NUM_CLASSES; c++) guard_class_exp[c] = sv_ge[c];

    /* ── verdicts ──────────────────────────────────────────────────────────── */
    if (!(t_high == LV)) {
        rf_puts("[g33-controlled] FAIL danger HIGH did not raise the level\r\n"); fails++; }
    if (!(t_clear == 0)) {
        rf_puts("[g33-controlled] FAIL danger CLEAR did not drop the level promptly\r\n"); fails++; }
    if (!(t_persist == LV)) {
        rf_puts("[g33-controlled] FAIL persisting danger dropped when clock passed HOLD"
                " (still a timer!)\r\n"); fails++; }
    /* the DISTINGUISHING property: persist-past-HOLD == danger-high (no decay),
     * and that differs from what a HOLD timer would yield (0). */
    if (!(t_persist == t_high && t_clear != t_high)) {
        rf_puts("[g33-controlled] FAIL level not controlled by the quantity"
                " (persist != high, or clear == high)\r\n"); fails++; }
    if (!(t_cap == 0)) {
        rf_puts("[g33-controlled] FAIL safety cap did not eventually guard a silent latch\r\n"); fails++; }
    if (!(live_hi > 0 && live_safe == 0)) {
        rf_puts("[g33-controlled] FAIL live accessor not driven by observed danger\r\n"); fails++; }

    if (fails == 0)
        rf_puts("[g33-controlled] PASS (threat level rises with observed danger and"
                " falls PROMPTLY on the SAFE signal; persisting danger survives the"
                " clock passing HOLD; the timer is only a safety cap)\r\n");
    else
        rf_puts("[g33-controlled] FAIL\r\n");
    return fails;
}

INT reflex_self_test(void)
{
    INT fails = 0;
    rf_puts("[reflex-test] ==== §8/§9 closed-loop tests (G17 wiring / G18 / G33) ====\r\n");
    fails += rf_test_feedback();
    fails += rf_test_learning();
    fails += rf_test_controlled();
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
        shield_until = 0;
        danger_active = 0;
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
