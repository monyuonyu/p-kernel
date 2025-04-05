#include "func.h"

int decto10(char* _str)
{
	int cnt = 0;
	int val = 0;
	int magnifi = 1;

	// 文字列のケタをカウントする
	while(1)
	{
		if(_str[cnt] == 0x00)	// NULLでカウンタ終了
			break;

		if(!((_str[cnt] >= '0' && _str[cnt] <= '9') || (_str[cnt] >= 'A' && _str[cnt] <= 'F'))) // 16進数チェック
			return 0;

		cnt++;
	}

	while(cnt > 0)
	{
		if(_str[cnt - 1] >= '0' && _str[cnt - 1] <= '9')
		{
			val += (_str[cnt - 1] - 0x30) * magnifi;
		}
		else if(_str[cnt - 1] >= 'A' && _str[cnt - 1] <= 'F')
		{
			val += (_str[cnt - 1] - 0x37) * magnifi;
		}

		magnifi *= 16;
		cnt--;
	}

	return val;
}
