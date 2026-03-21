/*
 *  05_all_demo/task.c — タスク管理 デモ
 *
 *  デモ一覧:
 *    D10 — tk_ref_tsk:  タスク情報を参照 (優先度・ポリシー・スライス)
 *    D11 — tk_chg_pri:  実行中タスクの優先度を動的に変更
 *    D12 — tk_chg_slt:  タイムスライスを動的に変更
 *    D13 — tk_wup_tsk:  タイムアウト前にタスクを早期起床させる
 *    D14 — tk_slp_tsk:  タイムアウトで自然に起床 (E_TMOUT 確認)
 */

#include "common.h"

/* ================================================================= */
/* T13 用タスク: TMO_FEVR でスリープ、tk_wup_tsk で早期起床を受け取る */
/* ================================================================= */

static volatile int g_wup_done = 0;

static void task_wup_target(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /*
     *  TMO_FEVR で永遠にスリープする。
     *  メインが tk_wup_tsk を呼ぶと、r=0 (正常起床) で戻る。
     */
    int r = tk_slp_tsk(TMO_FEVR);
    g_wup_done = (r == 0) ? 1 : -1;
    tk_sig_sem(g_sem, 1);   /* メインに完了を通知 */
    tk_ext_tsk();
}

/* ================================================================= */
/* T14 用タスク: 指定時間でタイムアウト、E_TMOUT を確認する            */
/* ================================================================= */

static volatile int g_tmout_result = 999;

static void task_slp_timeout(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /*
     *  50ms でタイムアウトするはず。
     *  E_TMOUT (-50) が返ることを確認する。
     */
    int r = tk_slp_tsk(50);
    g_tmout_result = r;
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* テスト本体                                                         */
/* ================================================================= */

void demo_task(void)
{
    /* ----------------------------------------------------------------
     *  T10: tk_ref_tsk — タスク情報の参照
     * ----------------------------------------------------------------
     *  タスクを作成した直後に tk_ref_tsk で情報を読み出し、
     *  作成時に指定した優先度・ポリシー・スライスと一致するか確認します。
     */
    plib_puts("--- T10: tk_ref_tsk ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);

        PK_CRE_TSK pk = {
            .task     = task_sem_signal,
            .pri      = 7,
            .policy   = SCHED_RR,
            .slice_ms = 80,
            .stksz    = 0,
            .exinf    = (void*)(long)g_sem   /* セマフォ ID を渡す */
        };
        int tid = tk_cre_tsk(&pk);
        ASSERT("T10-cre", tid >= 0);

        PK_REF_TSK ref;
        int r = tk_ref_tsk(tid, &ref);
        ASSERT_EQ("T10-ref-ret",    r,           0);
        ASSERT_EQ("T10-ref-pri",    ref.pri,      7);
        ASSERT_EQ("T10-ref-policy", ref.policy,   SCHED_RR);
        ASSERT_EQ("T10-ref-slice",  ref.slice_ms, 80);

        tk_sta_tsk(tid, 0);
        tk_wai_sem(g_sem, 1, 5000);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T11: tk_chg_pri — 動的優先度変更
     * ----------------------------------------------------------------
     *  DORMANT 状態のタスクの優先度を tk_chg_pri で変更し、
     *  tk_ref_tsk で変更後の優先度を確認します。
     *
     *  作成時 pri=10 → tk_chg_pri で pri=5 に変更 → ref_tsk で 5 を確認
     */
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

        /* 起動前に優先度を 10 → 5 に変更 */
        int r = tk_chg_pri(tid, 5);
        ASSERT_EQ("T11-chg-ret", r, 0);

        PK_REF_TSK ref;
        tk_ref_tsk(tid, &ref);
        ASSERT_EQ("T11-chg-verify", ref.pri, 5);   /* 5 になっているはず */

        tk_sta_tsk(tid, 0);
        tk_wai_sem(g_sem, 1, 5000);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T12: tk_chg_slt — タイムスライス動的変更
     * ----------------------------------------------------------------
     *  DORMANT 状態のタスクのタイムスライスを tk_chg_slt で変更し、
     *  tk_ref_tsk で変更後の値を確認します。
     *
     *  作成時 slice_ms=100 → tk_chg_slt で 200 に変更 → ref_tsk で 200 を確認
     */
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

    /* ----------------------------------------------------------------
     *  T13: tk_wup_tsk — スリープ中タスクを早期起床させる
     * ----------------------------------------------------------------
     *  タスクが TMO_FEVR (永遠に待つ) でスリープ中に、
     *  メインから tk_wup_tsk を呼んで早期起床させます。
     *
     *  正常起床した場合、tk_slp_tsk の戻り値は 0 です。
     *  g_wup_done に 1 が設定されることで確認します。
     */
    plib_puts("\r\n--- T13: tk_wup_tsk (早期起床) ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);
        g_wup_done = 0;

        int tid = start_task(task_wup_target, 8, SCHED_FIFO, 0, 0, 0);
        ASSERT("T13-cre", tid >= 0);

        /* 50ms 待ってからタスクを起こす */
        tk_slp_tsk(50);
        int r = tk_wup_tsk(tid);
        ASSERT_EQ("T13-wup-ret", r, 0);

        /* タスクが完了してセマフォを送信するまで待つ */
        tk_wai_sem(g_sem, 1, 5000);
        ASSERT_EQ("T13-wup-done", g_wup_done, 1);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T14: tk_slp_tsk 自然タイムアウト
     * ----------------------------------------------------------------
     *  タスクが 50ms スリープし、誰も起こさないまま自然にタイムアウトします。
     *  タイムアウト時の tk_slp_tsk 戻り値が E_TMOUT (-50) であることを確認します。
     */
    plib_puts("\r\n--- T14: tk_slp_tsk 自然タイムアウト ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 2 };
        g_sem = tk_cre_sem(&csem);
        g_tmout_result = 999;

        int tid = start_task(task_slp_timeout, 8, SCHED_FIFO, 0, 0, 0);

        /* タスクの 50ms タイムアウトを待つ (セマフォで完了通知) */
        tk_wai_sem(g_sem, 1, 5000);
        ASSERT_EQ("T14-tmout-val", g_tmout_result, -50);   /* E_TMOUT = -50 */
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    plib_puts("\r\n");
}
