/*
 *  selftest.c (x86)
 *  Kernel built-in self-test — runs at boot from the initial task,
 *  before any background tasks are started.
 *
 *  Five tests, ~100ms total:
 *   T1: Semaphore signal/wait (no context switch)
 *   T2: Timer fire via tk_dly_tsk (20ms sleep)
 *   T3: Cyclic handler → tk_sig_sem  (Bug-1 regression: END_CRITICAL_SECTION
 *       must not dispatch from IRQ/task-independent context)
 *   T4: Mutex lock/unlock across two tasks
 *   T5: Config sanity (PAGING_MAX_TASKS vs CFN_MAX_TSKID)
 */

#include "kernel.h"
#include "paging.h"

IMPORT void tm_putstring(UB *str);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void st_puts(const char *s)
{
    tm_putstring((UB *)s);
}

static void st_puti(W n)
{
    char buf[12]; INT i = 11;
    buf[i] = '\0';
    BOOL neg = (n < 0);
    if (neg) n = -n;
    if (n == 0) { buf[--i] = '0'; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    if (neg && i > 0) buf[--i] = '-';
    st_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* T3 state — must be file-scope so the cyclic handler can reach it   */
/* ------------------------------------------------------------------ */

static ID st_cyc_sem = 0;

static void st_cyc3_handler(VP exinf)
{
    (void)exinf;
    if (st_cyc_sem > 0)
        tk_sig_sem(st_cyc_sem, 1);
}

/* ------------------------------------------------------------------ */
/* T4 state — helper task for mutex test                              */
/* ------------------------------------------------------------------ */

static ID st_mtx_id      = 0;
static ID st_mtx_done    = 0;

static void st_mtx_helper(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* Will block here until the init task releases the mutex */
    tk_loc_mtx(st_mtx_id, TMO_FEVR);
    tk_unl_mtx(st_mtx_id);
    tk_sig_sem(st_mtx_done, 1);
    tk_ext_tsk();
}

/* ================================================================== */
/* Individual tests                                                    */
/* ================================================================== */

static int run_t1_sem(void)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 4 };
    ID sid = tk_cre_sem(&cs);
    if (sid < E_OK) { st_puts("[FAIL] T1: tk_cre_sem er="); st_puti(sid); st_puts("\r\n"); return 0; }

    ER r;
    /* Signal once */
    r = tk_sig_sem(sid, 1);
    if (r != E_OK) { tk_del_sem(sid); st_puts("[FAIL] T1: sig er="); st_puti(r); st_puts("\r\n"); return 0; }

    /* Immediate poll — must succeed (count=1) */
    r = tk_wai_sem(sid, 1, TMO_POL);
    if (r != E_OK) { tk_del_sem(sid); st_puts("[FAIL] T1: poll1 er="); st_puti(r); st_puts("\r\n"); return 0; }

    /* Poll again — must timeout (count=0) */
    r = tk_wai_sem(sid, 1, TMO_POL);
    if (r != E_TMOUT) { tk_del_sem(sid); st_puts("[FAIL] T1: poll2 er="); st_puti(r); st_puts("\r\n"); return 0; }

    tk_del_sem(sid);
    st_puts("[PASS] T1: sem signal/wait\r\n");
    return 1;
}

static int run_t2_timer(void)
{
    SYSTIM t0, t1;
    tk_get_otm(&t0);
    tk_dly_tsk(20);    /* 20ms sleep — PIT must fire to wake us */
    tk_get_otm(&t1);

    W elapsed = (W)(t1.lo - t0.lo);
    if (elapsed < 20 || elapsed > 100) {
        st_puts("[FAIL] T2: timer elapsed="); st_puti(elapsed); st_puts("ms\r\n");
        return 0;
    }
    st_puts("[PASS] T2: timer 20ms dly\r\n");
    return 1;
}

static int run_t3_cyclic_bug1(void)
{
    /* Semaphore that the cyclic handler will signal */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 4 };
    st_cyc_sem = tk_cre_sem(&cs);
    if (st_cyc_sem < E_OK) {
        st_puts("[FAIL] T3: sem er="); st_puti(st_cyc_sem); st_puts("\r\n");
        st_cyc_sem = 0; return 0;
    }

    /* Cyclic handler — fire every 50ms; do NOT use TA_STA|cycphs=0
     * (that triggers knl_immediate_call_cychdr which runs in task context,
     *  not the real IRQ path we need to exercise for Bug-1 regression).
     * We start it manually via tk_sta_cyc so the first fire comes from
     * the PIT ISR through knl_call_cychdr with ENABLE_INTERRUPT_UPTO. */
    T_CCYC cc;
    cc.exinf   = NULL;
    cc.cycatr  = TA_HLNG;                  /* no TA_STA */
    cc.cychdr  = (FP)st_cyc3_handler;
    cc.cyctim  = 50;
    cc.cycphs  = 50;                       /* first fire 50ms after tk_sta_cyc */
    ID cycid = tk_cre_cyc(&cc);
    if (cycid < E_OK) {
        st_puts("[FAIL] T3: cre_cyc er="); st_puti(cycid); st_puts("\r\n");
        tk_del_sem(st_cyc_sem); st_cyc_sem = 0; return 0;
    }

    tk_sta_cyc(cycid);    /* starts the 50ms countdown through the timer queue */

    /* Wait up to 200ms for the cyclic handler to signal us */
    ER r = tk_wai_sem(st_cyc_sem, 1, 200);

    tk_stp_cyc(cycid);
    tk_del_cyc(cycid);
    tk_del_sem(st_cyc_sem);
    st_cyc_sem = 0;

    if (r != E_OK) {
        st_puts("[FAIL] T3: cyclic->sem er="); st_puti(r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T3: cyclic->sem (bug1 guard)\r\n");
    return 1;
}

static int run_t4_mutex(void)
{
    /* Semaphore signaled by helper when done */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    st_mtx_done = tk_cre_sem(&cs);
    if (st_mtx_done < E_OK) {
        st_puts("[FAIL] T4: sem er="); st_puti(st_mtx_done); st_puts("\r\n");
        st_mtx_done = 0; return 0;
    }

    /* Mutex */
    T_CMTX cm = { .exinf = NULL, .mtxatr = TA_TFIFO, .ceilpri = 0 };
    st_mtx_id = tk_cre_mtx(&cm);
    if (st_mtx_id < E_OK) {
        st_puts("[FAIL] T4: cre_mtx er="); st_puti(st_mtx_id); st_puts("\r\n");
        tk_del_sem(st_mtx_done); st_mtx_done = 0; st_mtx_id = 0; return 0;
    }

    /* Lock mutex before starting the helper */
    tk_loc_mtx(st_mtx_id, TMO_FEVR);

    /* Create helper at lower priority (15) so it won't preempt init task (pri=1) */
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = st_mtx_helper, .itskpri = 15, .stksz = 2048 };
    ID helper = tk_cre_tsk(&ct);
    if (helper < E_OK) {
        st_puts("[FAIL] T4: cre_tsk er="); st_puti(helper); st_puts("\r\n");
        tk_unl_mtx(st_mtx_id); tk_del_mtx(st_mtx_id);
        tk_del_sem(st_mtx_done); st_mtx_id = 0; st_mtx_done = 0; return 0;
    }
    tk_sta_tsk(helper, 0);   /* helper blocks immediately waiting for mutex */

    /* Release mutex — helper becomes ready but init task (pri=1) still runs */
    tk_unl_mtx(st_mtx_id);

    /* Block on semaphore — helper now runs, unlocks mutex, signals done */
    ER r = tk_wai_sem(st_mtx_done, 1, 200);

    tk_del_tsk(helper);      /* task is dormant after tk_ext_tsk */
    tk_del_mtx(st_mtx_id);
    tk_del_sem(st_mtx_done);
    st_mtx_id = 0; st_mtx_done = 0;

    if (r != E_OK) {
        st_puts("[FAIL] T4: mutex 2-task er="); st_puti(r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T4: mutex 2-task\r\n");
    return 1;
}

static int run_t5_config(void)
{
    int ok = 1;
#if PAGING_MAX_TASKS < (CFN_MAX_TSKID + 1)
    st_puts("[FAIL] T5: PAGING_MAX_TASKS too small\r\n");
    ok = 0;
#endif
#if CFN_MAX_TSKID < 32
    st_puts("[FAIL] T5: CFN_MAX_TSKID < 32\r\n");
    ok = 0;
#endif
    if (ok) st_puts("[PASS] T5: config sanity\r\n");
    return ok;
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

EXPORT void kernel_selftest(void)
{
    st_puts("\r\n[SELFTEST] kernel built-in self-test\r\n");

    int pass = 0;
    pass += run_t1_sem();
    pass += run_t2_timer();
    pass += run_t3_cyclic_bug1();
    pass += run_t4_mutex();
    pass += run_t5_config();

    st_puts("[SELFTEST] ");
    st_puti(pass);
    st_puts("/5 passed");
    if (pass == 5) {
        st_puts(" -- OK\r\n\r\n");
    } else {
        st_puts(" -- KERNEL UNHEALTHY\r\n\r\n");
    }
}
