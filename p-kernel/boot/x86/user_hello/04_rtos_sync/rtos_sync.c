/*
 *  04_rtos_sync/rtos_sync.c — 同期プリミティブ サンプル
 *
 *  複数のタスクが協調して動作するには「同期」が必要です。
 *  p-kernel には 2 種類の同期プリミティブがあります。
 *
 *  ─────────────────────────────────────────────
 *  セマフォ (Semaphore)
 *  ─────────────────────────────────────────────
 *
 *  カウント付きの「信号機」です。
 *
 *    tk_cre_sem()   セマフォを作成
 *    tk_sig_sem(n)  カウントを +n する（タスクが「完了した」と通知）
 *    tk_wai_sem(n)  カウントが n 以上になるまで待つ（「n 個完了」まで待機）
 *    tk_del_sem()   セマフォを削除
 *
 *  典型的な使い方:
 *    メインタスクが「n 個のワーカータスクの完了」を待つ場合、
 *    セマフォの初期値を 0 にし、各ワーカーが完了時に +1 シグナル、
 *    メインが n 個のシグナルを待つ。
 *
 *  ─────────────────────────────────────────────
 *  イベントフラグ (Event Flag)
 *  ─────────────────────────────────────────────
 *
 *  ビットパターンで複数の「イベント」を同時に管理できます。
 *
 *    tk_cre_flg()         イベントフラグを作成
 *    tk_set_flg(bits)     指定ビットをセット
 *    tk_clr_flg(bits)     指定ビットをクリア（bits はクリアするマスク）
 *    tk_wai_flg(pat, mode) パターン成立まで待つ
 *    tk_del_flg()         イベントフラグを削除
 *
 *  待ちモード:
 *    TWF_ANDW (0x00): waiptn のビットが「すべてセット」されるまで待つ
 *    TWF_ORW  (0x01): waiptn のビットが「どれかセット」されるまで待つ
 *
 *  ─────────────────────────────────────────────
 *  このサンプルで学べること
 *  ─────────────────────────────────────────────
 *    1. セマフォによるタスク完了待ち
 *    2. セマフォのタイムアウト (TMO_POL / 有限時間)
 *    3. イベントフラグ AND 待ち (TWF_ANDW)
 *    4. イベントフラグ OR 待ち  (TWF_ORW)
 *    5. フラグのクリア操作
 *
 *  ビルド方法:
 *    make 04_rtos_sync/rtos_sync.elf
 *
 *  QEMU 実行方法:
 *    p-kernel> exec rtos_sync.elf
 */

#include "plibc.h"

/* ─────────────────────────────────────────────────────────────
 *  デモ1: セマフォによるタスク完了待ち
 * ───────────────────────────────────────────────────────────── */

static int g_sem = -1;
static int g_flg = -1;

/* ワーカータスク: 処理を行い、セマフォで完了を通知する */
static void worker_task(int stacd, void *exinf)
{
    (void)exinf;
    /* stacd でワーカー番号を受け取る */
    plib_puts("  [ワーカー");
    plib_puti(stacd);
    plib_puts("] 処理中...\r\n");

    /* 処理時間のシミュレーション（スリープで代替） */
    tk_slp_tsk(stacd * 20);    /* ワーカー番号 × 20ms */

    plib_puts("  [ワーカー");
    plib_puti(stacd);
    plib_puts("] 完了 → セマフォ送信\r\n");

    tk_sig_sem(g_sem, 1);   /* カウント +1 */
    tk_ext_tsk();
}

static void demo_semaphore(void)
{
    plib_puts("--- [1] セマフォ: 3 タスクの完了待ち ---\r\n");

    /* セマフォを作成 (初期カウント=0, 最大=8) */
    PK_CSEM csem = { 0, 0, 8 };
    g_sem = tk_cre_sem(&csem);
    plib_puts("  セマフォ作成 (id=");
    plib_puti(g_sem);
    plib_puts(")\r\n\r\n");

    /* 3 つのワーカータスクを起動 */
    int tids[3];
    for (int i = 0; i < 3; i++) {
        PK_CRE_TSK pk = {
            .task   = worker_task,
            .pri    = 8,
            .policy = SCHED_FIFO,
            .stksz  = 0, .slice_ms = 0,
            .exinf  = 0,
        };
        tids[i] = tk_cre_tsk(&pk);
        tk_sta_tsk(tids[i], i + 1);    /* stacd = ワーカー番号 (1,2,3) */
    }

    plib_puts("  [メイン] 3 タスクの完了を待機中...\r\n");

    /* カウントが 3 になるまで待つ（3 ワーカーが全員完了するまで） */
    int r = tk_wai_sem(g_sem, 3, 5000);
    if (r == 0)
        plib_puts("  [メイン] 3 タスクすべて完了!\r\n");
    else {
        plib_puts("  [メイン] タイムアウト (E_TMOUT)\r\n");
    }

    for (int i = 0; i < 3; i++) tk_del_tsk(tids[i]);
    tk_del_sem(g_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ2: セマフォのタイムアウト (TMO_POL)
 * ───────────────────────────────────────────────────────────── */

static void demo_semaphore_poll(void)
{
    plib_puts("--- [2] セマフォ タイムアウト (TMO_POL) ---\r\n");

    PK_CSEM csem = { 0, 0, 4 };
    g_sem = tk_cre_sem(&csem);

    /* TMO_POL (=0): 即時ポーリング — カウントが 0 なので失敗する */
    int r = tk_wai_sem(g_sem, 1, TMO_POL);
    plib_puts("  カウント=0 で TMO_POL: r=");
    plib_puti(r);
    plib_puts(" (E_TMOUT=-50 を期待)\r\n");

    /* カウントを +1 してから再試行 */
    tk_sig_sem(g_sem, 1);
    r = tk_wai_sem(g_sem, 1, TMO_POL);
    plib_puts("  sig 後に TMO_POL: r=");
    plib_puti(r);
    plib_puts(" (0=成功 を期待)\r\n");

    tk_del_sem(g_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ3: イベントフラグ AND 待ち (TWF_ANDW)
 *  ─────────────────────────────────────────────────────────────
 *
 *  「条件 A AND 条件 B AND 条件 C がすべて成立するまで待つ」パターン。
 *  それぞれの条件を別々のタスクが担当します。
 * ───────────────────────────────────────────────────────────── */

/* 指定ビットをセットして終了するタスク */
static void flag_setter(int stacd, void *exinf)
{
    (void)exinf;
    unsigned int bit = (unsigned int)stacd;

    plib_puts("  [フラグセッター bit=0x");
    plib_putu(bit);
    plib_puts("] セット\r\n");

    tk_set_flg(g_flg, bit);
    tk_ext_tsk();
}

static void demo_eventflag_andw(void)
{
    plib_puts("--- [3] イベントフラグ AND 待ち (TWF_ANDW) ---\r\n");
    plib_puts("  bit0, bit1, bit2 がすべてセットされるまで待ちます\r\n\r\n");

    /* イベントフラグを作成 (初期パターン=0x00) */
    PK_CFLG cflg = { 0, 0x00 };
    g_flg = tk_cre_flg(&cflg);

    /* 3 つのタスクが別々のビットをセットする */
    int tids[3];
    unsigned int bits[] = { 0x01, 0x02, 0x04 };
    for (int i = 0; i < 3; i++) {
        PK_CRE_TSK pk = {
            .task   = flag_setter,
            .pri    = 8,
            .policy = SCHED_FIFO,
            .stksz  = 0, .slice_ms = 0, .exinf = 0,
        };
        tids[i] = tk_cre_tsk(&pk);
        tk_sta_tsk(tids[i], (int)bits[i]);   /* stacd = ビット値 */
    }

    /* 3 ビットがすべてセットされるまで待つ */
    unsigned int flgptn = 0;
    int r = tk_wai_flg(g_flg, 0x07, TWF_ANDW, &flgptn, 5000);
    plib_puts("  AND 待ち完了: r=");
    plib_puti(r);
    plib_puts(", pattern=0x");
    plib_putu(flgptn);
    plib_puts(" (0x7 を期待)\r\n");

    for (int i = 0; i < 3; i++) tk_del_tsk(tids[i]);
    tk_del_flg(g_flg);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ4: イベントフラグ OR 待ち (TWF_ORW) とフラグクリア
 * ───────────────────────────────────────────────────────────── */

static void demo_eventflag_orw(void)
{
    plib_puts("--- [4] イベントフラグ OR 待ち / フラグクリア ---\r\n");
    plib_puts("  bit0 OR bit2 のどちらかがセットされるまで待ちます\r\n\r\n");

    PK_CFLG cflg = { 0, 0x00 };
    g_flg = tk_cre_flg(&cflg);

    /* bit2 (0x04) だけをセットするタスクを起動 */
    PK_CRE_TSK pk = {
        .task   = flag_setter,
        .pri    = 8,
        .policy = SCHED_FIFO,
        .stksz  = 0, .slice_ms = 0, .exinf = 0,
    };
    int tid = tk_cre_tsk(&pk);
    tk_sta_tsk(tid, 0x04);    /* bit2 をセット */

    /* bit0 OR bit2 — bit2 だけでも条件成立 */
    unsigned int flgptn = 0;
    int r = tk_wai_flg(g_flg, 0x05, TWF_ORW, &flgptn, 5000);
    plib_puts("  OR 待ち完了: r=");
    plib_puti(r);
    plib_puts(", pattern=0x");
    plib_putu(flgptn);
    plib_puts("\r\n");

    /* ── フラグのクリア ──────────────────────────────────────── */
    plib_puts("  tk_clr_flg で全ビットをクリア\r\n");
    tk_clr_flg(g_flg, 0x00);   /* 0x00 を「残すビット」として指定 → 全クリア */

    /* クリア後は TMO_POL で失敗するはず */
    r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &flgptn, TMO_POL);
    plib_puts("  クリア後 TMO_POL: r=");
    plib_puti(r);
    plib_puts(" (E_TMOUT=-50 を期待)\r\n");

    tk_del_tsk(tid);
    tk_del_flg(g_flg);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  エントリーポイント
 * ───────────────────────────────────────────────────────────── */
void _start(void)
{
    plib_puts("=== 同期プリミティブ サンプル ===\r\n\r\n");

    demo_semaphore();
    demo_semaphore_poll();
    demo_eventflag_andw();
    demo_eventflag_orw();

    plib_puts("=== 完了 ===\r\n");
    plib_puts("全機能テスト: exec test_all.elf\r\n");
    sys_exit(0);
}
