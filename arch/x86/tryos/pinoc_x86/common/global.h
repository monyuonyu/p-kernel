#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include "typedef.h"

/* レジスタ */
union
{
	u4 eax;
	struct
	{
		u2 ax;
	};
	struct
	{
		BYTEBIT al;
		BYTEBIT ah;
	};
}EAX;

union
{
	u4 ecx;
	struct
	{
		u2 cx;
	};
	struct
	{
		BYTEBIT cl;
		BYTEBIT ch;
	};
}ECX;

union
{
	u4 edx;
	struct
	{
		u2 dx;
	};
	struct
	{
		BYTEBIT dl;
		BYTEBIT dh;
	};
}EDX;

union
{
	u4 esi;
}ESI;

union
{
	u4 edi;
}EDI;

// 画モードをメモ
struct
{
	s1* vram;
	u4 xsize;
	u4 ysize;
	u1 color_bit;

} graphicmode;

#endif
