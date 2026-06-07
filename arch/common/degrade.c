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

UW capacity_experts(void)
{
    UW n = alive_node_cnt ? alive_node_cnt : 1;   /* ノード ≒ expert */
    if (n > CAP_E_MAX) n = CAP_E_MAX;
    return n;
}

UW capacity_depth(void)
{
    /* pipeline は region 内 (密) で組む。region 未確立なら自分 1 台。 */
    UB rs = (drpc_my_node == 0xFF) ? 1 : region_size();
    if (rs < 1) rs = 1;
    return 1 + floor_log2((UW)rs);
}

UW capacity_kv(void)
{
    if (last_kv_entries > 0) return last_kv_entries;   /* 実測優先 */
    /* 推論前の見積り: region 内ノードがそれぞれ満杯のキャッシュを持つ想定。 */
    UB rs = (drpc_my_node == 0xFF) ? 1 : region_size();
    if (rs < 1) rs = 1;
    return (UW)rs * DKVA_CACHE_SIZE;
}

UW capacity_score(void)
{
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
