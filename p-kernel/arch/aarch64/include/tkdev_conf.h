/*
 *  tkdev_conf.h (aarch64)
 *  Hardware addresses: board-selected at build time via -DBOARD_xxx
 */

#ifndef _TKDEV_CONF_
#define _TKDEV_CONF_

/* Timer frequency */
#define TIMER_HZ        100     /* 100 Hz = 10 ms period */

/* PL011 UART base — board-dependent */
#if defined(BOARD_RPI3)
#  define PL011_BASE    0x3F201000UL
#elif defined(BOARD_RPI4)
#  define PL011_BASE    0xFE201000UL
#elif defined(BOARD_RPI5)
#  define PL011_BASE    0x107D001000UL
#else
#  define PL011_BASE    0x09000000UL    /* QEMU virt */
#endif

/* GICv2 base addresses (QEMU virt; RPi3 uses a different GIC layout) */
#if defined(BOARD_RPI3)
#  define GICD_BASE     0x40041000UL
#  define GICC_BASE     0x40042000UL
#else
#  define GICD_BASE     0x08000000UL    /* QEMU virt GICv2 distributor */
#  define GICC_BASE     0x08010000UL    /* QEMU virt GICv2 CPU interface */
#endif

/* GICv2 register offsets */
#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100
#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00C
#define GICC_EOIR       0x010

/* ARM Generic Timer: EL1 physical timer PPI 30 */
#define INTNO_TIMER_GIC     30

/* RTL8139 over QEMU virt PCIe legacy IRQ.
 * QEMU virt routes PCIe slots 0..3 INTA pin to GIC SPIs 3..6
 * (= INTIDs 35..38). Slot 0 INTA → INTID 35. Other boards (e.g. real
 * hardware with UEFI) must re-derive this from PCI_INT_PIN/PCI_INT_LINE. */
#define INTNO_RTL8139_GIC   35

/* Max interrupt vector slots */
#define N_INTVEC        512

#endif /* _TKDEV_CONF_ */
