/*
 *  pci.c (aarch64)
 *  PCI configuration space access via PCIe ECAM (Enhanced Configuration
 *  Access Mechanism). QEMU 'virt' maps the PCIe config region at
 *  0x4010000000; each (bus,dev,func) gets a 4 KB window.
 *
 *  ECAM address: BASE + (bus << 20) | (dev << 15) | (func << 12) | offset
 *  Bus range:    0..255, but QEMU virt typically populates only bus 0.
 *
 *  Provides the same public API as arch/x86/pci.c so the upper layers
 *  (netstack, rtl8139 init, etc.) compile unchanged.
 */

#include "pci.h"
#include "kernel.h"
#include "mmio.h"

/* QEMU virt PCIe ECAM base. RPi 3 has no PCIe — when we later port to
 * RPi 4 the base would be 0x600000000 (announced in the DTB). */
#define ECAM_BASE  0x4010000000UL

/* Scan limits — virt exposes bus 0 device 0..1F. Limit bus scan to keep
 * boot fast; bridges (if present) would push us higher. */
#define ECAM_MAX_BUS  1

static inline unsigned long ecam_addr(UB bus, UB dev, UB func, UB off)
{
    return ECAM_BASE
         | ((unsigned long)bus  << 20)
         | ((unsigned long)(dev  & 0x1F) << 15)
         | ((unsigned long)(func & 0x07) << 12)
         | (off & 0xFFF);
}

UW pci_read32(UB bus, UB dev, UB func, UB off)
{
    return mmio_read32(ecam_addr(bus, dev, func, (UB)(off & ~3)));
}

UH pci_read16(UB bus, UB dev, UB func, UB off)
{
    UW v = pci_read32(bus, dev, func, (UB)(off & ~3));
    return (UH)((v >> ((off & 2) * 8)) & 0xFFFF);
}

UB pci_read8(UB bus, UB dev, UB func, UB off)
{
    UW v = pci_read32(bus, dev, func, (UB)(off & ~3));
    return (UB)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(UB bus, UB dev, UB func, UB off, UW val)
{
    mmio_write32(ecam_addr(bus, dev, func, (UB)(off & ~3)), val);
    MMIO_DSB();
}

void pci_write16(UB bus, UB dev, UB func, UB off, UH val)
{
    UW cur   = pci_read32(bus, dev, func, (UB)(off & ~3));
    INT shift = (off & 2) * 8;
    cur = (cur & ~((UW)0xFFFF << shift)) | ((UW)val << shift);
    pci_write32(bus, dev, func, (UB)(off & ~3), cur);
}

INT pci_find_device(UH vendor, UH device,
                    UB *bus_out, UB *dev_out, UB *func_out)
{
    for (UW bus = 0; bus < ECAM_MAX_BUS; bus++) {
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
