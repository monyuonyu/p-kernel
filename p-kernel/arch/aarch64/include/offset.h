/*
 *  offset.h (aarch64)
 *  TCB field offsets for AArch64 (64-bit pointers, computed via offsetof).
 *
 *  QUEUE(16) + tskid(4) + pad(4) + exinf(8) + tskatr(4) + pad(4) + task(8)
 *  → TCB_task = 40
 *
 *  tskctxb is the last significant field before optional name[].
 *  → TCB_tskctxb = 168, CTXB_ssp = 0  → TCB_SSP = 168
 */

#include "utk_config_depend.h"
#include "config.h"

#ifndef _OFFSET_
#define _OFFSET_

#if CFN_MAX_PORID > 0
#define TCBSZ_POR       (8)     /* 64-bit: pointer-size slot */
#else
#define TCBSZ_POR       (0)
#endif

#if CFN_MAX_MTXID > 0
#define TCBSZ_MTX       (8)     /* 64-bit: pointer */
#else
#define TCBSZ_MTX       (0)
#endif

#if CFN_MAX_PORID > 0
#define TCBSZ_WINFO     (32)    /* RNO + 3 union variants, 64-bit aligned */
#else
#if CFN_MAX_FLGID > 0
#define TCBSZ_WINFO     (16)
#else
#if CFN_MAX_MBFID > 0 || CFN_MAX_MPLID > 0
#define TCBSZ_WINFO     (16)
#else
#if CFN_MAX_SEMID > 0 || CFN_MAX_MBXID > 0 || CFN_MAX_MPFID > 0
#define TCBSZ_WINFO     (8)
#else
#define TCBSZ_WINFO     (0)
#endif
#endif
#endif
#endif

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
#define TCBSZ_EXECTIME  (8)
#else
#define TCBSZ_EXECTIME  (0)
#endif

/* Alignment macros for AArch64 (8-byte natural alignment) */
#define _ALIGN_CPU(x)   (((x)+7)&~7UL)
#define _ALIGN_64(x)    (((x)+7)&~7UL)

#define TCBSZ_GP        (0)

/* --- Fixed offsets extracted from real T-Kernel headers via offsetof().
 *
 *  Do NOT hand-calculate these; the bitfield slots, struct padding, and
 *  WINFO union alignment all conspire to make manual computation wrong
 *  (we found this the hard way — initial guess of 168 → dispatcher loaded
 *  SSP=0 → ret to garbage → fault at PC=0x3000000).
 *
 *  Verification: see arch/aarch64/include/offset_check.c (build-time check)
 *  Result on AArch64 with CFN_MAX_PORID > 0 and CFN_MAX_MTXID > 0:
 *      TCB_task    = 40
 *      TCB_isstack = 192
 *      TCB_tskctxb = 200      ← key one for the dispatcher
 *      sizeof(TCB) = 216
 */
#define TCB_tskid       16
#define TCB_exinf       24
#define TCB_tskatr      32
#define TCB_task        40
#define TCB_sstksz      48
#define TCB_state       59
#define TCB_winfo       104
#define TCB_wtmeb       136
#define TCB_isstack     192
#define TCB_tskctxb     200
#define CTXB_ssp        0
#define TCB_SSP         200     /* TCB_tskctxb + CTXB_ssp             */

/* winfo/wtmeb offsets — not needed in assembler, computed by C compiler */

#endif /* _OFFSET_ */
