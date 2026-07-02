/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Linux AArch64 ユーザモードポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tm_com.c
 *	T-Monitor 互換ライブラリ 低レベル通信（Linux AArch64 ユーザモード）
 *
 *	シリアルポートの代わりに termios raw モードの標準入出力
 *	（arch/linux/aarch64/sio.c）へ接続します。
 */

#include <tk/tkernel.h>
#ifdef LINUX_AARCH64

#include <tm/tmonitor.h>
#include "../../libtm.h"

#if USE_TMONITOR

/* 低レベルシリアル入出力（arch/linux/aarch64/sio.c） */
IMPORT void sio_send_frame(const UB *buf, INT len);
IMPORT INT  sio_recv_frame(UB *buf, INT maxlen);

/*
 * 1 フレーム送信（tm_putchar / tm_putstring の下回り）
 */
EXPORT void tm_snd_dat( const UB* buf, INT size )
{
	sio_send_frame(buf, size);
}

/*
 * size バイト受信するまでブロック（tm_getchar の下回り）
 *	sio は非ブロッキングのため、データが来るまでポーリングします。
 */
EXPORT void tm_rcv_dat( UB* buf, INT size )
{
	INT n;

	while ( size > 0 ) {
		n = sio_recv_frame(buf, size);
		if ( n > 0 ) {
			buf  += n;
			size -= n;
		}
	}
}

/*
 * 通信路の初期化
 *	termios の設定は boot/linux_x86_64/main_mtk3.c の sio_init() で
 *	実施済みのため、ここでは何もしません。
 */
EXPORT void tm_com_init( void )
{
}

#endif /* USE_TMONITOR */
#endif /* LINUX_AARCH64 */
