/*
 *  arch/linux/x86_64/include/machine_depend.h
 *
 *  Machine flags for the hosted x86_64-linux port.
 *
 *  Shadows the bare-metal aarch64 version that would otherwise be pulled
 *  in from arch/aarch64/include/ — that one sets _APP_AARCH64_=1 which
 *  would silently leak the wrong arch ifdef into the x86_64 build.
 *  _APP_X86_64_ is already passed via -D from the Makefile.
 */

#ifndef __SYS_MACHINE_DEPEND_H__
#define __SYS_MACHINE_DEPEND_H__

#define ALLOW_MISALIGN      1   /* x86_64 allows unaligned loads */
#define BIGENDIAN           0
#define VIRTUAL_ADDRESS     0   /* hosted, no MMU management here */
#define INT_BITWIDTH        64

#endif /* __SYS_MACHINE_DEPEND_H__ */
