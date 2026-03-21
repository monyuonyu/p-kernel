/*
 *  05_all_demo/common.h — デモスイート 共通定義
 *
 *  全デモモジュール (posix.c, sem.c, flg.c, task.c, sched.c) から
 *  インクルードされます。
 *
 *  含まれるもの:
 *    - plibc.h のインクルード
 *    - ASSERT / ASSERT_EQ マクロ
 *    - グローバル変数の extern 宣言
 *    - ヘルパー関数の extern 宣言
 *    - 複数モジュールで使用するタスク関数の宣言
 *    - 各デモモジュールの関数プロトタイプ
 */
#pragma once

#include "plibc.h"

/* ================================================================= */
/* 動作確認マクロ                                                     */
/* ================================================================= */

/*
 *  ASSERT(name, cond)
 *    cond が真なら OK、偽なら NG を記録します。
 *    name は確認項目名の文字列リテラルです。
 */
#define ASSERT(name, cond) \
    do { if (cond) ok(name); else ng(name, #cond); } while (0)

/*
 *  ASSERT_EQ(name, got, expect)
 *    got == expect なら OK、異なれば実際値と期待値を表示して NG を記録します。
 */
#define ASSERT_EQ(name, got, expect) \
    do { \
        int _g = (int)(got); int _e = (int)(expect); \
        if (_g == _e) ok(name); \
        else { plib_puts("[ NG] "); plib_puts(name); \
               plib_puts(" : got="); plib_puti(_g); \
               plib_puts(" expect="); plib_puti(_e); plib_puts("\r\n"); \
               g_ng++; } \
    } while (0)

/* ================================================================= */
/* グローバル変数 (main.c で定義、各モジュールで参照)                */
/* ================================================================= */

extern int           g_ok;           /* OK カウンタ                      */
extern int           g_ng;           /* NG カウンタ                      */
extern int           g_sem;          /* デモ用セマフォ ID                */
extern int           g_flg;          /* デモ用イベントフラグ ID          */
extern volatile int  g_order[16];    /* タスク実行順記録バッファ         */
extern volatile int  g_order_cnt;    /* g_order の書き込み位置           */

/* ================================================================= */
/* ヘルパー関数 (main.c で定義)                                      */
/* ================================================================= */

/*
 *  ok(name) — 確認成功を記録して "[ OK] name" を出力します
 */
void ok(const char *name);

/*
 *  ng(name, reason) — 確認失敗を記録して "[ NG] name : reason" を出力します
 */
void ng(const char *name, const char *reason);

/*
 *  start_task(fn, pri, policy, slice_ms, stacd, exinf)
 *    タスクを作成して即座に起動するショートカット関数です。
 *    成功時はタスク ID (>= 0)、失敗時は負値を返します。
 */
int start_task(void (*fn)(int, void*), int pri, int policy,
               int slice_ms, int stacd, void *exinf);

/* ================================================================= */
/* 共有タスク関数 (main.c で定義、複数モジュールで使用)              */
/* ================================================================= */

/*
 *  task_sem_signal — セマフォに +1 シグナルして終了するタスク
 *    exinf にセマフォ ID (int) を渡します。
 *    D4/D5 (sem.c), D10/D11/D12 (task.c), D19 (sched.c) で使用。
 */
void task_sem_signal(int stacd, void *exinf);

/*
 *  task_flg_set — イベントフラグにビットをセットして終了するタスク
 *    exinf に「フラグID を上位16bit、ビットパターンを下位16bit」に
 *    パックした int 値を渡します。
 *    D7/D8 (flg.c) で使用。
 */
void task_flg_set(int stacd, void *exinf);

/* ================================================================= */
/* デモモジュール 関数プロトタイプ                                    */
/* ================================================================= */

/*
 *  demo_posix() — D1: 基本ファイルI/O, D2: lseek, D3: mkdir/rename,
 *                 D20: 部分読み
 */
void demo_posix(void);

/*
 *  demo_sem() — D4: セマフォ基本, D5: バルクsignal/wait, D6: TMO_POL
 */
void demo_sem(void);

/*
 *  demo_flg() — D7: TWF_ANDW, D8: TWF_ORW, D9: TMO_POL + tk_clr_flg
 */
void demo_flg(void);

/*
 *  demo_task() — D10: tk_ref_tsk, D11: tk_chg_pri, D12: tk_chg_slt,
 *                D13: tk_wup_tsk, D14: tk_slp_tsk タイムアウト
 */
void demo_task(void);

/*
 *  demo_sched() — D15: FIFO抢占, D16: RRローテーション,
 *                 D17: FIFO+RR混在, D18: リソース再利用, D19: タスク上限
 */
void demo_sched(void);
