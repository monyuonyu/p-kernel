/*
 * XMODEM.c
 *
 *  Created on: 2011/02/27
 *      Author: mizu
 */
#include "XMODEM.h"
#include "3069f_sci.h"

#define XMODEM_DBG 0

#if 0 /* 現状 H8 に依存するコードのため コンパイル対象外とする */

// 通信が開始されると関数を抜けます
int xmodem_start_ack()
{
	unsigned long cnt = 0;

	// 何か値が入るまで一定間隔ごとにNAKを送信
	while (!(sci_read_pol(SCI_NO_1)))
	{
		if (cnt++ > 200000)
		{
			sci_write(SCI_NO_1, XMODEM_NAK);
			cnt = 0;
#if XMODEM_DBG
//			return 60;
#endif
		}
	}
	return 0;
}


// XMODEM 1ブロック受信
int xmodem_read_block(unsigned char block_num_now, char* buff)
{
	unsigned char c, block_num_rec, check_sum = 0;
//	char c;
	int i;

	// ブロック番号確認
	sci_read(SCI_NO_1, (char*)&block_num_rec, 1);
	if(block_num_now != block_num_rec)
		return 1;

	// ビット反転のブロック番号確認
	c = block_num_rec;
	sci_read(SCI_NO_1, (char*)&block_num_rec, 1);
	block_num_rec ^= c;
	if(block_num_rec != 0xFF)
		return 1;

	// 128byteのデータ受信
	for(i = 0; i < XMODEM_BLOCK_SIZE; i++)
	{
		sci_read(SCI_NO_1, (char*)&c, 1);
		*(buff++) = c;
		check_sum += c;
	}

	// チェックサム確認
	sci_read(SCI_NO_1, (char*)&c, 1);
	check_sum ^= c;
	if(check_sum)
		return 1;

	// 正常終了
	return 0;

}

// 転送が完了すると転送したブロック数。 失敗・中断すると１を返す
int xmodem_start(char* buf)
{
	unsigned char block_num_now = 1;
	char c;
	int starting = 0;

	while(1)
	{
		if(!starting)
		{
#if !XMODEM_DBG
			xmodem_start_ack();
#else
			if(xmodem_start_ack()) return 60;
#endif
		}

		sci_read(SCI_NO_1, &c, 1);

		switch(c)
		{
		case XMODEM_SOH:	// 通信開始！
#if XMODEM_DBG
			return 10;
#endif
			starting = 1;
			if(xmodem_read_block(block_num_now, buf))
			{
				sci_write(SCI_NO_1, XMODEM_NAK);	// 失敗を通知
#if XMODEM_DBG
				return 70;
#endif
			}
			else
			{
				block_num_now++;
				buf += XMODEM_BLOCK_SIZE;		// バッファのポインタを1ブロック進める
				sci_write(SCI_NO_1, XMODEM_ACK);	// 成功を通知 → 次のブロック送信開始
			}
			break;

		case XMODEM_EOT:	// 転送完了
			sci_write(SCI_NO_1, XMODEM_ACK);
#if !XMODEM_DBG
			return block_num_now;
#else
			return 20;
#endif

		case XMODEM_CAN:	// 中断
#if !XMODEM_DBG
			return 0;
#else
			return 30;
#endif

		case XMODEM_EOF:
#if !XMODEM_DBG
			return 0;
#else
			return 40;
#endif

		default:
#if !XMODEM_DBG
			if(starting)
				return 0;
#else
				return 50;
#endif
		}

	}
}

#endif /* 現状 H8 に依存するコードのため コンパイル対象外とする */

//#include "lib.h"
//#include "string.h"
//#include "serial.h"
//#include "xmodem.h"
//
//#define XMODEM_SOH 0x01
//#define XMODEM_STX 0x02
//#define XMODEM_EOT 0x04
//#define XMODEM_ACK 0x06
//#define XMODEM_NAK 0x15
//#define XMODEM_CAN 0x18
//#define XMODEM_EOF 0x1a /* Ctrl-Z */
//
//#define XMODEM_BLOCK_SIZE 128
//
//static int xmodem_start_ack()
//{
//  int cnt0 = 0, cnt1 = 0;
//
//	while (!sci_read_pol(SCI_NO_1))
//	{
//		if (++cnt0 >= 200)
//		{
//			cnt0 = 0;
//			if (++cnt1 >= 1000)
//			{
//				cnt1 = 0;
//				sci_write(SCI_NO_1,XMODEM_NAK);
//			}
//		}
//	}
//
//	return 0;
//}
//
//static char *xmodem_read_block(char *buf)
//{
//  unsigned char c, block_num, check_sum;
//  int i;
//
//  block_num  = serial_getc();
//  block_num ^= serial_getc();
//
//  if (block_num != 0xff)
//    return NULL;
//
//  check_sum = 0;
//  for (i = 0; i < XMODEM_BLOCK_SIZE; i++) {
//    c = serial_getc();
//    *(buf++) = c;
//    check_sum += c;
//  }
//
//  check_sum ^= serial_getc();
//  if (check_sum)
//    return NULL;
//
//  return buf;
//}
//
//int xmodem_start(char *buf)
//{
//  int receiving = 0, size = 0;
//  unsigned char c;
//  unsigned char *p;
//
//  while (1) {
//    if (!receiving)
//      xmodem_wait();
//
//    c = serial_getc();
//
//    if (c == XMODEM_EOT) {
//      serial_putc(XMODEM_ACK);
//      break;
//    } else if (c == XMODEM_SOH) {
//      receiving++;
//      p = xmodem_read_block(buf);
//      if (!p) {
//	serial_putc(XMODEM_NAK);
//      } else {
//	buf = p;
//	size += XMODEM_BLOCK_SIZE;
//	serial_putc(XMODEM_ACK);
//      }
//    } else {
//      if (receiving)
//	return -1;
//    }
//  }
//
//  return size;
//}
