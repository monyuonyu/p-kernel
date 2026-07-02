/*
 *  cpu_insn.h (aarch64)
 *  Interrupt control, memory barriers, task-independent helpers
 */

#ifndef _CPU_INSN_
#define _CPU_INSN_

#include <sysinfo.h>
#include "sysdef_depend.h"
#include "tkdev_conf.h"

/* -----------------------------------------------------------------------
 *  Interrupt enable / disable via DAIF
 *  disint() reads DAIF and masks IRQ+FIQ; enaint() restores the I bit.
 * --------------------------------------------------------------------- */

static inline UINT disint(void)
{
    UINT daif;
    __asm__ volatile (
        "mrs %0, daif\n\t"
        "msr daifset, #0x3"     /* set I (bit1) + F (bit0) → mask IRQ+FIQ */
        : "=r"(daif) : : "memory"
    );
    return daif;
}

static inline void enaint(UINT daif)
{
    /* DAIF register bit 7 = I (IRQ masked when 1) */
    if (!(daif & DAIF_I)) {
        __asm__ volatile ("msr daifclr, #0x3" : : : "memory");
    }
}

/* Returns TRUE when IRQ is masked in saved daif value */
#define isDI(daif)  (((daif) & DAIF_I) != 0)

static inline UINT knl_getBASEPRI(void)
{
    UINT daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));
    return daif;
}

/* -----------------------------------------------------------------------
 *  Interrupt vector table
 * --------------------------------------------------------------------- */

IMPORT FP knl_intvec[N_INTVEC];

/* p-kernel 変更: μT-Kernel 3.0 と同じ 3 引数シグネチャに統一
 * （intatr は未使用。2.0/3.0 両ビルドから同じ呼び出しにするため） */
static inline void knl_define_inthdr(INT vecno, ATR intatr, FP inthdr)
{
    (void)intatr;
    if ((UINT)vecno < N_INTVEC) {
        knl_intvec[vecno] = inthdr;
    }
}

/* -----------------------------------------------------------------------
 *  Task independent part counter
 * --------------------------------------------------------------------- */

IMPORT W knl_taskindp;

static inline BOOL knl_isTaskIndependent(void)
{
    return (knl_taskindp > 0) ? TRUE : FALSE;
}

static inline void knl_EnterTaskIndependent(void)
{
    knl_taskindp++;
}

static inline void knl_LeaveTaskIndependent(void)
{
    knl_taskindp--;
}

/* -----------------------------------------------------------------------
 *  Memory barriers
 * --------------------------------------------------------------------- */

#define DSB()   __asm__ volatile ("dsb sy" ::: "memory")
#define ISB()   __asm__ volatile ("isb"    ::: "memory")
#define DMB()   __asm__ volatile ("dmb sy" ::: "memory")

#endif /* _CPU_INSN_ */
