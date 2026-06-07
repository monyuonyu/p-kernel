/*
 * 3069f_sci.h
 *
 *  Created on: 2010/11/27
 *      Author: pino
 */

#ifndef _SCI_H_
#define _SCI_H_

#include "ctype.h"
#include "libstr.h"
#include "basic.h"

#define BIT_CTL
#define C20MHZ 1
#define C25MHZ 0

#ifndef BIT_CTL
#define CKS0	(1 << 0)
#define CKS1	(1 << 1)
#define MP		(1 << 2)
#define STOP	(1 << 3)
#define OE		(1 << 4)
#define PE		(1 << 5)
#define CHR	(1 << 6)
#define CA		(1 << 7)
#define SHIFT_CTL

#else

#define CKS_1C	0x00
#define CKS_4C	0x01
#define CKS_16C	0x02
#define CKS_64C	0x03
#endif

#if C20MHZ
typedef enum {	//内部クロック20MHz
	BitRate_type_br4800  = 129,		// CKS=00
	BitRate_type_br9600  = 64,		// CKS=00
	BitRate_type_br19200 = 32,		// CKS=00
	BitRate_type_br38400 = 15		// CKS=00
}BitRate_type_t;
#endif

#if C25MHZ
enum { 	//内部クロック25MHz
  br4800  = 162,		// CKS=00
  br9600  = 80,		// CKS=00
  br19200 = 40,		// CKS=00
  br38400 = 19		// CKS=00
}BitRate;
#endif

struct SCI
{
	union
	{
		UB BYTE;
		struct
		{
			UB CA		:1;		// 0=調歩同期 1=クロック同期
			UB CHR		:1;		// 0=8bit data 1=7bit data
			UB PE		:1;		// 0=パリティなし 1=パリティあり
			UB OE		:1;		// 0=偶数パリティ 1=奇数パリティ
			UB STOP		:1;		// 0=ストップビット長1bit 1=ストップビット長2bit
			UB MP		:1;		// 0=シングルプロセッサ 1=マルチプロセッサ
			UB CKS1		:1;		// クロックセレクト
			UB CKS0		:1;		// クロックセレクト
		}BIT;

		struct
		{
			UB			:6;
			UB CKS		:2;
		};
	}SMR;

	UB BRR;

	union
	{
		UB BYTE;
		struct
		{
			UB TIE		:1;		// 送信完了時割り込み 0=TXI禁止 1=TXI許可
			UB RIE		:1;		// 受信完了時割り込み 0=RXI禁止 1=RXI許可
			UB TE		:1;		// 0=送信禁止 1=送信許可
			UB RE		:1;		// 0=受信禁止 1=受信許可
			UB MPIE		:1;		// マルチプロセッサ割り込み 0=禁止 1=許可
			UB TEIE		:1;		// 送信終了割り込み 0=禁止 1=許可
			UB CKE1		:1;		// 0=内部クロック(調歩同期) 1=外部クロック(クロック同期式)
			UB CKE0		:1;		// CKE1=0,0=SCI は、SCK 端子を使用しません CKE1=1,0=ビットレートと同じ周波数のクロックを出力
		}BIT;
	}SCR;

	UB TDR;

	union
	{
		UB BYTE;
		struct
		{
			UB TDRE		:1;		// 0=TDRに有効な送信データがライトされている 1=TDRに有効な送信データがない
								// ［クリア条件］TDRE=1 の状態をリードした後、0 をライトしたとき, DMAC でTDR へデータをライトしたとき
								// ［セット条件］TDR からTSR にデータ転送が行われてTDR にデータライトが可能になったとき, SCR のTE ビットが0 のとき, リセット、またはスタンバイモード時
			UB RDRF		:1;		// 0=RDRに受信データが格納されていない 1=RDRに受信データが格納されている
								// ［クリア条件］DMACでRDR のデータをリードしたとき, RDRF=1 の状態をリードした後、0 をライトしたとき, リセット、またはスタンバイモード時
								// ［セット条件］シリアル受信が正常終了し、RSR からRDR へ受信データが転送されたとき
			UB ORER		:1;		// 0=受信中、または正常に受信を完了した 1=受信時にオーバランエラーが発生した
			UB FER_ERS	:1;		// 0=受信中、または正常に受信を完了した 1=受信時にフレーミングエラーが発生した
			UB PER		:1;		// 0=受信中、または正常に受信を完了した 1=受信時にパリティエラーが発生した
			UB TEND		:1;		// 0=送信中である 1=送信を終了した
			UB MPB		:1;		// マルチプロセッサ関連
			UB MPBT		:1;		//	"
		}BIT;
	}SSR;

	UB RDR;

	union
	{
		UB BYTE;
		struct
		{
			UB			:4;
			UB SDIR		:1;
			UB SINV		:1;
			UB			:1;
			UB SMIF		:1;
		}BIT;
	}SCMR;
};

//typedef int SCI_NO;
//#define SCI_NO_0 0
//#define SCI_NO_1 1
//#define SCI_NO_2 2

typedef enum {
	SCI_NO_0 = 0,
	SCI_NO_1,
	SCI_NO_2
}SCI_NO;

#define SCI0A ((volatile struct SCI*)0xFFFFB0)
#define SCI1A ((volatile struct SCI*)0xFFFFB8)
#define SCI2A ((volatile struct SCI*)0xFFFFC0)

#define SMR0	0xFFFFB0 // シリアルモードレジスタ（SMR)
#define BRR0	0xFFFFB1 // ビットレートレジスタ（BRR）
#define SCR0	0xFFFFB2 // シリアルコントロールレジスタ（SCR）
#define TDR0	0xFFFFB3 // トランスミットデータレジスタ（TDR)		送信データ
#define SSR0	0xFFFFB4 // シリアルステータスレジスタ（SSR）
#define RDR0	0xFFFFB5 // レシーブシフトレジスタ（RSR）			受信データ
#define SCMR0	0xFFFFB6 // スマートカードモードレジスタ(SCMR)

#define SMR1	0xFFFFB8
#define BRR1	0xFFFFB9
#define SCR1	0xFFFFBa
#define TDR1	0xFFFFBb
#define SSR1	0xFFFFBc
#define RDR1	0xFFFFBd
#define SCMR1	0xFFFFBe

#define SMR2	0xFFFFC0
#define BRR2	0xFFFFC1
#define SCR2	0xFFFFC2
#define TDR2	0xFFFFC3
#define SSR2	0xFFFFC4
#define RDR2	0xFFFFC5
#define SCMR2	0xFFFFC6

/********************************************************************************
	ポーリング
********************************************************************************/
void sci_init(SCI_NO no, BitRate_type_t type);
void sci_write(SCI_NO no, char c);
void sci_write_str(SCI_NO no,const char* _Str);
void sci_read(SCI_NO no, char* buff, int size);
char sci_read_byte(SCI_NO no);
int sci_read_pol(SCI_NO no);

/********************************************************************************
	割り込み
********************************************************************************/
char sci_read_byte_intr(SCI_NO no);
void sci_write_intr_enable(SCI_NO no);
void sci_write_intr_disable(SCI_NO no);
void sci_read_intr_enable(SCI_NO no);
void sci_read_intr_disable(SCI_NO no);

#endif /* SCI_H_ */
