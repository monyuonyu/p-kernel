/*
 * load_mot.c
 *
 *  Created on: 2011/03/28
 *      Author: mizu
 */

#include "load_mot.h"
#include "3069f_sci.h"
#include "XMODEM.h"
#include <stdlib.h>
#include <string.h>

#define XMODEM_DBG 0


void mot_deploy(char* buf)
{
	static int mot_dst = 0;
	static int data_cnt = 0;
	char* addr_dst;
	unsigned char checksum = 0;
	int i;


	while (1)
	{

		// 制御コードならポインタを進める
		while (buf[mot_dst] == 0x0A || buf[mot_dst] == 0x0D)
			mot_dst++;

		// モトローラSコードでなければリターン
		if (buf[mot_dst] != 'S')
			return;

		// バイトカウンタ読み込み
		asciitobin(&buf[mot_dst + 2], 2);
		data_cnt = buf[mot_dst + 2] * 2;

		switch (buf[mot_dst + 1])
		{
		case '0': // ヘッダー形式

			break;
		case '1':
			asciitobin(&buf[mot_dst + 4], data_cnt);
			addr_dst = (char*) *(&((short*) buf)[mot_dst + 2]);			// ※ エンディアンに注意!!

			// Deploy
			for (i = 0; i < (data_cnt / 2) - 2 - 1; i++)
			{
				*addr_dst = buf[i + 6 + mot_dst];
				checksum += buf[i + 6 + mot_dst];
				addr_dst++;
			}

			// チェックサム判定
			checksum += buf[mot_dst + 2];
			checksum += buf[mot_dst + 4];
			checksum += buf[mot_dst + 5];
			checksum ^= buf[(data_cnt / 2) - 2 - 1 + mot_dst + 6];
			if (checksum != 0xFF)
			{
				return; // 失敗
			}

			mot_dst += data_cnt + 4;

			break;
		case '2':
			break;
		case '3':
			break;
		case '8':	// フッタ
			break;
		case '9':
			return;

		default: // 対応していなければリターン
			return;

		};

	}
}


// 転送が完了すると転送したブロック数。 失敗・中断すると１を返す
int load_lod(char* buf)
{
	unsigned char block_num_now = 1;
	char c;
	int starting = 0;
//	int mot_no = 0;

	while (1)
	{
		if (!starting)
		{
			xmodem_start_ack();
		}

		sci_read(SCI_NO_1, &c, 1);

		switch (c)
		{
		case XMODEM_SOH: // 通信開始！
			starting = 1;
			if (xmodem_read_block(block_num_now, buf))
			{
				sci_write(SCI_NO_1, XMODEM_NAK); // 失敗を通知
			}
			else
			{
				/********************************************************************************
					モトローラSフォーマット解析
				********************************************************************************/
				mot_deploy(buf);

//				if(buf[mot_no] == 'S' && buf[mot_no + 1] == '0')	// 解析開始
//				{
//
//				}

				block_num_now++;
//				buf += XMODEM_BLOCK_SIZE; // バッファのポインタを1ブロック進める
				sci_write(SCI_NO_1, XMODEM_ACK); // 成功を通知 → 次のブロック送信開始
			}
			break;

		case XMODEM_EOT: // 転送完了
			sci_write(SCI_NO_1, XMODEM_ACK);
			return block_num_now;

		case XMODEM_CAN: // 中断
			return 0;

		case XMODEM_EOF:
			return 0;

		default:
			if (starting)
				return 0;
		}

	}
}
