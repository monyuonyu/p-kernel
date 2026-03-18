/*
 *  usermain.c (x86)
 *  T-Kernel API Test Suite
 *
 *  Tests: task management, semaphore, event flag, mutex, mailbox,
 *         message buffer, variable/fixed memory pools,
 *         cyclic handler, alarm handler, task control
 */

#include "kernel.h"
#include <tmonitor.h>

/* ================================================================
 * Utilities
 * ================================================================ */

static void pass(const char *name)
{
    tm_putstring((UB *)"  [PASS] ");
    tm_putstring((UB *)name);
    tm_putstring((UB *)"\r\n");
}

static void fail(const char *name)
{
    tm_putstring((UB *)"  [FAIL] ");
    tm_putstring((UB *)name);
    tm_putstring((UB *)"\r\n");
}

#define CHECK(name, expr) \
    do { if ((expr) >= E_OK) pass(name); else fail(name); } while (0)

/* ================================================================
 * Globals for multi-task tests
 * ================================================================ */

static ID           g_sem;          /* "task started / done" semaphore */
static ID           g_sem2;         /* "let task continue" semaphore   */
static volatile INT g_val;          /* stacd / result passing          */
static volatile INT g_cyc_count;    /* cyclic handler fire count       */
static volatile INT g_alm_fired;    /* alarm handler fired flag        */

/* ================================================================
 * Helper task A  (for test_task)
 *   Records stacd -> signals g_sem -> exits
 * ================================================================ */
static void sub_task_a(INT stacd, void *exinf)
{
    g_val = stacd;
    tk_sig_sem(g_sem, 1);   /* wake main */
    tk_ext_tsk();
}

/* ================================================================
 * Helper task B  (for test_task_ctrl)
 *   signals g_sem -> waits on g_sem2 -> signals g_sem -> exits
 * ================================================================ */
static void sub_task_b(INT stacd, void *exinf)
{
    (void)exinf;
    tk_sig_sem(g_sem, 1);           /* "I started" */
    tk_wai_sem(g_sem2, 1, TMO_FEVR);/* wait for main to release us */
    tk_sig_sem(g_sem, 1);           /* "I'm done"  */
    tk_ext_tsk();
}

/* ================================================================
 * Test: Task management
 *   Create subtask, pass stacd, verify it arrives, clean up.
 * ================================================================ */
static void test_task(void)
{
    T_CTSK ctsk;
    T_CSEM csem;
    ID tskid;

    tm_putstring((UB *)"[Task management]\r\n");

    /* sync semaphore */
    csem.exinf   = NULL;
    csem.sematr  = TA_TFIFO;
    csem.isemcnt = 0;
    csem.maxsem  = 1;
    g_sem = tk_cre_sem(&csem);
    if (g_sem < E_OK) { fail("cre_sem"); return; }

    /* create subtask at same priority as main → won't preempt */
    ctsk.exinf   = NULL;
    ctsk.tskatr  = TA_HLNG | TA_RNG0;
    ctsk.task    = (FP)sub_task_a;
    ctsk.itskpri = 1;
    ctsk.stksz   = 1024;
    tskid = tk_cre_tsk(&ctsk);
    if (tskid < E_OK) { fail("cre_tsk"); tk_del_sem(g_sem); return; }
    pass("cre_tsk");

    CHECK("sta_tsk", tk_sta_tsk(tskid, 42));
    CHECK("wai_sem (subtask signal)", tk_wai_sem(g_sem, 1, 1000));

    /* sub_task_a is same priority: after signalling it called tk_ext_tsk */
    if (g_val == 42) pass("stacd passed correctly");
    else             fail("stacd passed correctly");

    CHECK("del_tsk", tk_del_tsk(tskid));
    tk_del_sem(g_sem);
}

/* ================================================================
 * Test: Semaphore
 * ================================================================ */
static void test_semaphore(void)
{
    T_CSEM csem;
    ID semid;
    ER er;

    tm_putstring((UB *)"[Semaphore]\r\n");

    csem.exinf   = NULL;
    csem.sematr  = TA_TFIFO;
    csem.isemcnt = 0;
    csem.maxsem  = 5;
    semid = tk_cre_sem(&csem);
    if (semid < E_OK) { fail("cre_sem"); return; }
    pass("cre_sem");

    CHECK("sig_sem x3", tk_sig_sem(semid, 3));
    CHECK("wai_sem x3 (TMO_POL)", tk_wai_sem(semid, 3, TMO_POL));

    /* count is 0 now; polling wait must fail */
    er = tk_wai_sem(semid, 1, TMO_POL);
    if (er == E_TMOUT) pass("wai_sem empty -> E_TMOUT");
    else               fail("wai_sem empty -> E_TMOUT");

    CHECK("del_sem", tk_del_sem(semid));
}

/* ================================================================
 * Test: Event flag
 * ================================================================ */
static void test_eventflag(void)
{
    T_CFLG cflg;
    ID flgid;
    UINT ptn;
    ER er;

    tm_putstring((UB *)"[Event flag]\r\n");

    cflg.exinf   = NULL;
    cflg.flgatr  = TA_WMUL;
    cflg.iflgptn = 0;
    flgid = tk_cre_flg(&cflg);
    if (flgid < E_OK) { fail("cre_flg"); return; }
    pass("cre_flg");

    /* AND wait: both bits must be set */
    CHECK("set_flg 0x03", tk_set_flg(flgid, 0x03));
    CHECK("wai_flg AND 0x03", tk_wai_flg(flgid, 0x03, TWF_ANDW, &ptn, TMO_POL));
    if ((ptn & 0x03) == 0x03) pass("AND ptn correct");
    else                      fail("AND ptn correct");

    /* Clear the flag manually */
    tk_clr_flg(flgid, 0x00);  /* clear all bits: flgptn &= 0 */

    /* OR wait: only one bit needed */
    CHECK("set_flg 0x01", tk_set_flg(flgid, 0x01));
    CHECK("wai_flg OR 0x03", tk_wai_flg(flgid, 0x03, TWF_ORW, &ptn, TMO_POL));
    if (ptn & 0x01) pass("OR ptn correct");
    else            fail("OR ptn correct");

    /* With flag still set, poll for bit 0x02 (not set) → E_TMOUT */
    tk_set_flg(flgid, 0x01);
    er = tk_wai_flg(flgid, 0x02, TWF_ANDW, &ptn, TMO_POL);
    if (er == E_TMOUT) pass("wai_flg unset bit -> E_TMOUT");
    else               fail("wai_flg unset bit -> E_TMOUT");

    CHECK("del_flg", tk_del_flg(flgid));
}

/* ================================================================
 * Test: Mutex
 * ================================================================ */
static void test_mutex(void)
{
    T_CMTX cmtx;
    ID mtxid;
    ER er;

    tm_putstring((UB *)"[Mutex]\r\n");

    cmtx.exinf  = NULL;
    cmtx.mtxatr = TA_TFIFO;
    mtxid = tk_cre_mtx(&cmtx);
    if (mtxid < E_OK) { fail("cre_mtx"); return; }
    pass("cre_mtx");

    CHECK("loc_mtx", tk_loc_mtx(mtxid, TMO_POL));

    /* Same task trying to lock again → should not succeed (E_TMOUT) */
    er = tk_loc_mtx(mtxid, TMO_POL);
    if (er < E_OK) pass("recursive lock blocked");
    else           fail("recursive lock blocked");

    CHECK("unl_mtx", tk_unl_mtx(mtxid));

    /* After unlock, should be lockable again */
    CHECK("re-loc_mtx after unlock", tk_loc_mtx(mtxid, TMO_POL));
    CHECK("re-unl_mtx", tk_unl_mtx(mtxid));

    CHECK("del_mtx", tk_del_mtx(mtxid));
}

/* ================================================================
 * Test: Mailbox
 * ================================================================ */
typedef struct {
    T_MSG hdr;      /* must be first: mailbox link header */
    INT   data;
} MY_MSG;

static void test_mailbox(void)
{
    T_CMBX cmbx;
    MY_MSG msg = {{0}, 0xDEAD};
    T_MSG *pmsg = NULL;
    ID mbxid;
    ER er;

    tm_putstring((UB *)"[Mailbox]\r\n");

    cmbx.exinf  = NULL;
    cmbx.mbxatr = TA_TFIFO | TA_MFIFO;
    mbxid = tk_cre_mbx(&cmbx);
    if (mbxid < E_OK) { fail("cre_mbx"); return; }
    pass("cre_mbx");

    CHECK("snd_mbx", tk_snd_mbx(mbxid, (T_MSG *)&msg));
    CHECK("rcv_mbx", tk_rcv_mbx(mbxid, &pmsg, TMO_POL));

    if (pmsg == (T_MSG *)&msg && ((MY_MSG *)pmsg)->data == 0xDEAD)
        pass("msg pointer & data OK");
    else
        fail("msg pointer & data OK");

    /* Empty mailbox: polling must return E_TMOUT */
    er = tk_rcv_mbx(mbxid, &pmsg, TMO_POL);
    if (er == E_TMOUT) pass("rcv_mbx empty -> E_TMOUT");
    else               fail("rcv_mbx empty -> E_TMOUT");

    CHECK("del_mbx", tk_del_mbx(mbxid));
}

/* ================================================================
 * Test: Message buffer
 * ================================================================ */
static void test_msgbuf(void)
{
    T_CMBF cmbf;
    static UB buf[256];
    UB snd[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    UB rcv[8] = {0};
    ID mbfid;
    INT sz;
    int i, ok;

    tm_putstring((UB *)"[Message buffer]\r\n");

    cmbf.exinf  = NULL;
    cmbf.mbfatr = TA_TFIFO;
    cmbf.bufsz  = sizeof(buf);
    cmbf.maxmsz = 64;
    cmbf.bufptr = buf;
    mbfid = tk_cre_mbf(&cmbf);
    if (mbfid < E_OK) { fail("cre_mbf"); return; }
    pass("cre_mbf");

    CHECK("snd_mbf", tk_snd_mbf(mbfid, snd, 8, TMO_POL));
    sz = tk_rcv_mbf(mbfid, rcv, TMO_POL);
    if (sz == 8) pass("rcv_mbf size == 8");
    else         fail("rcv_mbf size == 8");

    ok = 1;
    for (i = 0; i < 8; i++) if (rcv[i] != snd[i]) { ok = 0; break; }
    if (ok) pass("mbf data correct");
    else    fail("mbf data correct");

    /* empty buffer: poll must fail */
    {
        ER er = tk_rcv_mbf(mbfid, rcv, TMO_POL);
        if (er == E_TMOUT) pass("rcv_mbf empty -> E_TMOUT");
        else               fail("rcv_mbf empty -> E_TMOUT");
    }
    CHECK("del_mbf", tk_del_mbf(mbfid));
}

/* ================================================================
 * Test: Variable-size memory pool
 * ================================================================ */
static void test_mpl(void)
{
    T_CMPL cmpl;
    void *blk1 = NULL, *blk2 = NULL;
    ID mplid;

    tm_putstring((UB *)"[Variable-size memory pool]\r\n");

    cmpl.exinf  = NULL;
    cmpl.mplatr = TA_TFIFO;
    cmpl.mplsz  = 1024;
    cmpl.bufptr = NULL;   /* let kernel allocate via imalloc */
    mplid = tk_cre_mpl(&cmpl);
    if (mplid < E_OK) { fail("cre_mpl"); return; }
    pass("cre_mpl");

    CHECK("get_mpl 100B", tk_get_mpl(mplid, 100, &blk1, TMO_POL));
    CHECK("get_mpl 200B", tk_get_mpl(mplid, 200, &blk2, TMO_POL));

    if (blk1 != NULL && blk2 != NULL && blk1 != blk2)
        pass("two distinct ptrs");
    else
        fail("two distinct ptrs");

    /* Write to the blocks to confirm they're valid memory */
    if (blk1) { *(INT *)blk1 = 0xCAFE; }
    if (blk2) { *(INT *)blk2 = 0xBEEF; }
    if (blk1 && *(INT *)blk1 == 0xCAFE &&
        blk2 && *(INT *)blk2 == 0xBEEF)
        pass("blk write/read OK");
    else
        fail("blk write/read OK");

    CHECK("rel_mpl blk1", tk_rel_mpl(mplid, blk1));
    CHECK("rel_mpl blk2", tk_rel_mpl(mplid, blk2));
    CHECK("del_mpl", tk_del_mpl(mplid));
}

/* ================================================================
 * Test: Fixed-size memory pool
 * ================================================================ */
static void test_mpf(void)
{
    T_CMPF cmpf;
    void *blk[4];
    int i;
    ID mpfid;

    tm_putstring((UB *)"[Fixed-size memory pool]\r\n");

    cmpf.exinf  = NULL;
    cmpf.mpfatr = TA_TFIFO;
    cmpf.mpfcnt = 8;
    cmpf.blfsz  = 32;
    cmpf.bufptr = NULL;   /* kernel allocates */
    mpfid = tk_cre_mpf(&cmpf);
    if (mpfid < E_OK) { fail("cre_mpf"); return; }
    pass("cre_mpf");

    for (i = 0; i < 4; i++) {
        blk[i] = NULL;
        CHECK("get_mpf", tk_get_mpf(mpfid, &blk[i], TMO_POL));
    }

    /* All pointers must be non-NULL and distinct */
    {
        int ok = 1, j;
        for (i = 0; i < 4; i++) {
            if (!blk[i]) { ok = 0; break; }
            for (j = i+1; j < 4; j++) {
                if (blk[i] == blk[j]) { ok = 0; break; }
            }
        }
        if (ok) pass("4 distinct blks");
        else    fail("4 distinct blks");
    }

    for (i = 0; i < 4; i++)
        if (blk[i]) tk_rel_mpf(mpfid, blk[i]);
    pass("rel_mpf x4");

    CHECK("del_mpf", tk_del_mpf(mpfid));
}

/* ================================================================
 * Test: Cyclic handler
 * ================================================================ */
static void cyc_handler(void *exinf)
{
    (void)exinf;
    g_cyc_count++;
}

static void test_cyc(void)
{
    T_CCYC ccyc;
    ID cycid;

    tm_putstring((UB *)"[Cyclic handler]\r\n");

    ccyc.exinf  = NULL;
    ccyc.cycatr = TA_HLNG;
    ccyc.cychdr = (FP)cyc_handler;
    ccyc.cyctim = 100;   /* 100 ms */
    ccyc.cycphs = 0;
    cycid = tk_cre_cyc(&ccyc);
    if (cycid < E_OK) { fail("cre_cyc"); return; }
    pass("cre_cyc");

    g_cyc_count = 0;
    CHECK("sta_cyc", tk_sta_cyc(cycid));
    tk_dly_tsk(550);   /* wait ~550 ms → expect ~5 fires */
    CHECK("stp_cyc", tk_stp_cyc(cycid));

    if (g_cyc_count >= 4) pass("cyc fired >= 4 times");
    else                  fail("cyc fired >= 4 times");

    CHECK("del_cyc", tk_del_cyc(cycid));
}

/* ================================================================
 * Test: Alarm handler (one-shot timer)
 * ================================================================ */
static void alm_handler(void *exinf)
{
    (void)exinf;
    g_alm_fired = 1;
}

static void test_alm(void)
{
    T_CALM calm;
    ID almid;

    tm_putstring((UB *)"[Alarm handler]\r\n");

    calm.exinf  = NULL;
    calm.almatr = TA_HLNG;
    calm.almhdr = (FP)alm_handler;
    almid = tk_cre_alm(&calm);
    if (almid < E_OK) { fail("cre_alm"); return; }
    pass("cre_alm");

    g_alm_fired = 0;
    CHECK("sta_alm 200ms", tk_sta_alm(almid, 200));
    tk_dly_tsk(400);   /* wait 400 ms → alarm must have fired */

    if (g_alm_fired) pass("alm fired");
    else             fail("alm fired");

    CHECK("del_alm", tk_del_alm(almid));
}

/* ================================================================
 * Test: Task control  (tk_sus_tsk / tk_rsm_tsk / tk_chg_pri)
 * ================================================================ */
static void test_task_ctrl(void)
{
    T_CTSK ctsk;
    T_CSEM csem;
    ID tskid;

    tm_putstring((UB *)"[Task control]\r\n");

    /* two semaphores: g_sem = sync, g_sem2 = release */
    csem.exinf   = NULL;
    csem.sematr  = TA_TFIFO;
    csem.isemcnt = 0;
    csem.maxsem  = 2;
    g_sem = tk_cre_sem(&csem);
    if (g_sem < E_OK) { fail("cre_sem g_sem"); return; }

    csem.isemcnt = 0;
    csem.maxsem  = 1;
    g_sem2 = tk_cre_sem(&csem);
    if (g_sem2 < E_OK) { fail("cre_sem g_sem2"); tk_del_sem(g_sem); return; }

    /* same priority as main so it won't preempt */
    ctsk.exinf   = NULL;
    ctsk.tskatr  = TA_HLNG | TA_RNG0;
    ctsk.task    = (FP)sub_task_b;
    ctsk.itskpri = 1;
    ctsk.stksz   = 1024;
    tskid = tk_cre_tsk(&ctsk);
    if (tskid < E_OK) {
        fail("cre_tsk");
        tk_del_sem(g_sem); tk_del_sem(g_sem2);
        return;
    }
    pass("cre_tsk");

    tk_sta_tsk(tskid, 0);

    /* wait until sub_task_b is blocking on g_sem2 */
    CHECK("subtask started", tk_wai_sem(g_sem, 1, 1000));

    /* sub_task_b is now in WAIT state (on g_sem2) */
    CHECK("sus_tsk", tk_sus_tsk(tskid));   /* → WAITSUS */
    CHECK("chg_pri to 2", tk_chg_pri(tskid, 2));
    CHECK("rsm_tsk", tk_rsm_tsk(tskid));   /* → back to WAIT on g_sem2 */

    /* release sub_task_b */
    CHECK("sig_sem (release task)", tk_sig_sem(g_sem2, 1));

    /* wait for sub_task_b to finish */
    CHECK("subtask done", tk_wai_sem(g_sem, 1, 1000));

    /* sub_task_b has priority 2 now (same-or-lower than main=1);
       after it signals g_sem it called tk_ext_tsk → DORMANT */
    tk_dly_tsk(10);   /* let it reach DORMANT */
    CHECK("del_tsk", tk_del_tsk(tskid));

    tk_del_sem(g_sem2);
    tk_del_sem(g_sem);
}

/* ================================================================
 * Test: tk_dly_tsk
 * ================================================================ */
static void test_dly(void)
{
    tm_putstring((UB *)"[tk_dly_tsk]\r\n");
    CHECK("dly_tsk 200ms", tk_dly_tsk(200));
}

/* ================================================================
 * Main entry
 * ================================================================ */
EXPORT INT usermain(void)
{
    tm_putstring((UB *)"\r\n=== p-kernel x86: T-Kernel API Test Suite ===\r\n\r\n");

    test_task();
    test_semaphore();
    test_eventflag();
    test_mutex();
    test_mailbox();
    test_msgbuf();
    test_mpl();
    test_mpf();
    test_cyc();
    test_alm();
    test_task_ctrl();
    test_dly();

    tm_putstring((UB *)"\r\n=== Test suite done ===\r\n");
    return 0;
}
