/*
 *  15_task_ext/task_ext.c — T-Kernel task supplement API demo
 *
 *  Tests:
 *    tk_get_tid()   — get current task ID
 *    tk_sus_tsk()   — suspend another task
 *    tk_rsm_tsk()   — resume a suspended task
 *    tk_rel_wai()   — release a task from wait state
 *    tk_can_wup()   — cancel pending wakeup requests
 *    tk_ter_tsk()   — force-terminate another task
 */
#include "plibc.h"

/* ----------------------------------------------------------------- */
/* Shared state                                                        */
/* ----------------------------------------------------------------- */
static volatile int victim_reached  = 0;  /* victim task started    */
static volatile int victim_done     = 0;  /* victim task terminated */
static volatile int rel_task_woke   = 0;  /* rel_wai target woke    */

static int main_tid;     /* set by main task before starting sub-tasks */

/* ----------------------------------------------------------------- */
/* Task: victim — gets suspended and resumed by main                  */
/* ----------------------------------------------------------------- */
static void victim_task(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    victim_reached = 1;
    /* Sleep a long time; main will suspend/resume/terminate us */
    tk_slp_tsk(TMO_FEVR);
    victim_done = 1;
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* Task: rel_target — will be released from wait by main              */
/* ----------------------------------------------------------------- */
static void rel_target_task(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* Wait on semaphore (id=0 arg forces error, but rel_wai wakes us) */
    tk_slp_tsk(5000);  /* sleep 5 s — main will wake early via rel_wai */
    rel_task_woke = 1;
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* Main task                                                          */
/* ----------------------------------------------------------------- */
void _start(void)
{
    int ok = 1;

    /* --- Test 1: tk_get_tid() ------------------------------------ */
    main_tid = tk_get_tid();
    if (main_tid > 0) {
        plib_puts("[15] tk_get_tid OK: tid=");
        plib_puti(main_tid);
        plib_puts("\r\n");
    } else {
        plib_puts("[15] tk_get_tid FAIL\r\n");
        ok = 0;
    }

    /* --- Test 2: tk_can_wup() — wakeup queue --------------------- */
    /* Create a low-priority sleeping task, queue 2 wakeups, cancel them */
    PK_CRE_TSK cwt;
    cwt.task     = victim_task;   /* victim sleeps forever (same fn as Test 3) */
    cwt.pri      = 14;            /* very low priority so main keeps running */
    cwt.stksz    = 0;
    cwt.policy   = SCHED_FIFO;
    cwt.slice_ms = 0;
    cwt.exinf    = NULL;
    int wuptid = tk_cre_tsk(&cwt);
    tk_sta_tsk(wuptid, 0);
    tk_dly_tsk(20);               /* let victim_task enter slp_tsk */

    tk_wup_tsk(wuptid);           /* queue wakeup 1 */
    tk_wup_tsk(wuptid);           /* queue wakeup 2 (re-queue after first wakes) */
    /* victim has lower pri so main still runs; cancel before it can run */
    int cancelled = tk_can_wup(wuptid);
    tk_ter_tsk(wuptid);
    tk_del_tsk(wuptid);
    victim_reached = 0;           /* reset for Test 3 */

    if (cancelled >= 1) {
        plib_puts("[15] tk_can_wup OK: cancelled=");
        plib_puti(cancelled);
        plib_puts("\r\n");
    } else {
        plib_puts("[15] tk_can_wup FAIL: cancelled=");
        plib_puti(cancelled);
        plib_puts("\r\n");
        ok = 0;
    }

    /* --- Test 3: tk_sus_tsk() / tk_rsm_tsk() -------------------- */
    PK_CRE_TSK ct;
    ct.task     = victim_task;
    ct.pri      = 8;
    ct.stksz    = 0;
    ct.policy   = SCHED_FIFO;
    ct.slice_ms = 0;
    ct.exinf    = NULL;
    int vtid = tk_cre_tsk(&ct);
    tk_sta_tsk(vtid, 0);

    /* Wait for victim to start */
    int wait = 0;
    while (!victim_reached && wait++ < 500) tk_dly_tsk(2);

    /* Suspend victim */
    int r = tk_sus_tsk(vtid);
    if (r == 0) {
        plib_puts("[15] tk_sus_tsk OK\r\n");
    } else {
        plib_puts("[15] tk_sus_tsk FAIL\r\n");
        ok = 0;
    }

    /* Resume victim */
    r = tk_rsm_tsk(vtid);
    if (r == 0) {
        plib_puts("[15] tk_rsm_tsk OK\r\n");
    } else {
        plib_puts("[15] tk_rsm_tsk FAIL\r\n");
        ok = 0;
    }

    /* --- Test 4: tk_rel_wai() ------------------------------------ */
    PK_CRE_TSK ct2;
    ct2.task     = rel_target_task;
    ct2.pri      = 8;
    ct2.stksz    = 0;
    ct2.policy   = SCHED_FIFO;
    ct2.slice_ms = 0;
    ct2.exinf    = NULL;
    int rtid = tk_cre_tsk(&ct2);
    tk_sta_tsk(rtid, 0);

    tk_dly_tsk(50);  /* let rel_target enter tk_slp_tsk */
    r = tk_rel_wai(rtid);
    if (r == 0) {
        plib_puts("[15] tk_rel_wai OK\r\n");
    } else {
        plib_puts("[15] tk_rel_wai FAIL\r\n");
        ok = 0;
    }
    tk_dly_tsk(30);  /* wait for rel_target to finish */
    if (rel_task_woke) {
        plib_puts("[15] rel_target woke OK\r\n");
    } else {
        plib_puts("[15] rel_target woke FAIL\r\n");
        ok = 0;
    }

    /* --- Test 5: tk_ter_tsk() ------------------------------------ */
    /* victim is still alive (sleeping forever after resume) */
    r = tk_ter_tsk(vtid);
    if (r == 0) {
        plib_puts("[15] tk_ter_tsk OK\r\n");
    } else {
        plib_puts("[15] tk_ter_tsk FAIL\r\n");
        ok = 0;
    }

    /* Clean up */
    tk_del_tsk(vtid);

    plib_puts(ok ? "[15] task_ext => OK\r\n" : "[15] task_ext => FAIL\r\n");
    sys_exit(0);
}
