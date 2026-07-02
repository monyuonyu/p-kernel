/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel 互換シム
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	syscall.h（互換シム）
 *
 *	micro T-Kernel 2.0 では <syscall.h> がトップレベルにあったため、
 *	アプリ層に `#include <syscall.h>` の書き方が残っています。
 *	μT-Kernel 3.0 の <tk/syscall.h> へ付け替えます。
 */

#ifndef __MTK3_COMPAT_SYSCALL_H__
#define __MTK3_COMPAT_SYSCALL_H__

#include <tk/tkernel.h>

#endif /* __MTK3_COMPAT_SYSCALL_H__ */
