/*
 *  18_ref/ref.c — T-Kernel ref API + system info demo
 *
 *  Creates one object of each type, calls the corresponding ref
 *  function, and verifies the returned values are plausible.
 *
 *  Objects tested:
 *    semaphore  → tk_ref_sem()
 *    event flag → tk_ref_flg()
 *    mutex      → tk_ref_mtx()
 *    mailbox    → tk_ref_mbx()
 *    msg buf    → tk_ref_mbf()
 *    var pool   → tk_ref_mpl()
 *    fixed pool → tk_ref_mpf()
 *    cyclic hdl → tk_ref_cyc()
 *    alarm hdl  → tk_ref_alm()
 *    system     → tk_ref_ver() + tk_ref_sys()
 */
#include "plibc.h"

/* Backing buffers for msgbuf / mempools */
static unsigned char mbf_buf[256];
static unsigned char mpl_buf[256];
static unsigned char mpf_buf[8 * 32];  /* 8 blocks × 32 bytes */

/* Cyclic/alarm handlers (must not block) */
static void dummy_cyc_hdr(void) {}
static void dummy_alm_hdr(void) {}

void _start(void)
{
    int ok = 1;
    int r;

    /* ---- Semaphore -------------------------------------------- */
    PK_CSEM csem = { NULL, 3, 10 };
    int semid = tk_cre_sem(&csem);
    PK_REF_SEM rsem;
    r = tk_ref_sem(semid, &rsem);
    if (r == 0 && rsem.semcnt == 3) {
        plib_puts("[18] ref_sem OK: semcnt=");
        plib_puti(rsem.semcnt);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_sem FAIL\r\n");
        ok = 0;
    }
    tk_del_sem(semid);

    /* ---- Event flag ------------------------------------------- */
    PK_CFLG cflg = { NULL, 0xAB };
    int flgid = tk_cre_flg(&cflg);
    PK_REF_FLG rflg;
    r = tk_ref_flg(flgid, &rflg);
    if (r == 0 && rflg.flgptn == 0xAB) {
        plib_puts("[18] ref_flg OK: flgptn=0x");
        plib_putu(rflg.flgptn);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_flg FAIL\r\n");
        ok = 0;
    }
    tk_del_flg(flgid);

    /* ---- Mutex ------------------------------------------------- */
    PK_CMTX cmtx = { TA_MTX_TFIFO, 0 };
    int mtxid = tk_cre_mtx(&cmtx);
    PK_REF_MTX rmtx;
    r = tk_ref_mtx(mtxid, &rmtx);
    if (r == 0 && rmtx.htsk == 0) {
        plib_puts("[18] ref_mtx OK: htsk=0 (unlocked)\r\n");
    } else {
        plib_puts("[18] ref_mtx FAIL\r\n");
        ok = 0;
    }
    tk_del_mtx(mtxid);

    /* ---- Mailbox ----------------------------------------------- */
    int mbxid = tk_cre_mbx(TA_MBX_TFIFO);
    PK_REF_MBX rmbx;
    r = tk_ref_mbx(mbxid, &rmbx);
    if (r == 0 && rmbx.wtsk == 0) {
        plib_puts("[18] ref_mbx OK: wtsk=0 (no waiters)\r\n");
    } else {
        plib_puts("[18] ref_mbx FAIL\r\n");
        ok = 0;
    }
    tk_del_mbx(mbxid);

    /* ---- Message buffer --------------------------------------- */
    PK_CMBF cmbf = { 0, sizeof(mbf_buf), 64, mbf_buf };
    int mbfid = tk_cre_mbf(&cmbf);
    PK_REF_MBF rmbf;
    r = tk_ref_mbf(mbfid, &rmbf);
    if (r == 0 && rmbf.maxmsz == 64) {
        plib_puts("[18] ref_mbf OK: maxmsz=");
        plib_puti(rmbf.maxmsz);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_mbf FAIL\r\n");
        ok = 0;
    }
    tk_del_mbf(mbfid);

    /* ---- Variable memory pool --------------------------------- */
    PK_CMPL cmpl = { 0, sizeof(mpl_buf), mpl_buf };
    int mplid = tk_cre_mpl(&cmpl);
    PK_REF_MPL rmpl;
    r = tk_ref_mpl(mplid, &rmpl);
    if (r == 0 && rmpl.frsz > 0) {
        plib_puts("[18] ref_mpl OK: frsz=");
        plib_puti(rmpl.frsz);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_mpl FAIL\r\n");
        ok = 0;
    }
    tk_del_mpl(mplid);

    /* ---- Fixed memory pool ------------------------------------ */
    PK_CMPF cmpf = { 0, 8, 32, mpf_buf };
    int mpfid = tk_cre_mpf(&cmpf);
    PK_REF_MPF rmpf;
    r = tk_ref_mpf(mpfid, &rmpf);
    if (r == 0 && rmpf.frbcnt == 8) {
        plib_puts("[18] ref_mpf OK: frbcnt=");
        plib_puti(rmpf.frbcnt);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_mpf FAIL\r\n");
        ok = 0;
    }
    tk_del_mpf(mpfid);

    /* ---- Cyclic handler --------------------------------------- */
    PK_CCYC ccyc = { TA_CYC_STA, dummy_cyc_hdr, 500, 0 };
    int cycid = tk_cre_cyc(&ccyc);
    PK_REF_CYC rcyc;
    r = tk_ref_cyc(cycid, &rcyc);
    if (r == 0) {
        plib_puts("[18] ref_cyc OK: cycstat=");
        plib_putu(rcyc.cycstat);
        plib_puts(" lfttim=");
        plib_puti(rcyc.lfttim_ms);
        plib_puts(" ms\r\n");
    } else {
        plib_puts("[18] ref_cyc FAIL\r\n");
        ok = 0;
    }
    tk_stp_cyc(cycid);
    tk_del_cyc(cycid);

    /* ---- Alarm handler --------------------------------------- */
    PK_CALM calm = { 0, dummy_alm_hdr };
    int almid = tk_cre_alm(&calm);
    tk_sta_alm(almid, 2000);  /* fire after 2 s */
    PK_REF_ALM ralm;
    r = tk_ref_alm(almid, &ralm);
    if (r == 0) {
        plib_puts("[18] ref_alm OK: almstat=");
        plib_putu(ralm.almstat);
        plib_puts(" lfttim=");
        plib_puti(ralm.lfttim_ms);
        plib_puts(" ms\r\n");
    } else {
        plib_puts("[18] ref_alm FAIL\r\n");
        ok = 0;
    }
    tk_stp_alm(almid);
    tk_del_alm(almid);

    /* ---- Version info --------------------------------------- */
    PK_RVER rver;
    r = tk_ref_ver(&rver);
    if (r == 0) {
        plib_puts("[18] ref_ver OK: spver=0x");
        plib_putu(rver.spver);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_ver FAIL\r\n");
        ok = 0;
    }

    /* ---- System state --------------------------------------- */
    PK_RSYS rsys;
    r = tk_ref_sys(&rsys);
    if (r == 0 && rsys.runtskid > 0) {
        plib_puts("[18] ref_sys OK: runtskid=");
        plib_puti(rsys.runtskid);
        plib_puts("\r\n");
    } else {
        plib_puts("[18] ref_sys FAIL\r\n");
        ok = 0;
    }

    plib_puts(ok ? "[18] ref => OK\r\n" : "[18] ref => FAIL\r\n");
    sys_exit(0);
}
