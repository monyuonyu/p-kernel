#ifndef __TYPEDEF_H_
#define __TYPEDEF_H_

#ifndef __CODING__
#define __CODING__

#define FOR(si_i, si_Cnt) for(si_i = 0; ((si_i) < (si_Cnt)); (si_i++))

typedef signed int si;
typedef signed char s1;
typedef signed short int s2;
typedef signed long int s4;
typedef signed long long int s8;

typedef unsigned int ui;
typedef unsigned char u1;
typedef unsigned short int u2;
typedef unsigned long int u4;
typedef unsigned long long int u8;

typedef void vo;

// bit
typedef union _BYTEBIT
{
	u1 byte;
	struct{
		u1 b1:1;
		u1 b2:1;
		u1 b3:1;
		u1 b4:1;
		u1 b5:1;
		u1 b6:1;
		u1 b7:1;
		u1 b8:1;
	};

}BYTEBIT;

typedef struct _RGB
{
	u1 B;
	u1 G;
	u1 R;
}RGB;

// セグメントディスクリプタ
typedef struct _SEGMENT_DESCRIPTOR
{
	u2 limit1;

	u2 base1;

	u1 p:1;
	u1 dpl:2;
	u1 s:1;
	u1 type:3;
	u1 a:1;

	u1 base3;

	u1 base2;

	u1 g:1;
	u1 db:1;
	u1 no:1;
	u1 avl:1;
	u1 limit2:4;

} SD;

#endif

#endif

// デバッグモード切り替え
#define DEBUG 1
