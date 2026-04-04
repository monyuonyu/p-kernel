/*
 *  utk_config_depend.h (x86/QEMU)
 *  System configuration for x86 QEMU environment
 */

/* System RAM area (QEMU: 1MB - 128MB available) */
#define SYSTEMAREA_TOP      0x00200000  /* 2MB: above kernel load area */
#define SYSTEMAREA_END      0x04000000  /* 64MB */

/* User area */
#define RI_USERAREA_TOP     SYSTEMAREA_TOP
#define RI_USERINIT         NULL

/* SYSCONF */
#define CFN_TIMER_PERIOD    10          /* 10ms timer period */
#define CFN_MAX_TSKID       128
#define CFN_MAX_SEMID       48   /* Phase 9+: dproc/kdds/dtr/swim 等で大量消費するため拡張 */
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

/* Use zero-clear bss section */
#define USE_NOINIT          (0)

/* Stack sizes */
#define EXC_STACK_SIZE      0x1000  /* 4KB exception stack */
#define TMP_STACK_SIZE      0x400   /* 1KB temp stack */
#define USR_STACK_SIZE      0

#define EXCEPTION_STACK_TOP     (SYSTEMAREA_TOP)
#define TMP_STACK_TOP           (EXCEPTION_STACK_TOP - EXC_STACK_SIZE)
#define APPLICATION_STACK_TOP   (TMP_STACK_TOP - TMP_STACK_SIZE)

/* Use dynamic memory allocation */
#define USE_IMALLOC         (1)

/* Disable hook trace (simpler) */
#define USE_HOOK_TRACE      (0)

/* Use clean-up sequence */
#define USE_CLEANUP         (1)

/* Use high level programming language support */
#define USE_HLL_INTHDR      (1)

/* Use TRAP instruction for system calls - NO for x86 (use direct call) */
#define USE_TRAP            (0)

/* Use debugger support */
#define USE_DBGSPT          (0)

/* Use kernel message output */
#define USE_KERNEL_MESSAGE  (1)
