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
 *
 *  Network syscalls (0x200+):
 *    0x200  SYS_UDP_BIND    — bind local UDP port (arg0=port)
 *    0x201  SYS_UDP_SEND    — send UDP datagram (arg0=PK_UDP_SEND*)
 *    0x202  SYS_UDP_RECV    — receive UDP datagram (arg0=PK_UDP_RECV*)
 *
 *  AI syscalls (0x210+):
 *    0x210  SYS_INFER       — local MLP inference (arg0=packed sensor)
 *    0x211  SYS_AI_SUBMIT   — submit async AI job (arg0=packed sensor)
 *    0x212  SYS_AI_WAIT     — wait AI job completion (arg0=handle, arg1=tmout_ms)
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
#define SYS_TK_DEL_TSK  0x108

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
/* Network syscall numbers (p-kernel extension)                      */
/* ----------------------------------------------------------------- */
#define SYS_UDP_BIND    0x200
#define SYS_UDP_SEND    0x201
#define SYS_UDP_RECV    0x202

/* ----------------------------------------------------------------- */
/* AI syscall numbers (p-kernel extension)                           */
/* ----------------------------------------------------------------- */
#define SYS_INFER       0x210
#define SYS_AI_SUBMIT   0x211
#define SYS_AI_WAIT     0x212

/* ----------------------------------------------------------------- */
/* PK_UDP_SEND — args for SYS_UDP_SEND                              */
/* Layout must match PK_SYS_UDP_SEND in plibc.h (both 32-bit)      */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  dst_ip;       /* destination IP (host byte order, IP4 format) */
    UH  src_port;     /* source port (host byte order)                */
    UH  dst_port;     /* destination port (host byte order)           */
    UW  buf_ptr;      /* pointer to payload (const UB*)               */
    UH  len;          /* payload length in bytes                      */
    UH  _pad;
} PK_UDP_SEND;

/* ----------------------------------------------------------------- */
/* PK_UDP_RECV — args for SYS_UDP_RECV (IN/OUT struct)              */
/* Layout must match PK_SYS_UDP_RECV in plibc.h (both 32-bit)      */
/* ----------------------------------------------------------------- */
typedef struct {
    UH  port;         /* IN:  local port to receive on               */
    UH  _pad;
    UW  buf_ptr;      /* IN:  user receive buffer pointer            */
    UH  buflen;       /* IN:  buffer capacity                        */
    UH  _pad2;
    W   timeout_ms;   /* IN:  timeout in ms; -1=forever, 0=poll      */
    /* Filled by kernel on return: */
    UW  src_ip;       /* OUT: sender IP                              */
    UH  src_port;     /* OUT: sender port                            */
    UH  data_len;     /* OUT: received byte count                    */
} PK_UDP_RECV;

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
