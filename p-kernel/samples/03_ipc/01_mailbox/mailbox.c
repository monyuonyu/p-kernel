/*
 *  11_mailbox/mailbox.c
 *
 *  Mailbox (メッセージパッシング) サンプル
 *  送信タスクがメッセージを mailbox へ送り、
 *  受信タスクがそれを受け取って内容を表示します。
 *
 *  学べること:
 *    - tk_cre_mbx(mbxatr)       — mailbox 作成
 *    - tk_snd_mbx(mbxid, msg*)  — メッセージ送信
 *    - tk_rcv_mbx(mbxid, &msg*, tmout) — メッセージ受信
 *    - tk_del_mbx(mbxid)        — mailbox 削除
 *    - PK_MSG — メッセージ構造体の先頭フィールド
 *
 *  実行例:
 *    p-kernel> exec mailbox.elf
 */
#include "plibc.h"

#define MSG_COUNT  4

/* ユーザー定義メッセージ構造体 — PK_MSG を先頭に置くこと */
typedef struct {
    PK_MSG hdr;    /* MUST be first */
    int    seq;    /* シーケンス番号 */
    int    value;  /* ペイロード */
} MyMsg;

static int mbxid;
static int done_sem;

/* 送信タスク */
static void sender(int stacd, void *exinf)
{
    (void)exinf;
    /* メッセージは静的配列 (mailbox は msg へのポインタだけ保持する) */
    static MyMsg msgs[MSG_COUNT];

    for (int i = 0; i < MSG_COUNT; i++) {
        msgs[i].seq   = i;
        msgs[i].value = (i + 1) * 10;
        tk_snd_mbx(mbxid, (PK_MSG *)&msgs[i]);
        tk_slp_tsk(30);
    }
    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

/* 受信タスク */
static void receiver(int stacd, void *exinf)
{
    (void)exinf;
    for (int i = 0; i < MSG_COUNT; i++) {
        PK_MSG *pmsg = 0;
        int r = tk_rcv_mbx(mbxid, &pmsg, TMO_FEVR);
        if (r < 0 || !pmsg) {
            plib_puts("  rcv_mbx error\r\n");
            break;
        }
        MyMsg *m = (MyMsg *)pmsg;
        plib_puts("  recv: seq="); plib_puti(m->seq);
        plib_puts(" value=");     plib_puti(m->value);
        plib_puts("\r\n");
    }
    tk_sig_sem(done_sem, 1);
    tk_ext_tsk();
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" mailbox: メッセージパッシングデモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* mailbox 作成 (FIFO順) */
    mbxid = tk_cre_mbx(TA_MBX_TFIFO | TA_MFIFO);
    if (mbxid < 0) {
        plib_puts("ERROR: tk_cre_mbx failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] mailbox created (id="); plib_puti(mbxid); plib_puts(")\r\n\r\n");

    PK_CSEM csem = { NULL, 0, 2 };
    done_sem = tk_cre_sem(&csem);

    /* 受信タスクを先に起動 (mailbox で待つ) */
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
    tk_del_mbx(mbxid);

    plib_puts("\r\n========================================\r\n");
    plib_puts(" mailbox: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
