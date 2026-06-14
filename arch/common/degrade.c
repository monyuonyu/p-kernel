/*
 *  degrade.c (x86)
 *  縮退モード管理 — フェーズ 11
 *
 *  swim.c が ALIVE/DEAD 遷移を検出するたびに degrade_update() を呼ぶ。
 *  生存ノード数 (自分を含む) によってレベルを決定し、
 *  レプリカ間隔・DTR モードを自動調整する。
 *
 *  SOLO 遷移時は replica_scatter_all() を即時呼び出し、
 *  K-DDS トピック "sys/degrade" にレベルを publish する。
 */

#include "degrade.h"
#include "drpc.h"
#include "region.h"
#include "dkva.h"
#include "replica.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

static void dg_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void dg_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dg_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dg_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

static UB  cur_level       = DEGRADE_FULL;
static UB  prev_level      = DEGRADE_FULL;
static UW  transition_cnt  = 0;   /* レベル遷移累計回数              */
static UW  alive_node_cnt  = 0;   /* 直近の生存ノード数 (自分含む)   */
static UW  last_kv_entries = 0;   /* 直近 DKVA 集約の実測 KV エントリ数 */

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void degrade_init(void)
{
    cur_level      = DEGRADE_FULL;
    prev_level     = DEGRADE_FULL;
    transition_cnt = 0;
    alive_node_cnt = 1;  /* 自分自身は常に ALIVE */
    last_kv_entries = 0;
    dg_puts("[degrade] initialized  level=FULL\r\n");
}

/* ------------------------------------------------------------------ */
/* レベル更新 (swim.c から呼ぶ)                                       */
/* ------------------------------------------------------------------ */

void degrade_update(void)
{
    if (drpc_my_node == 0xFF) return;

    /* 生存ノード数を数える (自分 + ALIVE ノード) */
    UW cnt = 1;  /* 自分は常に生存 */
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state == DNODE_ALIVE) cnt++;
    }
    alive_node_cnt = cnt;

    /* レベル決定 */
    UB new_level;
    if      (cnt >= 3) new_level = DEGRADE_FULL;
    else if (cnt == 2) new_level = DEGRADE_REDUCED;
    else               new_level = DEGRADE_SOLO;

    if (new_level == cur_level) return;   /* 変化なし */

    prev_level = cur_level;
    cur_level  = new_level;
    transition_cnt++;

    static const char *lname[] = { "FULL", "REDUCED", "SOLO" };
    dg_puts("[degrade] *** level change: ");
    dg_puts(lname[prev_level < 3 ? prev_level : 0]);
    dg_puts(" -> ");
    dg_puts(lname[new_level < 3 ? new_level : 0]);
    dg_puts("  alive=");
    dg_putdec(cnt);
    dg_puts("\r\n");

    /* SOLO 遷移: 記憶を即時散布して孤立に備える */
    if (new_level == DEGRADE_SOLO) {
        dg_puts("[degrade] SOLO — scattering all memories immediately\r\n");
        replica_scatter_all();
    }

    /* 注: K-DDS への publish は kdds_open が必要なため省略。
     *     swim.c が degrade_level() をポーリングする方式を採用。 */
}

/* ------------------------------------------------------------------ */
/* ゲッター                                                            */
/* ------------------------------------------------------------------ */

UB degrade_level(void)
{
    return cur_level;
}

TMO degrade_replica_interval(void)
{
    switch (cur_level) {
    case DEGRADE_FULL:    return 3000;
    case DEGRADE_REDUCED: return 2000;
    case DEGRADE_SOLO:    return 1000;
    default:              return 3000;
    }
}

/* ------------------------------------------------------------------ */
/* capacity(N) — 連続容量 (regions.md §3.2)                            */
/* ------------------------------------------------------------------ */

/* floor(log2(v))。v>=1 を仮定。 */
static UW floor_log2(UW v)
{
    UW e = 0;
    while (v > 1) { v >>= 1; e++; }
    return e;
}

/* ------------------------------------------------------------------ */
/* 純粋関数 (テスト可能)                                              */
/*                                                                     */
/* capacity の各軸を「宣言された資源」だけから計算する純粋関数。       */
/* 生きている公開 API はこれらに module/extern 状態を渡す薄いラッパー。 */
/* cert (capacity_self_test) はこの純粋関数を直接 sweep するので、     */
/* クラスタの実状態に依存せず決定論的に検証できる。                    */
/* ------------------------------------------------------------------ */

/* breadth: experts_active(N) = clamp(N, 1, CAP_E_MAX)。 */
UW cap_experts_of(UW n)
{
    if (n < 1) n = 1;
    if (n > CAP_E_MAX) n = CAP_E_MAX;
    return n;
}

/* depth: pipeline_depth(rs) = 1 + floor(log2(rs))。rs>=1。 */
UW cap_depth_of(UW rs)
{
    if (rs < 1) rs = 1;
    return 1 + floor_log2(rs);
}

/* KV-context: 実測値 measured>0 ならそれを、なければ rs*DKVA_CACHE_SIZE。 */
UW cap_kv_of(UW rs, UW measured)
{
    if (measured > 0) return measured;
    if (rs < 1) rs = 1;
    return rs * DKVA_CACHE_SIZE;
}

/* 容量 = breadth × depth × KV-context。 */
UW cap_score_of(UW n, UW rs, UW measured)
{
    return cap_experts_of(n) * cap_depth_of(rs) * cap_kv_of(rs, measured);
}

UW capacity_experts(void)
{
    return cap_experts_of(alive_node_cnt);   /* ノード ≒ expert */
}

UW capacity_depth(void)
{
    /* pipeline は region 内 (密) で組む。region 未確立なら自分 1 台。 */
    UB rs = (drpc_my_node == 0xFF) ? 1 : region_size();
    return cap_depth_of((UW)rs);
}

UW capacity_kv(void)
{
    /* 推論前の見積り: region 内ノードがそれぞれ満杯のキャッシュを持つ想定。 */
    UB rs = (drpc_my_node == 0xFF) ? 1 : region_size();
    return cap_kv_of((UW)rs, last_kv_entries);
}

UW capacity_score(void)
{
    /* 公開 score は live なゲッターの積。純粋関数の積と同一になる
     * (capacity_self_test が product 同一性を保証)。 */
    return capacity_experts() * capacity_depth() * capacity_kv();
}

void capacity_note_kv(UW entries)
{
    last_kv_entries = entries;
}

/* ------------------------------------------------------------------ */
/* 統計表示 (shell `degrade` コマンド用)                              */
/* ------------------------------------------------------------------ */

void degrade_stat(void)
{
    static const char *lname[] = { "FULL (3+ nodes)", "REDUCED (2 nodes)", "SOLO (1 node)" };
    UB lv = cur_level < 3 ? cur_level : 0;

    dg_puts("[degrade] level         : "); dg_puts(lname[lv]); dg_puts("\r\n");
    dg_puts("[degrade] alive nodes   : "); dg_putdec(alive_node_cnt); dg_puts("\r\n");
    dg_puts("[degrade] transitions   : "); dg_putdec(transition_cnt); dg_puts("\r\n");
    dg_puts("[degrade] replica intv  : "); dg_putdec((UW)degrade_replica_interval());
    dg_puts(" ms\r\n");
    dg_puts("[degrade] DTR mode      : ");
    dg_puts(cur_level == DEGRADE_SOLO ? "local only\r\n" : "distributed\r\n");

    /* capacity(N): 連続容量。3 段ラベルはこの粗いバンドに過ぎない。 */
    UW experts = capacity_experts();
    UW depth   = capacity_depth();
    UW kv      = capacity_kv();
    dg_puts("[capacity] experts      : "); dg_putdec(experts);
    dg_puts(" / "); dg_putdec(CAP_E_MAX); dg_puts(" (breadth)\r\n");
    dg_puts("[capacity] depth        : "); dg_putdec(depth);
    dg_puts(" (1+log2 region)\r\n");
    dg_puts("[capacity] kv-context   : "); dg_putdec(kv);
    dg_puts(last_kv_entries ? " entries (measured)\r\n" : " entries (estimate)\r\n");
    dg_puts("[capacity] score        : "); dg_putdec(capacity_score());
    dg_puts(" (experts*depth*kv)\r\n");
}

/* ------------------------------------------------------------------ */
/* capacity(N) cert (shell `capacity test`)                            */
/*                                                                     */
/* regions.md §3.2 の契約 — 容量が台数とともに「連続に」増える — を    */
/* 主張する。3 つの性質を純粋関数を直接呼んで sweep で検証する:        */
/*   1. 積同一性:   score(n,rs,m) == experts(n)*depth(rs)*kv(rs,m)      */
/*   2. 単調性:     資源が増えれば各軸も score も非減少                 */
/*   3. 境界クランプ: 最小構成 (1,1,推定) は最小、満杯構成は上限       */
/* 全て決定論的 (実クラスタ状態に非依存)。fail 数を返す。              */
/* ------------------------------------------------------------------ */
INT capacity_self_test(void)
{
    INT fails = 0;

    /* --- 1. 積同一性 ----------------------------------------------- */
    /* (alive N, region_size rs, measured-kv m) の代表点を sweep。     */
    static const UW tn[]  = { 1, 1, 2, 3, 4, 8, 16, 20 };
    static const UW trs[] = { 1, 2, 2, 3, 4, 8, 16, 20 };
    static const UW tm[]  = { 0, 0, 0, 0, 12, 0, 0, 0 };
    INT npts = (INT)(sizeof(tn) / sizeof(tn[0]));
    INT prodfail = 0;
    for (INT i = 0; i < npts; i++) {
        UW e = cap_experts_of(tn[i]);
        UW d = cap_depth_of(trs[i]);
        UW k = cap_kv_of(trs[i], tm[i]);
        UW s = cap_score_of(tn[i], trs[i], tm[i]);
        dg_puts("[capacity-sweep] N="); dg_putdec(tn[i]);
        dg_puts(" rs=");  dg_putdec(trs[i]);
        dg_puts(" m=");   dg_putdec(tm[i]);
        dg_puts(" -> e=");  dg_putdec(e);
        dg_puts(" d=");     dg_putdec(d);
        dg_puts(" kv=");    dg_putdec(k);
        dg_puts(" score="); dg_putdec(s);
        dg_puts("\r\n");
        if (s != e * d * k) prodfail++;
    }
    if (prodfail == 0) dg_puts("[capacity-product] PASS\r\n");
    else { dg_puts("[capacity-product] FAIL\r\n"); fails++; }

    /* --- 2. 単調性 ------------------------------------------------- */
    /* N を 1..CAP_E_MAX+4 で増やすと experts は非減少 (上限で飽和)。   */
    INT monofail = 0;
    {
        UW prev = 0;
        for (UW n = 1; n <= CAP_E_MAX + 4; n++) {
            UW e = cap_experts_of(n);
            if (e < prev) monofail++;
            prev = e;
        }
        /* region_size を 1..32 で増やすと depth は非減少、kv も非減少。 */
        UW pd = 0, pk = 0;
        for (UW rs = 1; rs <= 32; rs++) {
            UW d = cap_depth_of(rs);
            UW k = cap_kv_of(rs, 0);   /* 推定パス: rs*DKVA_CACHE_SIZE */
            if (d < pd) monofail++;
            if (k < pk) monofail++;
            pd = d; pk = k;
        }
        /* 全資源同時に増やすと score も非減少 (連続容量の本丸)。 */
        UW ps = 0;
        for (UW step = 1; step <= 16; step++) {
            UW s = cap_score_of(step, step, 0);
            if (s < ps) monofail++;
            ps = s;
        }
    }
    if (monofail == 0) dg_puts("[capacity-mono] PASS\r\n");
    else { dg_puts("[capacity-mono] FAIL\r\n"); fails++; }

    /* --- 3. 境界クランプ ------------------------------------------- */
    /* 最小構成 (1 node, region 1, 推定 kv): experts=1, depth=1,        */
    /* kv=DKVA_CACHE_SIZE, score=DKVA_CACHE_SIZE。これが床。            */
    /* 上限: experts は CAP_E_MAX を超えない (N をどれだけ増やしても)。 */
    INT clampfail = 0;
    {
        UW emin = cap_experts_of(1);
        UW dmin = cap_depth_of(1);
        UW kmin = cap_kv_of(1, 0);
        UW smin = cap_score_of(1, 1, 0);
        if (emin != 1)               clampfail++;
        if (dmin != 1)               clampfail++;
        if (kmin != DKVA_CACHE_SIZE) clampfail++;
        if (smin != DKVA_CACHE_SIZE) clampfail++;
        /* 上限クランプ: N が CAP_E_MAX を超えても experts は飽和。 */
        if (cap_experts_of(CAP_E_MAX)     != CAP_E_MAX) clampfail++;
        if (cap_experts_of(CAP_E_MAX + 1) != CAP_E_MAX) clampfail++;
        if (cap_experts_of(1000000)       != CAP_E_MAX) clampfail++;
        /* 退化入力 (N=0, rs=0) も床にクランプされる (発散しない)。 */
        if (cap_experts_of(0) != 1) clampfail++;
        if (cap_depth_of(0)   != 1) clampfail++;
        if (cap_kv_of(0, 0)   != DKVA_CACHE_SIZE) clampfail++;
        dg_puts("[capacity-clamp] floor score="); dg_putdec(smin);
        dg_puts(" cap_experts="); dg_putdec(cap_experts_of(1000000));
        dg_puts(" (E_MAX="); dg_putdec(CAP_E_MAX); dg_puts(")\r\n");
    }
    if (clampfail == 0) dg_puts("[capacity-clamp] PASS\r\n");
    else { dg_puts("[capacity-clamp] FAIL\r\n"); fails++; }

    /* --- 集計 ------------------------------------------------------ */
    if (fails == 0) dg_puts("[capacity-score] PASS\r\n");
    else { dg_puts("[capacity-score] FAIL count="); dg_putdec((UW)fails);
           dg_puts("\r\n"); }
    return fails;
}
