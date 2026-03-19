/*
 *  sio.c (x86)
 *  Serial I/O for T-Monitor on x86 (COM1 UART)
 */

#include <typedef.h>
#include <stddef.h>
#include "kernel.h"

#define COM1_DATA   0x3F8
#define COM1_LSR    (0x3F8 + 5)
#define LSR_THRE    0x20    /* Transmitter holding register empty */
#define LSR_DR      0x01    /* Data ready */

static inline void _outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char _inb(unsigned short port)
{
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * sio_send_frame - send bytes to COM1
 */
EXPORT void sio_send_frame(const UB *buf, INT size)
{
    for (INT i = 0; i < size; i++) {
        while ((_inb(COM1_LSR) & LSR_THRE) == 0) {}
        _outb(COM1_DATA, buf[i]);
    }
}

/*
 * sio_recv_frame - receive bytes from COM1 (blocking, yields to other tasks)
 */
EXPORT void sio_recv_frame(UB *buf, INT size)
{
    for (INT i = 0; i < size; i++) {
        while ((_inb(COM1_LSR) & LSR_DR) == 0) {
            tk_dly_tsk(1);  /* yield CPU so net_task can run */
        }
        buf[i] = _inb(COM1_DATA);
    }
}
