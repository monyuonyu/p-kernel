/*
 *  userspace.c (x86)
 *  Ring-3 IRET trampoline
 *
 *  Builds a privilege-change IRET frame and switches from ring-0 to ring-3.
 *
 *  IRET frame layout for ring0 → ring3 (32-bit compat mode, CS=0x08):
 *
 *     [esp+ 0]  EIP     ← user entry point
 *     [esp+ 4]  CS      ← USER_CS (0x23)
 *     [esp+ 8]  EFLAGS  ← IF=1 (interrupts enabled)
 *     [esp+12]  ESP     ← user stack top
 *     [esp+16]  SS      ← USER_DS (0x2B)
 *
 *  This is a 32-bit IRET because we run in 32-bit compatibility mode
 *  (CS=0x08, D=1, L=0) even though the CPU is in IA-32e long mode.
 */

#include "kernel.h"
#include "task.h"
#include "userspace.h"
#include "gdt_user.h"
#include <tmonitor.h>

void user_exec(UW entry, UW ustack_top)
{
    /*
     * Set TSS.RSP0 to the TOP of this task's kernel stack.
     * On INT 0x80 or IRQ from ring-3, the CPU loads RSP0 and pushes the
     * user context (SS, ESP, EFLAGS, CS, EIP) there.  Using isstack
     * (the task's initial stack pointer = top of the 8 KB kernel stack)
     * gives the full 8 KB to ring-0 handlers and avoids any overflow.
     */
    gdt_set_kernel_stack((UW)knl_ctxtsk->isstack);

    /*
     * Transition to ring-3:
     *   1. Load user data selectors into DS/ES/FS/GS (via EAX).
     *   2. Push IRET frame: SS, ESP, EFLAGS (with IF set), CS, EIP.
     *   3. IRET — CPU sees CPL change (ring0→ring3) and:
     *        - Reloads SS:ESP from the top two words of the frame.
     *        - Continues at CS:EIP in ring-3.
     *
     * Input constraints:
     *   %0 = USER_DS (pushed as SS, also loaded into segment registers)
     *   %1 = ustack_top (user ESP)
     *   %2 = USER_CS
     *   %3 = entry (user EIP)
     */
    asm volatile(
        /* Load user data selectors (segment regs require 16-bit source) */
        "movl %0, %%eax         \n"
        "movw %%ax, %%ds        \n"
        "movw %%ax, %%es        \n"
        "movw %%ax, %%fs        \n"
        "movw %%ax, %%gs        \n"
        /* Build IRET frame (grows toward lower addresses) */
        "pushl %0               \n"   /* SS      = USER_DS */
        "pushl %1               \n"   /* ESP     = ustack_top */
        "pushf                  \n"   /* EFLAGS */
        "orl  $0x200, (%%esp)   \n"   /* IF=1: ring-3でも割り込みを有効化 */
        "pushl %2               \n"   /* CS      = USER_CS */
        "pushl %3               \n"   /* EIP     = entry */
        "iret                   \n"   /* switch to ring-3 */
        :
        : "r"((UW)USER_DS), "r"(ustack_top),
          "r"((UW)USER_CS), "r"(entry)
        : "memory", "eax"
    );

    /* NOTREACHED — iret does not return */
    __builtin_unreachable();
}
