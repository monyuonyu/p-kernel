#include "interrupt.h"
#include "intr.h"


/* 共通ハンドラ */
void interrupt(softvec_type_t type, unsigned long sp)
{

//	softvec_handler_t *h;
//	h = (softvec_handler_t*)SOFTVEC_ADDR[type];

	if(((softvec_handler_t*)SOFTVEC_ADDR)[type])
	{
		(((softvec_handler_t*)SOFTVEC_ADDR)[type])(type, sp);	/* 登録ハンドラ呼び出し */
	}
}


/* 割り込みベクタへハンドラを登録 */
void softvec_setintr(softvec_type_t type, softvec_handler_t handler)
{
	((softvec_handler_t*)SOFTVEC_ADDR)[type] = handler;
}


///* 割り込みベクタの初期化 */
void softvec_init()
{
	int type;

	for(type = 0; type < SOFTVEC_TYPE_NUM; type++)
	{
		softvec_setintr(type, 0);
	}
}
