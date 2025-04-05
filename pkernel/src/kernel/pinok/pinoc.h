/*
 * pinoc.h
 *
 *  Created on: 2011/05/07
 *      Author: mizu
 */

#ifndef PINOC_H_
#define PINOC_H_

// #include "dbg.hpp"
#include "interrupt.h"

typedef int (*pinoc_func_t)(void);
typedef void (*pinoc_handler_t)(void);

// システムコールタイプ
typedef enum
{
	PINOC_SYSCALL_RUN = 0,
	PINOC_SYSCALL_EXIT
}pinoc_syacall_t;

// システムコールパラメータ
typedef struct
{
	union
	{
		struct
		{
			pinoc_func_t func;
			char* name;
			int stacksize;
			int argc;
			char** argv;
//			pinoc_thread_id_t ret;
		}run;
		struct
		{
			int dummy;
		}exit;

	}un;
}pinoc_syscall_param_t;

/********************************************************************************
 *		カーネル構造体定義
 ********************************************************************************/
#define THREAD_NUM 6
#define THREAD_NAME_SIZE 15

// ユーザー毎のコンテキストが保存されたスタックポインタ
typedef struct _pinoc_context
{
	long int sp;
}pinoc_context;

/*
 * スレッドの型
 */
typedef struct _pinoc_thread
{
	struct _pinoc_thread* next;			// レディーキュー
	char name[THREAD_NAME_SIZE + 1];	// スレッド名
	char* stack;						//

	struct
	{
		pinoc_func_t func;				// 関数名
		int argc;						// 引数1
		char** argv;					// 引数2
	}init;

	struct
	{
		pinoc_syacall_t type;			// システムコールの種類
		pinoc_syscall_param_t param;	// システみコールの引数
	}syscall;

	pinoc_context context;				// このスレッドのコンテキスト情報

}pinoc_thread;

// レディースキュー
static struct
{
	pinoc_thread* head;
	pinoc_thread* tail;
}readyque;




/********************************************************************************
 * 		プロトタイプ宣言
********************************************************************************/
// システム・コール
//void pinoc_run(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[]);
//void pinoc_exit();

// ライブラリ関数
void pinoc_start(pinoc_func_t func, char *name, int stack_size, int argc, char* argv[]);
//void pinoc_syserr();
//void pinoc_syscall(pinoc_syscall_type_t type, pinoc_syscall_param_t* param);

// ユーザー・スレッド
int test08_1_main(int argc, char* argv[]);

//static pinoc_handler_t handlers[softvec];

/*
 * ディスパッチの内容は外部ファイルにアセンブラで記述してある
 */
void dispatch(pinoc_context* context);


#endif /* PINOC_H_ */
