/*
 *  19_misc/misc.c — T-Kernel 残余API デモ
 *
 *  Tests:
 *    tk_ref_por()  — ランデブーポート状態参照
 *    tk_get_otm()  — 稼働時間取得（単調増加、set_timで変わらない）
 *    tk_set_tim()  — システム時刻設定
 *    tk_exd_tsk()  — タスク終了+削除（ext+del 合体版）
 *    tk_dis_dsp()  — ディスパッチ禁止
 *    tk_ena_dsp()  — ディスパッチ許可
 *    tk_rot_rdq()  — レディキュー回転（同優先度 yield）
 */
#include "plibc.h"

/* ----------------------------------------------------------------- */
/* Shared state for rot_rdq test                                      */
/* ----------------------------------------------------------------- */
#define N_PEERS  3
static volatile int order[N_PEERS];  /* 実行順序を記録 */
static volatile int order_idx = 0;
static int peer_sem;                 /* 全 peer 完了同期用 */

static void peer_task(int stacd, void *exinf)
{
    int id = stacd;           /* 0, 1, 2 */

    /* 自分の番号を記録してから yield → 他の peer に譲る */
    order[order_idx++] = id;
    tk_rot_rdq(0);            /* 自優先度でキュー回転 → 最後尾に移動 */

    /* 全員 yield 後にもう一度記録は省略; semで終了通知 */
    tk_sig_sem(peer_sem, 1);
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* exd_tsk target: exits with tk_exd_tsk (no separate del needed)    */
/* ----------------------------------------------------------------- */
static volatile int exd_done = 0;

static void exd_task(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    exd_done = 1;
    tk_exd_tsk();   /* 自タスクを終了+削除 */
}

/* ----------------------------------------------------------------- */
/* Main                                                               */
/* ----------------------------------------------------------------- */
void _start(void)
{
    int ok = 1;

    /* ---- tk_ref_por -------------------------------------------- */
    PK_CPOR cpor = { TA_RDV_TFIFO, 16, 16 };
    int pid = tk_cre_por(&cpor);
    PK_RPOR rpor;
    int r = tk_ref_por(pid, &rpor);
    if (r == 0 && rpor.maxcmsz == 16 && rpor.wtsk == 0) {
        plib_puts("[19] tk_ref_por OK: maxcmsz=");
        plib_puti(rpor.maxcmsz);
        plib_puts("\r\n");
    } else {
        plib_puts("[19] tk_ref_por FAIL\r\n");
        ok = 0;
    }
    tk_del_por(pid);

    /* ---- tk_get_otm / tk_set_tim ------------------------------- */
    PK_SYSTIM otm0, otm1, tim0, tim1;
    tk_get_otm(&otm0);
    tk_get_tim(&tim0);

    /* 時刻を大きな値に変更 */
    PK_SYSTIM new_tim = { 0, 9999000 };  /* 9999秒 相当 */
    tk_set_tim(&new_tim);

    tk_dly_tsk(50);

    tk_get_otm(&otm1);
    tk_get_tim(&tim1);

    /* otm は単調増加するはず（set_timで変わらない）*/
    if (otm1.lo > otm0.lo) {
        plib_puts("[19] tk_get_otm OK: otm advances (+");
        plib_putu(otm1.lo - otm0.lo);
        plib_puts(" ms)\r\n");
    } else {
        plib_puts("[19] tk_get_otm FAIL\r\n");
        ok = 0;
    }

    /* get_tim は set_tim の値から進むはず (≥9999000) */
    if (tim1.lo >= 9999000u) {
        plib_puts("[19] tk_set_tim OK: tim=");
        plib_putu(tim1.lo);
        plib_puts(" ms\r\n");
    } else {
        plib_puts("[19] tk_set_tim FAIL: tim=");
        plib_putu(tim1.lo);
        plib_puts("\r\n");
        ok = 0;
    }

    /* 時刻を元に戻す（otm0をベースに復元） */
    PK_SYSTIM restore = { 0, otm1.lo };
    tk_set_tim(&restore);

    /* ---- tk_dis_dsp / tk_ena_dsp ------------------------------- */
    r = tk_dis_dsp();
    /* ディスパッチ禁止中: 他タスクに切り替わらないことを確認
     * （単純に呼び出し成功を確認するだけ） */
    r = tk_ena_dsp();
    if (r == 0) {
        plib_puts("[19] tk_dis/ena_dsp OK\r\n");
    } else {
        plib_puts("[19] tk_dis/ena_dsp FAIL\r\n");
        ok = 0;
    }

    /* ---- tk_rot_rdq -------------------------------------------- */
    /* 同優先度の 3 タスクを作り、rot_rdq で順番に実行させる */
    PK_CSEM csem = { NULL, 0, N_PEERS };
    peer_sem = tk_cre_sem(&csem);

    PK_CRE_TSK pct;
    pct.pri      = 8;
    pct.stksz    = 0;
    pct.policy   = SCHED_FIFO;
    pct.slice_ms = 0;
    pct.exinf    = NULL;

    int tids[N_PEERS];
    for (int i = 0; i < N_PEERS; i++) {
        pct.task = peer_task;
        tids[i] = tk_cre_tsk(&pct);
    }

    /* 主タスクより低優先度にして peer が先に走らないようにする */
    /* ここでは pri=8 で起動し、main はその後 pri=7 相当で動く */
    /* (main task の pri は通常 NUM_PRI/2 = 8 なので同じになる)  */
    /* 全員起動してから tk_dly_tsk で yield */
    for (int i = 0; i < N_PEERS; i++) {
        tk_sta_tsk(tids[i], i);   /* stacd = peer id */
    }

    /* 全 peer が完了するまで待つ */
    tk_wai_sem(peer_sem, N_PEERS, 2000);
    tk_del_sem(peer_sem);

    /* order[] に N_PEERS 個の値が入っているはず */
    if (order_idx == N_PEERS) {
        plib_puts("[19] tk_rot_rdq OK: order=");
        for (int i = 0; i < N_PEERS; i++) {
            plib_puti(order[i]);
            if (i < N_PEERS-1) plib_puts(",");
        }
        plib_puts("\r\n");
    } else {
        plib_puts("[19] tk_rot_rdq FAIL: order_idx=");
        plib_puti(order_idx);
        plib_puts("\r\n");
        ok = 0;
    }

    /* ---- tk_exd_tsk -------------------------------------------- */
    PK_CRE_TSK ect;
    ect.task     = exd_task;
    ect.pri      = 8;
    ect.stksz    = 0;
    ect.policy   = SCHED_FIFO;
    ect.slice_ms = 0;
    ect.exinf    = NULL;
    int etid = tk_cre_tsk(&ect);
    tk_sta_tsk(etid, 0);
    tk_dly_tsk(50);  /* exd_task 完了を待つ */

    if (exd_done) {
        plib_puts("[19] tk_exd_tsk OK\r\n");
        /* tk_del_tsk 不要 — exd_tsk が TCB ごと削除済み */
    } else {
        plib_puts("[19] tk_exd_tsk FAIL\r\n");
        ok = 0;
    }

    plib_puts(ok ? "[19] misc => OK\r\n" : "[19] misc => FAIL\r\n");
    sys_exit(0);
}
