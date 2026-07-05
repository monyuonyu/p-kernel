/*
 *  compat/sys/mman.h — native Windows shim for the read-only file mmap
 *  the GGUF loader (arch/common/llm/gguf.c) uses. Reached via -idirafter
 *  (mingw lacks <sys/mman.h>). The real mmap/munmap implementations live
 *  in arch/windows/x86_64/win_mmap.c (Win32 file mapping, windows.h
 *  isolated there). Only PROT_READ / MAP_PRIVATE at offset 0 are used.
 */

#ifndef PKERNEL_COMPAT_SYS_MMAN_H
#define PKERNEL_COMPAT_SYS_MMAN_H

#include <stddef.h>

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define PROT_NONE   0x0

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED  ((void *)-1)

/* win_mmap.c */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap(void *addr, size_t length);

#endif /* PKERNEL_COMPAT_SYS_MMAN_H */
