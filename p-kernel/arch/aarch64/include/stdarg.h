/*
 *  stdarg.h (aarch64)
 */

#ifndef _STDARG_H_
#define _STDARG_H_

typedef __builtin_va_list   va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(d, s)       __builtin_va_copy(d, s)

/* glibc's <stdio.h> spells va_list as __gnuc_va_list internally.
 * When this header shadows the system <stdarg.h> (hosted builds in
 * arch/linux), provide that name too so glibc compiles. */
#ifndef __GNUC_VA_LIST
#define __GNUC_VA_LIST 1
typedef __builtin_va_list   __gnuc_va_list;
#endif

#endif /* _STDARG_H_ */
