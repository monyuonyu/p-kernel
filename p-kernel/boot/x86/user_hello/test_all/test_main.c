/*
 *  test_all/test_main.c — テストスイート エントリーポイント
 *
 *  このファイルの役割:
 *    - グローバル変数の実体を定義する
 *    - pass() / fail() / start_task() などのヘルパー関数を定義する
 *    - 共有タスク関数 task_sem_signal / task_flg_set を定義する
 *    - _start() から各テストモジュールを順番に呼び出す
 *
 *  ビルド方法:
 *    make test_all/test_all.elf
 *
 *  QEMU 実行方法:
 *    p-kernel> exec test_all.elf
 */

#include "test_common.h"

/* ================================================================= */
/* グローバル変数の実体                                               */
/* ================================================================= */

int          g_pass = 0;
int          g_fail = 0;
int          g_sem  = -1;
int          g_flg  = -1;
volatile int g_order[16];
volatile int g_order_cnt = 0;

/* ================================================================= */
/* テスト結果出力ヘルパー                                             */
/* ================================================================= */

void pass(const char *name)
{
    plib_puts("[PASS] ");
    plib_puts(name);
    plib_puts("\r\n");
    g_pass++;
}

void fail(const char *name, const char *reason)
{
    plib_puts("[FAIL] ");
    plib_puts(name);
    plib_puts(" : ");
    plib_puts(reason);
    plib_puts("\r\n");
    g_fail++;
}

/* ================================================================= */
/* タスク作成・起動ショートカット                                      */
/* ================================================================= */

/*
 *  start_task — PK_CRE_TSK を組み立ててタスクを作成・即起動します。
 *
 *  引数:
 *    fn        タスクのエントリー関数
 *    pri       優先度 (1=最高 〜 16=最低)
 *    policy    SCHED_FIFO または SCHED_RR
 *    slice_ms  RR タイムスライス（ms）; FIFO では 0 を指定
 *    stacd     タスクに渡すスタートコード (fn の第1引数)
 *    exinf     タスクに渡す拡張情報ポインタ (fn の第2引数)
 *
 *  戻り値: タスク ID (>= 0) または エラーコード (< 0)
 */
int start_task(void (*fn)(int, void*), int pri, int policy,
               int slice_ms, int stacd, void *exinf)
{
    PK_CRE_TSK pk = {
        .task     = fn,
        .pri      = pri,
        .policy   = policy,
        .slice_ms = slice_ms,
        .stksz    = 0,          /* 0 = デフォルト 4096 バイト */
        .exinf    = exinf
    };
    int tid = tk_cre_tsk(&pk);
    if (tid < 0) return tid;    /* タスク作成失敗 (E_LIMIT など) */
    tk_sta_tsk(tid, stacd);
    return tid;
}

/* ================================================================= */
/* 共有タスク関数                                                     */
/* ================================================================= */

/*
 *  task_sem_signal — セマフォに +1 シグナルして終了するタスク
 *
 *  使い方:
 *    int semid = tk_cre_sem(&csem);
 *    start_task(task_sem_signal, 8, SCHED_FIFO, 0, 0, (void*)(long)semid);
 *    tk_wai_sem(semid, 1, 5000);  ← このタスクの完了を待つ
 */
void task_sem_signal(int stacd, void *exinf)
{
    (void)stacd;
    int semid = (int)(long)exinf;   /* exinf にセマフォIDを渡す */
    tk_sig_sem(semid, 1);
    tk_ext_tsk();
}

/*
 *  task_flg_set — イベントフラグにビットをセットして終了するタスク
 *
 *  exinf のエンコード:
 *    上位 16bit = フラグ ID
 *    下位 16bit = セットするビットパターン
 *
 *  使い方:
 *    int packed = (flgid << 16) | 0x01;
 *    start_task(task_flg_set, 8, SCHED_FIFO, 0, 0, (void*)(long)packed);
 */
void task_flg_set(int stacd, void *exinf)
{
    (void)stacd;
    int packed         = (int)(long)exinf;
    int flgid          = (packed >> 16) & 0xFFFF;
    unsigned int bits  = (unsigned int)(packed & 0xFFFF);
    tk_set_flg(flgid, bits);
    tk_ext_tsk();
}

/* ================================================================= */
/* エントリーポイント                                                 */
/* ================================================================= */

void _start(void)
{
    plib_puts("\r\n");
    plib_puts("============================================\r\n");
    plib_puts("  p-kernel テストスイート (test_all.elf)\r\n");
    plib_puts("============================================\r\n\r\n");

    /* ---- 各テストモジュールを順番に呼び出す ---- */

    plib_puts(">>> POSIX ファイル I/O テスト\r\n");
    test_posix();

    plib_puts(">>> セマフォ テスト\r\n");
    test_sem();

    plib_puts(">>> イベントフラグ テスト\r\n");
    test_flg();

    plib_puts(">>> タスク管理 テスト\r\n");
    test_task();

    plib_puts(">>> スケジューリング テスト\r\n");
    test_sched();

    /* ---- 最終結果を表示 ---- */
    plib_puts("\r\n============================================\r\n");
    plib_puts("  結果: PASS=");
    plib_puti(g_pass);
    plib_puts("  FAIL=");
    plib_puti(g_fail);
    plib_puts("\r\n");
    if (g_fail == 0)
        plib_puts("  全テスト合格!\r\n");
    else
        plib_puts("  失敗あり — 上記 [FAIL] を確認してください\r\n");
    plib_puts("============================================\r\n");

    sys_exit(g_fail == 0 ? 0 : 1);
}
