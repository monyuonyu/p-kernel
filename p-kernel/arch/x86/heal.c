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
#include "dproc.h"
#include "elf_loader.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT BOOL vfs_ready;

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

static HEAL_GUARD     guards[HEAL_GUARD_MAX];
static BOOL           heal_triggered[DNODE_MAX];
static HEAL_ELF_GUARD elf_guards[HEAL_ELF_GUARD_MAX];

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void heal_init(void)
{
    for (INT i = 0; i < HEAL_GUARD_MAX; i++) guards[i].active = 0;
    for (INT i = 0; i < DNODE_MAX;      i++) heal_triggered[i] = FALSE;
    hl_puts("[heal] initialized\r\n");
}

void heal_elf_init(void)
{
    for (INT i = 0; i < HEAL_ELF_GUARD_MAX; i++) {
        elf_guards[i].active = 0;
        elf_guards[i].tid    = -1;
    }
    hl_puts("[heal] ELF watchdog ready\r\n");
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

    /* Phase 9: ユーザープロセスのフェイルオーバー
     * RUNNING だったプロセスのみ再起動。KILLED/EXITED は再起動しない。 */
    dproc_on_node_dead(dead_node);

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
    hl_puts("[heal] kernel task guards:\r\n");
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
            hl_puts("(!)");
        else
            hl_puts("   ");
        hl_puts("  "); hl_putdec((UW)guards[i].priority);
        hl_puts("\r\n");
    }
    if (!found) hl_puts("  (none)\r\n");

    hl_puts("[heal] ELF daemon guards:\r\n");
    hl_puts("  #  path                              tid   pri\r\n");
    found = 0;
    for (INT i = 0; i < HEAL_ELF_GUARD_MAX; i++) {
        if (!elf_guards[i].active) continue;
        found++;
        hl_puts("  "); hl_putdec((UW)i);
        hl_puts("  "); hl_puts(elf_guards[i].path);
        hl_puts("  "); hl_putdec((UW)elf_guards[i].tid);
        hl_puts("  "); hl_putdec((UW)elf_guards[i].priority);
        hl_puts("\r\n");
    }
    if (!found) hl_puts("  (none)\r\n");
}

/* ------------------------------------------------------------------ */
/* ring-3 ELF デーモン watchdog                                       */
/* ------------------------------------------------------------------ */

static BOOL hl_streq(const char *a, const char *b)
{
    INT i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return FALSE; i++; }
    return a[i] == '\0' && b[i] == '\0';
}

static void hl_strcpy(char *dst, const char *src, INT max)
{
    INT i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void heal_elf_register(const char *path, W priority)
{
    for (INT i = 0; i < HEAL_ELF_GUARD_MAX; i++) {
        if (elf_guards[i].active) continue;
        hl_strcpy(elf_guards[i].path, path, 64);
        elf_guards[i].tid      = -1;
        elf_guards[i].priority = priority;
        elf_guards[i].active   = 1;
        hl_puts("[heal] ELF guard \""); hl_puts(path);
        hl_puts("\" pri="); hl_putdec((UW)priority);
        hl_puts("\r\n");
        return;
    }
    hl_puts("[heal] ELF guard table full\r\n");
}

void heal_elf_update_tid(const char *path, ID tid)
{
    for (INT i = 0; i < HEAL_ELF_GUARD_MAX; i++) {
        if (!elf_guards[i].active) continue;
        if (!hl_streq(elf_guards[i].path, path)) continue;
        elf_guards[i].tid = tid;
        return;
    }
}

void heal_elf_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* 起動直後は他タスクの初期化を待つ */
    tk_dly_tsk(5000);

    for (;;) {
        if (vfs_ready) {
            for (INT i = 0; i < HEAL_ELF_GUARD_MAX; i++) {
                if (!elf_guards[i].active) continue;
                if (elf_guards[i].tid < 0)  continue;

                /* TID が DORMANT または存在しなければ死亡とみなす */
                T_RTSK rtsk;
                ER er = tk_ref_tsk((ID)elf_guards[i].tid, &rtsk);
                BOOL dead = (er == E_NOEXS) ||
                            (er == E_OK && (rtsk.tskstat & TTS_DMT));
                if (!dead) continue;

                hl_puts("[heal] ELF dead: "); hl_puts(elf_guards[i].path);
                hl_puts("  restarting...\r\n");

                tk_dly_tsk(500);   /* 短い冷却時間 */

                ID new_tid = elf_exec(elf_guards[i].path);
                if (new_tid >= E_OK) {
                    elf_guards[i].tid = new_tid;
                    dproc_register(elf_guards[i].path, new_tid);
                    hl_puts("[heal] ELF restarted  tid=");
                    hl_putdec((UW)new_tid); hl_puts("\r\n");
                } else {
                    hl_puts("[heal] ELF restart failed\r\n");
                }
            }
        }
        tk_dly_tsk(HEAL_WATCH_INTERVAL);
    }
}
