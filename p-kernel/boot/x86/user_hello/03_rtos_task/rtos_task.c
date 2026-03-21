/*
 *  03_rtos_task/rtos_task.c — RTOS タスク管理サンプル
 *
 *  p-kernel は micro T-Kernel 2.0 ベースの RTOS です。
 *  スレッドに相当する「タスク」を複数作成し、並行実行できます。
 *
 *  ─────────────────────────────────────────────
 *  タスクの状態遷移
 *  ─────────────────────────────────────────────
 *
 *    tk_cre_tsk()  tk_sta_tsk()   スケジューラが選択
 *      ┌──────┐    ┌────────┐    ┌─────────┐
 *      │DORMANT│───▶│ READY  │───▶│ RUNNING │
 *      └──────┘    └────────┘    └─────────┘
 *                       ▲              │
 *                       │  待ち解除    ▼
 *                       │          ┌────────┐
 *                       └──────────│WAITING │  ← tk_slp_tsk() など
 *                                  └────────┘
 *
 *  ─────────────────────────────────────────────
 *  スケジューリングポリシー
 *  ─────────────────────────────────────────────
 *
 *    SCHED_FIFO (デフォルト): 優先度ベース抢占スケジューリング
 *      → 高優先度のタスクが即座に実行される
 *      → 同優先度のタスクは先着順（FIFO）
 *
 *    SCHED_RR: ラウンドロビンスケジューリング
 *      → 同優先度のタスク間でタイムスライスを使って交互に実行
 *      → slice_ms でタイムスライスの長さを指定（デフォルト 100ms）
 *
 *  ─────────────────────────────────────────────
 *  このサンプルで学べること
 *  ─────────────────────────────────────────────
 *    1. タスクの作成・起動・終了
 *    2. 優先度による抢占スケジューリング (FIFO)
 *    3. ラウンドロビンスケジューリング (RR)
 *    4. タスク情報の参照 (tk_ref_tsk)
 *    5. 優先度・タイムスライスの動的変更
 *
 *  ビルド方法:
 *    make 03_rtos_task/rtos_task.elf
 *
 *  QEMU 実行方法:
 *    p-kernel> exec rtos_task.elf
 */

#include "plibc.h"

/* ─────────────────────────────────────────────────────────────
 *  タスク間の完了通知用セマフォ ID（グローバル変数）
 *
 *  複数のタスクが終了を通知するために使います。
 *  グローバル変数は ring-0/ring-3 両方からアクセスできます。
 * ───────────────────────────────────────────────────────────── */
static int g_done_sem = -1;

/* ─────────────────────────────────────────────────────────────
 *  デモ1: FIFO 優先度スケジューリング
 *  ─────────────────────────────────────────────────────────────
 *
 *  高優先度タスク (pri=3) と低優先度タスク (pri=12) を作成します。
 *  高優先度タスクが先に実行され、終了してから低優先度タスクが実行されます。
 * ───────────────────────────────────────────────────────────── */

/* 高優先度タスク (pri=3, FIFO) */
static void task_high_priority(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    plib_puts("  [高優先度タスク pri=3] 実行開始\r\n");
    plib_puts("  [高優先度タスク pri=3] 計算中...\r\n");
    plib_puts("  [高優先度タスク pri=3] 完了 → セマフォ送信\r\n");
    tk_sig_sem(g_done_sem, 1);
    tk_ext_tsk();   /* タスク終了 */
}

/* 低優先度タスク (pri=12, FIFO) */
static void task_low_priority(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    plib_puts("  [低優先度タスク pri=12] 実行開始\r\n");
    plib_puts("  [低優先度タスク pri=12] 完了 → セマフォ送信\r\n");
    tk_sig_sem(g_done_sem, 1);
    tk_ext_tsk();
}

static void demo_fifo_preemption(void)
{
    plib_puts("--- [1] FIFO 優先度抢占スケジューリング ---\r\n");
    plib_puts("  低優先 (pri=12) を先に作成・起動しますが、\r\n");
    plib_puts("  高優先 (pri=3) が先に実行されます。\r\n\r\n");

    /* セマフォを作成 (初期値=0, 最大値=4) */
    PK_CSEM csem = { 0, 0, 4 };
    g_done_sem = tk_cre_sem(&csem);

    /* ── タスク作成パラメータ ────────────────────────────────── */
    PK_CRE_TSK lo = {
        .task     = task_low_priority,
        .pri      = 12,           /* 優先度 12 (数値が大きいほど低優先) */
        .policy   = SCHED_FIFO,   /* FIFO スケジューリング */
        .slice_ms = 0,            /* FIFO では未使用 */
        .stksz    = 0,            /* 0 = デフォルト (4096 バイト) */
        .exinf    = 0,            /* タスクへの引数 */
    };
    PK_CRE_TSK hi = {
        .task     = task_high_priority,
        .pri      = 3,            /* 優先度 3 (低優先より高い) */
        .policy   = SCHED_FIFO,
        .slice_ms = 0,
        .stksz    = 0,
        .exinf    = 0,
    };

    /* 低優先タスクを先に作成・起動 */
    int tid_lo = tk_cre_tsk(&lo);
    tk_sta_tsk(tid_lo, 0);

    /* 高優先タスクを後から起動 → 即座に抢占して先に実行される */
    int tid_hi = tk_cre_tsk(&hi);
    tk_sta_tsk(tid_hi, 0);

    /* 両タスクの完了を待つ（セマフォを 2 回受け取る） */
    tk_wai_sem(g_done_sem, 2, 5000);

    tk_del_tsk(tid_hi);
    tk_del_tsk(tid_lo);
    tk_del_sem(g_done_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ2: ラウンドロビン (RR) スケジューリング
 *  ─────────────────────────────────────────────────────────────
 *
 *  同じ優先度の RR タスクは「タイムスライス」単位で交互に実行されます。
 *  タイムスライスが切れると次のタスクに CPU が渡されます。
 * ───────────────────────────────────────────────────────────── */

static void task_rr(int stacd, void *exinf)
{
    (void)exinf;
    char name = (char)('A' + stacd);  /* stacd で識別: 0→A, 1→B */

    plib_puts("  [Task");
    sys_write(1, &name, 1);
    plib_puts(":RR] 開始\r\n");

    /* 30ms スリープしてから再度出力（他のタスクが実行される） */
    tk_slp_tsk(30);

    plib_puts("  [Task");
    sys_write(1, &name, 1);
    plib_puts(":RR] スリープ後に再開\r\n");

    tk_sig_sem(g_done_sem, 1);
    tk_ext_tsk();
}

static void demo_round_robin(void)
{
    plib_puts("--- [2] ラウンドロビン (RR) スケジューリング ---\r\n");
    plib_puts("  同優先度 (pri=10) の 2 タスクが交互に実行されます。\r\n\r\n");

    PK_CSEM csem = { 0, 0, 4 };
    g_done_sem = tk_cre_sem(&csem);

    /* TaskA と TaskB を同じ優先度・同じタイムスライスで作成 */
    PK_CRE_TSK pk = {
        .task     = task_rr,
        .pri      = 10,
        .policy   = SCHED_RR,     /* ラウンドロビン */
        .slice_ms = 50,           /* 50ms ごとに切り替え */
        .stksz    = 0,
        .exinf    = 0,
    };

    /* stacd (start code) でタスクを識別 */
    int tid_a = tk_cre_tsk(&pk);
    tk_sta_tsk(tid_a, 0);         /* stacd=0 → 'A' */

    int tid_b = tk_cre_tsk(&pk);
    tk_sta_tsk(tid_b, 1);         /* stacd=1 → 'B' */

    tk_wai_sem(g_done_sem, 2, 5000);

    tk_del_tsk(tid_a);
    tk_del_tsk(tid_b);
    tk_del_sem(g_done_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ3: タスク情報の参照と動的変更
 *  ─────────────────────────────────────────────────────────────
 *
 *  実行中のタスクの情報（優先度・スケジューリングポリシーなど）を
 *  tk_ref_tsk() で取得し、tk_chg_pri() / tk_chg_slt() で変更できます。
 * ───────────────────────────────────────────────────────────── */

static void task_info_target(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    tk_sig_sem(g_done_sem, 1);
    tk_ext_tsk();
}

static void demo_task_info(void)
{
    plib_puts("--- [3] タスク情報の参照と動的変更 ---\r\n");

    PK_CSEM csem = { 0, 0, 2 };
    g_done_sem = tk_cre_sem(&csem);

    PK_CRE_TSK pk = {
        .task     = task_info_target,
        .pri      = 8,
        .policy   = SCHED_RR,
        .slice_ms = 100,
        .stksz    = 0,
        .exinf    = 0,
    };
    int tid = tk_cre_tsk(&pk);

    /* ── タスク情報を参照 ────────────────────────────────────── */
    PK_REF_TSK ref;
    tk_ref_tsk(tid, &ref);
    plib_puts("  作成直後: pri=");
    plib_puti(ref.pri);
    plib_puts(", policy=");
    plib_puts(ref.policy == SCHED_RR ? "RR" : "FIFO");
    plib_puts(", slice=");
    plib_puti(ref.slice_ms);
    plib_puts("ms\r\n");

    /* ── 優先度を動的に変更 ──────────────────────────────────── */
    tk_chg_pri(tid, 5);            /* pri=8 → pri=5 に変更 */
    tk_ref_tsk(tid, &ref);
    plib_puts("  優先度変更後: pri=");
    plib_puti(ref.pri);            /* 5 になるはず */
    plib_puts("\r\n");

    /* ── タイムスライスを動的に変更 ─────────────────────────── */
    tk_chg_slt(tid, 200);          /* 100ms → 200ms に変更 */
    tk_ref_tsk(tid, &ref);
    plib_puts("  スライス変更後: slice=");
    plib_puti(ref.slice_ms);       /* 200ms になるはず */
    plib_puts("ms\r\n");

    /* タスクを起動して完了を待つ */
    tk_sta_tsk(tid, 0);
    tk_wai_sem(g_done_sem, 1, 5000);

    tk_del_tsk(tid);
    tk_del_sem(g_done_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  デモ4: tk_slp_tsk と tk_wup_tsk
 *  ─────────────────────────────────────────────────────────────
 *
 *  タスクを指定時間スリープさせたり、他のタスクから
 *  tk_wup_tsk() で早期に起床させることができます。
 * ───────────────────────────────────────────────────────────── */

static volatile int g_woken = 0;

static void task_sleeper(int stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    plib_puts("  [スリーパー] 1秒スリープ開始 (早期起床を待つ)\r\n");

    /* TMO_FEVR: 永遠に待つ（tk_wup_tsk で起こされる） */
    int r = tk_slp_tsk(TMO_FEVR);

    g_woken = (r == 0) ? 1 : 0;
    plib_puts("  [スリーパー] 起床! (早期起床=");
    plib_puti(g_woken);
    plib_puts(")\r\n");

    tk_sig_sem(g_done_sem, 1);
    tk_ext_tsk();
}

static void demo_sleep_wakeup(void)
{
    plib_puts("--- [4] スリープと早期起床 ---\r\n");

    PK_CSEM csem = { 0, 0, 2 };
    g_done_sem = tk_cre_sem(&csem);

    PK_CRE_TSK pk = {
        .task   = task_sleeper,
        .pri    = 8,
        .policy = SCHED_FIFO,
        .stksz  = 0, .slice_ms = 0, .exinf = 0,
    };
    int tid = tk_cre_tsk(&pk);
    tk_sta_tsk(tid, 0);

    /* 50ms 待ってから強制起床 */
    tk_slp_tsk(50);
    plib_puts("  [メイン] tk_wup_tsk で早期起床を指示\r\n");
    tk_wup_tsk(tid);

    tk_wai_sem(g_done_sem, 1, 5000);

    tk_del_tsk(tid);
    tk_del_sem(g_done_sem);
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  エントリーポイント
 * ───────────────────────────────────────────────────────────── */
void _start(void)
{
    plib_puts("=== RTOS タスク管理サンプル ===\r\n\r\n");

    demo_fifo_preemption();
    demo_round_robin();
    demo_task_info();
    demo_sleep_wakeup();

    plib_puts("=== 完了 ===\r\n");
    plib_puts("次のサンプル: exec rtos_sync.elf\r\n");
    sys_exit(0);
}
