/*
 *  test_all.c — p-kernel 包括的テストスイート (ring-3 ELF)
 *
 *  テスト対象:
 *    T1  POSIX open/write/read/close               — 基本ファイルI/O
 *    T2  POSIX lseek SEEK_SET / SEEK_END            — シーク操作
 *    T3  POSIX mkdir / rename / unlink              — ディレクトリ・ファイル操作
 *    T4  Semaphore: 基本 signal/wait               — セマフォ同期
 *    T5  Semaphore: cnt>1 (bulk signal/wait)        — 複数カウント
 *    T6  Semaphore: TMO_POL (即時ポーリング)        — タイムアウト
 *    T7  Event flag: TWF_ANDW (全ビット待ち)       — イベントフラグAND待ち
 *    T8  Event flag: TWF_ORW  (任意ビット待ち)     — イベントフラグOR待ち
 *    T9  Event flag: TMO_POL + tk_clr_flg           — クリア操作
 *    T10 tk_ref_tsk — タスク情報読み出し           — 優先度・ポリシー確認
 *    T11 tk_chg_pri — 動的優先度変更               — 優先度変更後 ref_tsk で確認
 *    T12 tk_chg_slt — タイムスライス変更           — 変更後 ref_tsk で確認
 *    T13 tk_wup_tsk — スリープ中タスクを早期起床   — タイムアウト前に起こす
 *    T14 tk_slp_tsk + 自然タイムアウト (E_TMOUT)   — タイムアウト戻り値確認
 *    T15 FIFO タスク優先度抢占                     — 高優先FIFO が先に実行
 *    T16 RR タスク同優先度ローテーション           — 同優先RR で交互実行
 *    T17 FIFO + RR 混在キュー                      — 同優先混在の挙動
 *    T18 タスクリソース再利用                      — exit → 再作成でリーク無し
 *    T19 タスク上限 (9個目は失敗)                  — USR_TASK_MAX 境界
 *    T20 POSIX SYS_READ (複数バイト、部分読み)     — 読み取り正確性
 */

#include "plibc.h"

/* ================================================================= */
/* テスト結果集計                                                     */
/* ================================================================= */

static int g_pass = 0;
static int g_fail = 0;

static void pass(const char *name)
{
    plib_puts("[PASS] ");
    plib_puts(name);
    plib_puts("\r\n");
    g_pass++;
}

static void fail(const char *name, const char *reason)
{
    plib_puts("[FAIL] ");
    plib_puts(name);
    plib_puts(" : ");
    plib_puts(reason);
    plib_puts("\r\n");
    g_fail++;
}

#define ASSERT(name, cond) \
    do { if (cond) pass(name); else fail(name, #cond); } while (0)

#define ASSERT_EQ(name, got, expect) \
    do { \
        int _g = (int)(got); int _e = (int)(expect); \
        if (_g == _e) pass(name); \
        else { plib_puts("[FAIL] "); plib_puts(name); \
               plib_puts(" : got="); plib_puti(_g); \
               plib_puts(" expect="); plib_puti(_e); plib_puts("\r\n"); \
               g_fail++; } \
    } while (0)

/* ================================================================= */
/* 共有グローバル (タスク間通信用)                                    */
/* ================================================================= */

static volatile int g_order[16];   /* 実行順記録バッファ */
static volatile int g_order_cnt;
static int g_sem  = -1;
static int g_flg  = -1;

/* ================================================================= */
/* T4/T5/T6 — セマフォテスト用タスク                                 */
/* ================================================================= */

static void task_sem_signal(int stacd, void *exinf)
{
    (void)stacd;
    int semid = (int)(long)exinf;
    tk_sig_sem(semid, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T7/T8/T9 — イベントフラグテスト用タスク                           */
/* ================================================================= */

static void task_flg_set(int stacd, void *exinf)
{
    (void)stacd;
    /* exinf = packed: high 16bit=flgid, low 16bit=bits */
    int packed = (int)(long)exinf;
    int flgid  = (packed >> 16) & 0xFFFF;
    unsigned int bits = (unsigned int)(packed & 0xFFFF);
    tk_set_flg(flgid, bits);
    tk_ext_tsk();
}

/* ================================================================= */
/* T13 — tk_wup_tsk テスト用タスク                                   */
/* ================================================================= */

static volatile int g_wup_done = 0;

static void task_wup_target(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* TMO_FEVR でスリープ — main から wup される */
    int r = tk_slp_tsk(TMO_FEVR);
    g_wup_done = (r == 0) ? 1 : -1;  /* 0=正常起床 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T14 — tk_slp_tsk 自然タイムアウトテスト用タスク                   */
/* ================================================================= */

static volatile int g_tmout_result = 999;

static void task_slp_timeout(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* 50ms でタイムアウトするはず */
    int r = tk_slp_tsk(50);
    g_tmout_result = r;     /* E_TMOUT (-8) を期待 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T15 — FIFO 優先度抢占テスト用タスク                               */
/* ================================================================= */

static void task_fifo_hi(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* 高優先FIFO (pri=3) — 先に実行されるべき */
    g_order[g_order_cnt++] = 15;
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

static void task_fifo_lo(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* 低優先FIFO (pri=12) — 後に実行される */
    g_order[g_order_cnt++] = 16;
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T16 — RR ローテーションテスト用タスク                             */
/* ================================================================= */

static void task_rr_x(int stacd, void *exinf)
{
    (void)exinf;
    /* stacd に識別子 (1 or 2) を渡す */
    g_order[g_order_cnt++] = stacd;
    tk_slp_tsk(30);  /* 30ms スリープ → yield */
    g_order[g_order_cnt++] = stacd + 10;
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T17 — FIFO + RR 混在テスト用タスク                               */
/* ================================================================= */

static void task_mixed_fifo(int stacd, void *exinf)
{
    (void)exinf;
    g_order[g_order_cnt++] = stacd;  /* FIFO タスク = 20 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

static void task_mixed_rr(int stacd, void *exinf)
{
    (void)exinf;
    g_order[g_order_cnt++] = stacd;  /* RR タスク = 21 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T18 — タスクリソース再利用テスト用タスク                           */
/* ================================================================= */

static volatile int g_reuse_count = 0;

static void task_reuse(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    g_reuse_count++;
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* ヘルパー: タスク作成 + 起動 ショートカット                        */
/* ================================================================= */

static int start_task(void (*fn)(int, void*), int pri, int policy,
                      int slice_ms, int stacd, void *exinf)
{
    PK_CRE_TSK pk = {
        .task     = fn,
        .pri      = pri,
        .policy   = policy,
        .slice_ms = slice_ms,
        .stksz    = 0,
        .exinf    = exinf
    };
    int tid = tk_cre_tsk(&pk);
    if (tid < 0) return tid;
    tk_sta_tsk(tid, stacd);
    return tid;
}

/* ================================================================= */
/* テスト本体                                                         */
/* ================================================================= */

void _start(void)
{
    plib_puts("\r\n");
    plib_puts("============================================\r\n");
    plib_puts("  p-kernel test_all.c — 包括的テストスイート\r\n");
    plib_puts("============================================\r\n\r\n");

    /* ---- T1: POSIX open / write / read / close ----------- */
    plib_puts("--- T1: POSIX 基本ファイルI/O ---\r\n");
    {
        int fd = sys_open("/t1.txt", O_WRONLY | O_CREAT);
        ASSERT("T1-open", fd >= 3);
        if (fd >= 3) {
            int w = sys_write(fd, "HELLO", 5);
            ASSERT_EQ("T1-write", w, 5);
            sys_close(fd);

            fd = sys_open("/t1.txt", O_RDONLY);
            ASSERT("T1-reopen", fd >= 3);
            char buf[8] = {0};
            int r = sys_read(fd, buf, 7);
            ASSERT_EQ("T1-read-len", r, 5);
            ASSERT("T1-read-data", buf[0]=='H' && buf[4]=='O');
            sys_close(fd);
        }
        sys_unlink("/t1.txt");
    }

    /* ---- T2: POSIX lseek SEEK_SET / SEEK_END ------------- */
    plib_puts("\r\n--- T2: lseek SEEK_SET / SEEK_END ---\r\n");
    {
        int fd = sys_open("/t2.txt", O_WRONLY | O_CREAT);
        sys_write(fd, "ABCDE", 5);
        sys_close(fd);

        fd = sys_open("/t2.txt", O_RDONLY);
        sys_lseek(fd, 2, SEEK_SET);
        char buf[4] = {0};
        int r = sys_read(fd, buf, 3);
        ASSERT_EQ("T2-lseek-set-len", r, 3);
        ASSERT("T2-lseek-set-data", buf[0]=='C' && buf[1]=='D' && buf[2]=='E');

        /* SEEK_END: ファイル末尾に移動 → 読んでも 0 bytes */
        sys_lseek(fd, 0, SEEK_END);
        r = sys_read(fd, buf, 3);
        ASSERT_EQ("T2-lseek-end", r, 0);
        sys_close(fd);
        sys_unlink("/t2.txt");
    }

    /* ---- T3: POSIX mkdir / rename / unlink --------------- */
    plib_puts("\r\n--- T3: mkdir / rename / unlink ---\r\n");
    {
        /* 前回テストの残骸をクリーンアップ (ディレクトリ内ファイルのみ) */
        sys_unlink("/testdir/f.txt");
        sys_unlink("/testdir/g.txt");

        /* mkdir: 新規作成(0)またはすでに存在(-1)でもディレクトリが使えればOK */
        sys_mkdir("/testdir");
        int probe = sys_open("/testdir/.ok", O_WRONLY | O_CREAT);
        int r = (probe >= 3) ? 0 : -1;
        if (probe >= 3) { sys_close(probe); sys_unlink("/testdir/.ok"); }
        ASSERT("T3-mkdir", r == 0);

        int fd = sys_open("/testdir/f.txt", O_WRONLY | O_CREAT);
        ASSERT("T3-open-in-dir", fd >= 3);
        if (fd >= 3) sys_close(fd);

        r = sys_rename("/testdir/f.txt", "/testdir/g.txt");
        ASSERT_EQ("T3-rename", r, 0);

        /* 旧名は存在しない (open失敗) */
        int fd2 = sys_open("/testdir/f.txt", O_RDONLY);
        ASSERT("T3-old-gone", fd2 < 0);

        sys_unlink("/testdir/g.txt");
        sys_unlink("/testdir");
    }

    /* ---- T20: POSIX 部分読み (lseek後) ------------------- */
    plib_puts("\r\n--- T20: POSIX 部分読み確認 ---\r\n");
    {
        int fd = sys_open("/t20.txt", O_WRONLY | O_CREAT);
        sys_write(fd, "0123456789", 10);
        sys_close(fd);

        fd = sys_open("/t20.txt", O_RDONLY);
        char buf[12] = {0};
        sys_lseek(fd, 5, SEEK_SET);
        int r = sys_read(fd, buf, 4);
        ASSERT_EQ("T20-partial-len", r, 4);
        ASSERT("T20-partial-data", buf[0]=='5' && buf[3]=='8');
        sys_close(fd);
        sys_unlink("/t20.txt");
    }

    /* ---- T4: セマフォ基本 signal/wait -------------------- */
    plib_puts("\r\n--- T4: セマフォ基本 signal/wait ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        ASSERT("T4-cre-sem", g_sem >= 0);

        int t4_tid = start_task(task_sem_signal, 8, SCHED_FIFO, 0,
                   0, (void*)(long)g_sem);
        int r = tk_wai_sem(g_sem, 1, 5000);
        ASSERT_EQ("T4-wai-sem", r, 0);
        tk_del_tsk(t4_tid);
        tk_del_sem(g_sem);
    }

    /* ---- T5: セマフォ cnt>1 bulk signal/wait ------------- */
    plib_puts("\r\n--- T5: セマフォ cnt>1 (bulk) ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 8 };
        g_sem = tk_cre_sem(&csem);
        ASSERT("T5-cre-sem", g_sem >= 0);

        /* 3タスクそれぞれが +1 シグナル → 合計 +3 */
        int t5a = start_task(task_sem_signal, 8, SCHED_FIFO, 0,
                   0, (void*)(long)g_sem);
        int t5b = start_task(task_sem_signal, 8, SCHED_FIFO, 0,
                   0, (void*)(long)g_sem);
        int t5c = start_task(task_sem_signal, 8, SCHED_FIFO, 0,
                   0, (void*)(long)g_sem);
        /* cnt=3 を一度に消費 */
        int r = tk_wai_sem(g_sem, 3, 5000);
        ASSERT_EQ("T5-wai-bulk", r, 0);
        tk_del_tsk(t5a); tk_del_tsk(t5b); tk_del_tsk(t5c);
        tk_del_sem(g_sem);
    }

    /* ---- T6: セマフォ TMO_POL ------------------------------ */
    plib_puts("\r\n--- T6: セマフォ TMO_POL ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        /* カウント0 → TMO_POL は失敗 (E_TMOUT = -50) */
        int r = tk_wai_sem(g_sem, 1, TMO_POL);
        ASSERT_EQ("T6-pol-empty", r, -50);

        /* +1 シグナル後 → TMO_POL 成功 */
        tk_sig_sem(g_sem, 1);
        r = tk_wai_sem(g_sem, 1, TMO_POL);
        ASSERT_EQ("T6-pol-avail", r, 0);
        tk_del_sem(g_sem);
    }

    /* ---- T7: イベントフラグ TWF_ANDW ---------------------- */
    plib_puts("\r\n--- T7: イベントフラグ TWF_ANDW ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };
        g_flg = tk_cre_flg(&cflg);
        ASSERT("T7-cre-flg", g_flg >= 0);

        int packed_a = (g_flg << 16) | 0x01;
        int packed_b = (g_flg << 16) | 0x02;
        int t7a = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                   0, (void*)(long)packed_a);
        int t7b = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                   0, (void*)(long)packed_b);

        unsigned int ptn = 0;
        int r = tk_wai_flg(g_flg, 0x03, TWF_ANDW, &ptn, 5000);
        ASSERT_EQ("T7-andw-ret", r, 0);
        ASSERT_EQ("T7-andw-ptn", (int)ptn, 0x03);
        tk_del_tsk(t7a); tk_del_tsk(t7b);
        tk_del_flg(g_flg);
    }

    /* ---- T8: イベントフラグ TWF_ORW ----------------------- */
    plib_puts("\r\n--- T8: イベントフラグ TWF_ORW ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };
        g_flg = tk_cre_flg(&cflg);

        int packed = (g_flg << 16) | 0x04;
        int t8a = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                   0, (void*)(long)packed);

        unsigned int ptn = 0;
        /* bit0|bit2 を待つ — bit2 だけセットされても OK */
        int r = tk_wai_flg(g_flg, 0x05, TWF_ORW, &ptn, 5000);
        ASSERT_EQ("T8-orw-ret", r, 0);
        ASSERT("T8-orw-ptn", (ptn & 0x04) != 0);
        tk_del_tsk(t8a);
        tk_del_flg(g_flg);
    }

    /* ---- T9: イベントフラグ TMO_POL + tk_clr_flg ---------- */
    plib_puts("\r\n--- T9: イベントフラグ TMO_POL + clr ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };
        g_flg = tk_cre_flg(&cflg);

        /* パターン0 → POL 失敗 */
        unsigned int ptn = 0;
        int r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-pol-empty", r, -50);

        /* set → POL 成功 */
        tk_set_flg(g_flg, 0xFF);
        r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-pol-hit", r, 0);

        /* clr → 再度 POL 失敗 */
        tk_clr_flg(g_flg, ~0x01u);   /* bit0 以外をクリア */
        tk_clr_flg(g_flg, 0x00);     /* 全クリア */
        r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-after-clr", r, -50);
        tk_del_flg(g_flg);
    }

    /* ---- T10: tk_ref_tsk ------------------------------------- */
    plib_puts("\r\n--- T10: tk_ref_tsk ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);

        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 7,
            .policy   = SCHED_RR,
            .slice_ms = 80,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem
        };
        int tid = tk_cre_tsk(&pk);
        ASSERT("T10-cre", tid >= 0);

        PK_REF_TSK ref;
        int r = tk_ref_tsk(tid, &ref);
        ASSERT_EQ("T10-ref-ret", r, 0);
        ASSERT_EQ("T10-ref-pri", ref.pri, 7);
        ASSERT_EQ("T10-ref-policy", ref.policy, SCHED_RR);
        ASSERT_EQ("T10-ref-slice", ref.slice_ms, 80);

        tk_sta_tsk(tid, 0);
        tk_wai_sem(g_sem, 1, 5000);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ---- T11: tk_chg_pri ------------------------------------- */
    plib_puts("\r\n--- T11: tk_chg_pri ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);

        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 10,
            .policy   = SCHED_FIFO,
            .slice_ms = 0,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem
        };
        int tid = tk_cre_tsk(&pk);
        ASSERT("T11-cre", tid >= 0);

        /* 起動前に優先度変更 */
        int r = tk_chg_pri(tid, 5);
        ASSERT_EQ("T11-chg-ret", r, 0);

        PK_REF_TSK ref;
        tk_ref_tsk(tid, &ref);
        ASSERT_EQ("T11-chg-verify", ref.pri, 5);

        tk_sta_tsk(tid, 0);
        tk_wai_sem(g_sem, 1, 5000);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ---- T12: tk_chg_slt ------------------------------------- */
    plib_puts("\r\n--- T12: tk_chg_slt ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);

        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 10,
            .policy   = SCHED_RR,
            .slice_ms = 100,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem
        };
        int tid = tk_cre_tsk(&pk);
        ASSERT("T12-cre", tid >= 0);

        int r = tk_chg_slt(tid, 200);
        ASSERT_EQ("T12-chg-ret", r, 0);

        PK_REF_TSK ref;
        tk_ref_tsk(tid, &ref);
        ASSERT_EQ("T12-slt-verify", ref.slice_ms, 200);

        tk_sta_tsk(tid, 0);
        tk_wai_sem(g_sem, 1, 5000);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ---- T13: tk_wup_tsk ------------------------------------- */
    plib_puts("\r\n--- T13: tk_wup_tsk (早期起床) ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);
        g_wup_done = 0;

        int tid = start_task(task_wup_target, 8, SCHED_FIFO,
                             0, 0, 0);
        ASSERT("T13-cre", tid >= 0);

        /* 少し待ってから起こす */
        tk_slp_tsk(50);
        int r = tk_wup_tsk(tid);
        ASSERT_EQ("T13-wup-ret", r, 0);

        /* タスクが g_sem を signal するまで待つ */
        tk_wai_sem(g_sem, 1, 5000);
        ASSERT_EQ("T13-wup-done", g_wup_done, 1);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ---- T14: tk_slp_tsk 自然タイムアウト ------------------- */
    plib_puts("\r\n--- T14: tk_slp_tsk 自然タイムアウト ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);
        g_tmout_result = 999;

        int t14_tid = start_task(task_slp_timeout, 8, SCHED_FIFO, 0, 0, 0);

        /* タスクの50msタイムアウトが完了するまで待つ */
        tk_wai_sem(g_sem, 1, 5000);
        /* E_TMOUT = -50 を期待 */
        ASSERT_EQ("T14-tmout-val", g_tmout_result, -50);
        tk_del_tsk(t14_tid);
        tk_del_sem(g_sem);
    }

    /* ---- T15: FIFO 優先度抢占 -------------------------------- */
    plib_puts("\r\n--- T15: FIFO 優先度抢占 ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        /* 低優先を先に作成→起動 */
        int t15lo = start_task(task_fifo_lo, 12, SCHED_FIFO, 0, 0, 0);
        /* 高優先を後から起動 → 即抢占して先に実行 */
        int t15hi = start_task(task_fifo_hi, 3, SCHED_FIFO, 0, 0, 0);

        tk_wai_sem(g_sem, 2, 5000);
        /* g_order[0]=15(hi), g_order[1]=16(lo) の順が正しい */
        ASSERT_EQ("T15-fifo-order-0", g_order[0], 15);
        ASSERT_EQ("T15-fifo-order-1", g_order[1], 16);
        tk_del_tsk(t15lo); tk_del_tsk(t15hi);
        tk_del_sem(g_sem);
    }

    /* ---- T16: RR 同優先ローテーション ----------------------- */
    plib_puts("\r\n--- T16: RR 同優先ローテーション ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        /* 同優先 RR タスク2個 — スリープ後に交互実行される */
        int t16a = start_task(task_rr_x, 10, SCHED_RR, 50, 1, 0);
        int t16b = start_task(task_rr_x, 10, SCHED_RR, 50, 2, 0);

        tk_wai_sem(g_sem, 2, 5000);
        /* 各タスクが 2回ずつ (start + sleep後) 記録する */
        /* 両タスクの記録が混在すればRRが機能している */
        int saw1 = 0, saw2 = 0, saw11 = 0, saw12 = 0;
        for (int i = 0; i < g_order_cnt; i++) {
            if (g_order[i] == 1)  saw1  = 1;
            if (g_order[i] == 2)  saw2  = 1;
            if (g_order[i] == 11) saw11 = 1;
            if (g_order[i] == 12) saw12 = 1;
        }
        ASSERT("T16-rr-task1-ran", saw1 && saw11);
        ASSERT("T16-rr-task2-ran", saw2 && saw12);
        tk_del_tsk(t16a); tk_del_tsk(t16b);
        tk_del_sem(g_sem);
    }

    /* ---- T17: FIFO + RR 混在 --------------------------------- */
    plib_puts("\r\n--- T17: FIFO + RR 混在キュー ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        /* 同優先 pri=10: FIFO=20, RR=21 */
        int t17rr   = start_task(task_mixed_rr,   10, SCHED_RR,   50, 21, 0);
        int t17fifo = start_task(task_mixed_fifo, 10, SCHED_FIFO,  0, 20, 0);

        tk_wai_sem(g_sem, 2, 5000);
        /* どちらも実行されたことを確認 */
        int saw20 = 0, saw21 = 0;
        for (int i = 0; i < g_order_cnt; i++) {
            if (g_order[i] == 20) saw20 = 1;
            if (g_order[i] == 21) saw21 = 1;
        }
        ASSERT("T17-mixed-fifo-ran", saw20);
        ASSERT("T17-mixed-rr-ran",   saw21);
        tk_del_tsk(t17rr); tk_del_tsk(t17fifo);
        tk_del_sem(g_sem);
    }

    /* ---- T18: タスクリソース再利用 (リークなし確認) ---------- */
    plib_puts("\r\n--- T18: タスクリソース再利用 ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 10 };
        g_sem = tk_cre_sem(&csem);
        g_reuse_count = 0;

        /* 8スロット分繰り返す (USR_TASK_MAX=8) */
        for (int i = 0; i < 8; i++) {
            int tid = start_task(task_reuse, 8, SCHED_FIFO, 0, 0, 0);
            ASSERT("T18-cre-ok", tid >= 0);
            tk_wai_sem(g_sem, 1, 5000);
            tk_del_tsk(tid);
        }
        ASSERT_EQ("T18-reuse-count", g_reuse_count, 8);
        tk_del_sem(g_sem);
    }

    /* ---- T19: タスク上限 (9個目は失敗) ----------------------- */
    plib_puts("\r\n--- T19: タスク上限チェック ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 10 };
        g_sem = tk_cre_sem(&csem);

        /* 8スロット全部埋める */
        int tids[8];
        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 8,
            .policy   = SCHED_FIFO,
            .slice_ms = 0,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem
        };
        for (int i = 0; i < 8; i++) {
            tids[i] = tk_cre_tsk(&pk);
        }
        /* 9個目は失敗するはず */
        int tid9 = tk_cre_tsk(&pk);
        ASSERT("T19-limit", tid9 < 0);

        /* 全タスクを起動してスロットを解放 */
        int t19_started = 0;
        for (int i = 0; i < 8; i++) {
            if (tids[i] >= 0) { tk_sta_tsk(tids[i], 0); t19_started++; }
        }
        tk_wai_sem(g_sem, t19_started, 5000);
        for (int i = 0; i < 8; i++) {
            if (tids[i] >= 0) tk_del_tsk(tids[i]);
        }
        tk_del_sem(g_sem);
    }

    /* ================================================================= */
    /* 最終結果                                                            */
    /* ================================================================= */
    plib_puts("\r\n============================================\r\n");
    plib_puts("  結果: PASS=");
    plib_puti(g_pass);
    plib_puts("  FAIL=");
    plib_puti(g_fail);
    plib_puts("\r\n");
    if (g_fail == 0)
        plib_puts("  全テスト合格!\r\n");
    else
        plib_puts("  失敗あり — 上記 [FAIL] を確認してください\r\n");
    plib_puts("============================================\r\n");

    sys_exit(g_fail == 0 ? 0 : 1);
}
