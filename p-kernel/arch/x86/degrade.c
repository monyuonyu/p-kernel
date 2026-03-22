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

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void degrade_init(void)
{
    cur_level      = DEGRADE_FULL;
    prev_level     = DEGRADE_FULL;
    transition_cnt = 0;
    alive_node_cnt = 1;  /* 自分自身は常に ALIVE */
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
}
