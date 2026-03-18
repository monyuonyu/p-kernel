/*
 *  cpu_status.h (x86)
 *  x86-dependent critical section and system state macros
 */

#ifndef _CPU_STATUS_
#define _CPU_STATUS_

#include <syslib.h>
#include <sysdef.h>
#include "cpu_insn.h"

/* -----------------------------------------------------------------------
 *  Critical section: disable interrupts, check for dispatch on exit
 * --------------------------------------------------------------------- */

#define BEGIN_CRITICAL_SECTION  { UINT _imask_ = disint();
#define END_CRITICAL_SECTION    if (!isDI(_imask_)                      \
                                  && knl_ctxtsk != knl_schedtsk         \
                                  && !knl_dispatch_disabled) {          \
                                    knl_dispatch();                     \
                                }                                       \
                                enaint(_imask_); }

/* -----------------------------------------------------------------------
 *  Interrupt disable section (no dispatch on exit)
 * --------------------------------------------------------------------- */

#define BEGIN_DISABLE_INTERRUPT { UINT _imask_ = disint();
#define END_DISABLE_INTERRUPT   enaint(_imask_); }

/* Explicit enable/disable */
#define ENABLE_INTERRUPT        { __asm__ volatile("sti" ::: "memory"); }
#define DISABLE_INTERRUPT       { __asm__ volatile("cli" ::: "memory"); }

#define ENABLE_INTERRUPT_UPTO(level) { __asm__ volatile("sti" ::: "memory"); }

/* -----------------------------------------------------------------------
 *  Task independent part transitions
 * --------------------------------------------------------------------- */

#define ENTER_TASK_INDEPENDENT  { knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT  { knl_LeaveTaskIndependent(); }

/* -----------------------------------------------------------------------
 *  System state queries
 * --------------------------------------------------------------------- */

/* TRUE when called from task-independent part or no current task */
#define in_indp()   (knl_isTaskIndependent() || knl_ctxtsk == NULL)

/* TRUE during dispatch disable (includes task-independent and CLI) */
#define in_ddsp()   (knl_dispatch_disabled || in_indp() || isDI(knl_getBASEPRI()))

/* TRUE during CPU lock (interrupt disable) */
#define in_loc()    (isDI(knl_getBASEPRI()) || in_indp())

/* TRUE during quasi-task part */
#define in_qtsk()   (knl_ctxtsk->sysmode > knl_ctxtsk->isysmode)

/* -----------------------------------------------------------------------
 *  Dispatcher interface
 * --------------------------------------------------------------------- */

/* On x86 (no PendSV), dispatch_request is a no-op.
 * Actual dispatch happens via END_CRITICAL_SECTION. */
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
