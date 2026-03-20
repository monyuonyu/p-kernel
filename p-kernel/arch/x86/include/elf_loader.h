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
 * Load and execute an ELF32 binary from the VFS path.
 * Creates a ring-3 T-Kernel task; returns its task ID (≥ 1) on success,
 * or negative error code on failure.
 */
ID elf_exec(const char *path);
