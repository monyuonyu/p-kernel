/*
 *  arch/linux/x86_64/include/cpu_status.h
 *  T-Kernel critical section macros — userspace flavour, x86_64-linux.
 *
 *  Shadows arch/x86/include/cpu_status.h via the Linux build's include
 *  path ordering. Identical semantics to the aarch64 sibling — the
 *  only thing host-arch about this header is the comment.
 */

#ifndef _CPU_STATUS_
#define _CPU_STATUS_

#include <syslib.h>
#include <sysdef.h>
#include "cpu_insn.h"

#define BEGIN_CRITICAL_SECTION  { UINT _imask_ = disint();
#define END_CRITICAL_SECTION    if (!isDI(_imask_)                      \
                                  && knl_ctxtsk != knl_schedtsk         \
                                  && !knl_dispatch_disabled             \
                                  && !knl_isTaskIndependent()) {        \
                                    knl_dispatch();                     \
                                }                                       \
                                enaint(_imask_); }

#define BEGIN_DISABLE_INTERRUPT { UINT _imask_ = disint();
#define END_DISABLE_INTERRUPT   enaint(_imask_); }

/*
 * Inline (no asm) IRQ control. The flag lives in preempt.c. Real signal
 * masking happens lazily: arch_irq_disable just sets the flag; the
 * SIGALRM handler checks it on entry and defers if set. No sigprocmask
 * syscall in this path.
 */
IMPORT volatile int arch_irq_disabled_flag;
IMPORT void arch_irq_enable_with_drain(void);

#define DISABLE_INTERRUPT   do { arch_irq_disabled_flag = 1; } while (0)
#define ENABLE_INTERRUPT    do { arch_irq_enable_with_drain(); } while (0)
#define ENABLE_INTERRUPT_UPTO(level) ENABLE_INTERRUPT

#define ENTER_TASK_INDEPENDENT  { knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT  { knl_LeaveTaskIndependent(); }

#define in_indp()   (knl_isTaskIndependent() || knl_ctxtsk == NULL)
#define in_ddsp()   (knl_dispatch_disabled || in_indp() || isDI(knl_getBASEPRI()))
#define in_loc()    (isDI(knl_getBASEPRI()) || in_indp())
#define in_qtsk()   (knl_ctxtsk->sysmode > knl_ctxtsk->isysmode)

#define knl_dispatch_request()  /* no-op; dispatch happens at END_CRITICAL_SECTION */

IMPORT void knl_force_dispatch(void);
IMPORT void knl_dispatch(void);

/*
 * CTXB layout matches the aarch64 sibling: just a saved stack pointer.
 * The dormant 64-byte frame lives on the task's own stack (see
 * cpu_task.h). offset.h's TCB_SSP=192 stays valid because both LP64
 * ABIs lay out the surrounding TCB identically.
 */
typedef struct {
    void *ssp;
} CTXB;

#endif /* _CPU_STATUS_ */
