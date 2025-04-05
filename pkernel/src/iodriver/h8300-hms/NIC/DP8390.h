/*
 * DP8390.h
 *
 *  Created on: 2010/12/06
 *      Author: mizu
 */

#ifndef _DP8390_H_
#define _DP8390_H_

#include "ctype.h"
#include "libstr.h"
#include "basic.h"

// DP8390制御レジスタ
struct DP8390
{
	// 00h
	union // コマンドレジスタ PS=00 PS=01 PS=10
	{
		struct
		{
			UB STP :1; // すべてストップ
			UB STA :1; // 開始
			UB TXP :1; // パケット送信開始
			UB RD0 :1; //
			UB RD1 :1; // 000=禁止 001=リード 010=ライト 011パケット送信 1xx=強制停止
			UB RD2 :1; // リモートDMA
			UB PS0 :1; // 00=p0 01=p1 10=p2 11=p予約
			UB PS1 :1; // ページ選択
		};

		struct
		{
			UB :6;
			UB PS :2;
		};

		struct
		{

			UB :3;
			UB RD :3;
			UB :2;
		};
	}CR;

	// 01h
	union
	{
		UB CLDA0;	// PS=00(r) PS=10(w)
		UB PSTART;	// PS=00(w) PS=10(r)
		UB PAR0;	// PS=01
	};

	//02h
	union
	{
		UB CLDA1;	// PS=00(r) PS=10(w)
		UB PSTOP;	// PS=00(w) PS=10(r)
		UB PAR1;	// PS=01
	};

	// 03h
	union
	{
		UB BNRY;	// PS=00
		UB PAR2;	// PS=01
		UB RNPP;	// PS=10
	};

	//04h
	union
	{
		union // 送信ステータス・レジスタ PS=00
		{
			struct
			{
				UB PTX :1;
				UB :1; // 予約
				UB COL :1;
				UB ABT :1;
				UB CSR :1;
				UB FU :1;
				UB CHD :1;
				UB OWC :1;
			};
		}TSR;

		UB TPSR;	// PS=00→(w) PS=10→(r)
		UB PAR3;	// PS=01(rw)

	};

	// 05h
	union
	{
		UB NCR;		// PS=00(r)
		UB TBCR0;	// PS=00(w)
		UB PAR4;	// PS=01(rw)
		UB LNPP;	// PS=10(rw)
	};

	// 06h
	union
	{
		UB FIFO;	// PS=00(r)
		UB TBCR1;	// PS=00(w)
		UB PAR5;	// PS=01(rw)
		UB HAC;		// PS=10(r)
	};

	// 07h
	union // 割り込みステータス・レジスタ PS=00(rw)
	{
		struct
		{
			UB RST :1;
			UB RDC :1;
			UB CNT :1;
			UB OVW :1;
			UB TXE :1;
			UB RXE :1;
			UB PTX :1;
			UB PRX :1;
		};

		UB CURR;	// PS=01(rw)
		UB LAC;		// PS=10(rw)
	}ISR;

	// 08h
	union
	{
		UB CRDA0;	// PS=00(r)
		UB RSAD0;	// PS=00(w)
		UB MAR0;	// PS=01(rw)
	};

	// 09h
	union
	{
		UB CRSA1;	// PS=00(r)
		UB RSAD1;	// PS=00(w)
		UB MAR1;	// PS=01(rw)
	};

	// 0Ah
	union
	{
		UB RBCR0;	// PS=00(w)
		UB MAR2;	// PS=01(rw)
	};

	// 0Bh;
	union
	{
		UB RBCR1;	// PS=00(w)
		UB MAR3;	// PS=01(rw)
	};

	// 0Ch
	union
	{
		union // 受信ステータス・レジスタ PS=00(r)
		{
			struct
			{
				UB PRX :1;
				UB CRC :1;
				UB FAE :1;
				UB FO :1;
				UB MPA :1;
				UB PHY :1;
				UB DIS :1;
				UB DFR :1;
			};
		}RSR;

		// 受信コンフィギュレーション・レジスタ
		union  	// PS=00(w) PS=10(r)
		{
			struct
			{
				UB SEP :1;
				UB AR :1;
				UB AB :1;
				UB AM :1;
				UB PRO :1;
				UB MON :1;
				UB :2; // 予約
			};
		}RCR;

		UB MAR4;	// PS=01(rw)
	};

	// 0Dh
	union
	{
		UB CNTR0;	// PS=00(r)

		 // 送信コンフィギュレーション・レジスタ
		union	// PS=00(w) PS=10(r)
		{
			struct
			{
				UB CRC :1; // CRC制御
				UB LB0 :1; // ループバック指定
				UB LB1 :1; //
				UB ATD :1; //
				UB OFST :1; // 衝突オフセット
				UB :3; // 予約

			};

			struct
			{
				UB :1;
				UB LB :2;
				UB :5;
			};
		}TCR;

		UB MAR5;	// PS=01(rw)
	};

	// 0Eh
	union
	{
		UB CNTR1;	// PS=00(r)

		// データコンフィギュレーション・レジスタ
		union // PS=00(w) PS=10(r)
		{
			struct
			{
				UB WTS :1; // ワード転送指定
				UB BOS :1; // バイト順番指定
				UB LAS :1; // ロングアドレス指定
				UB LS :1; // ループバック指定
				UB AR :1; // 自動初期化リモート
				UB FT0 :1; // DMA転送を起動するタイミングの指定
				UB FT1 :1; // FIFO スレッショルド指定
				UB :1; // 予約
			}DCR;

			struct
			{
				UB :5;
				UB FT :2;
				UB :1;
			};
		};

		UB MAR6;	//PS=01(rw)
	};

	// 0Fh
	union
	{
		UB CNTR2;	// PS=00(r)

		// 割り込みマスク・レジスタ PS=00(w) PS=10(r)
		union
		{
			struct
			{
				UB PRXE :1;
				UB PTXE :1;
				UB RXEE :1;
				UB TXEE :1;
				UB OVWE :1;
				UB CNTE :1;
				UB RDCE :1;
				UB :1;
			};
			UB UB_IMR;
		}IMR;

		UB MAR7;	// PS=01(rw)
	};
};

#endif /* DP8390_H_ */

