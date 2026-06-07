#ifndef _STDARG_H_
#define _STDARG_H_

typedef char *va_list;
void __builtin_va_start(va_list);
#define va_start(ap,parmN)  __builtin_va_start(ap)
#define va_arg(ap,type)     (*(type *)((ap+=((sizeof(type) + 1) & ~1))-((sizeof(type) + 1) & ~1)))
#define va_end(ap)          (ap=(va_list)0, (void)0)

#endif  /* _STDARG_H_ */

