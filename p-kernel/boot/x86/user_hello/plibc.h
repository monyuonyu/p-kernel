/*
 *  plibc.h — p-kernel user-space library
 *
 *  POSIX-compatible file I/O + T-Kernel native API via INT 0x80.
 *  Self-contained: no kernel headers required.
 *  Works from ring-3 programs AND from ring-0 tasks created via tk_cre_tsk().
 *
 *  Usage:  #include "plibc.h"
 */
#pragma once

/* ================================================================= */
/* Constants                                                          */
/* ================================================================= */

/* Scheduling policy */
#define SCHED_FIFO   0   /* priority preemptive only (RTOS default) */
#define SCHED_RR     1   /* round-robin within same priority        */

/* open() flags */
#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040
#define O_TRUNC      0x0200

/* lseek() whence */
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2

/* tk_wai_flg() mode */
#define TWF_ANDW     0x00   /* wait until ALL bits set  */
#define TWF_ORW      0x01   /* wait until ANY bit set   */

/* Timeout values */
#define TMO_FEVR     (-1)   /* wait forever              */
#define TMO_POL      (0)    /* poll (no wait)            */

/* ================================================================= */
/* Structures                                                         */
/* ================================================================= */

/* Task creation parameters (SYS_TK_CRE_TSK) */
typedef struct {
    void  (*task)(int, void *);  /* entry: void fn(int stacd, void *exinf) */
    int     pri;                  /* priority 1 (highest) .. 16 (lowest)   */
    int     stksz;                /* stack bytes; 0 = default (4096)       */
    int     policy;               /* SCHED_FIFO or SCHED_RR                */
    int     slice_ms;             /* time slice ms; 0 = default (100 ms)   */
    void   *exinf;                /* passed as 2nd arg to task entry       */
} PK_CRE_TSK;

/* Task reference info (SYS_TK_REF_TSK) */
typedef struct {
    int     pri;        /* current priority              */
    int     state;      /* TTS_* state                   */
    int     policy;     /* SCHED_FIFO or SCHED_RR        */
    int     slice_ms;   /* current time slice ms         */
} PK_REF_TSK;

/* Semaphore creation (SYS_TK_CRE_SEM) — matches T-Kernel T_CSEM */
typedef struct {
    void   *exinf;      /* extended info (set to 0)      */
    int     isemcnt;    /* initial semaphore count       */
    int     maxsem;     /* maximum semaphore count       */
} PK_CSEM;

/* Event flag creation (SYS_TK_CRE_FLG) — matches T-Kernel T_CFLG */
typedef struct {
    void          *exinf;    /* extended info (set to 0)    */
    unsigned int   iflgptn;  /* initial flag pattern        */
} PK_CFLG;

/* Event flag wait args (SYS_TK_WAI_FLG — packed into single ptr) */
typedef struct {
    int           flgid;
    unsigned int  waiptn;
    unsigned int  wfmode;
    unsigned int *p_flgptn;  /* out: satisfied pattern      */
    int           tmout;
} PK_WAI_FLG;

/* ================================================================= */
/* Syscall helper                                                     */
/* ================================================================= */

static inline int __sc(int nr, int a0, int a1, int a2)
{
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(nr), "b"(a0), "c"(a1), "d"(a2)
                 : "memory");
    return ret;
}

/* ================================================================= */
/* POSIX-compatible API                                               */
/* ================================================================= */

static inline void sys_exit(int code)
{
    __sc(1, code, 0, 0);
    for (;;) asm volatile("hlt");
}

static inline int sys_write(int fd, const void *buf, int len)
    { return __sc(4, fd, (int)(long)buf, len); }

static inline int sys_read(int fd, void *buf, int len)
    { return __sc(3, fd, (int)(long)buf, len); }

static inline int sys_open(const char *path, int flags)
    { return __sc(5, (int)(long)path, flags, 0); }

static inline int sys_close(int fd)
    { return __sc(6, fd, 0, 0); }

static inline int sys_lseek(int fd, int offset, int whence)
    { return __sc(7, fd, offset, whence); }

static inline int sys_mkdir(const char *path)
    { return __sc(8, (int)(long)path, 0, 0); }

static inline int sys_unlink(const char *path)
    { return __sc(9, (int)(long)path, 0, 0); }

static inline int sys_rename(const char *old, const char *nw)
    { return __sc(10, (int)(long)old, (int)(long)nw, 0); }

/* ================================================================= */
/* T-Kernel native: task management                                   */
/* ================================================================= */

static inline int tk_cre_tsk(PK_CRE_TSK *pk)
    { return __sc(0x100, (int)(long)pk, 0, 0); }

static inline int tk_sta_tsk(int tid, int stacd)
    { return __sc(0x101, tid, stacd, 0); }

static inline void tk_ext_tsk(void)
{
    __sc(0x102, 0, 0, 0);
    for (;;) asm volatile("hlt");
}

static inline int tk_slp_tsk(int tmout_ms)
    { return __sc(0x103, tmout_ms, 0, 0); }

static inline int tk_wup_tsk(int tid)
    { return __sc(0x104, tid, 0, 0); }

static inline int tk_chg_pri(int tid, int pri)
    { return __sc(0x105, tid, pri, 0); }

static inline int tk_chg_slt(int tid, int slice_ms)
    { return __sc(0x106, tid, slice_ms, 0); }

static inline int tk_ref_tsk(int tid, PK_REF_TSK *out)
    { return __sc(0x107, tid, (int)(long)out, 0); }

static inline int tk_del_tsk(int tid)
    { return __sc(0x108, tid, 0, 0); }

/* ================================================================= */
/* T-Kernel native: semaphore                                         */
/* ================================================================= */

static inline int tk_cre_sem(PK_CSEM *pk)
    { return __sc(0x110, (int)(long)pk, 0, 0); }

static inline int tk_del_sem(int semid)
    { return __sc(0x111, semid, 0, 0); }

static inline int tk_sig_sem(int semid, int cnt)
    { return __sc(0x112, semid, cnt, 0); }

static inline int tk_wai_sem(int semid, int cnt, int tmout_ms)
    { return __sc(0x113, semid, cnt, tmout_ms); }

/* ================================================================= */
/* T-Kernel native: event flag                                        */
/* ================================================================= */

static inline int tk_cre_flg(PK_CFLG *pk)
    { return __sc(0x120, (int)(long)pk, 0, 0); }

static inline int tk_del_flg(int flgid)
    { return __sc(0x121, flgid, 0, 0); }

static inline int tk_set_flg(int flgid, unsigned int setptn)
    { return __sc(0x122, flgid, (int)setptn, 0); }

static inline int tk_clr_flg(int flgid, unsigned int clrptn)
    { return __sc(0x123, flgid, (int)clrptn, 0); }

static inline int tk_wai_flg(int flgid, unsigned int waiptn,
                               unsigned int wfmode,
                               unsigned int *p_flgptn, int tmout_ms)
{
    PK_WAI_FLG args = { flgid, waiptn, wfmode, p_flgptn, tmout_ms };
    return __sc(0x124, (int)(long)&args, 0, 0);
}

/* ================================================================= */
/* Minimal string helpers (no libc)                                   */
/* ================================================================= */

static inline int plib_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline void plib_puts(const char *s)
{
    sys_write(1, s, plib_strlen(s));
}

/* Print unsigned decimal */
static inline void plib_putu(unsigned int v)
{
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; } }
    plib_puts(buf + i);
}

/* Print signed decimal */
static inline void plib_puti(int v)
{
    if (v < 0) { sys_write(1, "-", 1); plib_putu((unsigned int)(-v)); }
    else        { plib_putu((unsigned int)v); }
}
