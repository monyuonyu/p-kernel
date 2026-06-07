/*
 * p-kernel libc implementation
 * 文字列から整数変換実装 (atoi.c)
 *
 * このファイルはatoi関数を実装します。
 * 文字列を整数(int)に変換します。
 */

#include "stdlib.h"

/**
 * @brief 文字列を整数に変換
 * @param _str 変換する文字列 (数字のみ)
 * @return 変換された整数値
 * @note 数字以外の文字が含まれる場合は0を返す
 * @warning オーバーフローや負の値には対応していない
 */
int atoi(const char* _str)
{
	int cnt = 0;
	int val = 0;
	int magnifi = 1;

	// 「数字の」文字列のケタをカウントする
	while(1)
	{
		if(_str[cnt] == 0x00)	// NULLでカウンタ終了
			break;

		if(_str[cnt] < 0x30 || _str[cnt] > 0x39)	// アスキーコードの数字でなかったら
			return 0;

		cnt++;
	}

	while(cnt > 0)
	{
		val += (_str[cnt - 1] - 0x30) * magnifi;
		magnifi *= 10;
		cnt--;
	}

	return val;
}
