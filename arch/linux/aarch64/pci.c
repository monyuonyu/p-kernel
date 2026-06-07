/*
 *  arch/linux/aarch64/pci.c
 *  PCI is meaningless inside a Linux user-space process. Provide
 *  empty stubs matching arch/common/include/pci.h.
 */

#include "kernel.h"
#include "pci.h"

UW   pci_read32(UB bus, UB dev, UB func, UB off)                { (void)bus; (void)dev; (void)func; (void)off; return 0xFFFFFFFFu; }
UH   pci_read16(UB bus, UB dev, UB func, UB off)                { (void)bus; (void)dev; (void)func; (void)off; return 0xFFFFu; }
UB   pci_read8 (UB bus, UB dev, UB func, UB off)                { (void)bus; (void)dev; (void)func; (void)off; return 0xFFu; }
void pci_write32(UB bus, UB dev, UB func, UB off, UW val)       { (void)bus; (void)dev; (void)func; (void)off; (void)val; }
void pci_write16(UB bus, UB dev, UB func, UB off, UH val)       { (void)bus; (void)dev; (void)func; (void)off; (void)val; }

INT pci_find_device(UH vendor, UH device,
                    UB *bus_out, UB *dev_out, UB *func_out)
{
    (void)vendor; (void)device;
    (void)bus_out; (void)dev_out; (void)func_out;
    return 0;   /* not found */
}
