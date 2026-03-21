/*
 *  p_syscall.h (x86)
 *  Syscall interface via INT 0x80
 *
 *  Ring-3 calling convention:
 *    EAX = syscall number
 *    EBX = arg0,  ECX = arg1,  EDX = arg2
 *    Return value in EAX.
 *
 *  Syscall table:
 *    1      SYS_EXIT      — terminate current user process
 *    3      SYS_READ      — read fd (0=stdin, 3+=file)
 *    4      SYS_WRITE     — write fd (1/2=serial, 3+=file)
 *    5      SYS_OPEN      — open file, returns fd>=3
 *    6      SYS_CLOSE     — close fd
 *    7      SYS_LSEEK     — seek within fd
 *    8      SYS_MKDIR     — create directory
 *    9      SYS_UNLINK    — delete file
 *    10     SYS_RENAME    — rename/move file
 *
 *  T-Kernel native (p-kernel extension, 0x100+):
 *    0x100  SYS_TK_CRE_TSK  — create task (PK_CRE_TSK*)
 *    0x101  SYS_TK_STA_TSK  — start task
 *    0x102  SYS_TK_EXT_TSK  — exit self
 *    0x103  SYS_TK_SLP_TSK  — sleep (ms)
 *    0x104  SYS_TK_WUP_TSK  — wakeup task
 *    0x105  SYS_TK_CHG_PRI  — change priority
 *    0x106  SYS_TK_CHG_SLT  — change time slice
 *    0x107  SYS_TK_REF_TSK  — reference task info
 *    0x110  SYS_TK_CRE_SEM  — create semaphore
 *    0x111  SYS_TK_DEL_SEM  — delete semaphore
 *    0x112  SYS_TK_SIG_SEM  — signal semaphore
 *    0x113  SYS_TK_WAI_SEM  — wait semaphore
 *    0x120  SYS_TK_CRE_FLG  — create event flag
 *    0x121  SYS_TK_DEL_FLG  — delete event flag
 *    0x122  SYS_TK_SET_FLG  — set event flag bits
 *    0x123  SYS_TK_CLR_FLG  — clear event flag bits
 *    0x124  SYS_TK_WAI_FLG  — wait event flag (args via struct*)
 */
#pragma once
#include "kernel.h"

/* ----------------------------------------------------------------- */
/* POSIX-compatible syscall numbers                                   */
/* ----------------------------------------------------------------- */
#define SYS_EXIT        1
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_LSEEK       7
#define SYS_MKDIR       8
#define SYS_UNLINK      9
#define SYS_RENAME      10

/* ----------------------------------------------------------------- */
/* T-Kernel native syscall numbers (p-kernel extension)              */
/* ----------------------------------------------------------------- */
#define SYS_TK_CRE_TSK  0x100
#define SYS_TK_STA_TSK  0x101
#define SYS_TK_EXT_TSK  0x102
#define SYS_TK_SLP_TSK  0x103
#define SYS_TK_WUP_TSK  0x104
#define SYS_TK_CHG_PRI  0x105
#define SYS_TK_CHG_SLT  0x106
#define SYS_TK_REF_TSK  0x107

#define SYS_TK_CRE_SEM  0x110
#define SYS_TK_DEL_SEM  0x111
#define SYS_TK_SIG_SEM  0x112
#define SYS_TK_WAI_SEM  0x113

#define SYS_TK_CRE_FLG  0x120
#define SYS_TK_DEL_FLG  0x121
#define SYS_TK_SET_FLG  0x122
#define SYS_TK_CLR_FLG  0x123
#define SYS_TK_WAI_FLG  0x124  /* arg2 = (W*)pk_waiflg struct */

/* ----------------------------------------------------------------- */
/* PK_CRE_TSK — extended task creation (used by SYS_TK_CRE_TSK)    */
/* ----------------------------------------------------------------- */
typedef struct {
    FP    task;       /* entry: void task(INT stacd, void *exinf) */
    INT   pri;        /* priority 1 (highest) .. NUM_PRI          */
    INT   stksz;      /* stack size bytes; 0 = default (4096)     */
    INT   policy;     /* SCHED_FIFO or SCHED_RR                   */
    INT   slice_ms;   /* time slice ms; 0 = default (100 ms)      */
    void *exinf;      /* passed as 2nd arg to task entry          */
} PK_CRE_TSK;

/* ----------------------------------------------------------------- */
/* PK_REF_TSK — task reference info (used by SYS_TK_REF_TSK)       */
/* ----------------------------------------------------------------- */
typedef struct {
    INT   pri;        /* current priority                         */
    INT   state;      /* TS_READY/TS_WAIT/TS_DORMANT …            */
    INT   policy;     /* SCHED_FIFO or SCHED_RR                   */
    INT   slice_ms;   /* current time slice ms                    */
} PK_REF_TSK;

/* ----------------------------------------------------------------- */
/* PK_WAI_FLG — packed args for SYS_TK_WAI_FLG (5 params → 1 ptr) */
/* ----------------------------------------------------------------- */
typedef struct {
    ID    flgid;
    UINT  waiptn;
    UINT  wfmode;
    UINT *p_flgptn;   /* out: satisfied pattern                   */
    TMO   tmout;
} PK_WAI_FLG;

/* ----------------------------------------------------------------- */
/* Register IDT gate 0x80 (DPL=3, callable from ring3).             */
/* ----------------------------------------------------------------- */
void syscall_init(void);

/* C-level syscall dispatcher (called from syscall_isr in isr.S). */
W    syscall_dispatch(W nr, W arg0, W arg1, W arg2);
