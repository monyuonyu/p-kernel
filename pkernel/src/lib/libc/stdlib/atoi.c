#include "stdlib.h"

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
