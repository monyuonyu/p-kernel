/*
 *  dproc.c (x86)
 *  Distributed Process Registry — 分散プロセスレジストリ
 *
 *  K-DDS トピック "proc/0".."proc/7" を使って ELF プロセスの
 *  ライフサイクルをクラスタ全体で追跡する。
 *
 *  設計の核心:
 *    「ユーザーが意図して停止したものは、ノードが死んでも復活しない」
 *    KILLED / EXITED 状態は "墓石" として機能し、
 *    フェイルオーバー時の誤った再起動を防ぐ。
 */

#include "dproc.h"
#include "kdds.h"
#include "vfs.h"
#include "elf_loader.h"
#include "kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void dp_puts(const char *s) { tm_putstring((UB *)s); }

static void dp_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { dp_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    dp_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 文字列ユーティリティ                                                */
/* ------------------------------------------------------------------ */

static INT dp_strlen(const char *s) { INT n = 0; while (s[n]) n++; return n; }

static INT dp_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* パス末尾のファイル名部分を返す: "/foo/bar.elf" → "bar.elf" */
static const char *dp_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) { if (*p == '/') last = p + 1; p++; }
    return last;
}

/* "name" が path の末尾 (basename) と一致するか、または完全パスと一致するか */
static INT dp_name_match(const char *path, const char *name)
{
    if (dp_streq(path, name)) return 1;
    return dp_streq(dp_basename(path), name);
}

static void dp_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src; UB *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

static void dp_memset(void *dst, UB val, INT n)
{
    UB *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = val;
}

/* ------------------------------------------------------------------ */
/* K-DDS トピック名生成: "proc/N"                                     */
/* ------------------------------------------------------------------ */

static void slot_topic_name(INT slot, char *out)
{
    /* "proc/0" .. "proc/7" */
    out[0] = 'p'; out[1] = 'r'; out[2] = 'o'; out[3] = 'c'; out[4] = '/';
    out[5] = (char)('0' + slot);
    out[6] = '\0';
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

static W dproc_handles[DPROC_MAX];   /* K-DDS ハンドル (pub 用) */

/* ------------------------------------------------------------------ */
/* 内部: スロットに DPROC_ENTRY を書き込んで K-DDS へ pub する         */
/* ------------------------------------------------------------------ */

static void pub_entry(INT slot, const DPROC_ENTRY *e)
{
    W h = dproc_handles[slot];
    if (h < 0) return;
    kdds_pub(h, e, (W)sizeof(*e));
}

/* ------------------------------------------------------------------ */
/* dproc_init                                                          */
/* ------------------------------------------------------------------ */

void dproc_init(void)
{
    for (INT s = 0; s < DPROC_MAX; s++) {
        char tname[8];
        slot_topic_name(s, tname);
        dproc_handles[s] = kdds_open(tname, KDDS_QOS_LATEST_ONLY);
    }
    dp_puts("[dproc] process registry ready  slots=");
    dp_putdec((UW)DPROC_MAX);
    dp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* 空きスロット検索                                                    */
/* (K-DDS トピックデータを読んで state を確認する)                     */
/* ------------------------------------------------------------------ */

static INT find_free_slot(void)
{
    char tname[8];
    for (INT s = 0; s < DPROC_MAX; s++) {
        slot_topic_name(s, tname);
        /* kdds_topics[] を直接参照して state を確認 */
        BOOL free_slot = TRUE;
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!dp_streq(kdds_topics[i].name, tname)) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(DPROC_ENTRY)) continue;
            DPROC_ENTRY e;
            dp_memcpy(&e, kdds_topics[i].data, (INT)sizeof(e));
            if (e.state == DPROC_RUNNING) { free_slot = FALSE; break; }
            /* EXITED / KILLED / FREE は再利用可 */
        }
        if (free_slot) return s;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* dproc_register — exec 直後に呼ぶ                                   */
/* ------------------------------------------------------------------ */

void dproc_register(const char *path, ID tid)
{
    INT slot = find_free_slot();
    if (slot < 0) {
        dp_puts("[dproc] slot full, cannot register: ");
        dp_puts(path);
        dp_puts("\r\n");
        return;
    }

    DPROC_ENTRY e;
    dp_memset(&e, 0, (INT)sizeof(e));
    INT plen = dp_strlen(path);
    if (plen >= DPROC_PATH_MAX) plen = DPROC_PATH_MAX - 1;
    dp_memcpy(e.path, path, plen);
    e.path[plen] = '\0';
    e.tid        = (UW)tid;
    e.node_id    = drpc_my_node;
    e.state      = DPROC_RUNNING;
    { SYSTIM st; tk_get_otm(&st); e.start_tick = (UW)st.lo; }

    pub_entry(slot, &e);

    dp_puts("[dproc] registered  slot=");
    dp_putdec((UW)slot);
    dp_puts("  path=");
    dp_puts(path);
    dp_puts("  tid=");
    dp_putdec((UW)tid);
    dp_puts("  node=");
    dp_putdec((UW)drpc_my_node);
    dp_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* dproc_exit_by_tid — 正常終了時に呼ぶ (再起動しない)                */
/* ------------------------------------------------------------------ */

void dproc_exit_by_tid(ID tid)
{
    char tname[8];
    for (INT s = 0; s < DPROC_MAX; s++) {
        slot_topic_name(s, tname);
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!dp_streq(kdds_topics[i].name, tname)) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(DPROC_ENTRY)) continue;

            DPROC_ENTRY e;
            dp_memcpy(&e, kdds_topics[i].data, (INT)sizeof(e));
            if (e.tid != (UW)tid) continue;
            if (e.node_id != drpc_my_node) continue;
            if (e.state != DPROC_RUNNING) continue;

            e.state = DPROC_EXITED;
            pub_entry(s, &e);

            dp_puts("[dproc] exited  slot=");
            dp_putdec((UW)s);
            dp_puts("  path=");
            dp_puts(e.path);
            dp_puts("\r\n");
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* dproc_kill_by_name — 明示的に kill (再起動しない)                  */
/* ------------------------------------------------------------------ */

W dproc_kill_by_name(const char *name)
{
    char tname[8];
    for (INT s = 0; s < DPROC_MAX; s++) {
        slot_topic_name(s, tname);
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!dp_streq(kdds_topics[i].name, tname)) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(DPROC_ENTRY)) continue;

            DPROC_ENTRY e;
            dp_memcpy(&e, kdds_topics[i].data, (INT)sizeof(e));
            if (e.state != DPROC_RUNNING) continue;
            if (!dp_name_match(e.path, name)) continue;

            /* T-Kernel タスクを終了させる (ローカルの場合のみ) */
            if (e.node_id == drpc_my_node) {
                tk_ter_tsk((ID)e.tid);
                tk_del_tsk((ID)e.tid);
            }

            e.state = DPROC_KILLED;
            pub_entry(s, &e);

            dp_puts("[dproc] killed  slot=");
            dp_putdec((UW)s);
            dp_puts("  path=");
            dp_puts(e.path);
            dp_puts("  node=");
            dp_putdec((UW)e.node_id);
            dp_puts("\r\n");
            return 0;
        }
    }
    dp_puts("[dproc] kill: not found: ");
    dp_puts(name);
    dp_puts("\r\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* dproc_on_node_dead — フェイルオーバー                              */
/* RUNNING のプロセスのみ再起動。EXITED/KILLED は再起動しない。        */
/* ------------------------------------------------------------------ */

void dproc_on_node_dead(UB dead_node)
{
    if (!vfs_ready) return;

    char tname[8];
    for (INT s = 0; s < DPROC_MAX; s++) {
        slot_topic_name(s, tname);
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!dp_streq(kdds_topics[i].name, tname)) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(DPROC_ENTRY)) continue;

            DPROC_ENTRY e;
            dp_memcpy(&e, kdds_topics[i].data, (INT)sizeof(e));

            /* RUNNING かつ dead_node のプロセスだけフェイルオーバー */
            if (e.state != DPROC_RUNNING) continue;
            if (e.node_id != dead_node)   continue;

            dp_puts("[dproc] failover: re-exec \"");
            dp_puts(e.path);
            dp_puts("\" from node ");
            dp_putdec((UW)dead_node);
            dp_puts(" -> node ");
            dp_putdec((UW)drpc_my_node);
            dp_puts("\r\n");

            /* ELF を自ノードで再起動 (daemon mode: stdin 不接続) */
            ID new_tid = elf_exec(e.path);
            if (new_tid < E_OK) {
                dp_puts("[dproc] re-exec failed: ");
                dp_puts(e.path);
                dp_puts("\r\n");
                /* スロットを EXITED にして誤った再試行を防ぐ */
                e.state = DPROC_EXITED;
                pub_entry(s, &e);
                continue;
            }

            /* 新しい実行情報でスロットを更新 */
            e.node_id    = drpc_my_node;
            e.tid        = (UW)new_tid;
            e.state      = DPROC_RUNNING;
            { SYSTIM st; tk_get_otm(&st); e.start_tick = (UW)st.lo; }
            pub_entry(s, &e);

            dp_puts("[dproc] re-exec ok  new_tid=");
            dp_putdec((UW)new_tid);
            dp_puts("\r\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* dproc_list — クラスタ全体のプロセス一覧                            */
/* ------------------------------------------------------------------ */

static const char *state_name(UB st)
{
    switch (st) {
    case DPROC_RUNNING: return "RUNNING";
    case DPROC_EXITED:  return "EXITED ";
    case DPROC_KILLED:  return "KILLED ";
    default:            return "FREE   ";
    }
}

void dproc_list(void)
{
    dp_puts("[dproc] cluster process table:\r\n");
    dp_puts("  slot  state    node  tid   path\r\n");

    BOOL any = FALSE;
    char tname[8];
    for (INT s = 0; s < DPROC_MAX; s++) {
        slot_topic_name(s, tname);
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (!kdds_topics[i].open) continue;
            if (!dp_streq(kdds_topics[i].name, tname)) continue;
            if (kdds_topics[i].data_len < (UH)sizeof(DPROC_ENTRY)) continue;

            DPROC_ENTRY e;
            dp_memcpy(&e, kdds_topics[i].data, (INT)sizeof(e));
            if (e.state == DPROC_FREE) continue;

            dp_puts("  "); dp_putdec((UW)s);
            dp_puts("     "); dp_puts(state_name(e.state));
            dp_puts("  "); dp_putdec((UW)e.node_id);
            dp_puts("     "); dp_putdec(e.tid);
            dp_puts("  "); dp_puts(e.path);
            dp_puts("\r\n");
            any = TRUE;
        }
    }
    if (!any) dp_puts("  (no processes registered)\r\n");
}
