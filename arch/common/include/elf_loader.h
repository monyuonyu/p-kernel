/*
 *  elf_loader.h (x86)
 *  ELF32 static executable loader
 *
 *  Loads an ELF32 binary from the VFS into memory and launches it
 *  as a ring-3 user-mode task.
 *
 *  Supported:  ET_EXEC, EM_386, PT_LOAD segments, static linking only.
 *  User code is loaded at the virtual address specified in the ELF
 *  (typically USER_CODE_BASE = 0x10000000).
 */
#pragma once
#include "kernel.h"

/*
 * Load and execute an ELF32 binary from the VFS.
 *   path    — VFS path of the ELF file
 *   cmdline — full command line string (e.g. "tcc -nostdlib foo.c")
 *             used to build argc/argv on the user stack; may be NULL
 *             for native p-kernel ELFs (argc=0).
 * Returns task ID (≥ 1) on success, or negative error code on failure.
 */
ID elf_exec(const char *path, const char *cmdline);

/*
 * Tear down a user process's kernel-side resources from ANOTHER
 * task's context (the killer's): subsystem/fd cleanup, shell-relay
 * exit-sem, per-process page tables (bare-metal x86), per-task FPU
 * image.  Call AFTER tk_ter_tsk(tid) and BEFORE tk_del_tsk(tid).
 * Implemented for real in arch/x86/syscall.c; hosted and aarch64
 * targets (no ring-3 ELF processes) provide a no-op stub next to
 * their elf_exec stub in vfs_stub.c.
 */
void user_proc_teardown(ID tid);
