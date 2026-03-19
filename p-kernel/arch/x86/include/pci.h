/*
 *  pci.h (x86)
 *  PCI configuration space access
 */

#pragma once
#include "kernel.h"

#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

/* Well-known vendor/device IDs */
#define PCI_VENDOR_REALTEK  0x10EC
#define PCI_DEVICE_RTL8139  0x8139

/* PCI config space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_BAR0            0x10
#define PCI_INT_LINE        0x3C

/* PCI command register bits */
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_CMD_BUS_MASTER  0x0004

UW  pci_read32(UB bus, UB dev, UB func, UB off);
UH  pci_read16(UB bus, UB dev, UB func, UB off);
UB  pci_read8 (UB bus, UB dev, UB func, UB off);
void pci_write32(UB bus, UB dev, UB func, UB off, UW val);
void pci_write16(UB bus, UB dev, UB func, UB off, UH val);

/* Returns 1 if found; sets bus/dev/func */
INT pci_find_device(UH vendor, UH device,
                    UB *bus_out, UB *dev_out, UB *func_out);
