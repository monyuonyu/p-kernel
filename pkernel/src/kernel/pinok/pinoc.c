/*
 * main.c
 *
 *  Created on: 2011/03/29
 *      Author: mizu
 */

#include "3069f_io.h"
#include "3069f_sci.h"
// #include "dbg.hpp"
#include "pinoc.h"
#include "interrupt.h"
#include "intr.h"

///********************************************************************************
// * 		グローバル宣言
//********************************************************************************/
static pinoc_thread* current;						// 使用中のスレッド情報が格納される
static pinoc_thread threads[THREAD_NUM];			// スレッドの数だけ領域確保
static pinoc_handler_t handlers[SOFTVEC_TYPE_NUM];	// ハンドラの数だけ領域確保


static pinoc_thread* thread_run(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[]);

///********************************************************************************
// * 		レディースキュー操作
//********************************************************************************/
//static int getcurrent()
//{
//	if(current == 0)
//		return -1;
//
//	readyque.head = current->next;
//	if(readyque.head == 0)
//		readyque.tail = 0;
//
//	current->next = 0;
//
//	return 0;
//}
//
//static int putcurrent()
//{
//	if(current == 0)
//		return -1;
//
//	if(readyque.tail)
//		readyque.tail->next = current;
//	else
//		readyque.head = current;
//
//	readyque.tail = current;
//
//	return 0;
//}
//
///********************************************************************************
// * 		スレッド操作
//********************************************************************************/
//static void thread_end()
//{
////	pinoc_exit();
//}
//
//static void thread_init(pinoc_thread* the)
//{
////	the->init.func(the->init.argc, the->init.argv);
////	thread_end();
//}
//
//
//
////static int thread_exit()
////{
////	sci_write_str(SCI_NO_1, current->name);
////	sci_write_str(SCI_NO_1, "EXIT.\n\r");
////	memset(current, 0, sizeof * current);
////	return 0;
////}
////
//static void thread_intr(softvec_type_t type, unsigned long sp)
//{
//	current->context.sp = sp;
//
//	// handlerに登録してあれば、実行
//	if(handlers[type])
//		handlers[type]();
//
//	// スレッドのスケジューリング		ラウンドロビン方式
//	// schedule();
//
//	// ディスパッチ
//	dispatch(&current->context);
//}
//
//// 二重の割り込みベクターから呼び出される
///*
// * システムコール呼び出し
// */
//void syscall_intr()		// 未実装
//{
//
//}
//
///*
// * システムエラー呼び出し
// */
//void syserror_intr()	// 未実装
//{
//
//}
////
/////*
//// * OSのスレッド関連の割り込みベクター ※ 二十構造
//// */
////void thread_intr(softvec_type_t type, unsigned long sp)
////{
////	switch(typy)
////	{
////	case SOFTVEC_TYPE_SYSTEM:
////		handlers[type]();
////		break;
////	case SOFTVEC_TYPE_SOFTERR:
////		handlers[type]();
////		break;
////
////	}
////}
////
////void pinoc_syscall(pinoc_syscall_type_t type, pinoc_syscall_param_t* param)
////{
////
////}
////
////// API (システムコールを利用してスレッド生成)
////void pinoc_run(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[])
////{
////	pinoc_syscall_param_t param;
////	param.un.run.func = func;
////	param.un.run.name = name;
////	param.un.run.stacksize = stack_size;
////	param.un.run.argc = argc;
////	param.un.run.argv = argv;
////
////	// システムコール
////	pinoc_syscall(PINOC_SYSCALL_RUN, param);
////
////	return param.un.run.ret;
////}
////
///*
// * ユーザースレッド起動
// */
//int start_thread()
//{
////	pinoc_run(test08_1_main);
//	dbg();
//	return 0;
//}
//
//// スレッド生成
//static pinoc_thread* thread_run(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[])
//{
//	dbg();
////	as_SLEEP_LOOP_3069
//
//	int i;
//	pinoc_thread* the;
//	volatile long int* sp;
//	volatile extern char u_stack;
//	volatile static char* thread_stack = &u_stack;
//
//	// スレッド情報格納領域確保
//	for(i = 0; i < THREAD_NUM; i++)
//	{
//		the = &threads[i];
//		if (!the->init.func)
//			break;
//	}
//	if(i == THREAD_NUM)
//		return (pinoc_thread*)-1;
//
//	memset(the, 0, sizeof *the);
//
//	// スレッド容器に新規スレッド情報を格納
//	strcpy(the->name, name);		// 名前
//	the->next = 0;				// ネクスト
//	the->init.func = func;			// 関数ポインタ
//	the->init.argc = argc;			// 引数1
//	the->init.argv = argv;			// 引数2
//
//	// スレッドスタックの初期化
//	memset(thread_stack, 0, stack_size);
//
//	// スタック確保開始ポインタを進める
//	thread_stack += stack_size;
//
//	// このスレッドへスタックポインタを設定する（常にスタックの先頭を示す）
//	the->stack = thread_stack;
//
//	// 新規スタックの初期化
//	sp = (long int)the->stack;
//
//	// スタックの最初にend関数を設置することで、最終的にreturnされたときend関数が呼び出される
//	*(--sp) = (long int)thread_end;
//
//	// スタックの２番目にinit関数を設置することで、Dispachしたときに、init関数が呼び出される
//	// Dispachの処理に関してはアセンブラ参照
//	*(--sp) = (long int)thread_init;
//	*(--sp) = 0;				// ER6
//	*(--sp) = 0;				// ER5
//	*(--sp) = 0;				// ER4
//	*(--sp) = 0;				// ER3
//	*(--sp) = 0;				// ER2
//	*(--sp) = 0;				// ER1
//	*(--sp) = (long int)the;	// ER0 (引数1)
//
//	// 現在のスタックポインタを格納
//	the->context.sp = (long int)sp;
//
//	// currentをレディースキューへ戻す(最初はcurrent==0)
//	putcurrent();
//
//	// 新規作成したスレッドをレディースキューへ接続
//	current = the;
//	putcurrent();
//
//	dbg();
//	return current;
//}
//
///*
// * 手動でスレッドの生成
// */
//void pinoc_start(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[])
//{
//	// スレッドコンテキストを初期化
//	current = 0;
//
//	// レディースキューを初期化
//	readyque.head = 0;
//	readyque.tail = 0;
//
//	// スレッドと、ハンドラ領域の初期化
//	memset(threads, sizeof(threads));
//	memset(handlers, sizeof(handlers));
//
//	dbg();
////	as_SLEEP_LOOP_3069
//
//	/*
//	 * スレッド用割り込みハンドラの初期化
//	 * ※ 二重構造に注意
//	 */
////	softvec_setintr(SOFTVEC_TYPE_SYSTEM, thread_intr);
////	softvec_setintr(SOFTVEC_TYPE_SOFTERR, thread_intr);
//
//	/*
//	 * threadVectorへ登録
//	 * RomVector -> SoftVector -> Handlers(threadVector)
//	 */
//	handlers[SOFTVEC_TYPE_SYSTEM] = syscall_intr;
//	handlers[SOFTVEC_TYPE_SOFTERR] = syserror_intr;
//
//	/*
//	 * スレッドを生成してスレッドIDを返却
//	 * スレッドを生成するだけで処理は帰ってくる
//	 */
//	current = (pinoc_thread*)thread_run(func, name, stack_size, argc, argv);
//
//	dbg();
//
//	/*
//	 * ディスパッチ
//	 * 現在のレディースキューに格納されているスタック情報(IPも含め)を元通りに復元する
//	 */
//	dispatch(&current->context);
//
//	// ここへは帰ってこない
//}

// ここからカーネルのプログラムが始まる。
int pinokernel_main()
{

	static char str2[] = "Welcome PINoC Kernel!!\n\r";
	sci_write_str(SCI_NO_1, str2);

	// dbg();

	// ポート4を光らすテスト
//	volatile unsigned char* p4ddr = (volatile unsigned char*) 0xFEE003;
//	volatile unsigned char* p4 = (volatile unsigned char*) 0xFFFFD3;
//	*p4ddr = 0x03;
//	*p4 = 0x01;


//	as_SLEEP_LOOP_3069

//	pinoc_start()

	// 初期スレッド開始 	※中でディスパッチが行われるのでこの関数へ帰ってこない
//	pinoc_start(start_thread, "test_tsk", 0x100, 0, 0);

	return 0;
}

/********************************************************************************
 * 		ユーザー関数
********************************************************************************/

// Threadとして起動	テスト用関数
//int test08_1_main(int argc, char* argv[])
//{
//	static char str1[] = "Hello test_tsk!!\n\r";
//	sci_write_str(SCI_NO_1, str1);
//	while(1);
//
//}
