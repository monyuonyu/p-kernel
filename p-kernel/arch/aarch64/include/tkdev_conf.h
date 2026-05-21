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

/* RTL8139's IRQ is no longer a build-time constant — rtl8139_init()
 * computes it dynamically from PCI dev + INT_PIN, since QEMU virt may
 * place the chip at different slots and the legacy PCI INTx swizzle
 * rotates the SPI assignment per device. */

/* Max interrupt vector slots */
#define N_INTVEC        512

#endif /* _TKDEV_CONF_ */
