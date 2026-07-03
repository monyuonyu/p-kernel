/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux x86-64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	システム依存定義（Linux x86-64 ユーザモードポート依存部）
 *
 *	ベアメタルポートと異なり、実ハードウェアのレジスタ定義は存在しません。
 *	カーネルコアが要求する最小限の定数のみを定義します。
 */

#ifndef __SYS_SYSDEF_DEPEND_H__
#define __SYS_SYSDEF_DEPEND_H__

/*
 * 割込みベクタ数
 *	実際のベクタテーブルは持ちませんが、tk_def_int の引数範囲
 *	チェック（int.c）が参照するため定義が必要です。
 */
#define N_INTVEC		(256)

/*
 * SVC ハンドラ数
 *	Linux ユーザモードでは SVC トラップを使用しません（ディスパッチは
 *	直接関数呼び出し）。互換のため形式的に定義します。
 */
#define N_SVCHDR		(0)

/*
 * システムタイマ周期の許容範囲 [ミリ秒]（knldef.h の検証が参照）
 */
#define MIN_TIMER_PERIOD	(1)
#define MAX_TIMER_PERIOD	(100)

/*
 * CPU 機能の有無（knldef.h の検証が参照）
 *	物理タイマ・FPU 管理・DSP はいずれも本ポートでは使用しません。
 */
#define CPU_HAS_PTMR		(0)
#define CPU_HAS_FPU		(0)
#define CPU_HAS_DSP		(0)

/*
 * タスクのシステムスタックサイズ [バイト]（task_manage.c が参照）
 *	休止フレーム（64 バイト）＋ ABI の余裕を見て最小 1KB とします。
 *	tk_cre_tsk で stksz=0 が指定された場合は既定値を使用します。
 */
#define MIN_SYS_STACK_SIZE	(1024)
#define DEFAULT_SYS_STKSZ	(MIN_SYS_STACK_SIZE)

/*
 * 低消費電力モード禁止要求の最大ネスト数（tk_set_pow）
 */
#define LOWPOW_LIMIT		(0x7fff)

/*
 * 初期タスクのスタックサイズ [バイト]（include/sys/inittask.h の上書き）
 *	初期タスクは usermain（p-kernel の対話シェル・分散レイヤー初期化）
 *	をそのまま実行するため、micro T-Kernel 2.0 ポートと同じ 256KB を
 *	確保します。
 */
#define INITTASK_STKSZ		(256 * 1024)

#endif /* __SYS_SYSDEF_DEPEND_H__ */
