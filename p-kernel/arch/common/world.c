/*
 *  world.c
 *  Decentralized whole-network situational-awareness map.
 *
 *  設計: docs/architecture/survival-network.md / docs/architecture/regions.md
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
#include "reflex.h"   /* CONSERVE: reflex_pressure_bias() を局所勾配へ上乗せ */
#include "kernel.h"

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
    /* §8 反射層 CONSERVE (収縮): reflex が危険を観測している間だけ、自ノードの
     * 逼迫度を底上げする。これがビーコンで配られ moe ゲートの局所勾配に乗り、
     * 近傍は当ノードへの委譲を避ける = 「受援不要・応援に出ない」(§6 応援・受援)。
     * 反射が解除されればバイアスは 0 に戻る (ヒステリシスは reflex 側)。 */
    p += (UW)reflex_pressure_bias();
    if (p > 100) p = 100;
    return (UB)p;
}

/* ------------------------------------------------------------------ */
/* 公開フック: MoE 発火を記録する                                      */
/* ------------------------------------------------------------------ */

void world_note_firing(UB gate_class)
{
    if (gate_class < MOE_NUM_CLASSES)
        my_firing |= WORLD_FIRE_BIT(gate_class);
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
    b.firing      = (UB)(my_firing & WORLD_FIRE_MASK);
    b.region_size = region_size();
    b._pad        = 0;
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
    return (INT)table[node].beacon.pressure;   /* 0..100 (-1 = 未知) */
}

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
        self._pad        = 0;
        self.seq         = 0;
        world_observe(&self);
    }

    UW since_beacon = WORLD_BEACON_MS;   /* 起動直後に1回発信 */
    for (;;) {
        tk_dly_tsk(WORLD_POLL_MS);
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
            self.firing      = (UB)(my_firing & WORLD_FIRE_MASK);
            self.region_size = region_size();
            self._pad        = 0;
            self.seq         = table[drpc_my_node].valid
                                 ? table[drpc_my_node].beacon.seq : 0;
            world_observe(&self);
        }

        /* 周期的に self-beacon を発信 */
        since_beacon += WORLD_POLL_MS;
        if (since_beacon >= WORLD_BEACON_MS) {
            since_beacon = 0;
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
        wo_puts("\r\n");
    }

    if (known == 0)
        wo_puts("[world]  (no beacons received yet — wait a few seconds)\r\n");

    wo_puts("[world] known nodes: "); wo_putdec(known);
    wo_puts("  (this view is local & may be stale/incomplete by design)\r\n");
}
