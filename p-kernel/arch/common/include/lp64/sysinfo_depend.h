/*
 *  sysinfo_depend.h (aarch64)
 */

#ifndef __SYS_SYSINFO_DEPEND_H__
#define __SYS_SYSINFO_DEPEND_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _in_asm_source_

#define N_INTVEC    512

IMPORT  FP  knl_intvec[];
IMPORT  W   knl_taskindp;
IMPORT  UW  knl_taskmode;

#endif /* _in_asm_source_ */

#ifdef __cplusplus
}
#endif

#endif /* __SYS_SYSINFO_DEPEND_H__ */
