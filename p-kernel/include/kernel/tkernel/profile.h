/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */

/*
 *	@(#)profile.h (sys)
 *
 *	Service Profile
 */

#ifndef __SYS_PROFILE_H__
#define __SYS_PROFILE_H__

#include <profile_common.h>

#if !(TK_SUPPORT_USERBUF || TK_SUPPORT_AUTOBUF)
# error "Either TK_SUPPORT_USERBUF or TK_SUPPORT_AUTOBUF MUST be defined as TRUE."
#endif

#if TK_MAX_TSKPRI < 16
# error "TK_MAX_TSKPRI MUST be greater than or equal to 16."
#endif

#if TK_WAKEUP_MAXCNT < 1
# error "TK_WAKEUP_MAXCNT MUST be greater than or equal to 1."
#endif

#if TK_SEMAPHORE_MAXCNT < 32767
# error "TK_WAKEUP_MAXCNT MUST be greater than or equal to 32767."
#endif

#if TK_SUSPEND_MAXCNT < 32767
# error "TK_WAKEUP_MAXCNT MUST be greater than or equal to 32767."
#endif

#if !(TK_MEM_RNG0 >= 0 && TK_MEM_RNG0 <= 3)
# error "TK_MEM_RNG0 has an invalid value."
#endif

#if !(TK_MEM_RNG1 >= 0 && TK_MEM_RNG1 <= 3 && TK_MEM_RNG0 <= TK_MEM_RNG1)
# error "TK_MEM_RNG1 has an invalid value."
#endif

#if !(TK_MEM_RNG2 >= 0 && TK_MEM_RNG2 <= 3 && TK_MEM_RNG1 <= TK_MEM_RNG2)
# error "TK_MEM_RNG1 has an invalid value."
#endif

#if !(TK_MEM_RNG3 >= 0 && TK_MEM_RNG3 <= 3 && TK_MEM_RNG2 <= TK_MEM_RNG3)
# error "TK_MEM_RNG1 has an invalid value."
#endif

#endif /* __SYS_PROFILE_H__ */
