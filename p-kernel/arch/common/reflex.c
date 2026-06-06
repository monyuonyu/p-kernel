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
    return REFLEX_CONSERVE_PRESSURE;
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

    for (;;) {
        tk_dly_tsk(REFLEX_POLL_MS);
        if (drpc_my_node == 0xFF) { check_release(); continue; }

        for (UB n = 0; n < DNODE_MAX; n++) {
            if (h_sub[n] < 0) continue;
            REFLEX_ALARM a;
            W r = kdds_sub(h_sub[n], &a, (W)sizeof(a), 0);
            if (r >= (W)sizeof(REFLEX_ALARM) && a.src_node == n)
                on_alarm(&a);
        }

        check_release();
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
    /* default / "stat" */
    print_stat();
    print_table();
}
