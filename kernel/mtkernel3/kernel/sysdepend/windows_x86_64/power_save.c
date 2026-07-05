/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel Windows x86-64 ネイティブポート
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	power_save.c
 *	省電力処理（Windows x86-64 ネイティブポート）
 *
 *	アイドル時の省電力は dispatch.c のアイドルパス（knl_idle_wait の
 *	Sleep）で実現済みのため、ここでの追加処理はありません。
 */

#include <sys/machine.h>
#ifdef WINDOWS_X86_64

#include "kernel.h"

EXPORT void low_pow( void )
{
}

EXPORT void off_pow( void )
{
}

#endif /* WINDOWS_X86_64 */
