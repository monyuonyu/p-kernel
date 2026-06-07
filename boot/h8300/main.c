/*
 * main.c
 *
 *  Created on: 2010/11/27
 *      Author: pino
 */

//#include "3069s.h"
#include "3069f_io.h"
#include "3069f_sci.h"
#include "XMODEM.h"
#include "load_mot.h"
#include "ctype.h"

#include <string.h>
#include <stdlib.h>
// #include "dbg.hpp"
// #include "DP8390.h"
#include "read_elf.h"
// #include "ascii.h"
#include "interrupt.h"
#include "intr.h"

extern int buf_start;
extern int text_start;

void getstring(char* buf)
{

	int i;
	int cnt = 0;
	for (i = 0; i < 30; i++)
	{
		buf[cnt] = tolower(sci_read_byte(SCI_NO_1));
		sci_write(SCI_NO_1, buf[cnt]);

		if (buf[cnt] == 0x0d)
		{
			buf[cnt] = 0x00;
			break;
		}
		cnt++;
	}
}


void main_vec(softvec_type_t type, unsigned long sp)
{
	static char buf[32];
	static int i = 0;
	static char* entry_point;
	static void (*start)(void);
//	char str[32] = {0,};

	// 受信
	buf[i] = sci_read_byte_intr(SCI_NO_1);
	sci_write(SCI_NO_1, buf[i]);

	// スイッチ
	if(buf[i] == 0x0d)
	{

		buf[i] = '\0';

		if(!strncmp(buf, "dump", 4))
		{
//			getstring(str);
//			asciitobin(str, 4);

			for (i = 0; i < 128; i++)
				sci_write(SCI_NO_1, *((char*)0xffc520 + i));
		}
		else if(!strncmp(buf, "load", 4))
		{
			sci_write_str(SCI_NO_1, "  started!! XMODEM...\n\r");
			xmodem_start((char*)&buf_start);
			sci_write_str(SCI_NO_1, "  finish!! XMODEM\n\r");
			sci_write_str(SCI_NO_1, "    Transfer is Completed...\n\r");
		}
		else if(!strncmp(buf, "devlop", 6))
		{
			// RAM展開
			sci_write_str(SCI_NO_1, "  started!! devloping...\n\r");
			entry_point = elf_develop((char*) &buf_start);
			start = (void(*)(void)) entry_point;
			sci_write_str(SCI_NO_1, "  finish!! devloped...\n\r");
		}
		else if(!strncmp(buf, "start", 5))
		{
			// 開始
			start();
		}

		i = 0;
		sci_write_str(SCI_NO_1, "\n\rPINoC Console>_ ");
		return;
	}

	i++;
}

int boot_main()
{
//	long i;
//	static int ii;
//	const char str[][32] = {{"ok sci\n\r"}, {"ok bootloader\n\r"}, {"ok started!! XMODEM\n\r"}, {"ok finish!! XMODEM\n\r"}, {"ok started!! Deploy\n\r"}, {"ok finish!! Deploy\n\r"}};

	// ポート4を光らすテスト
//	volatile unsigned char* p4ddr = (volatile unsigned char*) 0xFEE003;
//	volatile unsigned char* p4 = (volatile unsigned char*) 0xFFFFD3;
//	*p4ddr = 0x03;
//	*p4 = 0x01;

//	sci_write_str(SCI_NO_1, str[0]);
//
//	sci_write_str(SCI_NO_1, str[2]);
//	ii = xmodem_start(buf_start);
//	sci_write_str(SCI_NO_1, str[3]);
//
//	sci_write_str(SCI_NO_1, str[4]);
//	mot_deploy(buf_start);
//	sci_write_str(SCI_NO_1, str[5]);

//	sci_write_str(SCI_NO_1, (const char*)buf_start);

//	sci_write_str(SCI_NO_1, str[1]);

//	*p4 = 0x02;

	// ソフト・割り込みベクタへ登録
	softvec_setintr(softvec_type_serial, main_vec);

	// シリアル通信割り込みEnable
	sci_read_intr_enable(SCI_NO_1);

	// 割り込みENABLE
//	INTERUUPT_ENABLE


	sci_write_str(SCI_NO_1, "PINoC Console>_ \r\n");

	// Sleep
//	as_SLEEP_LOOP_3069

//	sci_write_str(SCI_NO_1, (char*)&text_start);

//	char* entry_point;
//	void (*start)(void);
//	char str[32] = {0,};
//	while(1)
//	{
//		int i;
//		sci_write_str(SCI_NO_1, "PINoC Console>_ ");
//
//
//		// エンターが入力されるまでループ
//		getstring(str);
//
//		sci_write(SCI_NO_1, '\n');
//
//		// メインスイッチ
//		switch (str[0])
//		{
//		// RAMへロード
//		case 'l':
//			sci_write_str(SCI_NO_1, "  started!! XMODEM...\n\r");
//			xmodem_start((char*)&buf_start);
//			sci_write_str(SCI_NO_1, "  finish!! XMODEM\n\r");
//			sci_write_str(SCI_NO_1, "    Transfer is Completed...\n\r");
//			break;
//
//			// void main(void) の形式で関数ポインタから開始
//		case 's':
//			start = (void*)&buf_start;
//			start();
//			break;
//
//			// メモリダンプ
//		case 'd':
//			getstring(str);
//			asciitobin(str, 4);
//
//			for (i = 0; i < (int)*((short*)str); i++)
//				sci_write(SCI_NO_1, *(((char*)&buf_start) + i));
//			break;
//
//			// elfヘッダ情報表示
//		case 'e':
//			if(elf_read((char*)&buf_start))
//			{
//				sci_write_str(SCI_NO_1, "it's not elf file...\n\r");
//			}
//			else
//			{
//				sci_write_str(SCI_NO_1, "it's elf file!!\n\r");
//			}
//
//			if(elf_analysis((char*)&buf_start))
//			{
//				sci_write_str(SCI_NO_1, "it's not load file...\n\r");
//			}
//			else
//			{
//				sci_write_str(SCI_NO_1, "it's load file!!\n\r");
//			}

//			// RAM展開
//			entry_point = elf_develop((char*)&buf_start);
//			start = (void (*)(void))entry_point;
//
//			// ヘルプ
//		case 'h':
//			break;
//
//		case 'q':
//			dbg();
//			as_SLEEP_3069
//			dbg();
//			break;
//
//		}
//
//	}

	return 0;
}

void init()
{
	extern int bss_start, bss_end, data_start, data_end, rodata_end;

	memset(&bss_start, 0, (long)&bss_end - (long)&bss_start);					// Bssセクション初期化
	memcpy(&data_start, &rodata_end, (long)&data_end - (long)&data_start);		// dataセクション初期化
	memset(&buf_start, 0, (long*)0x600);										// bufセクション初期化

	// 割り込みDISABLE
//	INTERUUPT_DISABLE

	// ソフト割り込み初期化
	softvec_init();

	// シリアル通信初期化
	sci_init(SCI_NO_1, BitRate_type_br9600);

	boot_main();
}
