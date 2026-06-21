/*
 *  cpu_status.h (aarch64)
 *  Critical section macros using DAIF interrupt control
 */

#ifndef _CPU_STATUS_
#define _CPU_STATUS_

#include <syslib.h>
#include <sysdef.h>
#include "cpu_insn.h"

/* -----------------------------------------------------------------------
 *  ②.2a — per-CPU scheduler-state accessors (CUR_CTXTSK / CUR_SCHEDTSK /
 *  CUR_DISPATCH_DISABLED).  Defined HERE (the arch header kernel.h pulls
 *  everywhere) so they resolve wherever a critical section expands.
 *
 *  SMP OFF (default, EVERY arch): the plain globals → BYTE-IDENTICAL.
 *  SMP ON  (aarch64 -DSMP_SELFTEST): index the per-CPU SMP block
 *  g_smpcpu[smp_this_cpu()] (arch/aarch64/smp.c, the SAME storage the asm
 *  dispatcher loads).  See docs/.../smp-2-production-scheduler-plan.md §2.2.
 * --------------------------------------------------------------------- */
#ifdef SMP_SELFTEST
#include "smp_percpu.h"
#define CUR_CTXTSK            (g_smpcpu[smp_this_cpu()].ctxtsk)
#define CUR_SCHEDTSK          (g_smpcpu[smp_this_cpu()].schedtsk)
#define CUR_DISPATCH_DISABLED (g_smpcpu[smp_this_cpu()].dispatch_disabled)
/* The Big Kernel Lock (arch/aarch64/smp.c).  Under SMP, "disable interrupts"
 * is NOT the lock (it masks only the LOCAL CPU); the BKL makes "only one CPU
 * in the kernel at a time" true again.  Every critical section acquires it. */
extern void bkl_acquire(void);
extern void bkl_release(void);
#define BKL_ACQUIRE()  bkl_acquire()
#define BKL_RELEASE()  bkl_release()
#else
#define CUR_CTXTSK            (knl_ctxtsk)
#define CUR_SCHEDTSK          (knl_schedtsk)
#define CUR_DISPATCH_DISABLED (knl_dispatch_disabled)
#define BKL_ACQUIRE()  /* no-op: disint() IS the lock on a uniprocessor */
#define BKL_RELEASE()
#endif
#define _HAVE_CUR_SCHED_ACCESSORS_   /* tells task.h the accessors exist */

/* -----------------------------------------------------------------------
 *  Critical section: disable interrupts, check for dispatch on exit
 *
 *  ②.2a: under SMP, ALSO hold the BKL across the section (acquire after
 *  disint, release before enaint) so only one CPU mutates kernel state.
 *  Under no-SMP, BKL_ACQUIRE/RELEASE are empty → BYTE-IDENTICAL.
 * --------------------------------------------------------------------- */

#define BEGIN_CRITICAL_SECTION  { UINT _imask_ = disint(); BKL_ACQUIRE();
#define END_CRITICAL_SECTION    if (!isDI(_imask_)                      \
                                  && CUR_CTXTSK != CUR_SCHEDTSK         \
                                  && !CUR_DISPATCH_DISABLED             \
                                  && !knl_isTaskIndependent()) {        \
                                    knl_dispatch();                     \
                                }                                       \
                                BKL_RELEASE();                          \
                                enaint(_imask_); }

/* -----------------------------------------------------------------------
 *  Interrupt disable section (no dispatch on exit)
 * --------------------------------------------------------------------- */

#define BEGIN_DISABLE_INTERRUPT { UINT _imask_ = disint(); BKL_ACQUIRE();
#define END_DISABLE_INTERRUPT   BKL_RELEASE(); enaint(_imask_); }

#define ENABLE_INTERRUPT        { __asm__ volatile("msr daifclr, #0x3" ::: "memory"); }
#define DISABLE_INTERRUPT       { __asm__ volatile("msr daifset, #0x3" ::: "memory"); }

#define ENABLE_INTERRUPT_UPTO(level) ENABLE_INTERRUPT

/* -----------------------------------------------------------------------
 *  Task independent part transitions
 * --------------------------------------------------------------------- */

#define ENTER_TASK_INDEPENDENT  { knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT  { knl_LeaveTaskIndependent(); }

/* -----------------------------------------------------------------------
 *  System state queries
 * --------------------------------------------------------------------- */

#define in_indp()   (knl_isTaskIndependent() || CUR_CTXTSK == NULL)
#define in_ddsp()   (CUR_DISPATCH_DISABLED || in_indp() || isDI(knl_getBASEPRI()))
#define in_loc()    (isDI(knl_getBASEPRI()) || in_indp())
#define in_qtsk()   (CUR_CTXTSK->sysmode > CUR_CTXTSK->isysmode)

/* -----------------------------------------------------------------------
 *  Dispatcher interface (dispatch_request is no-op; actual dispatch
 *  happens at END_CRITICAL_SECTION, matching x86 behavior)
 * --------------------------------------------------------------------- */

#define knl_dispatch_request()  /* no-op */

IMPORT void knl_force_dispatch(void);
IMPORT void knl_dispatch(void);

/* -----------------------------------------------------------------------
 *  Task context block
 *  Only the system stack pointer is saved; all other state is on the stack.
 * --------------------------------------------------------------------- */

typedef struct {
    void *ssp;  /* System stack pointer */
} CTXB;

#endif /* _CPU_STATUS_ */
