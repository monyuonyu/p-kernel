/*
 *  test_all/test_common.h — テストスイート 共通定義
 *
 *  全テストモジュール (test_posix.c, test_sem.c, test_flg.c,
 *  test_task.c, test_sched.c) からインクルードされます。
 *
 *  含まれるもの:
 *    - plibc.h のインクルード
 *    - ASSERT / ASSERT_EQ マクロ
 *    - グローバル変数の extern 宣言
 *    - ヘルパー関数の extern 宣言
 *    - 複数モジュールで使用するタスク関数の宣言
 *    - 各テストモジュールの関数プロトタイプ
 */
#pragma once

#include "plibc.h"

/* ================================================================= */
/* テスト結果マクロ                                                   */
/* ================================================================= */

/*
 *  ASSERT(name, cond)
 *    cond が真なら PASS、偽なら FAIL を記録します。
 *    name はテスト名の文字列リテラルです。
 */
#define ASSERT(name, cond) \
    do { if (cond) pass(name); else fail(name, #cond); } while (0)

/*
 *  ASSERT_EQ(name, got, expect)
 *    got == expect なら PASS、異なれば実際値と期待値を表示して FAIL を記録します。
 */
#define ASSERT_EQ(name, got, expect) \
    do { \
        int _g = (int)(got); int _e = (int)(expect); \
        if (_g == _e) pass(name); \
        else { plib_puts("[FAIL] "); plib_puts(name); \
               plib_puts(" : got="); plib_puti(_g); \
               plib_puts(" expect="); plib_puti(_e); plib_puts("\r\n"); \
               g_fail++; } \
    } while (0)

/* ================================================================= */
/* グローバル変数 (test_main.c で定義、各モジュールで参照)           */
/* ================================================================= */

extern int           g_pass;         /* PASS カウンタ                    */
extern int           g_fail;         /* FAIL カウンタ                    */
extern int           g_sem;          /* テスト用セマフォ ID              */
extern int           g_flg;          /* テスト用イベントフラグ ID        */
extern volatile int  g_order[16];    /* タスク実行順記録バッファ         */
extern volatile int  g_order_cnt;    /* g_order の書き込み位置           */

/* ================================================================= */
/* ヘルパー関数 (test_main.c で定義)                                 */
/* ================================================================= */

/*
 *  pass(name) — テスト成功を記録して "[PASS] name" を出力します
 */
void pass(const char *name);

/*
 *  fail(name, reason) — テスト失敗を記録して "[FAIL] name : reason" を出力します
 */
void fail(const char *name, const char *reason);

/*
 *  start_task(fn, pri, policy, slice_ms, stacd, exinf)
 *    タスクを作成して即座に起動するショートカット関数です。
 *    成功時はタスク ID (>= 0)、失敗時は負値を返します。
 */
int start_task(void (*fn)(int, void*), int pri, int policy,
               int slice_ms, int stacd, void *exinf);

/* ================================================================= */
/* 共有タスク関数 (test_main.c で定義、複数モジュールで使用)          */
/* ================================================================= */

/*
 *  task_sem_signal — セマフォに +1 シグナルして終了するタスク
 *    exinf にセマフォ ID (int) を渡します。
 *    T4/T5 (test_sem.c), T10/T11/T12 (test_task.c), T19 (test_sched.c) で使用。
 */
void task_sem_signal(int stacd, void *exinf);

/*
 *  task_flg_set — イベントフラグにビットをセットして終了するタスク
 *    exinf に「フラグID を上位16bit、ビットパターンを下位16bit」に
 *    パックした int 値を渡します。
 *    T7/T8 (test_flg.c) で使用。
 */
void task_flg_set(int stacd, void *exinf);

/* ================================================================= */
/* テストモジュール 関数プロトタイプ                                  */
/* ================================================================= */

/*
 *  test_posix() — T1: 基本ファイルI/O, T2: lseek, T3: mkdir/rename,
 *                 T20: 部分読み
 */
void test_posix(void);

/*
 *  test_sem() — T4: セマフォ基本, T5: バルクsignal/wait, T6: TMO_POL
 */
void test_sem(void);

/*
 *  test_flg() — T7: TWF_ANDW, T8: TWF_ORW, T9: TMO_POL + tk_clr_flg
 */
void test_flg(void);

/*
 *  test_task() — T10: tk_ref_tsk, T11: tk_chg_pri, T12: tk_chg_slt,
 *                T13: tk_wup_tsk, T14: tk_slp_tsk タイムアウト
 */
void test_task(void);

/*
 *  test_sched() — T15: FIFO抢占, T16: RRローテーション,
 *                 T17: FIFO+RR混在, T18: リソース再利用, T19: タスク上限
 */
void test_sched(void);
