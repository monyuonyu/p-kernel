#ifndef _STDDEF_H
#define _STDDEF_H

#define NULL ((void*)0)
typedef long int ptrdiff_t;
typedef long unsigned int size_t;

/* wchar_t is needed by glibc's <stdlib.h> / <wchar.h>. Defined here
 * (rather than in a wchar.h placeholder) so the hosted Linux build
 * can include system <stdlib.h> without first finding wchar.h. */
#ifndef __WCHAR_T_DEFINED__
#define __WCHAR_T_DEFINED__
typedef int wchar_t;
#endif

#define offsetof(type, member)  ((size_t)((char *)&((type *)0)->member - (char *)0))

#endif /* _STDDEF_H */