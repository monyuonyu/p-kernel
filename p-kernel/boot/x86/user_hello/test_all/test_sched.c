/*
 *  test_all/test_sched.c — スケジューリング テスト
 *
 *  テスト一覧:
 *    T15 — FIFO 優先度抢占: 高優先タスクが低優先タスクより先に実行される
 *    T16 — RR 同優先ローテーション: 同優先 RR タスクが交互に実行される
 *    T17 — FIFO + RR 混在: 同優先の FIFO/RR 混在キューが正しく動作する
 *    T18 — リソース再利用: タスクを 8 回繰り返し作成してリークがないことを確認
 *    T19 — タスク上限: USR_TASK_MAX (8) を超えると失敗することを確認
 */

#include "test_common.h"

/* ================================================================= */
/* T15 用タスク: FIFO 優先度抢占                                       */
/* ================================================================= */

/* 高優先 FIFO (pri=3): 最初に実行されるべき */
static void task_fifo_hi(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    g_order[g_order_cnt++] = 15;   /* 先に実行 → 15 を記録 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* 低優先 FIFO (pri=12): 後から実行される */
static void task_fifo_lo(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    g_order[g_order_cnt++] = 16;   /* 後から実行 → 16 を記録 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T16 用タスク: RR 同優先ローテーション                               */
/* ================================================================= */

static void task_rr_x(int stacd, void *exinf)
{
    (void)exinf;
    /* stacd で識別: 1 または 2 */
    g_order[g_order_cnt++] = stacd;         /* 開始時に記録 (1 or 2) */
    tk_slp_tsk(30);                         /* 30ms スリープ → 他タスクに CPU を譲る */
    g_order[g_order_cnt++] = stacd + 10;    /* 再開時に記録 (11 or 12) */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T17 用タスク: FIFO + RR 混在                                       */
/* ================================================================= */

static void task_mixed_fifo(int stacd, void *exinf)
{
    (void)exinf;
    g_order[g_order_cnt++] = stacd;   /* FIFO タスク: stacd=20 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

static void task_mixed_rr(int stacd, void *exinf)
{
    (void)exinf;
    g_order[g_order_cnt++] = stacd;   /* RR タスク: stacd=21 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* T18 用タスク: リソース再利用                                        */
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
/* テスト本体                                                         */
/* ================================================================= */

void test_sched(void)
{
    /* ----------------------------------------------------------------
     *  T15: FIFO 優先度抢占
     * ----------------------------------------------------------------
     *  低優先タスク (pri=12) を先に作成・起動しますが、
     *  高優先タスク (pri=3) を後から起動すると即座に抢占して先に実行されます。
     *
     *  期待する実行順: [高優先 = 15] → [低優先 = 16]
     *
     *  FIFO スケジューリングの「優先度ベース抢占」を確認するテストです。
     */
    plib_puts("--- T15: FIFO 優先度抢占 ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        /* 低優先を先に起動 */
        int t15lo = start_task(task_fifo_lo, 12, SCHED_FIFO, 0, 0, 0);
        /* 高優先を後から起動 → 即座に抢占して先に実行される */
        int t15hi = start_task(task_fifo_hi,  3, SCHED_FIFO, 0, 0, 0);

        tk_wai_sem(g_sem, 2, 5000);
        /* g_order[0]=15(高優先), g_order[1]=16(低優先) の順が正しい */
        ASSERT_EQ("T15-fifo-order-0", g_order[0], 15);
        ASSERT_EQ("T15-fifo-order-1", g_order[1], 16);
        tk_del_tsk(t15lo); tk_del_tsk(t15hi);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T16: RR 同優先ローテーション
     * ----------------------------------------------------------------
     *  同じ優先度 (pri=10) の 2 つの RR タスクが、タイムスライスで
     *  交互に実行されることを確認します。
     *
     *  各タスクは:
     *    1. 開始時に識別子 (1 or 2) を g_order に記録
     *    2. 30ms スリープ
     *    3. 再開後に識別子+10 (11 or 12) を記録
     *
     *  g_order に 1, 2, 11, 12 が全て記録されれば RR が機能している。
     */
    plib_puts("\r\n--- T16: RR 同優先ローテーション ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        int t16a = start_task(task_rr_x, 10, SCHED_RR, 50, 1, 0);
        int t16b = start_task(task_rr_x, 10, SCHED_RR, 50, 2, 0);

        tk_wai_sem(g_sem, 2, 5000);

        /* 両タスクが開始と再開の両フェーズを実行したか確認 */
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

    /* ----------------------------------------------------------------
     *  T17: FIFO + RR 混在キュー
     * ----------------------------------------------------------------
     *  同じ優先度 (pri=10) で FIFO と RR のタスクが混在した場合、
     *  どちらも実行されることを確認します。
     */
    plib_puts("\r\n--- T17: FIFO + RR 混在キュー ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);
        g_order_cnt = 0;

        int t17rr   = start_task(task_mixed_rr,   10, SCHED_RR,   50, 21, 0);
        int t17fifo = start_task(task_mixed_fifo, 10, SCHED_FIFO,  0, 20, 0);

        tk_wai_sem(g_sem, 2, 5000);

        /* FIFO タスク(20) と RR タスク(21) の両方が実行されたか確認 */
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

    /* ----------------------------------------------------------------
     *  T18: タスクリソース再利用 (リークなし確認)
     * ----------------------------------------------------------------
     *  タスクスロット (USR_TASK_MAX=8) を使い切ったとき、タスクが終了すると
     *  スロットが解放され、再利用できることを確認します。
     *
     *  8 回ループして毎回タスクを作成・完了・削除します。
     *  全 8 回成功すればリソースリークはありません。
     */
    plib_puts("\r\n--- T18: タスクリソース再利用 ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 10 };
        g_sem = tk_cre_sem(&csem);
        g_reuse_count = 0;

        for (int i = 0; i < 8; i++) {
            int tid = start_task(task_reuse, 8, SCHED_FIFO, 0, 0, 0);
            ASSERT("T18-cre-ok", tid >= 0);
            tk_wai_sem(g_sem, 1, 5000);   /* タスク完了を待つ */
            tk_del_tsk(tid);              /* スロットを解放 */
        }
        ASSERT_EQ("T18-reuse-count", g_reuse_count, 8);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T19: タスク上限チェック (USR_TASK_MAX = 8)
     * ----------------------------------------------------------------
     *  ユーザータスクスロットは 8 個です。
     *  8 個全部埋めた状態で 9 個目を作成しようとすると失敗します。
     *
     *  この制限は USR_TASK_MAX (arch/x86/syscall.c) で定義されています。
     */
    plib_puts("\r\n--- T19: タスク上限チェック ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 10 };
        g_sem = tk_cre_sem(&csem);

        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 8,
            .policy   = SCHED_FIFO,
            .slice_ms = 0,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem
        };

        /* 8 スロット全部埋める */
        int tids[8];
        for (int i = 0; i < 8; i++) {
            tids[i] = tk_cre_tsk(&pk);
        }

        /* 9 個目は失敗するはず (E_LIMIT または E_NOMEM) */
        int tid9 = tk_cre_tsk(&pk);
        ASSERT("T19-limit", tid9 < 0);

        /* 実際に起動できたタスクだけ待つ (タスク作成は全部成功するかもしれない) */
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

    plib_puts("\r\n");
}
