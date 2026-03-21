/*
 *  12_msgbuf/msgbuf.c
 *
 *  Message Buffer (メッセージバッファ) サンプル
 *  可変長バイナリメッセージをバッファ経由でタスク間転送します。
 *  mailbox と違い、メッセージの「中身」をバッファにコピーします。
 *
 *  学べること:
 *    - tk_cre_mbf(PK_CMBF*) — message buffer 作成 (ユーザーバッファ指定)
 *    - tk_snd_mbf(mbfid, msg, msgsz, tmout) — 送信 (コピーイン)
 *    - tk_rcv_mbf(mbfid, buf, tmout)        — 受信 (コピーアウト); 戻り値=受信バイト数
 *    - tk_del_mbf(mbfid)                    — message buffer 削除
 *
 *  実行例:
 *    p-kernel> exec msgbuf.elf
 */
#include "plibc.h"

/* メッセージバッファ用バッキングストレージ */
#define BUF_SIZE   256
#define MAX_MSG    64

static unsigned char mbf_storage[BUF_SIZE];
static int mbfid;
static int done_sem;

/* 送信タスク: 3種類のサイズのメッセージを送る */
static void sender(int stacd, void *exinf)
{
    (void)exinf;

    const char *msgs[] = { "hello", "world!", "p-kernel msgbuf OK" };
    int counts[] = { 5, 6, 18 };

    for (int i = 0; i < 3; i++) {
        int r = tk_snd_mbf(mbfid, msgs[i], counts[i], TMO_FEVR);
        plib_puts("  snd["); plib_puti(i); plib_puts("]: sz=");
        plib_puti(counts[i]);
        plib_puts(" r="); plib_puti(r); plib_puts("\r\n");
        tk_slp_tsk(20);
    }

    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

/* 受信タスク */
static void receiver(int stacd, void *exinf)
{
    (void)exinf;
    unsigned char buf[MAX_MSG];

    for (int i = 0; i < 3; i++) {
        int n = tk_rcv_mbf(mbfid, buf, TMO_FEVR);
        if (n < 0) {
            plib_puts("  rcv_mbf error: "); plib_puti(n); plib_puts("\r\n");
            break;
        }
        buf[n] = '\0';
        plib_puts("  rcv["); plib_puti(i); plib_puts("]: sz=");
        plib_puti(n); plib_puts(" data=\"");
        sys_write(1, buf, n);
        plib_puts("\"\r\n");
    }

    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" msgbuf: メッセージバッファデモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* message buffer 作成 */
    PK_CMBF cmbf = {
        .mbfatr = 0,                   /* TA_TFIFO */
        .bufsz  = BUF_SIZE,
        .maxmsz = MAX_MSG,
        .bufptr = mbf_storage,
    };
    mbfid = tk_cre_mbf(&cmbf);
    if (mbfid < 0) {
        plib_puts("ERROR: tk_cre_mbf failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] msgbuf created (id="); plib_puti(mbfid);
    plib_puts(" bufsz="); plib_puti(BUF_SIZE);
    plib_puts(" maxmsz="); plib_puti(MAX_MSG);
    plib_puts(")\r\n\r\n");

    PK_CSEM csem = { NULL, 0, 2 };
    done_sem = tk_cre_sem(&csem);

    PK_CRE_TSK ctr = { receiver, 7, 0, SCHED_FIFO, 0, NULL };
    PK_CRE_TSK cts = { sender,   8, 0, SCHED_FIFO, 0, NULL };
    int tid_r = tk_cre_tsk(&ctr);
    int tid_s = tk_cre_tsk(&cts);

    if (tid_r < 0 || tid_s < 0) {
        plib_puts("ERROR: tk_cre_tsk failed\r\n");
        sys_exit(1);
    }

    tk_sta_tsk(tid_r, 0);
    tk_sta_tsk(tid_s, 0);

    tk_wai_sem(done_sem, 2, TMO_FEVR);

    tk_del_sem(done_sem);
    tk_del_mbf(mbfid);

    plib_puts("\r\n========================================\r\n");
    plib_puts(" msgbuf: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
