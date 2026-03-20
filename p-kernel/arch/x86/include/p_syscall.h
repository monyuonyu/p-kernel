/*
 *  p_syscall.h (x86)
 *  Syscall interface via INT 0x80 (Linux-compatible numbers)
 *
 *  Ring-3 calling convention (cdecl-like via registers):
 *    EAX = syscall number
 *    EBX = arg0
 *    ECX = arg1
 *    EDX = arg2
 *    Return value in EAX.
 *
 *  Implemented syscalls:
 *    SYS_EXIT  (1)  — terminate current user process
 *    SYS_WRITE (4)  — write to fd (fd=1/2 → serial output)
 */
#pragma once
#include "kernel.h"

#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4

/* Register IDT gate 0x80 (DPL=3, callable from ring3). */
void syscall_init(void);

/* C-level syscall dispatcher (called from syscall_isr in isr.S). */
W    syscall_dispatch(W nr, W arg0, W arg1, W arg2);
