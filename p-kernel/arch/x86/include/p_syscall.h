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
 *    11     SYS_READDIR   — list directory entries (arg0=path, arg1=PK_DIRENT*, arg2=max)
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
 *  T-Kernel mutex (0x130+):
 *    0x130  SYS_TK_CRE_MTX  — create mutex (arg0=PK_CRE_MTX*)
 *    0x131  SYS_TK_DEL_MTX  — delete mutex
 *    0x132  SYS_TK_LOC_MTX  — lock mutex (arg0=mtxid, arg1=tmout_ms)
 *    0x133  SYS_TK_UNL_MTX  — unlock mutex
 *
 *  T-Kernel mailbox (0x140+):
 *    0x140  SYS_TK_CRE_MBX  — create mailbox (arg0=mbxatr)
 *    0x141  SYS_TK_DEL_MBX  — delete mailbox
 *    0x142  SYS_TK_SND_MBX  — send message (arg0=mbxid, arg1=T_MSG*)
 *    0x143  SYS_TK_RCV_MBX  — receive message (arg0=mbxid, arg1=T_MSG**, arg2=tmout_ms)
 *
 *  T-Kernel message buffer (0x150+):
 *    0x150  SYS_TK_CRE_MBF  — create message buffer (arg0=PK_CRE_MBF*)
 *    0x151  SYS_TK_DEL_MBF  — delete message buffer
 *    0x152  SYS_TK_SND_MBF  — send message (arg0=PK_SND_MBF*)
 *    0x153  SYS_TK_RCV_MBF  — receive message (arg0=mbfid, arg1=buf, arg2=tmout_ms)
 *
 *  T-Kernel variable memory pool (0x160+):
 *    0x160  SYS_TK_CRE_MPL  — create variable pool (arg0=PK_CRE_MPL*)
 *    0x161  SYS_TK_DEL_MPL  — delete variable pool
 *    0x162  SYS_TK_GET_MPL  — get block (arg0=mplid, arg1=blksz, arg2=tmout_ms; ret=ptr)
 *    0x163  SYS_TK_REL_MPL  — release block (arg0=mplid, arg1=blk_ptr)
 *
 *  T-Kernel fixed memory pool (0x168+):
 *    0x168  SYS_TK_CRE_MPF  — create fixed pool (arg0=PK_CRE_MPF*)
 *    0x169  SYS_TK_DEL_MPF  — delete fixed pool
 *    0x16A  SYS_TK_GET_MPF  — get block (arg0=mpfid, arg1=tmout_ms; ret=ptr)
 *    0x16B  SYS_TK_REL_MPF  — release block (arg0=mpfid, arg1=blf_ptr)
 *
 *  T-Kernel cyclic handler (0x170+):
 *    0x170  SYS_TK_CRE_CYC  — create cyclic handler (arg0=PK_CRE_CYC*)
 *    0x171  SYS_TK_DEL_CYC  — delete cyclic handler
 *    0x172  SYS_TK_STA_CYC  — start cyclic handler
 *    0x173  SYS_TK_STP_CYC  — stop cyclic handler
 *
 *  T-Kernel alarm handler (0x178+):
 *    0x178  SYS_TK_CRE_ALM  — create alarm handler (arg0=PK_CRE_ALM*)
 *    0x179  SYS_TK_DEL_ALM  — delete alarm handler
 *    0x17A  SYS_TK_STA_ALM  — start alarm handler (arg0=almid, arg1=almtim_ms)
 *    0x17B  SYS_TK_STP_ALM  — stop alarm handler
 *
 *  T-Kernel task supplement (0x109+):
 *    0x109  SYS_TK_TER_TSK  — force-terminate task
 *    0x10A  SYS_TK_SUS_TSK  — suspend task
 *    0x10B  SYS_TK_RSM_TSK  — resume task
 *    0x10C  SYS_TK_FRSM_TSK — force-resume suspended task
 *    0x10D  SYS_TK_REL_WAI  — release task from wait state
 *    0x10E  SYS_TK_GET_TID  — get current task ID
 *    0x10F  SYS_TK_CAN_WUP  — cancel pending wakeup requests
 *
 *  T-Kernel ref APIs:
 *    0x114  SYS_TK_REF_SEM  — reference semaphore (arg0=semid, arg1=PK_REF_SEM*)
 *    0x125  SYS_TK_REF_FLG  — reference event flag (arg0=flgid, arg1=PK_REF_FLG*)
 *    0x134  SYS_TK_REF_MTX  — reference mutex (arg0=mtxid, arg1=PK_REF_MTX*)
 *    0x144  SYS_TK_REF_MBX  — reference mailbox (arg0=mbxid, arg1=PK_REF_MBX*)
 *    0x154  SYS_TK_REF_MBF  — reference message buffer (arg0=mbfid, arg1=PK_REF_MBF*)
 *    0x164  SYS_TK_REF_MPL  — reference variable pool (arg0=mplid, arg1=PK_REF_MPL*)
 *    0x16C  SYS_TK_REF_MPF  — reference fixed pool (arg0=mpfid, arg1=PK_REF_MPF*)
 *    0x174  SYS_TK_REF_CYC  — reference cyclic handler (arg0=cycid, arg1=PK_REF_CYC*)
 *    0x17C  SYS_TK_REF_ALM  — reference alarm handler (arg0=almid, arg1=PK_REF_ALM*)
 *
 *  T-Kernel time (0x180+):
 *    0x180  SYS_TK_GET_TIM  — get system time (arg0=PK_SYSTIM*)
 *    0x181  SYS_TK_DLY_TSK  — task delay (arg0=delay_ms)
 *
 *  T-Kernel rendezvous port (0x190+):
 *    0x190  SYS_TK_CRE_POR  — create rendezvous port (arg0=PK_CPOR*)
 *    0x191  SYS_TK_DEL_POR  — delete rendezvous port
 *    0x192  SYS_TK_CAL_POR  — call rendezvous (arg0=PK_CAL_POR*)
 *    0x193  SYS_TK_ACP_POR  — accept rendezvous (arg0=PK_ACP_POR*)
 *    0x194  SYS_TK_FWD_POR  — forward rendezvous (arg0=PK_FWD_POR*)
 *    0x195  SYS_TK_RPL_RDV  — reply to rendezvous (arg0=rdvno,arg1=msg,arg2=sz)
 *
 *  T-Kernel system info (0x1A0+):
 *    0x1A0  SYS_TK_REF_VER  — reference T-Kernel version (arg0=PK_RVER*)
 *    0x1A1  SYS_TK_REF_SYS  — reference system state (arg0=PK_RSYS*)
 *
 *  Network syscalls (0x200+):
 *    0x200  SYS_UDP_BIND       — bind local UDP port (arg0=port)
 *    0x201  SYS_UDP_SEND       — send UDP datagram (arg0=PK_UDP_SEND*)
 *    0x202  SYS_UDP_RECV       — receive UDP datagram (arg0=PK_UDP_RECV*)
 *    0x203  SYS_TCP_CONNECT    — TCP connect (arg0=PK_TCP_CONNECT*)
 *    0x204  SYS_TCP_WRITE      — TCP send (arg0=handle, arg1=buf, arg2=len)
 *    0x205  SYS_TCP_READ       — TCP receive (arg0=PK_TCP_READ*)
 *    0x206  SYS_TCP_CLOSE      — TCP close+free (arg0=handle)
 *    0x207  SYS_UDP_JOIN_GROUP — join multicast group (arg0=port, arg1=mcast_ip)
 *    0x208  SYS_UDP_LEAVE_GROUP— leave multicast group (arg0=port, arg1=mcast_ip)
 *
 *  Filesystem extended (0x300+):
 *    0x300  SYS_MOUNT          — mount filesystem (arg0="dev", arg1="path"; 0=show table)
 *    0x301  SYS_UMOUNT         — unmount path (arg0="path")
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
#define SYS_READDIR     11

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

/* Mutex */
#define SYS_TK_CRE_MTX  0x130
#define SYS_TK_DEL_MTX  0x131
#define SYS_TK_LOC_MTX  0x132
#define SYS_TK_UNL_MTX  0x133

/* Mailbox */
#define SYS_TK_CRE_MBX  0x140
#define SYS_TK_DEL_MBX  0x141
#define SYS_TK_SND_MBX  0x142
#define SYS_TK_RCV_MBX  0x143

/* Message buffer */
#define SYS_TK_CRE_MBF  0x150
#define SYS_TK_DEL_MBF  0x151
#define SYS_TK_SND_MBF  0x152
#define SYS_TK_RCV_MBF  0x153

/* Variable memory pool */
#define SYS_TK_CRE_MPL  0x160
#define SYS_TK_DEL_MPL  0x161
#define SYS_TK_GET_MPL  0x162
#define SYS_TK_REL_MPL  0x163

/* Fixed memory pool */
#define SYS_TK_CRE_MPF  0x168
#define SYS_TK_DEL_MPF  0x169
#define SYS_TK_GET_MPF  0x16A
#define SYS_TK_REL_MPF  0x16B

/* Cyclic handler */
#define SYS_TK_CRE_CYC  0x170
#define SYS_TK_DEL_CYC  0x171
#define SYS_TK_STA_CYC  0x172
#define SYS_TK_STP_CYC  0x173

/* Alarm handler */
#define SYS_TK_CRE_ALM  0x178
#define SYS_TK_DEL_ALM  0x179
#define SYS_TK_STA_ALM  0x17A
#define SYS_TK_STP_ALM  0x17B

/* Task supplement */
#define SYS_TK_TER_TSK   0x109
#define SYS_TK_SUS_TSK   0x10A
#define SYS_TK_RSM_TSK   0x10B
#define SYS_TK_FRSM_TSK  0x10C
#define SYS_TK_REL_WAI   0x10D
#define SYS_TK_GET_TID   0x10E
#define SYS_TK_CAN_WUP   0x10F

/* Ref APIs */
#define SYS_TK_REF_SEM   0x114
#define SYS_TK_REF_FLG   0x125
#define SYS_TK_REF_MTX   0x134
#define SYS_TK_REF_MBX   0x144
#define SYS_TK_REF_MBF   0x154
#define SYS_TK_REF_MPL   0x164
#define SYS_TK_REF_MPF   0x16C
#define SYS_TK_REF_CYC   0x174
#define SYS_TK_REF_ALM   0x17C

/* Time */
#define SYS_TK_GET_TIM   0x180
#define SYS_TK_DLY_TSK   0x181

/* Rendezvous port */
#define SYS_TK_CRE_POR   0x190
#define SYS_TK_DEL_POR   0x191
#define SYS_TK_CAL_POR   0x192
#define SYS_TK_ACP_POR   0x193
#define SYS_TK_FWD_POR   0x194
#define SYS_TK_RPL_RDV   0x195

/* System info */
#define SYS_TK_REF_VER   0x1A0
#define SYS_TK_REF_SYS   0x1A1

/* Rendezvous port ref (supplement) */
#define SYS_TK_REF_POR   0x196

/* Time (supplement) */
#define SYS_TK_GET_OTM   0x182
#define SYS_TK_SET_TIM   0x183

/* Task dispatch control */
#define SYS_TK_EXD_TSK   0x1C0
#define SYS_TK_DIS_DSP   0x1C1
#define SYS_TK_ENA_DSP   0x1C2
#define SYS_TK_ROT_RDQ   0x1C3

/* ----------------------------------------------------------------- */
/* Network syscall numbers (p-kernel extension)                      */
/* ----------------------------------------------------------------- */
#define SYS_UDP_BIND        0x200
#define SYS_UDP_SEND        0x201
#define SYS_UDP_RECV        0x202
#define SYS_TCP_CONNECT     0x203
#define SYS_TCP_WRITE       0x204
#define SYS_TCP_READ        0x205
#define SYS_TCP_CLOSE       0x206
#define SYS_UDP_JOIN_GROUP  0x207
#define SYS_UDP_LEAVE_GROUP 0x208

/* ----------------------------------------------------------------- */
/* Filesystem extended syscall numbers                               */
/* ----------------------------------------------------------------- */
#define SYS_MOUNT       0x300
#define SYS_UMOUNT      0x301

/* ----------------------------------------------------------------- */
/* AI syscall numbers (p-kernel extension)                           */
/* ----------------------------------------------------------------- */
#define SYS_INFER       0x210
#define SYS_AI_SUBMIT   0x211
#define SYS_AI_WAIT     0x212

/* ----------------------------------------------------------------- */
/* PK_DIRENT — one directory entry returned by SYS_READDIR          */
/* Layout must match PK_SYS_DIRENT in plibc.h (32-bit flat)        */
/* ----------------------------------------------------------------- */
#define PK_DIRENT_NAMELEN  64

typedef struct {
    char name[PK_DIRENT_NAMELEN]; /* null-terminated filename          */
    UW   size;                    /* file size in bytes (0 for dirs)   */
    W    is_dir;                  /* 1 = directory, 0 = file           */
} PK_DIRENT;

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
/* PK_TCP_CONNECT — args for SYS_TCP_CONNECT                        */
/* Layout must match PK_SYS_TCP_CONNECT in plibc.h (32-bit flat)   */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  dst_ip;       /* destination IP (IP4 format)                 */
    UH  dst_port;     /* destination port (host byte order)          */
    UH  _pad;
    W   timeout_ms;   /* connect timeout (-1 = use default 10 s)     */
} PK_TCP_CONNECT;

/* ----------------------------------------------------------------- */
/* PK_TCP_READ — args for SYS_TCP_READ                              */
/* Layout must match PK_SYS_TCP_READ in plibc.h (32-bit flat)      */
/* ----------------------------------------------------------------- */
typedef struct {
    W   handle;       /* connection handle from SYS_TCP_CONNECT      */
    UW  buf_ptr;      /* user buffer pointer                         */
    W   buflen;       /* buffer capacity                             */
    W   timeout_ms;   /* receive timeout in ms                       */
} PK_TCP_READ;

/* ----------------------------------------------------------------- */
/* PK_CRE_MTX — args for SYS_TK_CRE_MTX                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  mtxatr;    /* TA_TFIFO(0)/TA_TPRI(1)/TA_INHERIT(2)/TA_CEILING(3) */
    W   ceilpri;   /* ceiling priority (only when TA_CEILING)             */
} PK_CRE_MTX;

/* ----------------------------------------------------------------- */
/* PK_CRE_MBF — args for SYS_TK_CRE_MBF                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  mbfatr;    /* TA_TFIFO(0) or TA_TPRI(1)                          */
    W   bufsz;     /* message buffer size in bytes                        */
    W   maxmsz;    /* maximum message size in bytes                       */
    UW  buf_ptr;   /* user-provided backing buffer pointer                */
} PK_CRE_MBF;

/* ----------------------------------------------------------------- */
/* PK_SND_MBF — args for SYS_TK_SND_MBF (4 params → 1 ptr)        */
/* ----------------------------------------------------------------- */
typedef struct {
    W   mbfid;
    UW  msg_ptr;   /* pointer to message data                             */
    W   msgsz;     /* message size in bytes                               */
    W   tmout;     /* timeout in ms                                       */
} PK_SND_MBF;

/* ----------------------------------------------------------------- */
/* PK_CRE_MPL — args for SYS_TK_CRE_MPL                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  mplatr;    /* TA_TFIFO(0) or TA_TPRI(1)                          */
    W   mplsz;     /* total pool size in bytes                            */
    UW  buf_ptr;   /* user-provided backing buffer pointer                */
} PK_CRE_MPL;

/* ----------------------------------------------------------------- */
/* PK_CRE_MPF — args for SYS_TK_CRE_MPF                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  mpfatr;    /* TA_TFIFO(0) or TA_TPRI(1)                          */
    W   mpfcnt;    /* number of fixed-size blocks in pool                 */
    W   blfsz;     /* block size in bytes                                 */
    UW  buf_ptr;   /* user-provided backing buffer pointer                */
} PK_CRE_MPF;

/* ----------------------------------------------------------------- */
/* PK_CRE_CYC — args for SYS_TK_CRE_CYC                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  cycatr;      /* TA_HLNG(1) | TA_STA(2) | TA_PHS(4)              */
    UW  cychdr;      /* handler function pointer (FP)                    */
    W   cyctim_ms;   /* cycle interval in ms                             */
    W   cycphs_ms;   /* cycle phase offset in ms (0 = none)              */
} PK_CRE_CYC;

/* ----------------------------------------------------------------- */
/* PK_CRE_ALM — args for SYS_TK_CRE_ALM                           */
/* ----------------------------------------------------------------- */
typedef struct {
    UW  almatr;      /* TA_HLNG(1)                                       */
    UW  almhdr;      /* handler function pointer (FP)                    */
} PK_CRE_ALM;

/* ----------------------------------------------------------------- */
/* PK_REF_* — ref result structs (filled by kernel, read by user)   */
/* ----------------------------------------------------------------- */
typedef struct { W wtsk; W semcnt; }                        PK_REF_SEM;
typedef struct { W wtsk; UW flgptn; }                       PK_REF_FLG;
typedef struct { W htsk; W wtsk; }                          PK_REF_MTX;
typedef struct { W wtsk; }                                  PK_REF_MBX;
typedef struct { W wtsk; W stsk; W msgsz; W frbufsz; W maxmsz; } PK_REF_MBF;
typedef struct { W wtsk; W frsz; W maxsz; }                 PK_REF_MPL;
typedef struct { W wtsk; W frbcnt; }                        PK_REF_MPF;
typedef struct { W lfttim_ms; UW cycstat; }                 PK_REF_CYC;
typedef struct { W lfttim_ms; UW almstat; }                 PK_REF_ALM;

/* ----------------------------------------------------------------- */
/* PK_SYSTIM — system time returned by SYS_TK_GET_TIM               */
/* ----------------------------------------------------------------- */
typedef struct { W hi; UW lo; } PK_SYSTIM;

/* ----------------------------------------------------------------- */
/* PK_RVER / PK_RSYS — version/system info                          */
/* ----------------------------------------------------------------- */
typedef struct { UH maker; UH prid; UH spver; UH prver; UH prno[4]; } PK_RVER;
typedef struct { UW sysstat; W runtskid; W schedtskid; }    PK_RSYS;

/* ----------------------------------------------------------------- */
/* PK_CPOR — rendezvous port creation                               */
/* ----------------------------------------------------------------- */
typedef struct { UW poratr; W maxcmsz; W maxrmsz; } PK_CPOR;

/* ----------------------------------------------------------------- */
/* PK_RPOR — rendezvous port reference result                       */
/* ----------------------------------------------------------------- */
typedef struct { W wtsk; W atsk; W maxcmsz; W maxrmsz; } PK_RPOR;

/* ----------------------------------------------------------------- */
/* PK_CAL_POR — args for SYS_TK_CAL_POR (5 params → 1 ptr)        */
/* ----------------------------------------------------------------- */
typedef struct { W porid; UW calptn; UW msg_ptr; W cmsgsz; W tmout; } PK_CAL_POR;

/* ----------------------------------------------------------------- */
/* PK_ACP_POR — args for SYS_TK_ACP_POR (5 params → 1 ptr)        */
/* ----------------------------------------------------------------- */
typedef struct { W porid; UW acpptn; UW p_rdvno; UW msg_ptr; W tmout; } PK_ACP_POR;

/* ----------------------------------------------------------------- */
/* PK_FWD_POR — args for SYS_TK_FWD_POR (5 params → 1 ptr)        */
/* ----------------------------------------------------------------- */
typedef struct { W porid; UW calptn; UW rdvno; UW msg_ptr; W cmsgsz; } PK_FWD_POR;

/* ----------------------------------------------------------------- */
/* Register IDT gate 0x80 (DPL=3, callable from ring3).             */
/* ----------------------------------------------------------------- */
void syscall_init(void);

/* C-level syscall dispatcher (called from syscall_isr in isr.S). */
W    syscall_dispatch(W nr, W arg0, W arg1, W arg2);
