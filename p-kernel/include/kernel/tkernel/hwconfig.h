/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/*
 *	hardware configuration (MB9AF312K)
 */

#ifndef _HWCONFIG_H_
#define _HWCONFIG_H_

#define USE_TMONITOR 0

#define INTERNAL_RAM_SIZE       0x00004000
#define INTERNAL_RAM_START      0x1FFFE000

#define INTERNAL_RAM_END        (INTERNAL_RAM_START+INTERNAL_RAM_SIZE)

#define FLASH_BASE      0x00000000
#define SRAM_BASE       0x1FFFE000

#define FLASH_SIZE      0x00020000		/* 128KB */

#define FLASH_START     FLASH_BASE
#define FLASH_END       (FLASH_START+FLASH_SIZE)

#define SRAM_SIZE       0x0004000

#define SRAM_START      SRAM_BASE
#define SRAM_END        (SRAM_START+SRAM_SIZE)

#endif /* _CONFIG_H_ */
