/*
 *  test_all/test_sem.c — セマフォ テスト
 *
 *  テスト一覧:
 *    T4 — 基本 signal / wait (1 タスクが +1、メインが待つ)
 *    T5 — バルク signal / wait (3 タスクが各 +1、メインが cnt=3 を一気に消費)
 *    T6 — TMO_POL (即時ポーリング: カウント 0 では失敗、+1 後は成功)
 */

#include "test_common.h"

void test_sem(void)
{
    /* ----------------------------------------------------------------
     *  T4: セマフォ 基本 signal / wait
     * ----------------------------------------------------------------
     *  初期カウント 0 のセマフォを作成し、ワーカータスクが +1 シグナル、
     *  メインが cnt=1 で待つ最も基本的なパターンです。
     *
     *    セマフォ作成 (cnt=0)
     *    ワーカー起動 → tk_sig_sem(semid, 1) → カウント = 1
     *    メイン:        tk_wai_sem(semid, 1)  → カウントが 1 以上になったので通過
     */
    plib_puts("--- T4: セマフォ基本 signal/wait ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };     /* exinf=0, 初期カウント=0, 最大=4 */
        g_sem = tk_cre_sem(&csem);
        ASSERT("T4-cre-sem", g_sem >= 0);

        /* task_sem_signal は exinf にセマフォ ID を受け取り +1 シグナルする */
        int tid = start_task(task_sem_signal, 8, SCHED_FIFO, 0,
                             0, (void*)(long)g_sem);
        int r = tk_wai_sem(g_sem, 1, 5000);   /* 5秒タイムアウト */
        ASSERT_EQ("T4-wai-sem", r, 0);
        tk_del_tsk(tid);
        tk_del_sem(g_sem);
    }

    /* ----------------------------------------------------------------
     *  T5: セマフォ cnt>1 バルク signal / wait
     * ----------------------------------------------------------------
     *  3 つのワーカータスクがそれぞれ +1 シグナルします。
     *  メインは cnt=3 を一度に消費します (カウントが 3 以上になるまで待つ)。
     *
     *    タスクA: tk_sig_sem(id, 1) → カウント = 1
     *    タスクB: tk_sig_sem(id, 1) → カウント = 2
     *    タスクC: tk_sig_sem(id, 1) → カウント = 3
     *    メイン:  tk_wai_sem(id, 3)  → カウントが 3 になったので通過 (カウント = 0)
     */
    plib_puts("\r\n--- T5: セマフォ cnt>1 (バルク) ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 8 };
        g_sem = tk_cre_sem(&csem);
        ASSERT("T5-cre-sem", g_sem >= 0);

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

    /* ----------------------------------------------------------------
     *  T6: セマフォ TMO_POL (タイムアウト = 0: ポーリング)
     * ----------------------------------------------------------------
     *  TMO_POL を指定すると、カウントが足りなくても「待たずに即座に戻る」
     *  ポーリングモードになります。
     *
     *  カウント = 0 → TMO_POL → E_TMOUT (-50) が返るはず
     *  +1 シグナル後 → TMO_POL → 0 (成功) が返るはず
     */
    plib_puts("\r\n--- T6: セマフォ TMO_POL ---\r\n");
    {
        PK_CSEM csem = { 0, 0, 4 };
        g_sem = tk_cre_sem(&csem);

        /* カウント 0 → ポーリング失敗 */
        int r = tk_wai_sem(g_sem, 1, TMO_POL);
        ASSERT_EQ("T6-pol-empty", r, -50);   /* E_TMOUT = -50 */

        /* +1 シグナル → ポーリング成功 */
        tk_sig_sem(g_sem, 1);
        r = tk_wai_sem(g_sem, 1, TMO_POL);
        ASSERT_EQ("T6-pol-avail", r, 0);
        tk_del_sem(g_sem);
    }

    plib_puts("\r\n");
}
