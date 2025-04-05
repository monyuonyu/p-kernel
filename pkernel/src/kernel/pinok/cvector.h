#ifndef __CVECTOR_H__
#define __CVECTOR_H__

/* 変数命令規則 */
#ifndef __VARIABLE_RULE__
#define __VARIABLE_RULE__
typedef unsigned int ui;
typedef unsigned char u1;
typedef unsigned short u2;
typedef unsigned long int u4;
typedef unsigned long long int u8;
typedef signed int si;
typedef signed char s1;
typedef signed short s2;
typedef signed long int s4;
typedef signed long long int s8;
#endif

#if 0 /* 暫定 */
#ifndef __CPRINT__
#define __CPRINT__
#include <stdio.h>
#define _PL(str) printf(str)
#endif
#endif /* 暫定 */

#ifndef __CFOR__
#define __CFOR__
#define FOR(i, j) for(i = 0; ((i) < (j)); ((i)++))
#endif

/* リスト管理構造体 */
typedef struct VECLIST
{
	struct VECLIST* next;				/* 前要素	*/
	struct VECLIST* prev;				/* 次要素	*/
	void* address;						/* アドレス */

}VECLIST;

typedef struct CVECTOR_PARAMETER
{
	VECLIST* pri_head;				/* 先頭要素				*/
	VECLIST* pri_tail;				/* 最終要素				*/
	si pri_Elementcnt;				/* 要素数				*/
	si pri_ElementSize;				/* 要素サイズ			*/
	si pri_EraseFlag;				/* _Erase関数が呼ばれると1	*/

	VECLIST* Search_tmp;
	VECLIST* Search_tmp_next;
	si LastSearchNo;
	si search_firstflag;

}CVECTOR_PARAMETER;

typedef struct _C_VECTOR
{
	/* 個々のリストを管理するためのparameter */
	CVECTOR_PARAMETER parameter;

	void* (*at)(int _Dst);
	int (*insert)(void *_Src, int _Dst);
	int (*push_back)(void *_Src);
	int (*size)();
	int (*swap)(int _target1, int _target2);
	int (*erase)(int _Dst);
	int (*clear)();

}C_VECTOR;

/* プロトタイプ宣言 */
int CVECTOR_START(C_VECTOR *_Ldata, int Size);
int CVECTOR_END();
void CVECTOR_PARAMETER_SET(C_VECTOR* _Src);

#endif
