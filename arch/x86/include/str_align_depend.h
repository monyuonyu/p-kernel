/*
 *  str_align_depend.h (x86)
 *  Bit alignment definitions for x86 (32-bit, little-endian)
 */

#ifndef __SYS_STR_ALIGN_DEPEND_H__
#define __SYS_STR_ALIGN_DEPEND_H__

/* x86 is little-endian */
#define _pad_b(n)
#define _pad_l(n)   int :n;

/* No 64-bit alignment needed for x86 32-bit mode */
#define _align64

#endif /* __SYS_STR_ALIGN_DEPEND_H__ */
