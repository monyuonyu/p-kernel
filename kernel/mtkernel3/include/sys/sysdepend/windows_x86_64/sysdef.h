/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	システム依存定義（Windows x86-64 ネイティブポート依存部）
 *
 *	Linux ユーザモードポート（linux_x86_64/sysdef.h）と同値。
 *	実ハードウェアのレジスタ定義は存在せず、カーネルコアが要求する
 *	最小限の定数のみを定義します。
 */

#ifndef __SYS_SYSDEF_DEPEND_H__
#define __SYS_SYSDEF_DEPEND_H__

/*
 * 割込みベクタ数
 *	実ベクタテーブルは持ちませんが、tk_def_int の引数範囲チェック
 *	（int.c）が参照するため定義が必要です。
 */
#define N_INTVEC		(256)

/*
 * SVC ハンドラ数
 *	ディスパッチは直接関数呼び出し（SVC トラップ不使用）。互換のため
 *	形式的に定義します。
 */
#define N_SVCHDR		(0)

/*
 * システムタイマ周期の許容範囲 [ミリ秒]（knldef.h の検証が参照）
 */
#define MIN_TIMER_PERIOD	(1)
#define MAX_TIMER_PERIOD	(100)

/*
 * CPU 機能の有無（knldef.h の検証が参照）
 */
#define CPU_HAS_PTMR		(0)
#define CPU_HAS_FPU		(0)
#define CPU_HAS_DSP		(0)

/*
 * タスクのシステムスタックサイズ [バイト]（task_manage.c が参照）
 *	Fiber ポートでは実スタックは CreateFiber が確保しますが、コアが
 *	休止フレーム用に確保する最小値の契約は Linux 版と揃えます。
 */
#define MIN_SYS_STACK_SIZE	(1024)
#define DEFAULT_SYS_STKSZ	(MIN_SYS_STACK_SIZE)

/*
 * 低消費電力モード禁止要求の最大ネスト数（tk_set_pow）
 */
#define LOWPOW_LIMIT		(0x7fff)

/*
 * 初期タスクのスタックサイズ [バイト]（include/sys/inittask.h の上書き）
 *	初期タスクは usermain（対話シェル・分散レイヤー初期化）を実行する
 *	ため、Linux ポートと同じ 256KB を確保します。
 */
#define INITTASK_STKSZ		(256 * 1024)

#endif /* __SYS_SYSDEF_DEPEND_H__ */
