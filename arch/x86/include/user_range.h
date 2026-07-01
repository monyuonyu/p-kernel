/*
 *  user_range.h (x86)
 *  ISO-USERPTR — kernel-range guard for user-supplied pointers
 *  (gap-ledger ISO-USERPTR 🟠 / docs/review-2026-06-13-external-audit.md §1)
 *
 *  Pointer-taking syscalls and the ELF loader dereference USER-supplied
 *  addresses on the caller's behalf.  Without a kernel-range check a ring-3
 *  task can hand the kernel a pointer into KERNEL memory and the kernel will
 *  read/write it (confused deputy).  Today this is masked only by the flat
 *  identity map (VA==PA); it is a real isolation hole, the more so now that
 *  the AI core math runs in ring3/EL0 (docs/architecture/50-evolution/ring3-core.md) and
 *  user pointers genuinely cross the privilege boundary.
 *
 *  The ONLY ring-3-accessible virtual ranges are the two PD spans that
 *  paging_proc_create() marks U/S=1 (arch/x86/paging.c):
 *
 *    Region A — p-kernel native ELFs (user.ld @ 0x400000):
 *      PD[2..7]   0x00400000 – 0x00FFFFFF   (12 MB)  code/BSS/heap/stack
 *    Region B — Linux-standard ELFs (musl/glibc @ 0x08048000):
 *      PD[64..71] 0x08000000 – 0x08FFFFFF   (16 MB)  text/data/heap/TLS/stack
 *
 *  A legitimate user pointer (stack buffer, loaded segment, brk heap, TLS
 *  block) lies fully within one of these two spans.  Any pointer outside
 *  them — into kernel BSS, the page tables, MMIO, or an unmapped hole — is
 *  rejected.  We require [ptr, ptr+len) to fit ENTIRELY inside a SINGLE
 *  region (a buffer may not straddle the gap between A and B).
 */
#pragma once
#include "kernel.h"

/* Region A — native ELF span (PD[2..7]); half-open [lo, hi). */
#define USER_RA_LO   0x00400000UL
#define USER_RA_HI   0x01000000UL   /* end of PD[7] = 0x00FFFFFF + 1 */

/* Region B — Linux ELF span (PD[64..71]); half-open [lo, hi). */
#define USER_RB_LO   0x08000000UL
#define USER_RB_HI   0x09000000UL   /* end of PD[71] = 0x08FFFFFF + 1 */

/*
 * user_range_ok(ptr, len)
 *   Returns 1 IFF the byte range [ptr, ptr+len) lies fully within ONE
 *   ring-3-accessible region AND does not overflow the address space.
 *   Returns 0 otherwise.
 *
 *   - len == 0: accepted only if ptr itself is inside a region (a zero-byte
 *     access touches nothing, but a kernel-range base is still rejected so
 *     callers cannot smuggle a kernel pointer through a len==0 path).
 *   - Overflow: ptr + len is computed in UW; if it wraps below ptr the range
 *     is rejected.  (Using the half-open upper bound, end == region_hi is OK;
 *     end > region_hi is not.)
 */
static inline int user_range_ok(const void *ptr, UW len)
{
    UW lo = (UW)ptr;
    UW end = lo + len;
    if (end < lo) return 0;                       /* wrap / overflow */
    if (lo >= USER_RA_LO && end <= USER_RA_HI) return 1;
    if (lo >= USER_RB_LO && end <= USER_RB_HI) return 1;
    return 0;
}
