/*
 *  tk_conform.c — the contract suite, first version (inbox #5, stage 1).
 *
 *  p-kernel's sanctuary is not the kernel's insides but the promises it makes
 *  to user space: the μT-Kernel 3.0 tk_* service calls the shell / mind /
 *  distributed tasks actually use. This file checks those promises from the
 *  outside — normal paths, error codes and boundaries — so that the kernel may
 *  be changed (or one day swapped) as long as this stays green.
 *
 *  Scope of this version: tasks (create/start/exit/delete/priority/sleep/wake),
 *  semaphores (create/signal/wait with TMO_POL, a timeout, TMO_FEVR, delete),
 *  time (tk_dly_tsk, tk_get_otm). Error codes: E_PAR, E_ID, E_NOEXS, E_OBJ,
 *  E_QOVR, E_TMOUT, E_DLT. Each expectation cites the reference
 *  implementation's documented @retval (kernel/mtkernel3/kernel/tkernel/*.c);
 *  the IEEE 2050 text itself has not been re-read for this version.
 *
 *  Output: one "[tkc] PASS|FAIL <id> <what>" line per check, then
 *  "[tkc] <p> PASS / <f> FAIL". Driven by tests/host/run_tk_conform.sh.
 *  The checks run in a task of their own at priority 20 (helpers at 19/21).
 */
#include "kernel.h"
#include "tk_conform.h"

static void (*g_out)(const char *);
static INT g_pass, g_fail;

static void put_dec(char *b, INT *k, W v)
{
    char t[12]; INT n = 0;
    if (v < 0) { b[(*k)++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) b[(*k)++] = t[--n];
}

/* "[tkc] PASS S3 what (got N)" */
static void check(BOOL ok, const char *id, const char *what, W got)
{
    char b[160]; INT k = 0; const char *p;
    for (p = ok ? "[tkc] PASS " : "[tkc] FAIL "; *p; ) b[k++] = *p++;
    for (p = id; *p && k < 40; ) b[k++] = *p++;
    b[k++] = ' ';
    for (p = what; *p && k < 140; ) b[k++] = *p++;
    for (p = " (got "; *p; ) b[k++] = *p++;
    put_dec(b, &k, got);
    b[k++] = ')'; b[k++] = '\r'; b[k++] = '\n'; b[k] = 0;
    g_out(b);
    if (ok) g_pass++; else g_fail++;
}

static W now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (W)t.lo;
}

/* ---- helper tasks ---------------------------------------------------- */
static volatile INT h_flag;
static volatile ER  h_er1, h_er2;
static ID h_sem;

static void t_set_flag_and_exit(INT stacd, void *exinf)
{
    (void)exinf;
    h_flag = stacd;
    tk_ext_tsk();
}

static void t_sleep_then_exit(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    h_er1 = tk_slp_tsk(TMO_FEVR);
    h_flag = 1;
    tk_ext_tsk();
}

static void t_wait_sem_twice(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    h_er1 = tk_wai_sem(h_sem, 1, TMO_FEVR);
    h_flag = 1;
    h_er2 = tk_wai_sem(h_sem, 1, TMO_FEVR);   /* released by tk_del_sem */
    h_flag = 2;
    tk_ext_tsk();
}

static ID mk_task(void (*fn)(INT, void *), PRI pri)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)fn, .itskpri = pri, .stksz = 8192 };
    return tk_cre_tsk(&ct);
}

static ID mk_sem(INT isem, INT maxsem)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isem, .maxsem = maxsem };
    return tk_cre_sem(&cs);
}

/* ---- tasks ------------------------------------------------------------ */
static void suite_task(PRI me)
{
    T_RTSK r; ER er; ID t;
    T_CTSK bad = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)t_set_flag_and_exit, .itskpri = 0, .stksz = 8192 };

    /* task_manage.c tk_cre_tsk: @retval E_PAR (CHECK_PRI) */
    check(tk_cre_tsk(&bad) == E_PAR, "T1", "tk_cre_tsk itskpri=0 -> E_PAR", tk_cre_tsk(&bad));

    /* created task is DORMANT until started */
    t = mk_task(t_set_flag_and_exit, me - 1);
    check(t > 0, "T2", "tk_cre_tsk -> ID > 0", t);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && r.tskstat == TTS_DMT, "T3", "new task is TTS_DMT", r.tskstat);

    /* a HIGHER-priority task started by us runs before tk_sta_tsk returns */
    h_flag = 0;
    er = tk_sta_tsk(t, 7);
    check(er == E_OK && h_flag == 7, "T4", "higher-pri task runs before tk_sta_tsk returns (stacd=7)", h_flag);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && r.tskstat == TTS_DMT, "T5", "tk_ext_tsk -> back to TTS_DMT", r.tskstat);
    check(tk_del_tsk(t) == E_OK, "T6", "tk_del_tsk dormant -> E_OK", 0);
    check(tk_ref_tsk(t, &r) == E_NOEXS, "T7", "tk_ref_tsk deleted -> E_NOEXS", tk_ref_tsk(t, &r));
    check(tk_sta_tsk(t, 0) == E_NOEXS, "T8", "tk_sta_tsk deleted -> E_NOEXS", tk_sta_tsk(t, 0));

    /* a LOWER-priority task does not run until we block */
    t = mk_task(t_set_flag_and_exit, me + 1);
    h_flag = 0;
    tk_sta_tsk(t, 9);
    check(h_flag == 0, "T9", "lower-pri task has not run after tk_sta_tsk", h_flag);
    tk_dly_tsk(20);
    check(h_flag == 9, "T10", "lower-pri task ran once we slept", h_flag);
    tk_del_tsk(t);

    /* sleep / wake-up, and E_OBJ on a non-dormant task */
    t = mk_task(t_sleep_then_exit, me - 1);
    h_flag = 0; h_er1 = -999;
    tk_sta_tsk(t, 0);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && (r.tskstat & TTS_WAI) != 0 && h_flag == 0, "T11", "tk_slp_tsk(TMO_FEVR) -> TTS_WAI", r.tskstat);
    er = tk_sta_tsk(t, 0);
    check(er == E_OBJ, "T12", "tk_sta_tsk non-dormant -> E_OBJ", er);
    er = tk_del_tsk(t);
    check(er == E_OBJ, "T13", "tk_del_tsk non-dormant -> E_OBJ", er);
    er = tk_wup_tsk(t);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK, "T14", "tk_wup_tsk wakes it (tk_slp_tsk -> E_OK)", h_er1);
    tk_del_tsk(t);

    /* task_sync.c tk_slp_tsk: TMO_POL with no queued wake-up -> E_TMOUT */
    er = tk_slp_tsk(TMO_POL);
    check(er == E_TMOUT, "T15", "tk_slp_tsk(TMO_POL) nothing queued -> E_TMOUT", er);

    /* priority change: E_PAR on 0, then observable through tk_ref_tsk */
    er = tk_chg_pri(TSK_SELF, 0);
    check(er == E_PAR, "T16", "tk_chg_pri pri=0 -> E_PAR", er);
    er = tk_chg_pri(TSK_SELF, me + 1);
    tk_ref_tsk(TSK_SELF, &r);
    check(er == E_OK && r.tskpri == me + 1, "T17", "tk_chg_pri(self, me+1) seen by tk_ref_tsk", r.tskpri);
    tk_chg_pri(TSK_SELF, me);
    tk_ref_tsk(TSK_SELF, &r);
    check(r.tskpri == me, "T18", "priority restored", r.tskpri);

    /* E_ID on an out-of-range id */
    check(tk_ref_tsk(-5, &r) == E_ID, "T19", "tk_ref_tsk id=-5 -> E_ID", tk_ref_tsk(-5, &r));
}

/* ---- semaphores --------------------------------------------------------- */
static void suite_sem(PRI me)
{
    ER er; ID s, t; W t0, dt;
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 2, .maxsem = 1 };

    /* semaphore.c tk_cre_sem: @retval E_PAR (maxsem >= isemcnt) */
    er = tk_cre_sem(&cs);
    check(er == E_PAR, "S1", "tk_cre_sem isemcnt>maxsem -> E_PAR", er);
    s = mk_sem(1, 1);
    check(s > 0, "S2", "tk_cre_sem(1,1) -> ID > 0", s);

    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_OK, "S3", "tk_wai_sem(TMO_POL) with count 1 -> E_OK", er);
    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_TMOUT, "S4", "tk_wai_sem(TMO_POL) with count 0 -> E_TMOUT", er);
    er = tk_wai_sem(s, 0, TMO_POL);
    check(er == E_PAR, "S5", "tk_wai_sem cnt=0 -> E_PAR", er);
    er = tk_wai_sem(s, 2, TMO_POL);
    check(er == E_PAR, "S6", "tk_wai_sem cnt>maxsem -> E_PAR", er);
    er = tk_wai_sem(s, 1, -2);
    check(er == E_PAR, "S7", "tk_wai_sem tmout=-2 -> E_PAR", er);

    er = tk_sig_sem(s, 1);
    check(er == E_OK, "S8", "tk_sig_sem -> E_OK", er);
    er = tk_sig_sem(s, 1);
    check(er == E_QOVR, "S9", "tk_sig_sem over maxsem -> E_QOVR", er);
    er = tk_sig_sem(s, 0);
    check(er == E_PAR, "S10", "tk_sig_sem cnt=0 -> E_PAR", er);
    tk_wai_sem(s, 1, TMO_POL);               /* back to 0 */

    /* a real timeout: 50 ms, not earlier */
    t0 = now_ms();
    er = tk_wai_sem(s, 1, 50);
    dt = now_ms() - t0;
    check(er == E_TMOUT, "S11", "tk_wai_sem(50ms) on empty -> E_TMOUT", er);
    check(dt >= 50 && dt < 1000, "S12", "the 50 ms timeout lasted >= 50 ms", dt);

    /* a higher-priority waiter is released by tk_sig_sem at once, and by
     * tk_del_sem with E_DLT */
    h_sem = s; h_flag = 0; h_er1 = h_er2 = -999;
    t = mk_task(t_wait_sem_twice, me - 1);
    tk_sta_tsk(t, 0);
    check(h_flag == 0, "S13", "higher-pri waiter blocks on count 0", h_flag);
    er = tk_sig_sem(s, 1);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK, "S14", "tk_sig_sem releases it before returning", h_flag);
    er = tk_del_sem(s);
    check(er == E_OK && h_flag == 2 && h_er2 == E_DLT, "S15", "tk_del_sem releases the waiter with E_DLT", h_er2);
    tk_del_tsk(t);

    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_NOEXS, "S16", "tk_wai_sem deleted -> E_NOEXS", er);
    er = tk_sig_sem(s, 1);
    check(er == E_NOEXS, "S17", "tk_sig_sem deleted -> E_NOEXS", er);
    er = tk_wai_sem(0, 1, TMO_POL);
    check(er == E_ID, "S18", "tk_wai_sem id=0 -> E_ID", er);
}

/* ---- time ----------------------------------------------------------------- */
static void suite_time(void)
{
    ER er; W t0, dt;
    t0 = now_ms();
    er = tk_dly_tsk(30);
    dt = now_ms() - t0;
    check(er == E_OK, "D1", "tk_dly_tsk(30) -> E_OK", er);
    check(dt >= 30 && dt < 1000, "D2", "tk_dly_tsk(30) lasted >= 30 ms", dt);
    er = tk_dly_tsk(0);
    check(er == E_OK, "D3", "tk_dly_tsk(0) -> E_OK", er);
    t0 = now_ms(); tk_dly_tsk(10);
    check(now_ms() - t0 >= 10, "D4", "tk_get_otm advances across a 10 ms delay", now_ms() - t0);
}

/* The suite runs in its own task at TKC_PRI, so helpers can sit one above
 * and one below it whatever the caller's priority is (the hosted shell runs
 * at 1). The caller waits for it on a semaphore. */
#define TKC_PRI 20
static ID g_done;

static void t_runner(INT stacd, void *exinf)
{
    T_RTSK r;
    (void)stacd; (void)exinf;
    tk_ref_tsk(TSK_SELF, &r);
    suite_task(r.tskpri);
    suite_sem(r.tskpri);
    suite_time();
    tk_sig_sem(g_done, 1);
    tk_ext_tsk();
}

INT tk_conform_run(void (*out)(const char *))
{
    char b[64]; INT k = 0; const char *p;
    ID run; ER er;
    g_out = out; g_pass = g_fail = 0;
    out("[tkc] contract suite v1 (runner task pri 20)\r\n");
    g_done = mk_sem(0, 1);
    run = mk_task(t_runner, TKC_PRI);
    if (g_done <= 0 || run <= 0) {
        out("[tkc] FAIL setup: could not create the runner task / semaphore\r\n");
        return 1;
    }
    tk_sta_tsk(run, 0);
    er = tk_wai_sem(g_done, 1, 120000);
    if (er != E_OK) {
        out("[tkc] FAIL setup: the runner did not finish within 120 s\r\n");
        g_fail++;
        tk_ter_tsk(run);
    }
    tk_del_tsk(run);
    tk_del_sem(g_done);
    k = 0;
    for (p = "[tkc] "; *p; ) b[k++] = *p++;
    put_dec(b, &k, g_pass);
    for (p = " PASS / "; *p; ) b[k++] = *p++;
    put_dec(b, &k, g_fail);
    for (p = " FAIL\r\n"; *p; ) b[k++] = *p++;
    b[k] = 0;
    out(b);
    return g_fail;
}
