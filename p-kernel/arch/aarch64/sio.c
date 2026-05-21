/*
 *  sio.c (aarch64)
 *  PL011 UART driver — MMIO, board-selected base address
 *
 *  Same interface as x86/sio.c; called by tm_putchar / tm_getchar.
 */

#include <typedef.h>
#include <stddef.h>
#include "kernel.h"
#include "tkdev_conf.h"

/* PL011 register offsets */
#define UARTDR      0x000
#define UARTFR      0x018
#define UARTIBRD    0x024
#define UARTFBRD    0x028
#define UARTLCR_H  0x02C
#define UARTCR      0x030
#define UARTIMSC    0x038

#define FR_TXFF     (1 << 5)    /* TX FIFO full  */
#define FR_RXFE     (1 << 4)    /* RX FIFO empty */
#define FR_BUSY     (1 << 3)

static inline unsigned int pl011_read(unsigned int reg)
{
    return *((volatile unsigned int *)((unsigned long)PL011_BASE + reg));
}

static inline void pl011_write(unsigned int reg, unsigned int val)
{
    *((volatile unsigned int *)((unsigned long)PL011_BASE + reg)) = val;
}

#ifdef BOARD_RPI3
/* BCM2837 GPIO controller — needed to mux GPIO 14/15 onto PL011.
 *   GPFSEL1   @ 0x3F200004  bits[14:12]=GPIO14, bits[17:15]=GPIO15
 *                            ALT0 = 0b100 (PL011 TX / RX)
 *   GPPUD     @ 0x3F200094  pull-up/down control (0=disabled)
 *   GPPUDCLK0 @ 0x3F200098  apply control to selected pins
 *
 * QEMU raspi3b emulates the PL011 as always-on regardless of GPIO
 * mux, so the boot output already worked without this. On real
 * silicon the chip's TX line goes nowhere until the pins are switched
 * to ALT0, which is why this whole block exists.
 */
#define BCM_GPIO_BASE   0x3F200000UL
#define GPFSEL1         (BCM_GPIO_BASE + 0x04)
#define GPPUD           (BCM_GPIO_BASE + 0x94)
#define GPPUDCLK0       (BCM_GPIO_BASE + 0x98)

static inline void mmio_w32(unsigned long addr, unsigned int v)
{
    *((volatile unsigned int *)addr) = v;
}
static inline unsigned int mmio_r32(unsigned long addr)
{
    return *((volatile unsigned int *)addr);
}

static void bcm2837_gpio_pl011_setup(void)
{
    /* GPIO 14 + 15 → ALT0 (PL011 TXD/RXD). Mask out the 3-bit
     * function fields then OR in 0b100 for both pins. */
    unsigned int s = mmio_r32(GPFSEL1);
    s &= ~((7u << 12) | (7u << 15));            /* clear FSEL14, FSEL15 */
    s |=  ((4u << 12) | (4u << 15));            /* ALT0 = 0b100         */
    mmio_w32(GPFSEL1, s);

    /* Disable pull-up/down on GPIO 14/15. The BCM2835/6/7 sequence:
     *   GPPUD = 0; wait; GPPUDCLK0 = mask; wait; GPPUDCLK0 = 0.
     * 150 cycles of delay is recommended; a small busy loop is plenty. */
    mmio_w32(GPPUD, 0);
    for (volatile int i = 0; i < 150; i++) { }
    mmio_w32(GPPUDCLK0, (1u << 14) | (1u << 15));
    for (volatile int i = 0; i < 150; i++) { }
    mmio_w32(GPPUDCLK0, 0);
}
#endif /* BOARD_RPI3 */

/*
 * sio_init — initialise PL011 at 115200 8N1
 *   QEMU virt: PL011 base clock is 24 MHz; nothing else to set up.
 *   RPi 3:     same 24 MHz UART clock under config.txt core_freq=250,
 *              plus the GPIO 14/15 ALT0 mux above. config.txt also
 *              sets enable_uart=1 so the firmware doesn't shut PL011
 *              down at boot.
 */
EXPORT void sio_init(void)
{
#ifdef BOARD_RPI3
    bcm2837_gpio_pl011_setup();
#endif

    /* Disable UART so register writes below take effect. */
    pl011_write(UARTCR, 0);

    /* Wait for any in-progress transmission. Bounded — on a fault path
     * the chip may have left FR.BUSY stuck, and we'd rather drop a
     * char than hang the kernel before it even prints. */
    for (volatile int i = 0; i < 100000; i++) {
        if (!(pl011_read(UARTFR) & FR_BUSY)) break;
    }

    /* 115200 baud @ 24 MHz: IBRD=13, FBRD=1 */
    pl011_write(UARTIBRD, 13);
    pl011_write(UARTFBRD,  1);

    /* 8-bit, no parity, 1 stop, FIFO enable */
    pl011_write(UARTLCR_H, (3 << 5) | (1 << 4));

    /* Enable UART, TX, RX */
    pl011_write(UARTCR, (1 << 9) | (1 << 8) | 1);
}

/*
 * sio_send_frame — blocking transmit
 */
EXPORT void sio_send_frame(const UB *buf, INT size)
{
    for (INT i = 0; i < size; i++) {
        while (pl011_read(UARTFR) & FR_TXFF) {}
        pl011_write(UARTDR, buf[i]);
    }
}

/*
 * sio_data_ready — non-blocking RX check
 */
EXPORT BOOL sio_data_ready(void)
{
    return (pl011_read(UARTFR) & FR_RXFE) == 0;
}

/*
 * sio_recv_frame — blocking receive
 */
EXPORT void sio_recv_frame(UB *buf, INT size)
{
    for (INT i = 0; i < size; i++) {
        while (pl011_read(UARTFR) & FR_RXFE) {
            tk_dly_tsk(1);
        }
        buf[i] = (UB)(pl011_read(UARTDR) & 0xFF);
    }
}

/*
 * sio_read_line — read until \r or \n (for shell input)
 */
EXPORT INT sio_read_line(UB *buf, INT maxlen)
{
    INT n = 0;
    for (;;) {
        while (pl011_read(UARTFR) & FR_RXFE) {
            tk_dly_tsk(1);
        }
        UB c = (UB)(pl011_read(UARTDR) & 0xFF);
        if (c == '\r' || c == '\n') {
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7F || c == 0x08) { /* backspace */
            if (n > 0) n--;
            continue;
        }
        if (n < maxlen - 1)
            buf[n++] = c;
    }
}
