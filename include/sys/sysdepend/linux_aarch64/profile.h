/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	profile.h
 *	サービスプロファイル定義（Linux AArch64 ユーザモードポート依存部）
 *
 *	Linux ユーザモードでは特権命令・割込みコントローラ・キャッシュ制御
 *	などのハードウェア機能は使用できないため、対応プロファイルはすべて
 *	FALSE とします。
 */

#ifndef __SYS_DEPEND_PROFILE_H__
#define __SYS_DEPEND_PROFILE_H__

/*
 **** CPU コア依存プロファイル（x86-64 ユーザモード）
 */
#define TK_ALLOW_MISALIGN	(ALLOW_MISALIGN)	/* 非整列メモリアクセスの許可 */
#define TK_BIGENDIAN		(BIGENDIAN)		/* ビッグエンディアンか（要定義） */

#define TK_SUPPORT_FPU		FALSE	/* FPU コンテキスト管理（SysV ABI では XMM は
					   caller-saved のため退避不要） */
#define TK_SUPPORT_COP0		FALSE	/* コプロセッサ 0 */
#define TK_SUPPORT_COP1		FALSE	/* コプロセッサ 1 */
#define TK_SUPPORT_COP2		FALSE	/* コプロセッサ 2 */
#define TK_SUPPORT_COP3		FALSE	/* コプロセッサ 3 */

#define TK_SUPPORT_REGOPS	FALSE	/* レジスタ取得/設定操作（tk_get_reg 等） */
#define TK_SUPPORT_ASM		FALSE	/* アセンブリ言語によるハンドラ記述 */

#define TK_SUPPORT_INTCTRL	FALSE	/* 割込みコントローラ管理 */
#define TK_HAS_ENAINTLEVEL	FALSE	/* 割込み優先度レベル指定 */
#define TK_SUPPORT_CPUINTLEVEL	FALSE	/* CPU 割込みマスクレベルの取得/設定 */
#define TK_SUPPORT_CTRLINTLEVEL	FALSE	/* 割込みコントローラマスクレベルの取得/設定 */
#define TK_SUPPORT_INTMODE	FALSE	/* 割込みモード設定 */

#define TK_SUPPORT_CACHECTRL	FALSE	/* キャッシュ制御 */
#define TK_SUPPORT_SETCACHEMODE	FALSE	/* キャッシュモード設定 */
#define TK_SUPPORT_WBCACHE	FALSE	/* ライトバックキャッシュ */
#define TK_SUPPORT_WTCACHE	FALSE	/* ライトスルーキャッシュ */

/* メモリ保護レベル（未使用） */
#define TK_MEM_RNG0		0
#define TK_MEM_RNG1		0
#define TK_MEM_RNG2		0
#define TK_MEM_RNG3		0

#define TK_SUPPORT_MICROWAIT	FALSE	/* マイクロ秒待ち（WaitUsec 等） */

/*
 **** ターゲット依存プロファイル
 */
#define TK_SUPPORT_LOWPOWER	FALSE	/* 省電力管理（tk_set_pow） */

#endif /* __SYS_DEPEND_PROFILE_H__ */
