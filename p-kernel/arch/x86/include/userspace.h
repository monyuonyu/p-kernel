/*
 *  userspace.h (x86)
 *  Ring-3 IRET trampoline
 *
 *  user_exec() transitions the calling T-Kernel task from ring-0 to ring-3
 *  by building a privilege-change IRET frame on the current stack and
 *  issuing IRET.  It does NOT return.
 *
 *  Before calling:
 *    - gdt_init_userspace() must have been called once.
 *    - The ELF binary must have been loaded into memory at its p_vaddr.
 *    - A user stack buffer must be allocated; pass its top as ustack_top.
 */
#pragma once
#include "kernel.h"

/*
 * Switch to ring-3 user mode.
 *   entry      — virtual EIP to jump to (ELF e_entry)
 *   ustack_top — initial user ESP (top of user stack, 16-byte aligned)
 *
 * Sets TSS.RSP0 to the current kernel ESP + margin so that subsequent
 * INT 0x80 calls from ring-3 use a valid kernel stack.
 *
 * Does NOT return.
 */
void user_exec(UW entry, UW ustack_top);
