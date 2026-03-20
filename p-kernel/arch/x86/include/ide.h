/*
 *  ide.h (x86)
 *  ATA PIO mode driver — primary bus only (0x1F0)
 */
#pragma once
#include "kernel.h"

/* I/O port base */
#define IDE_PRIMARY_BASE    0x1F0
#define IDE_PRIMARY_CTRL    0x3F6

/* Register offsets from base */
#define IDE_REG_DATA        0   /* R/W: 16-bit data port           */
#define IDE_REG_ERROR       1   /* R:   error register             */
#define IDE_REG_FEATURES    1   /* W:   features                   */
#define IDE_REG_SECCOUNT    2   /* R/W: sector count               */
#define IDE_REG_LBA0        3   /* R/W: LBA bits  7:0              */
#define IDE_REG_LBA1        4   /* R/W: LBA bits 15:8              */
#define IDE_REG_LBA2        5   /* R/W: LBA bits 23:16             */
#define IDE_REG_DRIVE       6   /* R/W: drive select + LBA bits 27:24 */
#define IDE_REG_STATUS      7   /* R:   status                     */
#define IDE_REG_COMMAND     7   /* W:   command                    */

/* Status bits */
#define IDE_STATUS_ERR      0x01
#define IDE_STATUS_DRQ      0x08
#define IDE_STATUS_SRV      0x10
#define IDE_STATUS_DF       0x20
#define IDE_STATUS_RDY      0x40
#define IDE_STATUS_BSY      0x80

/* Commands */
#define IDE_CMD_READ_PIO    0x20    /* Read sectors with retry  */
#define IDE_CMD_WRITE_PIO   0x30    /* Write sectors with retry */
#define IDE_CMD_FLUSH_CACHE 0xE7    /* Flush write cache        */
#define IDE_CMD_IDENTIFY    0xEC

/* Sector size */
#define IDE_SECTOR_SIZE     512

/* Detect and initialise primary master drive.
 * Returns 0 if a drive is present, -1 if not found. */
INT  ide_init(void);

/* Read `count` sectors from LBA `lba` into `buf`.
 * buf must be at least count * 512 bytes.
 * Returns 0 on success, -1 on error. */
INT  ide_read(UW lba, UW count, void *buf);

/* Write `count` sectors from `buf` to LBA `lba`.
 * Returns 0 on success, -1 on error. */
INT  ide_write(UW lba, UW count, const void *buf);

/* Number of sectors on the drive (from IDENTIFY). */
UW   ide_sector_count(void);

/* TRUE if a drive was detected at init. */
extern BOOL ide_present;
