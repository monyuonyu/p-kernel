/*
 * sci.c
 *
 *  Created on: 2010/11/27
 *      Author: pino
 */

#include "3069f_sci.h"

volatile static struct{
	volatile struct SCI *sci;
}regs[3] =
{
	{SCI0A},
	{SCI1A},
	{SCI2A},
};

void sci_init(SCI_NO no, BitRate_type_t type)
{
	int i = 0;
	volatile struct SCI* sci = regs[no].sci;

	sci->SCR.BYTE = 0;
	sci->SMR.BYTE = 0;
	sci->BRR = type;

	// wait
	for(i = 0; i < 30;i++);

	// 送受信許可
	sci->SCR.BIT.RE = 1;
	sci->SCR.BIT.TE = 1;

	sci->SSR.BYTE = 0;
}

/********************************************************************************
	ポーリング
********************************************************************************/
void sci_write(SCI_NO no, char c)
{
	volatile struct SCI* sci = regs[no].sci;

	while(!sci->SSR.BIT.TDRE);
	sci->TDR = c;
	sci->SSR.BIT.TDRE = 0;

	while(sci->SSR.BIT.TEND);

}

void sci_write_str(SCI_NO no,const char* _Str)
{
	volatile struct SCI* sci = regs[no].sci;

	while(*_Str)
	{
		sci_write(no, *_Str++);
	}

	// バッファの中身が全て送信されるまで待機
	while(!sci->SSR.BIT.TEND);
}

// データを1byteだけ受信する
char sci_read_byte(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	volatile char c;

	// データが格納されるまで待機
	while (1)
	{
		if (sci->SSR.BIT.ORER)
		{
			sci_write_str(no, "Error_sci over run");
			sci->SSR.BIT.ORER = 0;
		}
		if (sci->SSR.BIT.PER)
		{
			sci_write_str(no, "Error_sci parity");
			sci->SSR.BIT.PER = 0;
		}
		if (sci->SSR.BIT.FER_ERS)
		{
			sci_write_str(no, "Error_sci flaming");
			sci->SSR.BIT.FER_ERS = 0;
		}

		// データが格納されているか確認
		if (sci->SSR.BIT.RDRF)
			break;
	}

	c = sci->RDR;
	sci->SSR.BIT.RDRF = 0;

	return c;
}

// 連続したデータを受信
void sci_read(SCI_NO no, char* buff, int size)
{
	volatile int i;
	for (i = 0; i < size; i++)
	{
		*(buff++) = sci_read_byte(no);
	}
}

// データを受信しているか確認する。
// ポーリングで確認する
// 格納されていれば1を返し、格納されていなければ、0を返す
int sci_read_pol(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;

	if (sci->SSR.BIT.ORER)
	{
		sci_write_str(no, "Error_sci over run");
		sci->SSR.BIT.ORER = 0;
	}
	if (sci->SSR.BIT.PER)
	{
		sci_write_str(no, "Error_sci parity");
		sci->SSR.BIT.PER = 0;
	}
	if (sci->SSR.BIT.FER_ERS)
	{
		sci_write_str(no, "Error_sci flaming");
		sci->SSR.BIT.FER_ERS = 0;
	}

	// データが格納されているか確認
	if (sci->SSR.BIT.RDRF)
		return 1;

	return 0;
}


/********************************************************************************
	割り込み
********************************************************************************/
char sci_read_byte_intr(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	volatile char c;

	c = sci->RDR;
	sci->SSR.BIT.RDRF = 0;

	return c;
}
// 送信完了割り込みEnable
void sci_write_intr_enable(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	sci->SCR.BIT.TIE = 1;
}

// 送信完了割り込みDisable
void sci_write_intr_disable(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	sci->SCR.BIT.TIE = 0;
}

// 受信完了割り込みEnable
void sci_read_intr_enable(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	sci->SCR.BIT.RIE = 1;
}

// 受信完了割り込みDisable
void sci_read_intr_disable(SCI_NO no)
{
	volatile struct SCI* sci = regs[no].sci;
	sci->SCR.BIT.RIE = 1;
}

