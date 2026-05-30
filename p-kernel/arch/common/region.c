/*
 *  region.c
 *  Regions — 遅延クラスタによる脳の領域分割 (R0)
 *
 *  設計: docs/architecture/regions.md
 *  詳細は region.h を参照。
 */

#include "region.h"
#include "swim.h"     /* swim_rtt_ms() */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void rg_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void rg_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rg_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rg_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 状態 (最後に再計算した自 region の局所ビュー)                       */
/* ------------------------------------------------------------------ */

static UB member[DNODE_MAX];   /* 1 = 自 region のメンバ        */
static UB member_cnt = 0;      /* メンバ数 (自分を含む)         */
static UB coord      = 0xFF;   /* coordinator = メンバ内最小 ID */

/* ------------------------------------------------------------------ */
/* 再計算                                                              */
/* ------------------------------------------------------------------ */

void region_recompute(void)
{
    for (UB n = 0; n < DNODE_MAX; n++) member[n] = 0;
    member_cnt = 0;
    coord      = 0xFF;

    UB me = drpc_my_node;
    if (me == 0xFF) return;   /* 分散モード未確立 — region 無し */

    /* 自分は常にメンバ */
    member[me] = 1;
    member_cnt = 1;
    coord      = me;

    /* ALIVE かつ RTT≤τ のノードを取り込む */
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == me) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;
        UW rtt = swim_rtt_ms(n);
        if (rtt == 0xFFFFFFFFUL) continue;   /* 未実測は判定保留 */
        if (rtt <= REGION_TAU_MS) {
            member[n] = 1;
            member_cnt++;
            if (n < coord) coord = n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 問い合わせ API                                                      */
/* ------------------------------------------------------------------ */

UB region_id(void)
{
    region_recompute();
    return coord;
}

UB region_coordinator(void)
{
    region_recompute();
    return coord;
}

BOOL region_contains(UB node)
{
    region_recompute();
    return (node < DNODE_MAX && member[node]) ? TRUE : FALSE;
}

UB region_size(void)
{
    region_recompute();
    return member_cnt;
}

/* ------------------------------------------------------------------ */
/* 表示                                                                */
/* ------------------------------------------------------------------ */

void region_print(void)
{
    region_recompute();

    if (drpc_my_node == 0xFF) {
        rg_puts("[region] single-node (no cluster)\r\n");
        return;
    }

    rg_puts("[region] id="); rg_putdec(coord);
    rg_puts("  coordinator=node"); rg_putdec(coord);
    if (coord == drpc_my_node) rg_puts(" (self)");
    rg_puts("  size="); rg_putdec(member_cnt);
    rg_puts("  tau="); rg_putdec(REGION_TAU_MS); rg_puts("ms\r\n");

    rg_puts("[region] members:");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (!member[n]) continue;
        rg_puts(" node"); rg_putdec(n);
        if (n == drpc_my_node) {
            rg_puts("(self)");
        } else {
            UW rtt = swim_rtt_ms(n);
            rg_puts("(rtt="); rg_putdec(rtt); rg_puts("ms)");
        }
    }
    rg_puts("\r\n");
}
