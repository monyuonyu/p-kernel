/*
 *  heal.c (x86)
 *  Self-Healing Kernel — フェーズ 3
 *
 *  DEAD ノードのガード済みタスクを、後継ノードが自動的に引き継ぐ。
 *
 *  後継ノード選択:
 *    dnode_table[] を走査し、ALIVE 状態で最も ID が小さいノードを選ぶ。
 *    自分自身も候補に含める (drpc_my_node は常に生存しているとみなす)。
 */

#include "heal.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* drpc.c で定義された公開ラッパー (heal.c から呼ぶ用) */
IMPORT W drpc_local_restart(UH func_id, INT pri, UB caller_node);

static void hl_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void hl_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { hl_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    hl_puts(&buf[i]);
}

static void hl_puthex4(UH v)
{
    char buf[5]; buf[4] = '\0';
    for (INT k = 3; k >= 0; k--) {
        INT d = (INT)(v & 0xF);
        buf[k] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    hl_puts(buf);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

static HEAL_GUARD guards[HEAL_GUARD_MAX];
static BOOL       heal_triggered[DNODE_MAX];   /* 二重発火防止フラグ */

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void heal_init(void)
{
    for (INT i = 0; i < HEAL_GUARD_MAX; i++) guards[i].active = 0;
    for (INT i = 0; i < DNODE_MAX;      i++) heal_triggered[i] = FALSE;
    hl_puts("[heal] initialized\r\n");
}

/* ------------------------------------------------------------------ */
/* ガード登録                                                          */
/* ------------------------------------------------------------------ */

void heal_register(const char *name, UH func_id, UB home_node, W priority)
{
    for (INT i = 0; i < HEAL_GUARD_MAX; i++) {
        if (guards[i].active) continue;
        INT j = 0;
        while (name[j] && j < 31) { guards[i].name[j] = name[j]; j++; }
        guards[i].name[j]   = '\0';
        guards[i].func_id      = func_id;
        guards[i].home_node    = home_node;
        guards[i].current_node = home_node;
        guards[i].priority     = priority;
        guards[i].active       = 1;
        hl_puts("[heal] guard \""); hl_puts(guards[i].name);
        hl_puts("\" func=0x"); hl_puthex4(func_id);
        hl_puts(" home="); hl_putdec(home_node);
        hl_puts(" pri="); hl_putdec((UW)priority);
        hl_puts("\r\n");
        return;
    }
    hl_puts("[heal] guard table full\r\n");
}

/* ------------------------------------------------------------------ */
/* DEAD 検出時のコールバック                                           */
/* ------------------------------------------------------------------ */

void heal_on_node_dead(UB dead_node)
{
    if (dead_node >= DNODE_MAX) return;

    /* 二重発火防止 */
    if (heal_triggered[dead_node]) return;
    heal_triggered[dead_node] = TRUE;

    /* 後継ノード選択: ALIVE で最小 ID (自分自身も候補) */
    UB heir = 0xFF;
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == dead_node) continue;
        if (n == drpc_my_node) {
            /* 自分自身は常に生存しているとみなす */
            if (heir == 0xFF || n < heir) heir = n;
            continue;
        }
        if (dnode_table[n].state == DNODE_ALIVE) {
            if (heir == 0xFF || n < heir) heir = n;
        }
    }

    hl_puts("[heal] node "); hl_putdec(dead_node);
    hl_puts(" DEAD  heir=");
    if (heir == 0xFF) { hl_puts("none\r\n"); return; }
    hl_putdec(heir);
    hl_puts(heir == drpc_my_node ? " (me)\r\n" : "\r\n");

    /* 自分が後継でなければ何もしない */
    if (heir != drpc_my_node) return;

    /* ガードを走査: current_node == dead_node のタスクを引き継ぐ
     * (home_node ではなく current_node を使うことで連鎖継承を実現) */
    for (INT i = 0; i < HEAL_GUARD_MAX; i++) {
        if (!guards[i].active) continue;
        if (guards[i].current_node != dead_node) continue;

        hl_puts("[heal] taking over \""); hl_puts(guards[i].name);
        hl_puts("\" from node "); hl_putdec(dead_node);
        if (guards[i].home_node != dead_node) {
            hl_puts(" (cascade: original home=");
            hl_putdec(guards[i].home_node); hl_puts(")");
        }
        hl_puts("\r\n");

        W r = drpc_local_restart(guards[i].func_id,
                                 (INT)guards[i].priority,
                                 dead_node);
        if (r < E_OK) {
            hl_puts("[heal] restart failed err=");
            hl_putdec((UW)(-r)); hl_puts("\r\n");
        } else {
            hl_puts("[heal] restarted tid="); hl_putdec((UW)r); hl_puts("\r\n");
            /* 連鎖継承: current_node を自分に更新 */
            guards[i].current_node = drpc_my_node;
        }
    }
}

/* ------------------------------------------------------------------ */
/* ガード一覧表示                                                      */
/* ------------------------------------------------------------------ */

void heal_list(void)
{
    hl_puts("[heal] guard table:\r\n");
    hl_puts("  #  name             func   home  current  pri\r\n");
    INT found = 0;
    for (INT i = 0; i < HEAL_GUARD_MAX; i++) {
        if (!guards[i].active) continue;
        found++;
        hl_puts("  "); hl_putdec((UW)i);
        hl_puts("  "); hl_puts(guards[i].name);
        hl_puts("  0x"); hl_puthex4(guards[i].func_id);
        hl_puts("  "); hl_putdec(guards[i].home_node);
        hl_puts("     ");
        hl_putdec(guards[i].current_node);
        if (guards[i].current_node != guards[i].home_node)
            hl_puts("(!)");   /* 連鎖継承中 */
        else
            hl_puts("   ");
        hl_puts("  "); hl_putdec((UW)guards[i].priority);
        hl_puts("\r\n");
    }
    if (!found) hl_puts("  (no guards registered)\r\n");
}
