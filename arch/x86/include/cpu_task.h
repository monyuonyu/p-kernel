/*
 *  cpu_task.h (x86)
 *  x86 task context and startup
 *
 *  Stack layout for a dormant/switched-out task (low → high):
 *
 *    ssp → [ ebx  ]  saved callee-saved registers
 *           [ esi  ]
 *           [ edi  ]
 *           [ ebp  ]
 *           [taskmode]
 *           [&knl_task_entry_trampoline]  ← "return address" used by dispatch
 *           [&tk_ext_tsk / exit stub]     ← return addr for the task function
 *           [ stacd ]                     ← 1st arg  (filled by setup_stacd)
 *           [ exinf ]                     ← 2nd arg  (filled by setup_stacd)
 *
 *  When the dispatcher switches to a new task:
 *    pop ebx, esi, edi, ebp
 *    pop taskmode (to global)
 *    ret  →  jumps to knl_task_entry_trampoline
 *
 *  knl_task_entry_trampoline then calls tcb->task(stacd, exinf).
 *  (The stack already has [exit_stub][stacd][exinf] so the task sees
 *   its arguments via the normal C calling convention.)
 */

#ifndef _CPU_TASK_
#define _CPU_TASK_

#include "cpu_insn.h"

/* Size of the context frame written by setup_context (excluding arg area).
 * = ebx + esi + edi + ebp + taskmode + trampoline_addr = 6 * 4 bytes */
#define DORMANT_STACK_SIZE  (sizeof(VW) * 6)

/* Forward declarations of assembly routines */
IMPORT void knl_task_entry_trampoline(void);

/*
 * knl_setup_context - build initial dormant stack frame
 *   Called from knl_make_dormant().
 */
Inline void knl_setup_context(TCB *tcb)
{
    UW *ssp = (UW *)tcb->isstack;

    /* Push argument area placeholders (stacd, exinf) - filled by setup_stacd */
    *(--ssp) = 0;               /* exinf */
    *(--ssp) = 0;               /* stacd */

    /* Push exit stub as the task function's return address */
    *(--ssp) = (UW)tk_ext_tsk;

    /* Push trampoline as the dispatcher's "return address" */
    *(--ssp) = (UW)knl_task_entry_trampoline;

    /* Push saved register frame (all zeros) */
    *(--ssp) = 0;               /* taskmode */
    *(--ssp) = 0;               /* ebp */
    *(--ssp) = 0;               /* edi */
    *(--ssp) = 0;               /* esi */
    *(--ssp) = 0;               /* ebx */

    tcb->tskctxb.ssp = ssp;
}

/*
 * knl_setup_stacd - set stacd and exinf in the task's stack frame
 *   Called from tk_sta_tsk().
 */
Inline void knl_setup_stacd(TCB *tcb, INT stacd)
{
    /* The stack layout from ssp (low) to high:
     *   ebx, esi, edi, ebp, taskmode, &trampoline, &exit_stub, stacd, exinf
     *   indices:  0    1    2    3       4              5          6     7   8
     * stacd is at offset 7*4, exinf at 8*4 from ssp */
    UW *ssp = (UW *)tcb->tskctxb.ssp;
    ssp[7] = (UW)stacd;
    ssp[8] = (UW)tcb->exinf;
}

/*
 * knl_cleanup_context - release any per-task context resources
 */
Inline void knl_cleanup_context(TCB *tcb)
{
    (void)tcb;
}

#endif /* _CPU_TASK_ */
