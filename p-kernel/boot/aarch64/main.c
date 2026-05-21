/*
 *  main.c (aarch64 / QEMU virt)
 *  Hardware init → T-Kernel startup
 *
 *  Called from arch/aarch64/start.S after stack setup and BSS clear.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel.h"
#include "task.h"
#include "offset.h"

/* T-Kernel entry point declared in kernel.h-via-includes; no local proto */
extern const void *knl_c_init_task;

/* PL011 UART init (arch/aarch64/sio.c) */
extern void sio_init(void);

/* PL011 base differs between QEMU virt and BCM2837 — keep this print
 * helper minimal so it works before sio_init() runs. */
#ifdef BOARD_RPI3
#  define PRINT_UART_BASE   0x3F201000UL    /* BCM2837 PL011 (UART0) */
#else
#  define PRINT_UART_BASE   0x09000000UL    /* QEMU virt PL011       */
#endif
static void print(const char *s)
{
    volatile unsigned int *uart = (volatile unsigned int *)(PRINT_UART_BASE + 0x00); /* UARTDR */
    volatile unsigned int *fr   = (volatile unsigned int *)(PRINT_UART_BASE + 0x18); /* UARTFR */
    for (; *s; s++) {
        while (*fr & (1 << 5)) {}   /* wait TX FIFO not full */
        *uart = (unsigned int)(unsigned char)*s;
    }
}

void main(void)
{
    /* UART init — must be first for debug output */
    sio_init();

    print("=== p-kernel aarch64 boot ===\r\n");
    print("[INIT] UART\r\n");

    /* Compile-time guard: keep TCB_SSP/TCB_tskctxb in offset.h in sync
     * with the actual TCB layout. If a TCB field's type ever changes
     * width (e.g. another typedef shift), the kernel boots into a
     * silent sync abort via knl_dispatch — much friendlier to fail at
     * link time. */
    _Static_assert(offsetof(TCB, tskctxb) == TCB_tskctxb,
                   "offset.h TCB_tskctxb out of sync with task.h");
    _Static_assert(sizeof(TCB) <= 256,
                   "TCB grew past tk_cre_tsk's assumed ceiling");

    print("[BOOT] Starting T-Kernel...\r\n");
    knl_t_kernel_main((void *)&knl_c_init_task);

    /* Never reached */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
