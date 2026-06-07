#ifndef __STRING_H_
#define __STRING_H_

// 超簡易標準ヘッダです。。。

#include "typedef_c.h"

void* memmove(void *_Dst, const void *_Src, size_t _Size)
{
	s1* Dst = (s1*) _Dst;
	s1* Src = (s1*) _Src;

	si si_cnt;
	FOR(si_cnt, _Size)
	{
		Dst[si_cnt] = Src[si_cnt];
	}
}

void* memset(void *_Dst, int _Val, size_t _Size)
{
	s1* Dst = (s1*)_Dst;

	si si_cnt;
	FOR(si_cnt, _Size)
	{
		Dst[si_cnt] = _Val;
	}

}

#endif
