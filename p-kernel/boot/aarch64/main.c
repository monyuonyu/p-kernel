/*
 *  main.c (aarch64 / QEMU virt)
 *  Hardware init → T-Kernel startup
 *
 *  Called from arch/aarch64/start.S after stack setup and BSS clear.
 */

#include <stdint.h>
#include <stddef.h>

/* T-Kernel entry point */
extern void knl_t_kernel_main(void *inittask);
extern const void *knl_c_init_task;

/* PL011 UART init (arch/aarch64/sio.c) */
extern void sio_init(void);

static void print(const char *s)
{
    volatile unsigned int *uart = (volatile unsigned int *)0x09000000; /* UARTDR */
    volatile unsigned int *fr   = (volatile unsigned int *)0x09000018; /* UARTFR */
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

    print("[BOOT] Starting T-Kernel...\r\n");
    knl_t_kernel_main((void *)&knl_c_init_task);

    /* Never reached */
    print("[ERROR] T-Kernel returned!\r\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
