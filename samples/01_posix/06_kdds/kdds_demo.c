/*
 *  kdds_demo.c — K-DDS ローカル pub/sub デモ
 *
 *  1 つの ELF の中で publisher と subscriber を分けて動作させる。
 *
 *    publisher  : T-Kernel タスクとして起動し、1 秒おきにトピックへ発行
 *    subscriber : メインが sub を呼び出し、3 回受信したら終了
 *
 *  確認ポイント:
 *    - sys_topic_open / sys_topic_pub / sys_topic_sub / sys_topic_close
 *    - ローカルゼロコピー配信 (同一ノード内セマフォ signal)
 */

#include "plibc.h"

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void puts_s(const char *s)
{
    sys_write(1, s, plib_strlen(s));
}

static void putu(unsigned int v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) { puts_s("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    puts_s(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* publisher タスク                                                    */
/* ------------------------------------------------------------------ */

#define TOPIC_NAME  "demo/hello"

static void pub_task(int stacd, void *exinf)
{
    (void)exinf;
    int h = sys_topic_open(TOPIC_NAME, 2);   /* 2 = LATEST_ONLY */
    if (h < 0) {
        puts_s("[pub] open failed\r\n");
        tk_ext_tsk();
    }

    for (int i = 1; i <= 3; i++) {
        char msg[32];
        /* "Msg #N" を組み立て */
        const char *pfx = "Msg #";
        int mi = 0;
        for (const char *p = pfx; *p; p++) msg[mi++] = *p;
        msg[mi++] = '0' + (char)i;
        msg[mi++] = ' ';
        msg[mi++] = 's';
        msg[mi++] = 'r';
        msg[mi++] = 'c';
        msg[mi++] = '=';
        msg[mi++] = (char)('0' + (char)stacd);
        msg[mi]   = '\0';

        int r = sys_topic_pub(h, msg, mi + 1);
        if (r < 0) {
            puts_s("[pub] pub failed\r\n");
        } else {
            puts_s("[pub] published: "); puts_s(msg); puts_s("\r\n");
        }
        tk_slp_tsk(1000);
    }

    sys_topic_close(h);
    puts_s("[pub] done\r\n");
    tk_ext_tsk();
}

/* ------------------------------------------------------------------ */
/* エントリポイント                                                    */
/* ------------------------------------------------------------------ */

void _start(void)
{
    puts_s("[kdds_demo] start\r\n");

    /* subscriber ハンドルを先に開く (publisher より先に登録しておく) */
    int sub_h = sys_topic_open(TOPIC_NAME, 2);
    if (sub_h < 0) {
        puts_s("[kdds_demo] sub open failed\r\n");
        sys_exit(1);
    }

    /* publisher タスクを起動 */
    PK_CRE_TSK ct;
    ct.task    = (void *)pub_task;
    ct.pri     = 5;
    ct.stksz   = 2048;
    ct.policy  = 0;
    ct.slice_ms = 0;
    ct.exinf   = (void *)0;
    int tid = tk_cre_tsk(&ct);
    if (tid < 0) {
        puts_s("[kdds_demo] task create failed\r\n");
        sys_exit(1);
    }
    tk_sta_tsk(tid, sys_getpid());

    /* 3 回受信する */
    char buf[64];
    for (int i = 0; i < 3; i++) {
        int n = sys_topic_sub(sub_h, buf, (int)sizeof(buf), 3000);
        if (n < 0) {
            puts_s("[sub] timeout or error\r\n");
        } else {
            puts_s("[sub] received("); putu((unsigned)i + 1);
            puts_s("): "); puts_s(buf); puts_s("\r\n");
        }
    }

    sys_topic_close(sub_h);
    puts_s("[kdds_demo] done\r\n");
    sys_exit(0);
}
