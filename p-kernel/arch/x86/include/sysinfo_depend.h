/*
 *  sysinfo_depend.h (x86)
 *  System common information for x86
 */

#ifndef __SYS_SYSINFO_DEPEND_H__
#define __SYS_SYSINFO_DEPEND_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _in_asm_source_

/* Number of interrupt vectors */
#define N_INTVEC    256

/* Global kernel variables */
IMPORT  FP  knl_intvec[];      /* Interrupt vector table */
IMPORT  W   knl_taskindp;      /* Task-independent part counter */
IMPORT  UW  knl_taskmode;      /* Current task mode */

#endif /* _in_asm_source_ */

#ifdef __cplusplus
}
#endif

#endif /* __SYS_SYSINFO_DEPEND_H__ */
