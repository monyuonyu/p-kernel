/*
 *  10_mutex/mutex.c
 *
 *  Mutex (相互排他) サンプル
 *  2つのタスクが共有カウンタを mutex で保護しながらインクリメントします。
 *
 *  学べること:
 *    - tk_cre_mtx(PK_CMTX*)  — mutex 作成
 *    - tk_loc_mtx(mtxid, tmout) — lock (待ち)
 *    - tk_unl_mtx(mtxid)     — unlock
 *    - tk_del_mtx(mtxid)     — mutex 削除
 *
 *  実行例:
 *    p-kernel> exec mutex.elf
 */
#include "plibc.h"

#define LOOPS    5       /* 各タスクのループ回数 */
#define DELAY_MS 50      /* ロック保持中のスリープ (競合演出) */

static int shared_counter = 0;
static int mtxid;

/* タスク終了を知らせるセマフォ */
static int done_sem;

static void task_a(int stacd, void *exinf)
{
    (void)exinf;
    for (int i = 0; i < LOOPS; i++) {
        tk_loc_mtx(mtxid, TMO_FEVR);
        int v = shared_counter + 1;
        tk_slp_tsk(DELAY_MS);   /* 競合が起きやすくなる */
        shared_counter = v;
        tk_unl_mtx(mtxid);
    }
    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

static void task_b(int stacd, void *exinf)
{
    (void)exinf;
    for (int i = 0; i < LOOPS; i++) {
        tk_loc_mtx(mtxid, TMO_FEVR);
        int v = shared_counter + 1;
        tk_slp_tsk(DELAY_MS);
        shared_counter = v;
        tk_unl_mtx(mtxid);
    }
    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" mutex: 相互排他デモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* mutex 作成 (TA_MTX_TPRI: 優先度順待ちキュー) */
    PK_CMTX cmtx = { TA_MTX_TPRI, 0 };
    mtxid = tk_cre_mtx(&cmtx);
    if (mtxid < 0) {
        plib_puts("ERROR: tk_cre_mtx failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] mutex created (id="); plib_puti(mtxid); plib_puts(")\r\n");

    /* 完了待ちセマフォ */
    PK_CSEM csem = { NULL, 0, 2 };
    done_sem = tk_cre_sem(&csem);

    /* タスク A, B を作成 (同じ優先度 → 交互実行) */
    PK_CRE_TSK cta = { task_a, 8, 0, SCHED_RR, 100, NULL };
    PK_CRE_TSK ctb = { task_b, 8, 0, SCHED_RR, 100, NULL };
    int tid_a = tk_cre_tsk(&cta);
    int tid_b = tk_cre_tsk(&ctb);

    if (tid_a < 0 || tid_b < 0) {
        plib_puts("ERROR: tk_cre_tsk failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] task_a (tid="); plib_puti(tid_a); plib_puts(") created\r\n");
    plib_puts("[+] task_b (tid="); plib_puti(tid_b); plib_puts(") created\r\n\r\n");

    tk_sta_tsk(tid_a, 0);
    tk_sta_tsk(tid_b, 0);

    /* 両タスクの完了を待つ */
    tk_wai_sem(done_sem, 2, TMO_FEVR);

    plib_puts("\r\n[result]\r\n");
    plib_puts("  shared_counter = "); plib_puti(shared_counter); plib_puts("\r\n");
    plib_puts("  expected       = "); plib_puti(LOOPS * 2);      plib_puts("\r\n");

    if (shared_counter == LOOPS * 2) {
        plib_puts("  => OK (no data race)\r\n");
    } else {
        plib_puts("  => NG (data race detected!)\r\n");
    }

    tk_del_sem(done_sem);
    tk_del_mtx(mtxid);

    plib_puts("\r\n========================================\r\n");
    plib_puts(" mutex: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
