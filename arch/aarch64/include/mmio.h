/*
 *  mmio.h (aarch64)
 *  Memory-mapped I/O accessors with volatile semantics.
 *  Drivers (PCI ECAM, GIC, PL011, RTL8139 MMIO) use these.
 */

#pragma once

static inline void mmio_write32(unsigned long addr, unsigned int val)
{
    *((volatile unsigned int *)addr) = val;
}

static inline unsigned int mmio_read32(unsigned long addr)
{
    return *((volatile unsigned int *)addr);
}

static inline void mmio_write16(unsigned long addr, unsigned short val)
{
    *((volatile unsigned short *)addr) = val;
}

static inline unsigned short mmio_read16(unsigned long addr)
{
    return *((volatile unsigned short *)addr);
}

static inline void mmio_write8(unsigned long addr, unsigned char val)
{
    *((volatile unsigned char *)addr) = val;
}

static inline unsigned char mmio_read8(unsigned long addr)
{
    return *((volatile unsigned char *)addr);
}

/* Data Synchronization Barrier — order MMIO before subsequent ops */
#define MMIO_DSB() __asm__ volatile ("dsb sy" ::: "memory")
