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
 *  Critical section: disable interrupts, check for dispatch on exit
 * --------------------------------------------------------------------- */

#define BEGIN_CRITICAL_SECTION  { UINT _imask_ = disint();
#define END_CRITICAL_SECTION    if (!isDI(_imask_)                      \
                                  && knl_ctxtsk != knl_schedtsk         \
                                  && !knl_dispatch_disabled             \
                                  && !knl_isTaskIndependent()) {        \
                                    knl_dispatch();                     \
                                }                                       \
                                enaint(_imask_); }

/* -----------------------------------------------------------------------
 *  Interrupt disable section (no dispatch on exit)
 * --------------------------------------------------------------------- */

#define BEGIN_DISABLE_INTERRUPT { UINT _imask_ = disint();
#define END_DISABLE_INTERRUPT   enaint(_imask_); }

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

#define in_indp()   (knl_isTaskIndependent() || knl_ctxtsk == NULL)
#define in_ddsp()   (knl_dispatch_disabled || in_indp() || isDI(knl_getBASEPRI()))
#define in_loc()    (isDI(knl_getBASEPRI()) || in_indp())
#define in_qtsk()   (knl_ctxtsk->sysmode > knl_ctxtsk->isysmode)

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
