/*
 *  20_multicast/multicast.c — UDP マルチキャスト デモ
 *
 *  Tests:
 *    sys_udp_bind()        — ポートバインド
 *    sys_udp_join_group()  — マルチキャストグループ参加 (239.1.1.1)
 *    sys_udp_send()        — マルチキャスト送信 (ソフトウェアループバック)
 *    sys_udp_recv()        — マルチキャスト受信
 *    sys_udp_leave_group() — グループ脱退
 *
 *  Group: 239.1.1.1  Port: 4000  (single-node software loopback)
 */
#include "plibc.h"

#define MCAST_IP    SYS_IP4(239, 1, 1, 1)
#define MCAST_PORT  4000
#define SRC_PORT    4001
#define N_MSGS      3

static volatile int recv_count = 0;
static int done_sem;

/* ----------------------------------------------------------------- */
/* Sender task: sends N_MSGS datagrams to the multicast group        */
/* ----------------------------------------------------------------- */
static void sender_task(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    tk_dly_tsk(80);   /* let main finish bind + join first */

    static const char msg[] = "p-kernel mcast hello";
    PK_SYS_UDP_SEND pk;
    pk.dst_ip   = MCAST_IP;
    pk.src_port = SRC_PORT;
    pk.dst_port = MCAST_PORT;
    pk.buf      = msg;
    pk.len      = sizeof(msg) - 1;
    pk._pad     = 0;

    for (int i = 0; i < N_MSGS; i++) {
        sys_udp_send(&pk);
        tk_dly_tsk(120);
    }

    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* Main                                                               */
/* ----------------------------------------------------------------- */
void _start(void)
{
    int ok = 1;

    /* ---- セマフォ作成 ------------------------------------------- */
    PK_CSEM cs = { (void *)0, 0, 2 };
    done_sem = tk_cre_sem(&cs);

    /* ---- UDP ポートバインド ------------------------------------- */
    int r = sys_udp_bind(MCAST_PORT);
    if (r == 0) {
        plib_puts("[20] udp_bind OK (port=4000)\r\n");
    } else {
        plib_puts("[20] udp_bind FAIL\r\n");
        ok = 0;
        goto finish;
    }

    /* ---- マルチキャストグループ参加 ----------------------------- */
    r = sys_udp_join_group(MCAST_PORT, MCAST_IP);
    if (r == 0) {
        plib_puts("[20] join_group OK (239.1.1.1)\r\n");
    } else {
        plib_puts("[20] join_group FAIL\r\n");
        ok = 0;
        goto finish;
    }

    /* ---- 送信タスク起動 ---------------------------------------- */
    PK_CRE_TSK ct;
    ct.task     = sender_task;
    ct.pri      = 8;
    ct.stksz    = 0;
    ct.policy   = SCHED_FIFO;
    ct.slice_ms = 0;
    ct.exinf    = (void *)0;
    int tid = tk_cre_tsk(&ct);
    tk_sta_tsk(tid, 0);

    /* ---- N_MSGS パケット受信 ------------------------------------ */
    static unsigned char buf[64];
    for (int i = 0; i < N_MSGS && ok; i++) {
        PK_SYS_UDP_RECV pk;
        pk.port       = MCAST_PORT;
        pk._pad       = 0;
        pk.buf        = buf;
        pk.buflen     = (unsigned short)sizeof(buf);
        pk._pad2      = 0;
        pk.timeout_ms = 2000;

        int n = sys_udp_recv(&pk);
        if (n > 0) {
            recv_count++;
            plib_puts("[20] recv[");
            plib_puti(i);
            plib_puts("]: ");
            /* print received text (NUL-terminate manually) */
            int len = n < 48 ? n : 48;
            buf[len] = '\0';
            plib_puts((const char *)buf);
            plib_puts("  src=");
            plib_put_ip(pk.src_ip);
            plib_puts("\r\n");
        } else {
            plib_puts("[20] recv TIMEOUT\r\n");
            ok = 0;
        }
    }

    /* ---- 送信タスク完了待ち ------------------------------------ */
    tk_wai_sem(done_sem, 1, 3000);
    tk_del_sem(done_sem);

    /* ---- グループ脱退 ----------------------------------------- */
    sys_udp_leave_group(MCAST_PORT, MCAST_IP);
    plib_puts("[20] leave_group OK\r\n");

finish:
    if (ok && recv_count == N_MSGS) {
        plib_puts("[20] multicast => OK\r\n");
    } else {
        plib_puts("[20] multicast => FAIL\r\n");
    }
    sys_exit(0);
}
