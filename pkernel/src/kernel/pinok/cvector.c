#include "cvector.h"
#include <stdlib.h>
#include <string.h>

/* 静的プロトタイプ宣言 */
static VECLIST* Search(si _Dst);
static void* _At(si _Dst);
static si _Insert(void *_Src, si _Dst);
static si _Push_back(void *_Src);
static si _Size();
static si _Swap(si _target1, si _target2);
static si _Erase(si _Dst);

/* 静的グローバル変数 */
static CVECTOR_PARAMETER* g_parameter;

/*******************************************************************************
	概要		：	要素番号を検索します
	説明		：	内部関数なので、外部から呼び出されることはありません
	Include	：
	引数		：	要素番号(int型)
	戻り値	：	VECLIST *
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static VECLIST* Search(si _Dst)
{

	/* 最初の一回だけ処理 */
	if(g_parameter->search_firstflag)
	{
		g_parameter->search_firstflag = 0;
		g_parameter->Search_tmp = g_parameter->pri_head;
	}

	/* 前回に_Erase関数が呼ばれていた */
	if(g_parameter->pri_EraseFlag)
	{
		g_parameter->Search_tmp = g_parameter->Search_tmp_next;
		g_parameter->pri_EraseFlag = 0;
	}

	/* 前回検索した番号と今回検索する番号が同じだったなら */
	if(g_parameter->LastSearchNo == _Dst)
		return g_parameter->Search_tmp;

	if((_Dst - g_parameter->LastSearchNo) > 0)
	{
		/* 前方向探査 */
		si cnt;
		FOR(cnt, (_Dst - g_parameter->LastSearchNo))
		{
			/* 末端まで検索したら強制終了 */
			if(!g_parameter->Search_tmp->next)
				break;

			g_parameter->Search_tmp = g_parameter->Search_tmp->next;
		}
	}
	else
	{
		/* 後方向探査 */
		si cnt;
		FOR(cnt, (g_parameter->LastSearchNo - _Dst))
		{
			/* 末端まで検索したら強制終了 */
			if(!g_parameter->Search_tmp->prev)
				break;

			g_parameter->Search_tmp = g_parameter->Search_tmp->prev;
		}
	}

	/* 最後に検索したリストの番号を記憶 */
	g_parameter->LastSearchNo = _Dst;

	/* _Erase対策 */
	g_parameter->Search_tmp_next =g_parameter-> Search_tmp->next;
	return g_parameter->Search_tmp;
}

/*******************************************************************************
	概要		：	指定した要素を取り出します
	説明		：	void型のポインタが戻り値なので、明示的にキャストする必要があります。
	Include	：
	引数		：	要素番号(int型)
	戻り値	：	void *
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static void* _At(si _Dst)
{
	VECLIST* Search_tmp = Search(++_Dst);

	/* headかtailだった場合error */
	if(!Search_tmp->address)
		return 0;

	return (void*)Search_tmp->address;
}

/*******************************************************************************
	概要		：	リストへ要素を格納します
	説明		：	登録したい要素を、指定した場所へ挿入します
	Include	：
	引数		：	要素のポインタ, 挿入したい場所(int型)
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Insert(void *_Src, si _Dst)
{
	void* data_tmp;
	VECLIST* list_tmp;
	VECLIST* Search_tmp = g_parameter->pri_head;

	data_tmp = malloc(g_parameter->pri_ElementSize);
	memmove(data_tmp, _Src, g_parameter->pri_ElementSize);

	list_tmp = (VECLIST*)calloc(1, sizeof(VECLIST));
	list_tmp->address = data_tmp;

	Search_tmp = Search(_Dst);

	/* リストへ挿入 */
	/*■  Search_tmp <-> list_tmp <-> Search_tmp->next  ■*/
	list_tmp->next = Search_tmp->next;
	list_tmp->prev = Search_tmp;
	Search_tmp->next->prev = list_tmp;
	Search_tmp->next = list_tmp;

	/* 要素数カウントアップ */
	g_parameter->pri_Elementcnt++;
	return 0;
}

/*******************************************************************************
	概要		：	リストへ要素を格納します
	説明		：	登録したい要素を一番最後のリストへ挿入
	Include	：
	引数		：	要素のポインタ
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Push_back(void *_Src)
{
	/* 一番最後のリストへ挿入 */
	_Insert(_Src, g_parameter->pri_Elementcnt);
	return 0;
}

/*******************************************************************************
	概要		：	現在の要素の数を返します
	説明		：	現在の要素の数をint型で返します。
	Include	：
	引数		：	無し
	戻り値	：	要素数(int型)
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Size()
{
	return g_parameter->pri_Elementcnt;
}

/*******************************************************************************
	概要		：	指定された要素の場所を入れ替えます
	説明		：	現在のparameterのセットされているリストを入れ替えますします
	Include	：
	引数		：	要素番号1(int型の数値), 要素番号2(int型の数値)
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Swap(si _target1, si _target2)
{
	VECLIST* Search_tmp1, * Search_tmp2;
	void* tmp;
	Search_tmp1 = Search(++_target1);
	Search_tmp2 = Search(++_target2);

	/* headかtailだった場合error */
	if(!Search_tmp1->address || !Search_tmp2->address)
		return 1;

	/* 要素の入れ替え　※リストは変更しない */
	tmp = Search_tmp1->address;
	Search_tmp1->address = Search_tmp2->address;
	Search_tmp2->address = tmp;

	return 0;
}

/*******************************************************************************
	概要		：	指定された要素をクリアします
	説明		：	現在のparameterのセットされているリストをクリアします
	Include	：
	引数		：	要素番号(int型の数値)
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Erase(si _Dst)
{

	VECLIST* Search_tmp;
	Search_tmp = Search(++_Dst);

	/* リストのつなぎ替え */
	Search_tmp->prev->next = Search_tmp->next;
	Search_tmp->next->prev = Search_tmp->prev;

	/* リストと要素を開放 */
	free(Search_tmp->address);
	free(Search_tmp);

	/* _Erase実行済みフラグON */
	g_parameter->pri_EraseFlag = 1;

	g_parameter->pri_Elementcnt--;
	return 0;
}

/*******************************************************************************
	概要		：	リスト構造の要素をすべてクリアします
	説明		：	現在のparameterのセットされているリストをクリアします
	Include	：
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
static si _Clear()
{
	si cnt;
	si Frequency = g_parameter->pri_Elementcnt;
	FOR(cnt, Frequency)
	{
		_Erase(0);
	}
	return 0;
}

/* 外部関数 ----------------------------------------------*/

/*******************************************************************************
	使い方	:	必ず最初に呼び出します。
	概要		：	CVECTORが利用するparameterをセットします
	説明		：	複数のリストを管理するために、parameterを分離させます
	Include	：	cvector.h stdlib.h
	引数		：	C_VECTOR *_Ldata	;オブジェクト
				int Size			;リストの要素サイズを指定します
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
void CVECTOR_PARAMETER_SET(C_VECTOR* _Src)
{
	g_parameter = &_Src->parameter;
}

/*******************************************************************************
	使い方	:	必ず最初に呼び出します。
	概要		：	CVECTORの全体の処理を開始します
	説明		：	構造体の初期化と、メソッドをセットします
	Include	：	cvector.h stdlib.h
	引数		：	C_VECTOR *_Ldata	;オブジェクト
				int Size			;リストの要素サイズを指定します
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
si CVECTOR_START(C_VECTOR *_Ldata, int Size)
{

	/* リストの先頭と後尾の空要素作成 */
	VECLIST* head = (VECLIST*)calloc(1, sizeof(VECLIST));
	VECLIST* tail = (VECLIST*)calloc(1, sizeof(VECLIST));

	/* parameterの初期化 */
	memset(&_Ldata->parameter, 0, sizeof(_Ldata->parameter));

	g_parameter->pri_head = head;
	g_parameter->pri_tail = tail;

	/* リスト構造の構築 */
	g_parameter->pri_head->next = tail;
	g_parameter->pri_head->prev = 0;
	g_parameter->pri_tail->prev = head;
	g_parameter->pri_tail->next = 0;

	/* メソッドを構造体へ格納 */
	_Ldata->at			= _At;
	_Ldata->insert		= _Insert;
	_Ldata->erase		= _Erase;
	_Ldata->push_back	= _Push_back;
	_Ldata->size		= _Size;
	_Ldata->swap		= _Swap;
	_Ldata->clear		= _Clear;

	/* 要素サイズ格納 */
	g_parameter->pri_ElementSize = Size;

	/* _Serch関数フラグ */
	g_parameter->search_firstflag = 1;

	return 0;
}

/*******************************************************************************
	使い方	:	プラグラムの一番最後に必ず呼び出します
	概要		：	C_VECTORを終了します
	説明		：	全ての要素をFreeしたあと、ダミーデータをFreeします
	Include	：	cvector.h stdlib.h
	戻り値	：	成功なら0不成功なら1を返します
	Global	：	CVECTOR_PARAMETER g_parameter;
*******************************************************************************/
si CVECTOR_END()
{
	_Clear();
	free(g_parameter->pri_head);
	free(g_parameter->pri_tail);

	return 0;
}
