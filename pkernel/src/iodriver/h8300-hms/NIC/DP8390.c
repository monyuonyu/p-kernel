/*
 * DP8390.c
 *
 *  Created on: 2010/12/13
 *      Author: mizu
 */

#include "DP8390.h"

struct DP8390* nic;

void nic_mac_set()
{

}

// ON = 1 , OFF = 0
void nic_send_ON_OFF(int ON_OFF)
{
	if(ON_OFF)
		nic->TCR.LB0 = 0;
	else
		nic->TCR.LB0 = 1;
}

void nic_rec_ON_OFF(int ON_OFF)
{
	if(ON_OFF)
		nic->RCR.MON = 0;
	else
		nic->RCR.MON = 1;
}

void select_page(int page_no)
{
	nic->CR.PS = 0;

	switch(page_no)
	{
	case 0:					// page0
		break;
	case 1:
		nic->CR.PS0 = 1;	// page1
		break;
	case 2:
		nic->CR.PS1 = 1;	// page2
		break;

	}
}

void nic_init(void* nic_base)
{
	long i;
	UB buff[16];

	nic = (struct DP8390*)nic_base;	// ベースアドレスセット

	*(UB*)((UW)nic + (UW)0x18) = (UB)0x00;			// リセット
	for(i = 0; i < 400000; i++);	// ウェイト

	select_page(0);					// page0選択

	nic->CR.STP = 1;				// ストップ
	nic->IMR.UB_IMR = 0;					// 割り込みマスククリア
	nic->ISR.CURR = 0xFF;				// 割り込み要求クリア
	nic->RBCR0 = 0;					// リモートバイトカウントレジスタ0 クリア
	nic->RBCR1 = 0;					// リモートバイトカウントレジスタ1 クリア
	nic->DCR.WTS = 1;				// ワード転送指定
	nic->DCR.LS = 1;				// ループバック設定
//	nic->DCR.FT1 = 1;

	nic_send_ON_OFF(0);				// 送信部OFF
	nic_rec_ON_OFF(0);				// 受信部OFF


	nic->PSTART = (0x4c00 >> 8);	// リングバッファ開始位置
	nic->PSTOP = (0x8000 >> 8);		// リングバッファ終了位置

	nic->BNRY = ((0x8000 >> 8) - 1);
	select_page(1);


}
