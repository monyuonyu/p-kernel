/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tm_com.c
 *	T-Monitor 互換ライブラリ 低レベル通信（Windows x86-64 ネイティブ）
 *
 *	シリアルポートの代わりに Win32 コンソールの標準入出力
 *	（arch/windows/x86_64/sio.c）へ接続します。
 */

#include <tk/tkernel.h>
#ifdef WINDOWS_X86_64

#include <tm/tmonitor.h>
#include "../../libtm.h"

#if USE_TMONITOR

/* 低レベルシリアル入出力（arch/windows/x86_64/sio.c） */
IMPORT void sio_send_frame(const UB *buf, INT len);
IMPORT void sio_recv_frame(UB *buf, INT size);

/*
 * 1 フレーム送信（tm_putchar / tm_putstring の下回り）
 */
EXPORT void tm_snd_dat( const UB* buf, INT size )
{
	sio_send_frame(buf, size);
}

/*
 * size バイト受信するまでブロック（tm_getchar の下回り）
 *	Windows 版の sio_recv_frame は指定バイト数を満たすまでブロックして
 *	埋めるため、一度呼ぶだけでよい（Linux 版が戻り値へ依存していた
 *	ABI 依存の脆さを避ける）。
 */
EXPORT void tm_rcv_dat( UB* buf, INT size )
{
	sio_recv_frame(buf, size);
}

/*
 * 通信路の初期化
 *	コンソールの設定は boot/windows/x86_64/main_win.c の sio_init() で
 *	実施済みのため、ここでは何もしません。
 */
EXPORT void tm_com_init( void )
{
}

#endif /* USE_TMONITOR */
#endif /* WINDOWS_X86_64 */
