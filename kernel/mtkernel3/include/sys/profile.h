/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.00
 *
 *    Copyright (C) 2006-2019 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2019/12/11.
 *
 *----------------------------------------------------------------------
 */

/**
 * @file	profile.h
 * @brief	サービスプロファイル定義
 *
 * 本カーネルがサポートする機能・仕様の有無を示す
 * TK_ 系プロファイルマクロを定義します。
 */

#ifndef __SYS_PROFILE_H__
#define __SYS_PROFILE_H__

#include <sys/machine.h>
#include <sys/knldef.h>

/*
 **** システム依存部のプロファイル
 */

/* システム依存部ヘッダのパス生成と取り込み */
#define PROF_PATH_(a)		#a
#define PROF_PATH(a)		PROF_PATH_(a)
#define PROF_SYSDEP()		PROF_PATH(sysdepend/TARGET_DIR/profile.h)
#include PROF_SYSDEP()


/*
 **** 共通プロファイル
 */

/*
 * OS 種別とバージョン
 */
#define TK_SPECVER_MAGIC		6					/* micro T-Kernel のマジックナンバー */
#define TK_SPECVER_MAJOR		3					/* メジャーバージョン番号 */
#define TK_SPECVER_MINOR		0					/* マイナーバージョン番号 */
#define TK_SPECVER			((TK_SPECVER_MAJOR << 8) | TK_SPECVER_MINOR)

/*
 * タスクの最大優先度（16 以上）
 */
#define TK_MAX_TSKPRI			(MAX_TSKPRI)		/* タスクの最大優先度 */
#define TK_WAKEUP_MAXCNT		(+2147483647L)		/* 起床要求のキューイング最大数 */
#define TK_SEMAPHORE_MAXCNT		(+2147483647L)		/* セマフォの最大資源数 */
/*
 * 強制待ち要求のネスト（キューイング）最大数
 */
#define TK_SUSPEND_MAXCNT		(+2147483647L)

/*
 * デバイスドライバ
 */
#define TK_SUPPORT_TASKEVENT		FALSE				/* タスクイベントのサポート */
#define TK_SUPPORT_DISWAI		FALSE				/* 待ち禁止（API: tk_dis_wai）のサポート */

/*
 * メモリ管理
 */
#define TK_SUPPORT_USERBUF		TRUE				/* ユーザ指定バッファ（TA_USERBUF）のサポート */
#define TK_SUPPORT_AUTOBUF		TRUE				/* バッファ自動割り当て（TA_USERBUF 指定なし）のサポート */
#define TK_SUPPORT_MEMLIB		(USE_IMALLOC)			/* メモリ割り当てライブラリのサポート */

/*
 * タスク例外
 */
#define TK_SUPPORT_TASKEXCEPTION	FALSE				/* タスク例外のサポート */

/*
 * サブシステム
 */
#define TK_SUPPORT_SUBSYSTEM		FALSE				/* サブシステムのサポート */
#define TK_SUPPORT_SSYEVENT		FALSE				/* サブシステムのイベント処理のサポート */

/*
 * システム構成情報
 */
#define TK_SUPPORT_SYSCONF		FALSE				/* システム構成情報の取得のサポート */

/*
 * データ型とサイズ
 */
#define TK_HAS_DOUBLEWORD		TRUE				/* 64 ビットデータ型（D, UD, VD）のサポート */
#define TK_SUPPORT_USEC			FALSE				/* マイクロ秒単位時間のサポート */
#define TK_SUPPORT_LARGEDEV		FALSE				/* 大容量デバイス（64 ビット）のサポート */
#define TK_SUPPORT_SERCD		FALSE				/* サブエラーコードのサポート */

/*
 * その他の機能
 */
#define TK_TRAP_SVC			FALSE				/* システムコール入口に CPU のトラップ命令を使用 */
#define TK_HAS_SYSSTACK			FALSE				/* タスクがシステムスタックを別に持つ */
#define	TK_SUPPORT_UTC			TRUE				/* UTC（UNIX 形式時刻）のサポート */
#define TK_SUPPORT_TRONTIME		TRUE				/* TRON 形式時刻のサポート */

/*
 * デバッグ支援
 */
#define TK_SUPPORT_DSNAME		(USE_OBJECT_NAME)	/* DS オブジェクト名のサポート */
#define TK_SUPPORT_DBGSPT		(USE_DBGSPT)		/* T-Kernel/DS のサポート */


#endif /* __SYS_PROFILE_H__ */
