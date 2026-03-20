/*
 *  ide.c (x86)
 *  ATA PIO mode driver — primary master (CS0, 0x1F0)
 *
 *  Supports 28-bit LBA, single-sector and multi-sector reads.
 *  QEMU emulates a standard ATA PIO device on -drive if=ide.
 */

#include "kernel.h"
#include "ide.h"
#include <tmonitor.h>

BOOL ide_present = FALSE;
static UW  ide_nsectors = 0;

/* --------------------------------------------------------------- */
/* Port helpers                                                     */
/* --------------------------------------------------------------- */

/* outb() and inb() are defined in cpu_insn.h (included via kernel.h) */

static inline UH inw(UH port)
    { UH v; asm volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }

static inline void io_wait(void)
    { inb(0x80); }   /* dummy write to port 80 ≈ ~1 µs delay */

/* --------------------------------------------------------------- */
/* Poll helpers                                                     */
/* --------------------------------------------------------------- */

/* Wait until BSY clears (max ~30 s in real hw; we use a loop count). */
static INT ide_wait_not_busy(void)
{
    for (INT i = 0; i < 100000; i++) {
        UB st = inb(IDE_PRIMARY_BASE + IDE_REG_STATUS);
        if (!(st & IDE_STATUS_BSY)) return 0;
        io_wait();
    }
    return -1;  /* timeout */
}

/* Wait until DRQ set (data ready to transfer). */
static INT ide_wait_drq(void)
{
    for (INT i = 0; i < 100000; i++) {
        UB st = inb(IDE_PRIMARY_BASE + IDE_REG_STATUS);
        if (st & IDE_STATUS_ERR) return -1;
        if (st & IDE_STATUS_DRQ) return 0;
        io_wait();
    }
    return -1;
}

/* --------------------------------------------------------------- */
/* Init                                                             */
/* --------------------------------------------------------------- */

INT ide_init(void)
{
    /* Select master drive (0xA0) before touching the bus */
    outb(IDE_PRIMARY_BASE + IDE_REG_DRIVE, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait();

    /* Check for floating bus: STATUS = 0xFF → no controller / no drive */
    UB st0 = inb(IDE_PRIMARY_BASE + IDE_REG_STATUS);
    if (st0 == 0xFF) {
        tm_putstring((UB *)"[ide]  no drive (floating bus)\r\n");
        return -1;
    }

    /* Software reset via control register */
    outb(IDE_PRIMARY_CTRL, 0x04);   /* SRST bit */
    io_wait(); io_wait();
    outb(IDE_PRIMARY_CTRL, 0x00);   /* clear reset */

    /* Wait up to ~500 ms for BSY to clear after reset */
    if (ide_wait_not_busy() < 0) {
        tm_putstring((UB *)"[ide]  no drive (BSY timeout)\r\n");
        return -1;
    }

    /* Re-select master after reset */
    outb(IDE_PRIMARY_BASE + IDE_REG_DRIVE, 0xA0);
    io_wait(); io_wait();

    /* Confirm STATUS is non-zero and non-FF (drive present) */
    UB st1 = inb(IDE_PRIMARY_BASE + IDE_REG_STATUS);
    if (st1 == 0x00 || st1 == 0xFF) {
        tm_putstring((UB *)"[ide]  no drive (status=0)\r\n");
        return -1;
    }

    /* IDENTIFY command */
    outb(IDE_PRIMARY_BASE + IDE_REG_DRIVE,    0xA0); /* master, LBA mode */
    outb(IDE_PRIMARY_BASE + IDE_REG_SECCOUNT, 0);
    outb(IDE_PRIMARY_BASE + IDE_REG_LBA0,     0);
    outb(IDE_PRIMARY_BASE + IDE_REG_LBA1,     0);
    outb(IDE_PRIMARY_BASE + IDE_REG_LBA2,     0);
    outb(IDE_PRIMARY_BASE + IDE_REG_COMMAND,  IDE_CMD_IDENTIFY);

    UB st = inb(IDE_PRIMARY_BASE + IDE_REG_STATUS);
    if (st == 0) {
        tm_putstring((UB *)"[ide]  no drive (status=0)\r\n");
        return -1;
    }

    if (ide_wait_not_busy() < 0 || ide_wait_drq() < 0) {
        tm_putstring((UB *)"[ide]  IDENTIFY failed\r\n");
        return -1;
    }

    /* Read 256 words of IDENTIFY data */
    UH ident[256];
    for (INT i = 0; i < 256; i++)
        ident[i] = inw(IDE_PRIMARY_BASE + IDE_REG_DATA);

    /* Words 60-61: 28-bit LBA sector count */
    ide_nsectors = ((UW)ident[61] << 16) | ident[60];

    ide_present = TRUE;

    tm_putstring((UB *)"[ide]  ATA drive ready  sectors=");
    /* print sector count */
    char buf[12]; INT bi = 11; buf[bi] = '\0';
    UW v = ide_nsectors;
    if (v == 0) { buf[--bi] = '0'; }
    else { while (v > 0 && bi > 0) { buf[--bi] = (char)('0' + v%10); v /= 10; } }
    tm_putstring((UB *)(buf+bi));
    tm_putstring((UB *)"\r\n");

    return 0;
}

/* --------------------------------------------------------------- */
/* Read sectors                                                     */
/* --------------------------------------------------------------- */

INT ide_read(UW lba, UW count, void *buf)
{
    if (!ide_present) return -1;
    if (count == 0)   return 0;

    UH *p = (UH *)buf;

    while (count > 0) {
        UW batch = (count > 255) ? 255 : count;

        if (ide_wait_not_busy() < 0) return -1;

        /* LBA 28-bit mode, master drive */
        outb(IDE_PRIMARY_BASE + IDE_REG_DRIVE,
             (UB)(0xE0 | ((lba >> 24) & 0x0F)));
        outb(IDE_PRIMARY_BASE + IDE_REG_SECCOUNT, (UB)batch);
        outb(IDE_PRIMARY_BASE + IDE_REG_LBA0, (UB)(lba));
        outb(IDE_PRIMARY_BASE + IDE_REG_LBA1, (UB)(lba >> 8));
        outb(IDE_PRIMARY_BASE + IDE_REG_LBA2, (UB)(lba >> 16));
        outb(IDE_PRIMARY_BASE + IDE_REG_COMMAND, IDE_CMD_READ_PIO);

        for (UW s = 0; s < batch; s++) {
            if (ide_wait_drq() < 0) return -1;
            /* Read 256 words (512 bytes) */
            for (INT w = 0; w < 256; w++)
                *p++ = inw(IDE_PRIMARY_BASE + IDE_REG_DATA);
        }

        lba   += batch;
        count -= batch;
    }
    return 0;
}

UW ide_sector_count(void) { return ide_nsectors; }
