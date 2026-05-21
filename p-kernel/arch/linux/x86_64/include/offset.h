/*
 *  arch/linux/x86_64/include/offset.h
 *
 *  TCB field offsets for x86_64-linux.
 *
 *  These match the AArch64 LP64 offsets byte-for-byte: both ABIs use
 *  8-byte pointers, 8-byte longs, and the same natural alignment for
 *  the {UB, UH, UW, void*} primitives that make up the TCB struct.
 *  The numbers below are the same ones the aarch64 sibling uses, and
 *  any change to them must be applied to both files (or extracted via
 *  offsetof in a one-off debug build).
 */

#include "utk_config_depend.h"
#include "config.h"

#ifndef _OFFSET_
#define _OFFSET_

#if CFN_MAX_PORID > 0
#define TCBSZ_POR       (8)
#else
#define TCBSZ_POR       (0)
#endif

#if CFN_MAX_MTXID > 0
#define TCBSZ_MTX       (8)
#else
#define TCBSZ_MTX       (0)
#endif

#if CFN_MAX_PORID > 0
#define TCBSZ_WINFO     (32)
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

#define _ALIGN_CPU(x)   (((x)+7)&~7UL)
#define _ALIGN_64(x)    (((x)+7)&~7UL)

#define TCBSZ_GP        (0)

#define TCB_tskid       16
#define TCB_exinf       24
#define TCB_tskatr      32
#define TCB_task        40
#define TCB_sstksz      48
#define TCB_state       59
#define TCB_winfo       104
#define TCB_wtmeb       136
#define TCB_isstack     184
#define TCB_tskctxb     192
#define CTXB_ssp        0
#define TCB_SSP         192

#endif /* _OFFSET_ */
