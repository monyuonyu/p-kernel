---
name: feedback-lp64-allocator-trap
description: "T-Kernel's allocator (memory.c / memory.h) packs the AREA_USE flag into a QUEUE* low bit via `(UW)ptr`. UW is 32-bit; on LP64 this truncates pointer addresses."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

T-Kernel's `setAreaFlag(q, f)`, `clrAreaFlag(q, f)`, `chkAreaFlag(q, f)`, and `FreeSize(fq)` in `include/kernel/tkernel/memory.h` all cast `QUEUE *` to `UW` (32-bit unsigned int) to do bitwise ops on the low bit (AREA_USE flag). On LP64 platforms — i.e. AArch64-Linux, x86_64-Linux, AArch64 bare-metal at any address ≥ 4 GB — those casts **silently truncate the upper 32 bits** of the pointer.

**Why:** Use `**: when porting T-Kernel to a hosted Linux build, knl_Imalloc enters an infinite loop / corrupts the freeque because the next/prev links in `QUEUE` get rebuilt from truncated values. Symptom in Session 3b: tk_cre_tsk_impl → knl_Imalloc → knl_searchFreeArea hangs at the first call after all module inits succeed.

**How to apply:** Before declaring T-Kernel's bare-metal AArch64 port done, verify it works at high addresses (mmap a 16 MB region near 0x4_0000_0000 and point knl_lowmem_top into it). The bug is latent there too — current bare-metal builds escape because `_kernel_end` is in low memory.

**The fix is global:** change all `(UW)q`, `(UW)(q)->prev`, `(UW)pointer` casts in `memory.h` and `memory.c` to `(uintptr_t)` (or a project alias). That ripples through `setAreaFlag`, `clrAreaFlag`, `chkAreaFlag`, `FreeSize`, `knl_searchFreeArea`, `knl_appendFreeArea`, and `knl_removeFreeQue`. Probably ~30 call sites; mechanical but touches the allocator core.

**Related:** the UW/W typedef trap ([[feedback-lp64-typedef-trap]]) was a similar latent LP64 problem in T-Kernel's struct sizes. The allocator's pointer-in-UW trick is in the same family — anywhere T-Kernel stores a pointer in `UW` is suspect on LP64.