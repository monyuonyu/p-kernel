/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel x86 ベアメタルポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tm_com.c
 *	T-Monitor 互換ライブラリ 低レベル通信（x86 ベアメタルポート）
 *
 *	COM1 シリアル（arch/x86/sio.c）へ接続します。
 */

#include <tk/tkernel.h>
#ifdef X86_PC

#include <tm/tmonitor.h>
#include "../../libtm.h"

#if USE_TMONITOR

/* 低レベルシリアル入出力（arch/x86/sio.c）
 *	sio_recv_frame は要求バイト数を受信するまでブロックし、
 *	待ちの間は tk_dly_tsk(1) で他タスクへ CPU を譲る。 */
IMPORT void sio_send_frame(const UB *buf, INT len);
IMPORT void sio_recv_frame(UB *buf, INT len);

/*
 * 1 フレーム送信（tm_putchar / tm_putstring の下回り）
 */
EXPORT void tm_snd_dat( const UB* buf, INT size )
{
	sio_send_frame(buf, size);
}

/*
 * size バイト受信するまでブロック（tm_getchar の下回り）
 */
EXPORT void tm_rcv_dat( UB* buf, INT size )
{
	sio_recv_frame(buf, size);
}

/*
 * 通信路の初期化（COM1 は boot/x86/main_mtk3.c で初期化済み）
 */
EXPORT void tm_com_init( void )
{
}

#endif /* USE_TMONITOR */
#endif /* X86_PC */
