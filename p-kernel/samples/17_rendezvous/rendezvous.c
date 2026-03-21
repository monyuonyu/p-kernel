/*
 *  17_rendezvous/rendezvous.c — T-Kernel rendezvous port API demo
 *
 *  Tests:
 *    tk_cre_por()  — create rendezvous port
 *    tk_cal_por()  — call (send request + wait for reply)
 *    tk_acp_por()  — accept rendezvous (receive request)
 *    tk_rpl_rdv()  — reply to rendezvous
 *    tk_del_por()  — delete rendezvous port
 *
 *  Protocol:
 *    Caller sends 4-byte request {42, 0, 0, 0}.
 *    Acceptor doubles the first byte and replies with {84, 0, 0, 0}.
 *    Caller checks reply == 84.
 */
#include "plibc.h"

#define MSG_SZ   4      /* call/reply message size in bytes */
#define CALPTN   0x01   /* call pattern bit */

static int por_id;
static volatile int test_ok = 0;

/* ----------------------------------------------------------------- */
/* Acceptor task: receives call, sends reply                         */
/* ----------------------------------------------------------------- */
static void acceptor_task(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    unsigned char msg[MSG_SZ];
    unsigned int rdvno = 0;

    /* Accept one rendezvous call */
    int r = tk_acp_por(por_id, CALPTN, &rdvno, msg, TMO_FEVR);
    if (r < 0) {
        plib_puts("[17] acp_por FAIL\r\n");
        tk_ext_tsk();
    }

    /* Echo: double the first byte */
    msg[0] = (unsigned char)(msg[0] * 2);

    /* Send reply */
    r = tk_rpl_rdv(rdvno, msg, MSG_SZ);
    if (r == 0) {
        test_ok = 1;
    }

    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* Main task                                                          */
/* ----------------------------------------------------------------- */
void _start(void)
{
    int ok = 1;

    /* Create rendezvous port */
    PK_CPOR cpor;
    cpor.poratr  = TA_RDV_TFIFO;
    cpor.maxcmsz = MSG_SZ;
    cpor.maxrmsz = MSG_SZ;

    por_id = tk_cre_por(&cpor);
    if (por_id <= 0) {
        plib_puts("[17] tk_cre_por FAIL\r\n");
        sys_exit(1);
    }
    plib_puts("[17] tk_cre_por OK: porid=");
    plib_puti(por_id);
    plib_puts("\r\n");

    /* Start acceptor task */
    PK_CRE_TSK ct;
    ct.task     = acceptor_task;
    ct.pri      = 7;   /* slightly higher priority so it runs first */
    ct.stksz    = 0;
    ct.policy   = SCHED_FIFO;
    ct.slice_ms = 0;
    ct.exinf    = NULL;
    int atid = tk_cre_tsk(&ct);
    tk_sta_tsk(atid, 0);

    /* Caller: send request {42} and wait for reply */
    unsigned char msg[MSG_SZ];
    msg[0] = 42;
    msg[1] = msg[2] = msg[3] = 0;

    int r = tk_cal_por(por_id, CALPTN, msg, MSG_SZ, 2000);
    if (r < 0) {
        plib_puts("[17] tk_cal_por FAIL: err=");
        plib_puti(r);
        plib_puts("\r\n");
        ok = 0;
    } else {
        /* r = reply message size */
        plib_puts("[17] tk_cal_por OK: reply[0]=");
        plib_putu(msg[0]);
        plib_puts("\r\n");
        if (msg[0] != 84) {
            plib_puts("[17] reply mismatch (expected 84)\r\n");
            ok = 0;
        }
    }

    if (!test_ok) {
        plib_puts("[17] acceptor side FAIL\r\n");
        ok = 0;
    }

    tk_del_por(por_id);
    plib_puts(ok ? "[17] rendezvous => OK\r\n" : "[17] rendezvous => FAIL\r\n");
    sys_exit(0);
}
