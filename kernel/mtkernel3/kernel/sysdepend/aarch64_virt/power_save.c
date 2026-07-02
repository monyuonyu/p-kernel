/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0  p-kernel AArch64 ベアメタルポート（QEMU virt）
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	power_save.c
 *	省電力処理（AArch64 ベアメタルポート（QEMU virt））
 *
 *	アイドル時の省電力は dispatch.S のアイドルパス（knl_idle_wait に
 *	よる sigsuspend）で実現済みのため、ここでの追加処理はありません。
 */

#include <sys/machine.h>
#ifdef AARCH64_VIRT

#include "kernel.h"

/*
 * 省電力モードへの移行（アイドル時に呼ばれ得る）
 */
EXPORT void low_pow( void )
{
}

/*
 * サスペンドモードへの移行（tk_set_pow TPW_DOSUSPEND）
 */
EXPORT void off_pow( void )
{
}

#endif /* AARCH64_VIRT */
