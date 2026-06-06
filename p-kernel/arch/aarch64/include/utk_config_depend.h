/*
 *  utk_config_depend.h (aarch64)
 *  System configuration — board-dependent RAM layout selected by
 *  BOARD_RPI3, defaults to QEMU virt.
 */

#ifndef _UTK_CONFIG_DEPEND_
#define _UTK_CONFIG_DEPEND_

#if defined(BOARD_RPI3)
/* BCM2837: ARM cores see RAM from 0x00000000 to 0x3F000000 (1 GB minus
 * the peripheral aperture). Firmware drops the AArch64 kernel at
 * 0x80000, so the kernel + stack live below ~0x100000 and the rest is
 * free for I-malloc and task stacks. */
#  define SYSTEMAREA_TOP    0x00200000UL    /* 2MB: above kernel + stack */
#  define SYSTEMAREA_END    0x3F000000UL    /* ceiling — peripheral base */
#else
/* QEMU virt: loads at 0x40000000, 256 MB visible RAM (-m 256M). */
#  define SYSTEMAREA_TOP    0x40200000UL    /* 2MB above kernel load */
#  define SYSTEMAREA_END    0x50000000UL    /* 256MB ceiling */
#endif

#define RI_USERAREA_TOP     SYSTEMAREA_TOP
#define RI_USERINIT         NULL

/* Timer */
#define CFN_TIMER_PERIOD    10          /* 10ms */

/* Object limits — keep same as x86 for compatibility */
#define CFN_MAX_TSKID       128
#define CFN_MAX_SEMID       256  /* DNODE_MAX=32: kdds_open は handle ごとに
                                  * sem を作る。dkva だけで 2×DNODE_MAX+2≈66 必要。
                                  * linux config と一致させること (TCB layout)。 */
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

/* Stack sizes */
#define EXC_STACK_SIZE      0x2000  /* 8KB exception stack */
#define TMP_STACK_SIZE      0x800
#define USR_STACK_SIZE      0

#define EXCEPTION_STACK_TOP     (SYSTEMAREA_TOP)
#define TMP_STACK_TOP           (EXCEPTION_STACK_TOP - EXC_STACK_SIZE)
#define APPLICATION_STACK_TOP   (TMP_STACK_TOP - TMP_STACK_SIZE)

#define USE_IMALLOC         (1)

#endif /* _UTK_CONFIG_DEPEND_ */
