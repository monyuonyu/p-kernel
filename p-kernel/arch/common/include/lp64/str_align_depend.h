/*
 *  str_align_depend.h (aarch64)
 */

#ifndef __SYS_STR_ALIGN_DEPEND_H__
#define __SYS_STR_ALIGN_DEPEND_H__

#define _pad_b(n)
#define _pad_l(n)   int :n;

/* AArch64 pointers are 8-byte aligned naturally — no padding field needed */
#define _align64

#endif /* __SYS_STR_ALIGN_DEPEND_H__ */
