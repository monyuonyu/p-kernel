/*
 *  boot/linux/main.c
 *  Entry point for the Linux-hosted p-kernel build.
 *
 *  The Linux process loader starts us at main() with a normal stack
 *  and BSS already cleared, so we skip the assembly bring-up that
 *  boot/aarch64/main.c relies on (start.S, BSS clear, stack switch).
 *  Initialise the serial console, then hand control to T-Kernel.
 */

#include "kernel.h"
#include "task.h"

extern const void *knl_c_init_task;
extern void sio_init(void);

__attribute__((visibility("default")))
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    sio_init();

    /* Match the QEMU virt / RPi 3 boot banner so any tooling that
     * greps for it (e.g. our /verify or /run scripts) still works.
     * Bypass tm_putstring here because T-Kernel isn't running yet. */
    {
        extern void sio_send_frame(const UB *buf, INT size);
        #define BANNER(s) sio_send_frame((const UB *)(s), (INT)sizeof(s) - 1)
        BANNER("=== p-kernel linux boot ===\r\n");
        BANNER("[INIT] termios stdin/stdout\r\n");
        BANNER("[BOOT] Starting T-Kernel...\r\n");
        #undef BANNER
    }

    knl_t_kernel_main((void *)&knl_c_init_task);

    /* Should never reach here — T-Kernel runs forever in usermain. */
    return 0;
}
