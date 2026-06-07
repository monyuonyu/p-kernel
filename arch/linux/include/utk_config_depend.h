/*
 *  arch/linux/include/utk_config_depend.h
 *
 *  System configuration shared by every hosted Linux port (aarch64-linux,
 *  x86_64-linux, future LP64 hosts).
 *
 *  The kernel's real backing memory is the 16 MB BSS array in cpu_init.c
 *  (knl_lowmem_top/knl_lowmem_limit). CFN_REALMEMEND below is the
 *  fallback "system area upper bound" T-Kernel internals use; the
 *  clamp in knl_init_Imalloc is `if (CFN_REALMEMEND > knl_lowmem_limit)
 *  memend = knl_lowmem_limit`, so CFN_REALMEMEND must be set HIGHER
 *  than any possible BSS address — otherwise PIE/ASLR placement of
 *  linux_heap[] at, say, 0x7f8000000000 leaves CFN_REALMEMEND lower
 *  than knl_lowmem_top, the clamp never fires, memsz wraps to garbage
 *  and the allocator faults on first use.
 *
 *  This file shadows the per-arch bare-metal utk_config_depend.h on
 *  every hosted build via the include-path order in boot/linux/Makefile
 *  and boot/linux_x86_64/Makefile.
 *
 *  The object-count limits (CFN_MAX_*) must match the bare-metal aarch64
 *  sibling byte-for-byte because the TCB struct layout — and therefore
 *  the offsets in arch/common/include/lp64/offset.h — depends on them.
 */

#ifndef _UTK_CONFIG_DEPEND_
#define _UTK_CONFIG_DEPEND_

/* SYSTEMAREA_END must be HIGHER than any heap address so the clamp in
 * knl_init_Imalloc always fires under PIE/ASLR. */
#define SYSTEMAREA_TOP      0x40200000UL
#define SYSTEMAREA_END      (~(unsigned long)0)

#define RI_USERAREA_TOP     SYSTEMAREA_TOP
#define RI_USERINIT         NULL

#define CFN_TIMER_PERIOD    10          /* 10ms */

/* Object limits — must match arch/aarch64/include/utk_config_depend.h
 * so the TCB layout (and hence lp64/offset.h) is identical. */
#define CFN_MAX_TSKID       128
#define CFN_MAX_SEMID       256  /* DNODE_MAX=32: kdds_open は handle ごとに
                                  * sem を作る。dkva だけで 2×DNODE_MAX+2≈66、
                                  * edf/moe/swim/dproc/app 分も含め余裕を確保。 */
#define CFN_MAX_FLGID       16
#define CFN_MAX_MBXID       8
#define CFN_MAX_MTXID       4
#define CFN_MAX_MBFID       8
#define CFN_MAX_PORID       4
#define CFN_MAX_MPLID       2
#define CFN_MAX_MPFID       8
#define CFN_MAX_CYCID       8
#define CFN_MAX_ALMID       8
#define CFN_MAX_SSYID       4
#define CFN_MAX_SSYPRI      16

#define CFN_MAX_REGDEV      (8)
#define CFN_MAX_OPNDEV      (16)
#define CFN_MAX_REQDEV      (16)
#define CFN_DEVT_MBFSZ0     (-1)
#define CFN_DEVT_MBFSZ1     (-1)

#define CFN_VER_MAKER       0x0000
#define CFN_VER_PRID        0
#define CFN_VER_SPVER       0x6101
#define CFN_VER_PRVER       0x0101
#define CFN_VER_PRNO1       0
#define CFN_VER_PRNO2       0
#define CFN_VER_PRNO3       0
#define CFN_VER_PRNO4       0

#define CFN_REALMEMEND      ((void *)SYSTEMAREA_END)

#define USE_NOINIT          (0)

/* Stack sizes — unused on hosted (no bare-metal start.S), kept here to
 * satisfy any T-Kernel TU that references them. */
#define EXC_STACK_SIZE      0x2000
#define TMP_STACK_SIZE      0x800
#define USR_STACK_SIZE      0

#define EXCEPTION_STACK_TOP     (SYSTEMAREA_TOP)
#define TMP_STACK_TOP           (EXCEPTION_STACK_TOP - EXC_STACK_SIZE)
#define APPLICATION_STACK_TOP   (TMP_STACK_TOP - TMP_STACK_SIZE)

#define USE_IMALLOC         (1)

#endif /* _UTK_CONFIG_DEPEND_ */
