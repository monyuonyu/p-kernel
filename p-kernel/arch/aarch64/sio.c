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

/*
 * sio_init — initialise PL011 at 115200 8N1
 *   QEMU virt's PL011 works without explicit init (clocked at 24MHz).
 *   RPi3 needs BCM2837 clock setup first; we do minimal init here.
 */
EXPORT void sio_init(void)
{
    /* Disable UART */
    pl011_write(UARTCR, 0);

    /* Wait for any in-progress transmission */
    while (pl011_read(UARTFR) & FR_BUSY) {}

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
