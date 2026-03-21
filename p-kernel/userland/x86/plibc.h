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

/* AI inference classes (SYS_INFER return value) */
#define AI_CLASS_NORMAL    0   /* all sensor values within range   */
#define AI_CLASS_ALERT     1   /* one value moderately out of range */
#define AI_CLASS_CRITICAL  2   /* multiple values severely out     */

/* AI job operation codes (SYS_AI_SUBMIT) */
#define SYS_AI_OP_MLP_FWD  0x10   /* full 4→8→8→3 MLP forward pass  */
#define SYS_AI_MODEL_SENSOR 0x0001 /* sensor classifier model ID     */

/* IP address construction helper (same byte layout as kernel IP4()) */
#define SYS_IP4(a,b,c,d) \
    ((unsigned int)(((unsigned char)(d)<<24) | ((unsigned char)(c)<<16) | \
                    ((unsigned char)(b)<<8)  |  (unsigned char)(a)))

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

/* Directory entry returned by sys_readdir() (SYS_READDIR)
 * Layout must match PK_DIRENT in p_syscall.h (32-bit flat) */
#define PLIB_DIRENT_NAMELEN  64

typedef struct {
    char         name[PLIB_DIRENT_NAMELEN]; /* null-terminated filename */
    unsigned int size;                       /* file size in bytes       */
    int          is_dir;                     /* 1 = directory, 0 = file  */
} PK_SYS_DIRENT;

/* TCP connect args (SYS_TCP_CONNECT)
 * Layout must match PK_TCP_CONNECT in p_syscall.h (32-bit flat) */
typedef struct {
    unsigned int   dst_ip;     /* destination IP (SYS_IP4 format)        */
    unsigned short dst_port;   /* destination port (host byte order)     */
    unsigned short _pad;
    int            timeout_ms; /* connect timeout (-1 = default 10 s)    */
} PK_SYS_TCP_CONNECT;

/* TCP read args (SYS_TCP_READ)
 * Layout must match PK_TCP_READ in p_syscall.h (32-bit flat) */
typedef struct {
    int          handle;     /* connection handle from sys_tcp_connect   */
    unsigned int buf_ptr;    /* receive buffer pointer (uint32)          */
    int          buflen;     /* buffer capacity                          */
    int          timeout_ms; /* receive timeout in ms                    */
} PK_SYS_TCP_READ;

/* UDP send args (SYS_UDP_SEND)
 * Layout must match PK_UDP_SEND in p_syscall.h (32-bit flat) */
typedef struct {
    unsigned int   dst_ip;    /* destination IP (SYS_IP4 format)    */
    unsigned short src_port;  /* source port (host byte order)      */
    unsigned short dst_port;  /* destination port (host byte order) */
    const void    *buf;       /* payload buffer                     */
    unsigned short len;       /* payload length in bytes            */
    unsigned short _pad;
} PK_SYS_UDP_SEND;

/* UDP receive args (SYS_UDP_RECV, IN/OUT)
 * Layout must match PK_UDP_RECV in p_syscall.h (32-bit flat) */
typedef struct {
    unsigned short port;       /* IN:  local port to receive on     */
    unsigned short _pad;
    void          *buf;        /* IN:  receive buffer               */
    unsigned short buflen;     /* IN:  buffer capacity              */
    unsigned short _pad2;
    int            timeout_ms; /* IN:  ms; -1=forever, 0=poll       */
    /* Filled by kernel on successful return: */
    unsigned int   src_ip;     /* OUT: sender IP                    */
    unsigned short src_port;   /* OUT: sender port                  */
    unsigned short data_len;   /* OUT: received byte count          */
} PK_SYS_UDP_RECV;

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

/* Print IP address as "A.B.C.D" */
static inline void plib_put_ip(unsigned int ip)
{
    unsigned char a = (unsigned char)(ip & 0xFF);
    unsigned char b = (unsigned char)((ip >> 8) & 0xFF);
    unsigned char c = (unsigned char)((ip >> 16) & 0xFF);
    unsigned char d = (unsigned char)((ip >> 24) & 0xFF);
    plib_putu(a); sys_write(1, ".", 1);
    plib_putu(b); sys_write(1, ".", 1);
    plib_putu(c); sys_write(1, ".", 1);
    plib_putu(d);
}

/* ================================================================= */
/* Network API (UDP)                                                  */
/* ================================================================= */

/* Bind a local UDP port to receive packets.
 * Returns 0 on success, -1 if no free slot or port already bound.  */
static inline int sys_udp_bind(unsigned short port)
    { return __sc(0x200, (int)port, 0, 0); }

/* Send a UDP datagram.
 * Returns 0 on success, -1 if ARP not yet resolved (will retry).   */
static inline int sys_udp_send(PK_SYS_UDP_SEND *pk)
    { return __sc(0x201, (int)(long)pk, 0, 0); }

/* Receive a UDP datagram (fills pk->src_ip, src_port, data_len).
 * Returns received byte count, or negative error (-50 = timeout).  */
static inline int sys_udp_recv(PK_SYS_UDP_RECV *pk)
    { return __sc(0x202, (int)(long)pk, 0, 0); }

/* ================================================================= */
/* Filesystem API (readdir)                                           */
/* ================================================================= */

/* List directory entries at `path`.
 * Fills `out[0..max-1]` with up to `max` entries (max ≤ 32).
 * Returns the number of entries found, or -1 on error.
 * The entries are NOT sorted; order depends on the FAT32 directory.  */
static inline int sys_readdir(const char *path, PK_SYS_DIRENT *out, int max)
    { return __sc(11, (int)(long)path, (int)(long)out, max); }

/* ================================================================= */
/* Network API (TCP)                                                  */
/* ================================================================= */

/* Open a TCP connection to dst_ip:dst_port.
 * timeout_ms: -1 = use kernel default (10 s).
 * Returns connection handle (0-3) on success, or -1 on error.      */
static inline int sys_tcp_connect(unsigned int dst_ip,
                                   unsigned short dst_port,
                                   int timeout_ms)
{
    PK_SYS_TCP_CONNECT pk = { dst_ip, dst_port, 0, timeout_ms };
    return __sc(0x203, (int)(long)&pk, 0, 0);
}

/* Send data over an established TCP connection.
 * Returns number of bytes sent, or -1 on error.                     */
static inline int sys_tcp_write(int handle, const void *buf, int len)
    { return __sc(0x204, handle, (int)(long)buf, len); }

/* Receive data from a TCP connection.
 * Returns bytes received (>0), 0 on connection close, <0 on error. */
static inline int sys_tcp_read(int handle, void *buf,
                                int buflen, int timeout_ms)
{
    PK_SYS_TCP_READ pk = { handle, (unsigned int)(long)buf,
                            buflen, timeout_ms };
    return __sc(0x205, (int)(long)&pk, 0, 0);
}

/* Close and release a TCP connection.
 * Returns 0 on success, -1 on invalid handle.                      */
static inline int sys_tcp_close(int handle)
    { return __sc(0x206, handle, 0, 0); }

/* ================================================================= */
/* AI inference API                                                   */
/* ================================================================= */

/* Sensor value packing: 4 int8 values → one int32.
 * Each value should already be normalised to [-127, 127].          */
#define SYS_SENSOR_PACK(t,h,p,l) \
    ( (int)(((signed char)(t)<<24) | (((unsigned char)(h))<<16) | \
            (((unsigned char)(p))<<8) | ((unsigned char)(l))) )

/* Synchronous local MLP inference.
 * packed = SYS_SENSOR_PACK(norm_temp, norm_hum, norm_press, norm_light)
 * Returns class: 0=normal, 1=alert, 2=critical, or -1 on error.    */
static inline int sys_infer(int packed)
    { return __sc(0x210, packed, 0, 0); }

/* Submit an async AI inference job.
 * packed = same format as sys_infer().
 * Returns job handle (0-3) on success, or -1 if queue is full.     */
static inline int sys_ai_submit(int packed)
    { return __sc(0x211, packed, 0, 0); }

/* Wait for a previously submitted AI job to complete.
 * handle = value returned by sys_ai_submit().
 * timeout_ms: -1 = forever, 0 = poll.
 * Returns class (0/1/2) on success, or -50 (E_TMOUT) on timeout.   */
static inline int sys_ai_wait(int handle, int timeout_ms)
    { return __sc(0x212, handle, timeout_ms, 0); }
