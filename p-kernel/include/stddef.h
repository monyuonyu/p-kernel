#ifndef _STDDEF_H
#define _STDDEF_H

#define NULL ((void*)0)
typedef long int ptrdiff_t;
typedef long unsigned int size_t;

#define offsetof(type, member)  ((size_t)((char *)&((type *)0)->member - (char *)0))

#endif /* _STDDEF_H */