/*
 *  pci.c (x86)
 *  PCI configuration space access via I/O ports 0xCF8/0xCFC
 */

#include "pci.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Raw 32-bit I/O (not in cpu_insn.h)                                  */
/* ------------------------------------------------------------------ */

static inline void outl(UH port, UW val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline UW inl(UH port)
{
    UW ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ------------------------------------------------------------------ */
/* PCI config space                                                     */
/* ------------------------------------------------------------------ */

static UW pci_addr(UB bus, UB dev, UB func, UB off)
{
    return ((UW)1 << 31)
         | ((UW)bus  << 16)
         | ((UW)(dev  & 0x1F) << 11)
         | ((UW)(func & 0x07) << 8)
         | (off & 0xFC);
}

UW pci_read32(UB bus, UB dev, UB func, UB off)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

UH pci_read16(UB bus, UB dev, UB func, UB off)
{
    UW v = pci_read32(bus, dev, func, (UB)(off & ~1));
    return (UH)((v >> ((off & 2) * 8)) & 0xFFFF);
}

UB pci_read8(UB bus, UB dev, UB func, UB off)
{
    UW v = pci_read32(bus, dev, func, (UB)(off & ~3));
    return (UB)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(UB bus, UB dev, UB func, UB off, UW val)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(UB bus, UB dev, UB func, UB off, UH val)
{
    UW cur = pci_read32(bus, dev, func, (UB)(off & ~3));
    INT shift = (off & 2) * 8;
    cur = (cur & ~((UW)0xFFFF << shift)) | ((UW)val << shift);
    pci_write32(bus, dev, func, (UB)(off & ~3), cur);
}

INT pci_find_device(UH vendor, UH device,
                    UB *bus_out, UB *dev_out, UB *func_out)
{
    for (UW bus = 0; bus < 256; bus++) {
        for (UW dev = 0; dev < 32; dev++) {
            UW id = pci_read32((UB)bus, (UB)dev, 0, PCI_VENDOR_ID);
            if (id == 0xFFFFFFFF) continue;
            if ((UH)(id & 0xFFFF)        == vendor &&
                (UH)((id >> 16) & 0xFFFF) == device) {
                *bus_out  = (UB)bus;
                *dev_out  = (UB)dev;
                *func_out = 0;
                return 1;
            }
        }
    }
    return 0;
}
