#ifndef __TYPEDEFC_H_
#define __TYPEDEFC_H_

#ifndef __STDCLIB__
#define __STDCLIB__
// 標準ライブラリ
typedef unsigned int size_t;

#endif

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

#endif

#endif
