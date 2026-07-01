/*
 *  world.c
 *  Decentralized whole-network situational-awareness map.
 *
 *  設計: docs/architecture/00-concept/survival-network.md / docs/architecture/20-architecture/regions.md
 *  詳細とNO-CENTRAL不変条件は world.h を参照。
 *
 *  ── NO-CENTRAL INVARIANT (再掲・実装の責任範囲) ─────────────────────
 *    このファイルには集約専用ノードの概念が一切無い。world_task は全ノードで
 *    完全に対称に走り、各ノードは「自分が受け取ったビーコンだけ」から world-table
 *    を作る。どのノードを落としても、残る各ノードのローカル world-table は
 *    生き続ける。地図は分散して冗長に存在する (§3 一点突破で殺せない構造)。
 *  ────────────────────────────────────────────────────────────────────
 */

#include "world.h"
#include "drpc.h"
#include "kdds.h"
#include "swim.h"
#include "region.h"
#include "degrade.h"
#include "moe.h"      /* MOE_NUM_CLASSES */
#include "reflex.h"   /* CONSERVE: reflex_threat_level() を脅威軸ビーコンへ (G20) */
#include "protect.h"  /* G28: protect_threat_level() — under-replication で接地 */
#include "kernel.h"
#ifdef _TK_HOSTED_LIBC_
#include "interocept.h"  /* survival-loop L0: the S_n bus drives the STATE FSM   */
#include "galaxy.h"      /* survival-loop L0: EV_STATE observation hook          */
#endif

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void wo_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void wo_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { wo_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    wo_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* device_type — このビルドの arch+role を判定                         */
/* ------------------------------------------------------------------ */

static UB my_device_type(void)
{
#if defined(__ANDROID__)
    return WORLD_DEV_ANDROID_UMP;
#elif defined(_APP_LINUX_) && defined(_APP_X86_64_)
    return WORLD_DEV_LINUX_X86_64;
#elif defined(_APP_LINUX_) && defined(_APP_AARCH64_)
    return WORLD_DEV_LINUX_AARCH64;
#elif defined(_APP_X86_)
    return WORLD_DEV_X86_BARE;
#elif defined(_APP_AARCH64_)
    return WORLD_DEV_AARCH64_BARE;
#else
    return WORLD_DEV_UNKNOWN;
#endif
}

static const char *device_type_name(UB dt)
{
    switch (dt) {
    case WORLD_DEV_X86_BARE:      return "X86_BARE";
    case WORLD_DEV_AARCH64_BARE:  return "AARCH64_BARE";
    case WORLD_DEV_LINUX_X86_64:  return "LINUX_X86_64";
    case WORLD_DEV_LINUX_AARCH64: return "LINUX_AARCH64";
    case WORLD_DEV_ANDROID_UMP:   return "ANDROID_UMP";
    default:                      return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* world-table — 各ノードの最新ビーコンと観測時刻                      */
/* (このノードのローカルな世界像。中央コレクタではない。)              */
/* ------------------------------------------------------------------ */

typedef struct {
    WORLD_BEACON beacon;     /* 最後に受信した内容                       */
    UW           last_ms;    /* tk_get_otm().lo で記録した受信時刻 (ms)  */
    UB           valid;      /* 1 = 一度でも受信した                     */
} WORLD_ENTRY;

static WORLD_ENTRY table[DNODE_MAX];

/* 自ノードの firing ビットマスク (MoE 発火が立て、ビーコンで配る) */
static UB my_firing = 0;
static UW my_seq    = 0;

/* テスト専用フック (G12 デモ): 起動後 beacon_hold_ms の間だけ self-beacon の
 * 発信を抑止し、gossip を意図的に未収束のまま保つ。0 = 抑止なし (既定; 本番は
 * 常に 0 なので挙動は不変)。samples/21_honest_degraded が degraded の honesty が
 * gossip 鮮度に依存しないことを「収束待ちで穴を隠さず」検証するために使う。 */
static UW beacon_hold_ms = 0;

/* K-DDS ハンドル (moe.c と同じ per-source topic パターン):
 *   h_pub     : 自ノード "world/beacon/<my>" へ発行
 *   h_sub[n]  : ピア "world/beacon/<n>" を購読 (n != my) */
static W h_pub = -1;
static W h_sub[DNODE_MAX];

/* "world/beacon/<node>" を out へ組み立てる (node は 0..DNODE_MAX-1, 1 桁) */
static void beacon_topic_name(char *out, UB node)
{
    const char *p = WORLD_BEACON_TOPIC_PFX;
    INT i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + node);
    out[i]   = '\0';
}

/* 現在の operating time を ms で読む (32bit lo で十分; age 計算用)。 */
static UW now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return t.lo;
}

/* ------------------------------------------------------------------ */
/* pressure — このノードの逼迫度 (0..100) を局所情報から導出 (§6)      */
/*                                                                     */
/* 「全体を見渡す指揮者は不要」という設計指針 (§7) に従い、自分の局所   */
/* 状態だけから算出する。capacity が低い (孤立/縮退) ほど、また直近の   */
/* 発火が多いほど逼迫度を高くする — これがゴシップで配られ、応援・受援  */
/* の勾配信号 (§6) の素地になる。                                       */
/* ------------------------------------------------------------------ */

static UB compute_pressure(void)
{
    /* 縮退レベルを基線にする: SOLO ほど逼迫 (余力が無い)。 */
    UB base;
    switch (degrade_level()) {
    case DEGRADE_SOLO:    base = 60; break;   /* 孤立 = 構造的に逼迫       */
    case DEGRADE_REDUCED: base = 35; break;
    case DEGRADE_FULL:    base = 15; break;   /* 群れがある = 余力が高い   */
    default:              base = 30; break;
    }

    /* 直近に発火しているクラス数だけ上乗せ (発火 = いま働いている = 逼迫) */
    UB fires = 0;
    for (INT c = 0; c < MOE_NUM_CLASSES; c++)
        if (my_firing & WORLD_FIRE_BIT(c)) fires++;
    UW p = (UW)base + (UW)fires * 12u;
    /* 【G20】CONSERVE はもう load 軸 (pressure) へは載せない。脅威を pressure に
     * 畳むと「脅威 = 避けよ」の符号倒錯になり、群れが守るべき一点から逃げる。
     * reflex の脅威は別軸 (compute_threat / WORLD_BEACON.threat) で *寄る* 符号
     * として配る。pressure はここでは純粋に「混んでいるか (load)」だけを表す。 */
    if (p > 100) p = 100;
    return (UB)p;
}

/* ------------------------------------------------------------------ */
/* threat — このノードの脅威度 (0..100) を局所情報から導出 (§2)        */
/*                                                                     */
/* load (compute_pressure) と独立した *脅威軸*。reflex 反射層が危険を観測   */
/* (CONSERVE engage) している間だけ立ち、ビーコンの threat フィールドで配ら  */
/* れる。受信側 moe ゲートはこれを *加点* し (load の逆符号)、群れの計算を    */
/* この一点へ集束させる (rally; survival §2 「守る対象へ全網の力を注ぐ」)。   */
/* 反射が解除されれば 0 に戻る (ヒステリシスは reflex 側)。                  */
/* ------------------------------------------------------------------ */

static UB compute_threat(void)
{
    /* 二つの脅威源の強い方を配る:
     *   - reflex_threat_level(): 反射 CONSERVE が立てる脅威 (G20; 時定数は
     *     reflex 側のヒステリシス)。
     *   - protect_threat_level(): G28 で接地した脅威。宣言された「守る対象」
     *     (p-fs オブジェクト) が >=R 近傍へ複製されていない (under-replicated)
     *     あいだだけ HIGH で、複製が進むと実在の状態として DROP する。タイマ
     *     ではなく実在の複製状態に縛られる = ループが閉じる。
     * どちらも局所/ゴシップ状態だけを読み、中央集約点を作らない (§7)。 */
    UB rt = reflex_threat_level();
    UB pt = protect_threat_level();
    return (pt > rt) ? pt : rt;
}

/* ------------------------------------------------------------------ */
/* atrisk — 同時に at-risk な protected 点の数 (G35/§5)                 */
/*                                                                     */
/* threat は最悪 1 点ぶんの脅威スカラ (rally 信号) しか運べない。これは     */
/* 「同時多発」(§5) を畳んでしまう。atrisk は「いま何点を並行して守って      */
/* いるか」を別バイトで運び、近傍が *多点性* を perceive できるようにする。  */
/* 0..PROTECT_MAX_OBJS に収まるので 12B ビーコンを増やさない。              */
/* ------------------------------------------------------------------ */
static UB compute_atrisk(void)
{
    INT n = protect_atrisk_count();
    if (n < 0)   n = 0;
    if (n > 255) n = 255;
    return (UB)n;
}

/* ------------------------------------------------------------------ */
/* 公開フック: MoE 発火を記録する                                      */
/* ------------------------------------------------------------------ */

void world_note_firing(UB gate_class)
{
    if (gate_class < MOE_NUM_CLASSES)
        my_firing |= WORLD_FIRE_BIT(gate_class);
}

/* selfc-ring3 §8 — a self-built unit germinated or rolled back; mark the
 * star as visibly rebuilding for one beacon period (decays with firing). */
void world_note_rebuild(void)
{
    my_firing |= WORLD_REBUILD_BIT;
}

/* ------------------------------------------------------------------ */
/* self-beacon を組み立てて publish する                               */
/* ------------------------------------------------------------------ */

static void publish_beacon(void)
{
    if (h_pub < 0) return;

    WORLD_BEACON b;
    b.node_id     = drpc_my_node;
    b.device_type = my_device_type();
    b.region_id   = region_id();                 /* 自 region (局所ビュー)  */
    b.pressure    = compute_pressure();
    b.firing      = (UB)(my_firing & WORLD_BEACON_FIRE_MASK);
#ifdef _TK_HOSTED_LIBC_
    /* survival-loop L0: carry my STATE in the firing byte's spare bits 3-4 so
     * peers read it via world_peer_state — NO wire change. */
    b.firing     |= (UB)(world_self_state() << WORLD_STATE_SHIFT);
#endif
    b.region_size = region_size();
    b.threat      = compute_threat();            /* 脅威軸 (§2 rally) — G20 */
    b.atrisk      = compute_atrisk();
    b.seq         = my_seq++;

    kdds_pub(h_pub, &b, sizeof(b));

    /* firing は「直近に発火したか」のエッジ情報。1 周期分配ったら減衰させ、
     * 古い発火が居座らないようにする (§8 ヒステリシス/減衰の時定数の素朴版)。 */
    my_firing = 0;
}

/* ------------------------------------------------------------------ */
/* 受信したビーコンを world-table へ取り込む                           */
/* ------------------------------------------------------------------ */

void world_observe(const WORLD_BEACON *b)
{
    if (!b) return;
    UB n = b->node_id;
    if (n >= DNODE_MAX) return;

    int first   = !table[n].valid;
    /* seq が前進したか = 「新しいビーコンが届いたか」。LATEST_ONLY トピックは
     * 同じ値を毎ポール返すので、seq が前進したときだけ last_ms を更新する。
     * これをしないと、発信が止まったノードでも再ポールで last_ms が更新され、
     * 永遠に stale にならない (古さの尊重 = 生死の検出に必須)。 */
    int fresher = first || ((U4)b->seq != (U4)table[n].beacon.seq);
    /* 古い seq への巻き戻りは無視 (リプレイ/順序逆転)。0 は初回扱い。 */
    if (!first && b->seq != 0 && (U4)b->seq < (U4)table[n].beacon.seq)
        return;

    table[n].beacon = *b;          /* 内容 (region/pressure/firing) は常に反映 */
    table[n].valid  = 1;
    if (fresher) table[n].last_ms = now_ms();   /* 鮮度は seq 前進時のみ */
}

/* ------------------------------------------------------------------ */
/* gating アクセサ — select_expert (§7) が局所勾配を読む窓口            */
/*                                                                     */
/* 重要 (NO-CENTRAL 不変条件): これらは中央集約器ではなく、このノードの */
/* ローカル world-table (受信したゴシップビーコンだけ) を読む。逼迫度は */
/* 各ノードが compute_pressure() で *自分の* 局所状態から算出し self-   */
/* beacon で配ったもの。読む側は近隣の勾配を見るだけで、全網の真実を    */
/* 集約する場所はどこにも無い。                                         */
/* ------------------------------------------------------------------ */

BOOL world_peer_known(UB node)
{
    if (node >= DNODE_MAX) return FALSE;
    return table[node].valid ? TRUE : FALSE;
}

INT world_peer_pressure(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    return (INT)table[node].beacon.pressure;   /* 負荷軸 0..100 (-1 = 未知) */
}

INT world_peer_threat(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    return (INT)table[node].beacon.threat;     /* 脅威軸 0..100 (-1 = 未知) */
}

INT world_peer_atrisk(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    return (INT)table[node].beacon.atrisk;     /* 同時 at-risk 点数 (-1=未知) */
}

INT world_peer_region(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    UB rid = table[node].beacon.region_id;
    return (rid == 0xFF) ? -1 : (INT)rid;       /* coordinator ID (-1=未知) */
}

/* world_peer_region と同じだが、ビーコンが *新鮮* (age <= WORLD_STALE_MS) な
 * ときだけ region を返す (G12, wave 12)。未受信 / 古い / region 未広告 (0xFF)
 * なら -1。dkva の requester はこれで「gossip で新鮮に確認できた」region だけ
 * から degraded の分母を組み、確認できない remote は『不明 (uncertain)』として
 * 別計上する。これにより degraded(k/n) の数字が gossip 鮮度に依存して過大計上
 * (各メンバを別 region と誤認) することを防ぐ。 */
INT world_peer_region_fresh(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    UW now = now_ms();
    UW age = (now >= table[node].last_ms) ? (now - table[node].last_ms) : 0;
    if (age > WORLD_STALE_MS) return -1;        /* 古い = 不確実 */
    UB rid = table[node].beacon.region_id;
    return (rid == 0xFF) ? -1 : (INT)rid;
}

/* galaxy v1 (galaxy.md §9, S1/S12): the table already stores device_type
 * from each beacon; expose it so /galaxy.json can label a peer star by
 * arch+role. Returns WORLD_DEV_* or -1 if the node is unknown. */
INT world_peer_device(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    return (INT)table[node].beacon.device_type;
}

/* galaxy v1 (galaxy.md §5, S11/S12 honesty): age in ms since this node
 * last freshly observed `node`'s beacon, so the page can fade stale
 * peers (古さの尊重). Returns -1 if the node is unknown. */
INT world_peer_age_ms(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    UW now = now_ms();
    UW age = (now >= table[node].last_ms) ? (now - table[node].last_ms) : 0;
    return (INT)age;
}

/* テスト専用: 起動後 ms ミリ秒だけ self-beacon を抑止する (G12 デモ用)。
 * usermain が PKERNEL_WORLD_BEACON_HOLD_MS から注入する。0 = 無効 (既定)。 */
void world_set_beacon_hold(UW ms)
{
    beacon_hold_ms = ms;
}

/* ------------------------------------------------------------------ */
/* survival-loop L0 — per-node STATE FSM (hosted only)                 */
/*                                                                     */
/* docs/architecture/20-architecture/survival-loop.md §1.1 / §6-L0 / §9. Axis-dependent */
/* (NOT monotone) 2-time-constant hysteresis over the S_n bus:         */
/*   - INTERO_AX_THREAT @hi  -> target ACTIVE  (the rally/activate arm; */
/*     "death-imminent -> activate" reflex G33; never STRESSED).        */
/*   - non-threat axes @hi   -> target STRESSED (shed own activity).    */
/* Hysteresis: a transition must hold WSTATE_MIN_DWELL consecutive      */
/* ticks; the enter/exit scalars form a deadband (S_EXIT < S_ENTER).    */
/* Hosted-only — bare-metal keeps state bits = 0 = ACTIVE, no FSM.      */
/* ------------------------------------------------------------------ */
#ifdef _TK_HOSTED_LIBC_

/* PROVISIONAL placeholders — discover from measured S_n curves
 * (interoception §2.4 / survival-loop §7 headline) — NOT final. The cert injects
 * hi/lo straddling these so the band values are not load-bearing; only the
 * axis-divergence (L0) and the flap-reduction (L1) are. */
#define WSTATE_S_ENTER    160   /* ACTIVE->STRESSED needs s >= this (non-threat) */
#define WSTATE_S_EXIT     100   /* STRESSED->ACTIVE allowed when s <= this       */
#define WSTATE_MIN_DWELL    3   /* base dwell (L0 used this symmetrically)        */

/* survival-loop L1 §8 — two-time-constant hysteresis (docs/architecture/
 * survival-loop.md §5.2 / §6-L1 / §10). The L0 FSM used ONE symmetric dwell; a
 * coupled S_n forcing (stress UP while ACTIVE/holding work, DOWN while STRESSED/
 * shedding) still flapped ACTIVE<->STRESSED > K times — MEASURED, not assumed,
 * see world_l1_flap_test. The cure splits the dwell so STRESSED is FAST to enter
 * and SLOW to leave, plus a relax refractory after entering — breaking the
 * symmetric ping-pong — while acute DANGER (the THREAT axis) still relaxes
 * INSTANTLY (rally, §1.1). The magnitudes are DISCOVERED (the cert straddles
 * them); only the flap-reduction is load-bearing. Hosted-only -> world.c
 * bare-metal .text byte-identical. */
#ifdef SURVIVAL_L1_NO_DAMP
/* falsifier: collapse the two time-constants back to one (= the symmetric naive
 * FSM) so the flap returns and [hysteresis] goes RED. */
#define WSTATE_ENTER_DWELL   WSTATE_MIN_DWELL
#define WSTATE_RELAX_DWELL   WSTATE_MIN_DWELL
#define WSTATE_RELAX_REFRAC  0u
#else
#define WSTATE_ENTER_DWELL   WSTATE_MIN_DWELL        /* fast to STRESS            */
#define WSTATE_RELAX_DWELL   (WSTATE_MIN_DWELL * 8)  /* slow to RELAX (discovered) */
#define WSTATE_RELAX_REFRAC  20u                     /* relax refractory (discovered) */
#endif

static UB self_state    = WSTATE_ACTIVE;  /* the committed STATE (WSTATE_*)       */
static UB self_dwell    = 0;              /* qualifying ticks held toward a switch */
static UW self_cooldown = 0;              /* relax refractory ticks after STRESS   */

UB world_self_state(void) { return self_state; }

/* The committed-state transition for ONE tick. Pure: operates on the passed
 * state/dwell/cooldown via pointers, with the scalar s, threat_acute, and the two
 * dwell time-constants + relax refractory as inputs. The production step AND
 * world_l1_flap_test's naive & damped arms ALL call this (no re-implementation —
 * the shape mirrors moe's shared deadband_pick). */
static void wstate_advance(UB *st, UB *dwell, UW *cooldown,
                           UB s, UB threat_acute,
                           UB enter_dwell, UB relax_dwell, UW relax_refrac)
{
    if (*cooldown) (*cooldown)--;            /* relax refractory ticks down       */

    if (*st == WSTATE_ACTIVE) {
        /* ACTIVE -> STRESSED: a non-threat axis with sustained high s, held
         * enter_dwell ticks (FAST). Entering arms the relax refractory. */
        if (!threat_acute && s >= WSTATE_S_ENTER) {
            if (++(*dwell) >= enter_dwell) {
                *st = WSTATE_STRESSED; *dwell = 0; *cooldown = relax_refrac;
            }
        } else *dwell = 0;
    } else if (*st == WSTATE_STRESSED) {
        if (threat_acute) {
            /* acute DANGER (THREAT axis) relaxes INSTANTLY — rally, overriding the
             * slow dwell + refractory (§1.1 "death-imminent -> activate"). */
            *st = WSTATE_ACTIVE; *dwell = 0; *cooldown = 0;
        } else if (s <= WSTATE_S_EXIT && *cooldown == 0) {
            /* scalar relax: SLOW — only after the refractory, held relax_dwell. */
            if (++(*dwell) >= relax_dwell) { *st = WSTATE_ACTIVE; *dwell = 0; }
        } else *dwell = 0;
    } else {
        /* HIBERNATING/DYING reserved for L2/L3 — not entered in L0/L1. */
        *dwell = 0;
    }
}

UB world_self_state_step(void)
{
    UB old = self_state;
    UB s   = intero_scalar();           /* re-samples live S_n (refreshes axis)  */
    UB ax  = intero_dominant_axis();
    (void)ax;
#ifdef SURVIVAL_L0_MONOTONE
    /* falsifier: ignore the axis -> high s always pushes toward STRESSED, so the
     * THREAT axis is no longer special and [state-axis] THREAT goes RED. */
    UB threat_acute = 0;
#else
    UB threat_acute = (UB)(ax == INTERO_AX_THREAT);
#endif

    wstate_advance(&self_state, &self_dwell, &self_cooldown, s, threat_acute,
                   WSTATE_ENTER_DWELL, WSTATE_RELAX_DWELL, WSTATE_RELAX_REFRAC);

    if (self_state != old)
        galaxy_emit(EV_STATE, drpc_my_node, GALAXY_NODE_NONE,
                    (UH)old, (UH)self_state);
    return self_state;
}

/* Read peer `node`'s gossiped STATE from the local world-table (mirrors
 * world_peer_pressure): bits WORLD_STATE_MASK of the beacon firing byte. A
 * 12-byte old beacon leaves those bits 0 -> WSTATE_ACTIVE (back-compat). */
INT world_peer_state(UB node)
{
    if (node >= DNODE_MAX || !table[node].valid) return -1;
    return (INT)((table[node].beacon.firing & WORLD_STATE_MASK) >> WORLD_STATE_SHIFT);
}

/* ── hosted cert: [state-axis] (load-bearing) + [state-gossip] ─────────────── */
INT world_survival_l0_test(void)
{
    INT fail = 0;
    const UB hi = 220;                       /* >> S_ENTER (160): straddles band */
    const INT steps = WSTATE_ENTER_DWELL + 2;
    /* L1 two-time-constant damping makes STRESSED->ACTIVE relax SLOW (refractory +
     * relax dwell). Give each calm-reset that many ticks (+margin) so a prior
     * STRESSED sub-test fully relaxes to ACTIVE before the next axis is injected. */
    const INT calm_steps = (INT)WSTATE_RELAX_REFRAC + WSTATE_RELAX_DWELL + 2;

    wo_puts("[survival-l0] STATE bus: axis-dependence + gossip (hosted cert)\r\n");

    /* [state-axis]: at ONE identical high scalar, force each axis dominant in
     * turn. THREAT@hi must stay ACTIVE (rally arm); every other axis@hi must
     * reach STRESSED. Only the AXIS varies -> divergent destination proves the
     * response is axis-dependent (NOT monotone). Each sub-test calm-resets to
     * ACTIVE first (a non-threat axis at s=0, inside the deadband). */
    struct { UB ax; UB want; const char *nm; } cases[] = {
        { INTERO_AX_THREAT,   WSTATE_ACTIVE,   "THREAT"   },
        { INTERO_AX_SURPRISE, WSTATE_STRESSED, "SURPRISE" },
        { INTERO_AX_FAULT,    WSTATE_STRESSED, "FAULT"    },
        { INTERO_AX_DEGRADE,  WSTATE_STRESSED, "DEGRADE"  },
        { INTERO_AX_LATENCY,  WSTATE_STRESSED, "LATENCY"  },
    };
    INT axis_fail = 0;
    for (INT i = 0; i < (INT)(sizeof(cases)/sizeof(cases[0])); i++) {
        intero_test_force_axis(INTERO_AX_LATENCY, 0);          /* calm -> ACTIVE */
        for (INT t = 0; t < calm_steps; t++) world_self_state_step();
        if (world_self_state() != WSTATE_ACTIVE) {
            wo_puts("[state-axis]   calm-reset did not reach ACTIVE FAIL\r\n");
            axis_fail = 1;
        }
        intero_test_force_axis(cases[i].ax, hi);               /* this axis @hi   */
        for (INT t = 0; t < steps; t++) world_self_state_step();
        UB got = world_self_state();
        wo_puts("[state-axis]   "); wo_puts(cases[i].nm);
        wo_puts("@hi -> "); wo_putdec(got);
        if (got == cases[i].want) {
            wo_puts(cases[i].want == WSTATE_ACTIVE
                    ? " ACTIVE (rally, not stressed) ok\r\n" : " STRESSED ok\r\n");
        } else {
            wo_puts(" want "); wo_putdec(cases[i].want); wo_puts(" FAIL\r\n");
            axis_fail = 1;
        }
    }
    intero_test_force(0, 0);          /* release the pin (resets force_axis too) */
    if (axis_fail) { wo_puts("[state-axis] FAIL\r\n"); fail = 1; }
    else            wo_puts("[state-axis] PASS\r\n");

    /* [state-gossip]: stamp a committed state into the firing byte's spare bits,
     * observe it as a peer beacon, read it back via world_peer_state; and a
     * beacon with bits 3-4 = 0 reads as WSTATE_ACTIVE (old-node default). */
    INT gos_fail = 0;
    UB peer = (UB)(DNODE_MAX - 1);
    if (peer == drpc_my_node) peer = (UB)(DNODE_MAX - 2);
    {
        WORLD_BEACON b;
        b.node_id = peer; b.device_type = 0; b.region_id = 0xFF;
        b.pressure = 0; b.region_size = 0; b.threat = 0; b.atrisk = 0;
        b.firing = (UB)(WSTATE_STRESSED << WORLD_STATE_SHIFT);   /* stamp STRESSED */
        b.seq = 1000;
        world_observe(&b);
        INT ps = world_peer_state(peer);
        wo_puts("[state-gossip] stamped STRESSED, peer reads "); wo_putdec((UW)ps);
        if (ps == WSTATE_STRESSED) wo_puts(" ok\r\n");
        else { wo_puts(" FAIL\r\n"); gos_fail = 1; }

        b.firing = WORLD_FIRE_BIT(0);    /* fire bit set, state bits 0 = old node */
        b.seq = 1001;
        world_observe(&b);
        ps = world_peer_state(peer);
        wo_puts("[state-gossip] bits3-4=0 reads "); wo_putdec((UW)ps);
        if (ps == WSTATE_ACTIVE) wo_puts(" = ACTIVE (back-compat) ok\r\n");
        else { wo_puts(" want ACTIVE FAIL\r\n"); gos_fail = 1; }
    }
    if (gos_fail) { wo_puts("[state-gossip] FAIL\r\n"); fail = 1; }
    else           wo_puts("[state-gossip] PASS\r\n");

#ifdef SURVIVAL_L0_MONOTONE
    wo_puts("[state-monotone-NOT] ARMED: FSM forced monotone (axis ignored) — "
            "[state-axis] THREAT must FAIL above\r\n");
#endif
    wo_puts(fail ? "[survival-l0] FAIL\r\n" : "[survival-l0] PASS\r\n");
    return fail;
}

/* ── survival-loop L1 §8: [hysteresis] / [hysteresis-NOT] ─────────────────────
 * docs/architecture/20-architecture/survival-loop.md §5.2 / §6-L1 / §10. Measure the disease
 * FIRST (wave-45 discipline: "the fix WAS the disease" — never credit a fix
 * without the same-harness unfixed control). Drive the SHARED wstate_advance
 * (the production transition) under a coupled S_n forcing: stress accrues while
 * ACTIVE (the node holds work) and falls while STRESSED (it sheds). Count
 * ACTIVE<->STRESSED flips over the run, for the NAIVE arm (one symmetric dwell =
 * the L0 FSM) and the DAMPED arm (the two-time-constant production params). Both
 * run in ONE build on LOCAL state (the [moe-osc] shape). */
static UW wstate_flap(UB enter_dwell, UB relax_dwell, UW relax_refrac)
{
    UB st = WSTATE_ACTIVE, dwell = 0; UW cooldown = 0;
    UB s = WSTATE_S_EXIT;                 /* start calm at the exit threshold     */
    const INT UP = 40, DOWN = 40, T = 200;
    UW flips = 0; UB prev = st;
    for (INT t = 0; t < T; t++) {
        wstate_advance(&st, &dwell, &cooldown, s, 0 /*non-threat axis*/,
                       enter_dwell, relax_dwell, relax_refrac);
        if (t > 1 && st != prev) flips++;          /* 2 ticks warmup             */
        prev = st;
        /* coupling: ACTIVE accrues stress (holds work); STRESSED sheds it. */
        if (st == WSTATE_ACTIVE) s = (UB)((UW)s + (UW)UP > 255u ? 255u : (UW)s + (UW)UP);
        else                     s = (UB)((UW)s < (UW)DOWN  ? 0u   : (UW)s - (UW)DOWN);
    }
    return flips;
}

INT world_l1_flap_test(void)
{
    INT fail = 0;
    const UW K = 12;                     /* reuse the moe flap bound (moe.c:929)  */
    UW flips_naive  = wstate_flap(WSTATE_MIN_DWELL, WSTATE_MIN_DWELL, 0u);
    UW flips_damped = wstate_flap(WSTATE_ENTER_DWELL, WSTATE_RELAX_DWELL,
                                  WSTATE_RELAX_REFRAC);

    wo_puts("[hysteresis] coupled S_n forcing, 200 ticks: naive flips=");
    wo_putdec(flips_naive);
    wo_puts(" damped flips="); wo_putdec(flips_damped);
    wo_puts(" (K="); wo_putdec(K); wo_puts(")\r\n");

    /* the disease must be real (naive flaps > K) — else any cure is vacuous. */
    if (!(flips_naive > K)) {
        wo_puts("[hysteresis] FAIL naive case did not flap > K"
                " (disease not reproduced)\r\n");
        fail = 1;
    }
    /* the cure: the EXACT [moe-osc] acceptance shape — under K AND at most half
     * of naive. -DSURVIVAL_L1_NO_DAMP collapses damped==naive so this RED-fires. */
    if (!(flips_damped <= K && flips_damped * 2 <= flips_naive)) {
        wo_puts("[hysteresis] FAIL two-time-constant damping did not break the"
                " flap\r\n");
        fail = 1;
    }
    if (!fail) {
        wo_puts("[hysteresis] PASS (flaps "); wo_putdec(flips_damped);
        wo_puts("<="); wo_putdec(K); wo_puts(" and <= half of naive ");
        wo_putdec(flips_naive); wo_puts(")\r\n");
    } else {
        wo_puts("[hysteresis] FAIL\r\n");
    }
#ifdef SURVIVAL_L1_NO_DAMP
    wo_puts("[hysteresis-NOT] ARMED: two time-constants collapsed to one"
            " (damped==naive) — the flap must return above (RED)\r\n");
#endif
    return fail;
}

/* ── survival-loop L1 driver: shell `survival l1` ─────────────────────────────
 * Runs the STATE-aware support-routing cert (moe.c) + the §8 hysteresis cert,
 * then the overall verdict. Pure in-process (no net): the support-route cert
 * drives the production moe_select_step, the hysteresis cert drives the
 * production wstate_advance. */
INT world_survival_l1_test(void)
{
    INT fail = 0;

    /* clean FSM/bus state — these certs are pure & deterministic. */
    self_state = WSTATE_ACTIVE; self_dwell = 0; self_cooldown = 0;
    intero_test_force(0, 0);

    wo_puts("[survival-l1] STATE-aware support routing + §8 hysteresis (hosted cert)\r\n");
    if (moe_support_route_test()) fail = 1;   /* [support-route] / [support-route-NOT] */
    if (world_l1_flap_test())     fail = 1;   /* [hysteresis]   / [hysteresis-NOT]     */

    /* restore calm. */
    intero_test_force(0, 0);
    self_state = WSTATE_ACTIVE; self_dwell = 0; self_cooldown = 0;

    wo_puts(fail ? "[survival-l1] FAIL\r\n" : "[survival-l1] PASS\r\n");
    return fail;
}

#endif /* _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* world-task: 発信 + 取り込み (全ノードで対称に走る)                  */
/* ------------------------------------------------------------------ */

void world_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* world_task は cmd_net (drpc_init 後) に起動されるので drpc_my_node は確定。
     * 自ノードの per-source ビーコントピックへ pub、ピアのトピックを sub する。
     * moe.c と同じ per-source パターン: 単一スロットへ全員が上書きし合う
     * 集約点 (=準・中央) を作らないため (NO-CENTRAL 不変条件)。 */
    if (drpc_my_node != 0xFF) {
        char tn[KDDS_NAME_MAX];
        /* poll-only オープン: ビーコンは LATEST_ONLY をポーリングで取り込み、
         * ブロッキング待ちはしないのでセマフォを消費しない。per-source topic を
         * DNODE_MAX 個開いても CFN_MAX_SEMID を枯渇させない (kdds.h 参照)。 */
        beacon_topic_name(tn, drpc_my_node);
        h_pub = kdds_open_poll(tn, KDDS_QOS_LATEST_ONLY);
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node) { h_sub[n] = -1; continue; }
            beacon_topic_name(tn, n);
            h_sub[n] = kdds_open_poll(tn, KDDS_QOS_LATEST_ONLY);
        }

        /* 自分のエントリも world-table に載せておく (自己観測)。 */
        WORLD_BEACON self;
        self.node_id     = drpc_my_node;
        self.device_type = my_device_type();
        self.region_id   = region_id();
        self.pressure    = compute_pressure();
        self.firing      = 0;
        self.region_size = region_size();
        self.threat      = compute_threat();
        self.atrisk      = compute_atrisk();
        self.seq         = 0;
        world_observe(&self);
    }

    UW since_beacon = WORLD_BEACON_MS;   /* 起動直後に1回発信 */
    UW uptime_ms    = 0;                 /* world_task 開始からの経過 (hold 判定) */
    for (;;) {
        tk_dly_tsk(WORLD_POLL_MS);
        uptime_ms += WORLD_POLL_MS;
        if (drpc_my_node == 0xFF) continue;

        /* 近隣ビーコンを取り込む (per-source なので衝突せず全ノード蓄積) */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (h_sub[n] < 0) continue;
            WORLD_BEACON b;
            W r = kdds_sub(h_sub[n], &b, (W)sizeof(b), 0);
            if (r >= (W)sizeof(WORLD_BEACON) && b.node_id == n)
                world_observe(&b);
        }

        /* 自分のエントリを更新 (自己観測; region/pressure は時々刻々変わる) */
        {
            WORLD_BEACON self;
            self.node_id     = drpc_my_node;
            self.device_type = my_device_type();
            self.region_id   = region_id();
            self.pressure    = compute_pressure();
            self.firing      = (UB)(my_firing & WORLD_BEACON_FIRE_MASK);
#ifdef _TK_HOSTED_LIBC_
            /* survival-loop L0: the ONE production call site that advances the
             * STATE FSM (once per world_task tick) and stamps the committed
             * state into the self-observation. */
            self.firing     |= (UB)(world_self_state_step() << WORLD_STATE_SHIFT);
#endif
            self.region_size = region_size();
            self.threat      = compute_threat();
            self.atrisk      = compute_atrisk();
            self.seq         = table[drpc_my_node].valid
                                 ? table[drpc_my_node].beacon.seq : 0;
            world_observe(&self);
        }

        /* 周期的に self-beacon を発信。テストフックが有効な間 (uptime <
         * beacon_hold_ms) は発信を抑止し、gossip を意図的に未収束のまま保つ
         * (G12 デモ; 本番は beacon_hold_ms==0 なので素通り)。 */
        since_beacon += WORLD_POLL_MS;
        if (since_beacon >= WORLD_BEACON_MS) {
            since_beacon = 0;
            if (uptime_ms >= beacon_hold_ms)
                publish_beacon();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void world_init(void)
{
    for (INT n = 0; n < DNODE_MAX; n++) {
        table[n].valid   = 0;
        table[n].last_ms = 0;
        h_sub[n]         = -1;
    }
    h_pub     = -1;
    my_firing = 0;
    my_seq    = 0;
    wo_puts("[world] situational-awareness map initialized (no central collector)\r\n");
}

/* ------------------------------------------------------------------ */
/* 表示: 全網の状況を ASCII で描く (shell `world` / `map`)             */
/* ------------------------------------------------------------------ */

/* pressure (0..100) を 10 段の ASCII バーで描く。 */
static void print_pressure_bar(UB p)
{
    INT filled = (INT)p / 10;     /* 0..10 */
    if (filled > 10) filled = 10;
    wo_puts("[");
    for (INT i = 0; i < 10; i++)
        wo_puts(i < filled ? "#" : ".");
    wo_puts("]");
}

void world_print(void)
{
    if (drpc_my_node == 0xFF) {
        wo_puts("[world] single-node (no cluster) — run 'net' to join a mesh\r\n");
        return;
    }

    UW now = now_ms();

    wo_puts("[world] whole-network map as seen by node");
    wo_putdec(drpc_my_node);
    wo_puts(" (built from gossip; no central collector)\r\n");
    wo_puts("[world]  id  device_type    region  rtt    state    pressure       firing\r\n");

    UB known = 0;
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (!table[n].valid) continue;
        known++;

        const WORLD_BEACON *b = &table[n].beacon;

        /* age / staleness の判定 (古さの尊重) */
        UW age = (now >= table[n].last_ms) ? (now - table[n].last_ms) : 0;
        int self  = (n == drpc_my_node);
        int stale = (!self && age > WORLD_STALE_MS);
        /* SWIM が DEAD と見ているなら、それも反映する (生死の2軸) */
        int dead  = (!self && dnode_table[n].state == DNODE_DEAD);

        wo_puts("  node"); wo_putdec(n); wo_puts("  ");

        /* device_type (パディングで列を揃える) */
        const char *dn = device_type_name(b->device_type);
        wo_puts(dn);
        for (INT pad = (INT)__builtin_strlen(dn); pad < 14; pad++) wo_puts(" ");

        /* region */
        if (b->region_id == 0xFF) wo_puts("--    ");
        else { wo_puts("r"); wo_putdec(b->region_id); wo_puts("     "); }

        /* RTT (空間配置の手がかり; swim_rtt_ms) */
        if (self) {
            wo_puts("0ms    ");
        } else {
            UW rtt = swim_rtt_ms(n);
            if (rtt == 0xFFFFFFFFUL) wo_puts("?      ");
            else { wo_putdec(rtt); wo_puts("ms    "); }
        }

        /* alive / age / state */
        if (self)       wo_puts("self     ");
        else if (dead)  wo_puts("DEAD     ");
        else if (stale) {
            wo_puts("stale(");
            wo_putdec(age / 1000); wo_puts("s) ");
        } else          wo_puts("alive    ");

        /* pressure bar — stale なら不完全さを示す */
        if (stale || dead) {
            wo_puts("[ unknown ]   ");
        } else {
            print_pressure_bar(b->pressure);
            wo_puts(" ");
            wo_putdec(b->pressure); wo_puts("%  ");
            if (b->pressure < 10) wo_puts("  ");
            else if (b->pressure < 100) wo_puts(" ");
        }

        /* firing インジケータ: 発火しているクラスは '*'、それ以外は '.' */
        wo_puts(" ");
        UB fires = (stale || dead) ? 0 : b->firing;
        int any = 0;
        for (INT c = 0; c < MOE_NUM_CLASSES; c++) {
            if (fires & WORLD_FIRE_BIT(c)) { wo_puts("*"); any = 1; }
            else                            wo_puts(".");
        }
        wo_puts(any ? " LIT" : " dark");

        /* 脅威軸 (§2): threat>0 のノードは「守る対象」= 群れの計算が *寄る*
         * 一点 (G20)。load の pressure bar とは別軸・逆符号であることを明示。 */
        if (!stale && !dead && b->threat > 0) {
            wo_puts("  threat="); wo_putdec(b->threat); wo_puts(" <-RALLY");
            /* G35/§5: many points at once — perceive the PLURALITY the single
             * threat scalar folds away (not "one crisis", "N concurrent"). */
            if (b->atrisk > 1) {
                wo_puts(" ("); wo_putdec(b->atrisk);
                wo_puts(" pts defended in parallel)");
            }
        }
        wo_puts("\r\n");
    }

    if (known == 0)
        wo_puts("[world]  (no beacons received yet — wait a few seconds)\r\n");

    wo_puts("[world] known nodes: "); wo_putdec(known);
    wo_puts("  (this view is local & may be stale/incomplete by design)\r\n");
}
