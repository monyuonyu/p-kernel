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
 *	kernel.h
 *	micro T-Kernel Common Definition
 */

#ifndef _KERNEL_
#define _KERNEL_

#include <basic.h>
#include <tk_typedef.h>
#include <errno.h>
#include <syscall.h>
#include <dbgspt.h>
#include <imalloc.h>
#include <queue.h>
#include <libstr.h>

#include "utk_config.h"

#include "isyscall.h"

#define SYSCALL		EXPORT		/* Definition of system call */

/*
 * Kernel configuration file
 */
#include "config.h"
#include "cpu_conf.h"
#include "tkdev_conf.h"

#ifndef __tcb__
#define __tcb__
typedef struct task_control_block	TCB;
#endif

/*
 * Object lock
 *	Locked task is the highest run priority.
 *	Unable to nest lock.
 */
typedef struct objlock {
	QUEUE		wtskq;		/* Wait task queue */
} OBJLOCK;

void knl_InitOBJLOCK( OBJLOCK *loc )
{
	loc->wtskq.next = NULL;
}
IMPORT void knl_LockOBJ( OBJLOCK* );
IMPORT void knl_UnlockOBJ( OBJLOCK* );

BOOL knl_isLockedOBJ( OBJLOCK *loc )
{
	return ( loc->wtskq.next != NULL )? TRUE: FALSE;
}


/*
 * CPU-dependent include file
 */
#include "cpu_status.h"

/*
 * System initialization (each module)
 */
IMPORT ER knl_task_initialize( void );
IMPORT ER knl_semaphore_initialize( void );
IMPORT ER knl_eventflag_initialize( void );
IMPORT ER knl_mailbox_initialize( void );
IMPORT ER knl_messagebuffer_initialize( void );
IMPORT ER knl_rendezvous_initialize( void );
IMPORT ER knl_mutex_initialize( void );
IMPORT ER knl_memorypool_initialize( void );
IMPORT ER knl_fix_memorypool_initialize( void );
IMPORT ER knl_cyclichandler_initialize( void );
IMPORT ER knl_alarmhandler_initialize( void );
IMPORT ER knl_subsystem_initialize( void );
IMPORT ER knl_devicemanager_initialize( void );

/*
 * Start/Exit system (tkstart.c)
 */
IMPORT void knl_t_kernel_main( T_CTSK* );
IMPORT void knl_t_kernel_exit( void );

/*
 * Target system-dependent routine (cpu_init.c tkdev_init.c)
 */
IMPORT ER   knl_cpu_initialize( void );
IMPORT void knl_cpu_shutdown( void );
IMPORT ER   knl_tkdev_initialize( void );
IMPORT void knl_tkdev_exit( void );

/*
 * Mutex
 */
IMPORT void knl_signal_all_mutex( TCB *tcb );
IMPORT INT knl_chg_pri_mutex( TCB *tcb, INT priority );

#endif /* _KERNEL_ */
