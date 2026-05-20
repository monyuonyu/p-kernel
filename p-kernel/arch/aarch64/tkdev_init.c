/*
 *  tkdev_init.c (aarch64)
 *  GICv2 + ARM Generic Timer setup for QEMU virt / RPi3
 *
 *  Timer: EL1 physical timer (PPI 30) → knl_timer_handler_startup()
 *  GIC:   minimal init — distributor enable, CPU interface enable,
 *         priority mask open, timer PPI unmasked.
 */

#include "kernel.h"
#include "cpu_insn.h"
#include "sysdef_depend.h"
#include "tkdev_conf.h"
#include "task.h"

IMPORT void knl_timer_handler_startup(void);
IMPORT void *gicc_base_ptr;     /* defined in cpu_support.S */

/* MMIO helpers */
static inline void mmio_write32(unsigned long addr, unsigned int val)
{
    *((volatile unsigned int *)addr) = val;
}
static inline unsigned int mmio_read32(unsigned long addr)
{
    return *((volatile unsigned int *)addr);
}

/* -----------------------------------------------------------------------
 *  Enable a single GIC interrupt by INTID.
 *  Works for PPIs (id 16..31) and SPIs (id 32..N). Used by drivers
 *  (e.g. RTL8139 wiring its PCIe legacy IRQ to a GIC SPI) without
 *  exposing GICD register layout to every caller.
 * --------------------------------------------------------------------- */
EXPORT void gic_enable_irq(UINT intid)
{
    UINT word = intid >> 5;          /* /32 */
    UINT bit  = intid & 31;          /* %32 */
    mmio_write32(GICD_BASE + GICD_ISENABLER + word * 4, 1U << bit);
    DSB();
}

/* -----------------------------------------------------------------------
 *  GICv2 initialisation
 *  Distributor: enable group 0; unmask timer PPI (id=30).
 *  CPU interface: enable; set priority mask to 0xFF (all pass).
 * --------------------------------------------------------------------- */
static void gic_init(void)
{
    /* Expose GICC base to the IRQ handler in cpu_support.S */
    *(unsigned long *)&gicc_base_ptr = GICC_BASE;

    /* Distributor enable */
    mmio_write32(GICD_BASE + GICD_CTLR, 1);

    /* Enable timer PPI (id=30) */
    gic_enable_irq(INTNO_TIMER_GIC);

    /* CPU interface: priority mask 0xFF = allow all, then enable */
    mmio_write32(GICC_BASE + GICC_PMR,  0xFF);
    mmio_write32(GICC_BASE + GICC_CTLR, 1);

    DSB();
}

/* -----------------------------------------------------------------------
 *  ARM Generic Timer: program interval and start counting
 *  CNTFRQ_EL0 holds the frequency (QEMU: 62.5 MHz; RPi3: 19.2 MHz).
 * --------------------------------------------------------------------- */
static void timer_init(void)
{
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));

    /* interval = freq / TIMER_HZ */
    unsigned long interval = freq / TIMER_HZ;

    /* Load countdown value */
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));

    /* Enable timer, unmask interrupt */
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"((unsigned long)1));

    DSB();
    ISB();
}

/* -----------------------------------------------------------------------
 *  Timer interrupt handler — reload and call T-Kernel handler
 * --------------------------------------------------------------------- */
static void timer_irq_handler(void)
{
    /* Reload the countdown */
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long interval = freq / TIMER_HZ;
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
    DSB();

    knl_timer_handler_startup();
}

/* -----------------------------------------------------------------------
 *  knl_tkdev_initialize
 *    Called from knl_t_kernel_main() as InitModule(tkdev).
 * --------------------------------------------------------------------- */
EXPORT ER knl_tkdev_initialize(void)
{
    gic_init();
    knl_define_inthdr(INTNO_TIMER_GIC, (FP)timer_irq_handler);
    timer_init();
    return E_OK;
}

#if USE_CLEANUP
EXPORT void knl_tkdev_exit(void)
{
    /* Disable timer */
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));
    /* Mask all exceptions */
    __asm__ volatile ("msr daifset, #0xF");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
#endif

#ifdef USE_FUNC_TK_DIS_DSP
SYSCALL ER tk_dis_dsp(void)
{
    knl_dispatch_disabled = DDS_DISABLE;
    return E_OK;
}
#endif

#ifdef USE_FUNC_TK_ENA_DSP
SYSCALL ER tk_ena_dsp(void)
{
    knl_dispatch_disabled = DDS_ENABLE;
    if (knl_ctxtsk != knl_schedtsk) {
        knl_dispatch();
    }
    return E_OK;
}
#endif
