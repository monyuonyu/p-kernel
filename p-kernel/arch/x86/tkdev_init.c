/*
 *  tkdev_init.c (x86)
 *  T-Kernel device-dependent initialization for x86/QEMU
 *
 *  Connects PIT IRQ0 (timer) to knl_timer_handler_startup().
 *  PIC and PIT hardware are initialized by boot/x86 (pic.c, timer.c);
 *  here we register the T-Kernel handler in the IRQ dispatch table.
 */

#include "kernel.h"
#include "cpu_insn.h"
#include "sysdef_depend.h"
#include "tkdev_conf.h"

IMPORT void knl_timer_handler_startup(void);

/* x86 IRQ dispatch table - defined in boot/x86/idt.c */
IMPORT void (*x86_irq_handlers[16])(void);

/*
 * knl_tkdev_initialize
 *   Called from knl_t_kernel_main() as InitModule(tkdev).
 *   Install T-Kernel timer handler for IRQ0.
 */
EXPORT ER knl_tkdev_initialize(void)
{
    /* Register PIT timer handler (IRQ0) */
    x86_irq_handlers[0] = knl_timer_handler_startup;

    /* Unmask PIT IRQ0 */
    {
        UB mask = inb(0x21);
        outb(0x21, (UB)(mask & ~0x01));
    }

    return E_OK;
}

#if USE_CLEANUP
/*
 * knl_tkdev_exit
 *   Disable timer and halt.
 */
EXPORT void knl_tkdev_exit(void)
{
    /* Mask all IRQs */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* Halt */
    __asm__ volatile("cli; hlt");
    for (;;) {}
}
#endif /* USE_CLEANUP */
