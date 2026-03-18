/*
 *  offset.h (x86 extension)
 *  Extends the common offset.h with x86-specific TCB field offsets
 */

/* Include the base offset definitions */
/* utk_config_depend.h must come first: defines CFN_MAX_* used by config.h */
#include "utk_config_depend.h"
#include "config.h"

#ifndef _OFFSET_
#define _OFFSET_

/* --- copy of common offset.h --- */

#if CFN_MAX_PORID > 0
#define TCBSZ_POR       (4)
#else
#define TCBSZ_POR       (0)
#endif

#if CFN_MAX_MTXID > 0
#define TCBSZ_MTX       (4)
#else
#define TCBSZ_MTX       (0)
#endif

#if CFN_MAX_PORID > 0
#define TCBSZ_WINFO     (16)
#else
#if CFN_MAX_FLGID > 0
#define TCBSZ_WINFO     (12)
#else
#if CFN_MAX_MBFID > 0 || CFN_MAX_MPLID > 0
#define TCBSZ_WINFO     (8)
#else
#if CFN_MAX_SEMID > 0 || CFN_MAX_MBXID > 0 || CFN_MAX_MPFID > 0
#define TCBSZ_WINFO     (4)
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

#define _ALIGN_CPU(x)   (((x)+3)&0xFFFFFFFC)
/* On x86-32, long long (LSYSTIM in TMEB) has 4-byte alignment, not 8 */
#define _ALIGN_64(x)    (((x)+3)&0xFFFFFFFC)

#define TCB_winfo       (60)
#define TCB_wtmeb       _ALIGN_64(TCB_winfo+TCBSZ_WINFO)
#define TCBsz_wtmeb2isstack (24+TCBSZ_MTX+TCBSZ_POR+TCBSZ_EXECTIME)
#define TCBSZ_GP        (0)

#define TCB_isstack     (TCB_wtmeb+TCBsz_wtmeb2isstack)
#define TCB_tskctxb     _ALIGN_CPU(TCB_isstack+4+TCBSZ_GP)

#define TCB_tskid       8
#define TCB_tskatr      16
#define TCB_task        20      /* FP task; (4th field after QUEUE+tskid+exinf+tskatr) */
#define TCB_state       35
#define CTXB_ssp        0

#endif /* _OFFSET_ */
