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

#ifdef BOARD_RPI3

/* -----------------------------------------------------------------------
 *  BCM2837 ARM Local Interrupt Controller — base 0x40000000.
 *
 *  It is NOT a GIC. There is no distributor or CPU interface and no
 *  EOI register; this block is a per-core routing matrix that decides
 *  whether each per-core source (generic timer, mailbox, local PMU)
 *  goes to IRQ or FIQ. The timer condition itself is cleared by
 *  writing CNTP_TVAL_EL0, so the IRQ vector has nothing to ack.
 *
 *  Register map (offsets from 0x40000000):
 *    0x40 + 4*n   CORE_n_TIMER_INTCTL — bit 1 = nCNTPNSIRQ → IRQ
 *    0x60 + 4*n   CORE_n_IRQ_SOURCE   — read-only, bit 1 set when
 *                                       the EL1 phys timer has fired
 * --------------------------------------------------------------------- */
#  define BCM2837_LOCAL_BASE        0x40000000UL
#  define LOCAL_CORE0_TIMER_INTCTL  (BCM2837_LOCAL_BASE + 0x40)
#  define LOCAL_CORE0_IRQ_SOURCE    (BCM2837_LOCAL_BASE + 0x60)
#  define LOCAL_TIMER_NCNTPNSIRQ    (1U << 1)

EXPORT void gic_enable_irq(UINT intid)
{
    /* Only the EL1 generic timer (PPI 30) is wired through the ARM
     * local controller in our build. Anything else is silently a
     * no-op until the matching Phase-3 driver lands. */
    if (intid == INTNO_TIMER_GIC) {
        mmio_write32(LOCAL_CORE0_TIMER_INTCTL, LOCAL_TIMER_NCNTPNSIRQ);
        DSB();
    }
}

static void gic_init(void)
{
    /* Make sure no stale routing is in effect before we enable our
     * one interrupt. CORE0_TIMER_INTCTL = 0 → all timer sources
     * disabled. The CPU interface in cpu_support.S takes the IRQ
     * source register address through a fixed-MMIO read (no base
     * pointer needed). */
    mmio_write32(LOCAL_CORE0_TIMER_INTCTL, 0);
    DSB();
}

#else

/* -----------------------------------------------------------------------
 *  Enable a single GIC interrupt by INTID. (QEMU virt / GICv2.)
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

#endif /* BOARD_RPI3 */

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
    /* On QEMU virt: GICv2 distributor + CPU interface.
     * On RPi 3:     BCM2837 ARM Local Interrupt Controller.
     * Both reach the same handler at INTID 30 (EL1 phys timer PPI). */
    gic_init();
    knl_define_inthdr(INTNO_TIMER_GIC, (FP)timer_irq_handler);
    gic_enable_irq(INTNO_TIMER_GIC);
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
