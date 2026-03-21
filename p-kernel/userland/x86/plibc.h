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

/* NULL pointer */
#ifndef NULL
#define NULL ((void *)0)
#endif

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
/* New POSIX interfaces                                               */
/* ================================================================= */

/* File type bits (st_mode) */
#define S_IFREG   0x8000u
#define S_IFDIR   0x4000u
#define S_ISREG(m) (((m) & S_IFREG) != 0)
#define S_ISDIR(m) (((m) & S_IFDIR) != 0)

typedef struct {
    unsigned int st_mode;
    unsigned int st_size;
    unsigned int st_ino;
    unsigned int st_mtime;
} struct_stat;

static inline int sys_getpid(void)
    { return __sc(20, 0, 0, 0); }

static inline int sys_chdir(const char *path)
    { return __sc(12, (int)(long)path, 0, 0); }

static inline int sys_getcwd(char *buf, int len)
    { return __sc(183, (int)(long)buf, len, 0); }

static inline int sys_stat(const char *path, struct_stat *st)
    { return __sc(106, (int)(long)path, (int)(long)st, 0); }

static inline int sys_fstat(int fd, struct_stat *st)
    { return __sc(28, fd, (int)(long)st, 0); }

static inline int sys_dup(int fd)
    { return __sc(41, fd, 0, 0); }

static inline int sys_dup2(int old_fd, int new_fd)
    { return __sc(63, old_fd, new_fd, 0); }

static inline int sys_pipe(int fds[2])
    { return __sc(42, (int)(long)fds, 0, 0); }

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

/* Join a UDP multicast group on a previously bound port.
 * Packets sent to mcast_ip:port will be delivered to this socket.
 * Software loopback: packets sent locally to the group arrive too.
 * Returns 0 on success, -1 if port not bound.                       */
static inline int sys_udp_join_group(unsigned short port,
                                      unsigned int   mcast_ip)
    { return __sc(0x207, (int)port, (int)mcast_ip, 0); }

/* Leave a UDP multicast group. Returns 0 on success.               */
static inline int sys_udp_leave_group(unsigned short port,
                                       unsigned int   mcast_ip)
    { return __sc(0x208, (int)port, (int)mcast_ip, 0); }

/* ================================================================= */
/* Filesystem extended API (mount)                                    */
/* ================================================================= */

/* Check if VFS (FAT32/IDE) is mounted and ready.
 * Returns 0 if ready, -1 if not.                                    */
static inline int sys_mount(void)
    { return __sc(0x300, 0, 0, 0); }

/* Unmount (no-op for single root mount — future use).              */
static inline int sys_umount(const char *path)
    { return __sc(0x301, (int)(long)path, 0, 0); }

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
/* T-Kernel native: mutex                                             */
/* ================================================================= */

/* Mutex attributes */
#define TA_MTX_TFIFO    0x00  /* wait queue: FIFO                    */
#define TA_MTX_TPRI     0x01  /* wait queue: priority order          */
#define TA_INHERIT      0x02  /* priority inheritance protocol       */
#define TA_CEILING      0x03  /* priority ceiling protocol           */

typedef struct {
    unsigned int mtxatr;   /* TA_MTX_TFIFO/TA_MTX_TPRI/TA_INHERIT/TA_CEILING */
    int          ceilpri;  /* ceiling priority (only for TA_CEILING)           */
} PK_CMTX;

/* Create mutex. Returns mtxid (>0) or -1 on error. */
static inline int tk_cre_mtx(PK_CMTX *pk)
    { return __sc(0x130, (int)(long)pk, 0, 0); }

/* Delete mutex. */
static inline int tk_del_mtx(int mtxid)
    { return __sc(0x131, mtxid, 0, 0); }

/* Lock mutex. tmout_ms: -1=forever, 0=poll. */
static inline int tk_loc_mtx(int mtxid, int tmout_ms)
    { return __sc(0x132, mtxid, tmout_ms, 0); }

/* Unlock mutex. */
static inline int tk_unl_mtx(int mtxid)
    { return __sc(0x133, mtxid, 0, 0); }

/* ================================================================= */
/* T-Kernel native: mailbox                                           */
/* ================================================================= */

/* Mailbox message header — MUST be the first field in every message struct. */
typedef struct {
    void *msgque[1];  /* kernel linkage — do not use directly */
} PK_MSG;

/* Mailbox attributes */
#define TA_MBX_TFIFO  0x00  /* wait queue: FIFO           */
#define TA_MBX_TPRI   0x01  /* wait queue: priority order */
#define TA_MFIFO      0x00  /* message queue: FIFO        */
#define TA_MPRI       0x02  /* message queue: priority    */

/* Create mailbox. mbxatr = TA_MBX_TFIFO | TA_MFIFO etc.
 * Returns mbxid (>0) or -1 on error. */
static inline int tk_cre_mbx(unsigned int mbxatr)
    { return __sc(0x140, (int)mbxatr, 0, 0); }

/* Delete mailbox. */
static inline int tk_del_mbx(int mbxid)
    { return __sc(0x141, mbxid, 0, 0); }

/* Send message. pk_msg must point to a struct with PK_MSG as first field. */
static inline int tk_snd_mbx(int mbxid, PK_MSG *pk_msg)
    { return __sc(0x142, mbxid, (int)(long)pk_msg, 0); }

/* Receive message. On success, *ppk_msg points to the received message.
 * tmout_ms: -1=forever, 0=poll. Returns 0 or negative error. */
static inline int tk_rcv_mbx(int mbxid, PK_MSG **ppk_msg, int tmout_ms)
    { return __sc(0x143, mbxid, (int)(long)ppk_msg, tmout_ms); }

/* ================================================================= */
/* T-Kernel native: message buffer                                    */
/* ================================================================= */

typedef struct {
    unsigned int  mbfatr;  /* TA_MBX_TFIFO=0 or TA_MBX_TPRI=1 */
    int           bufsz;   /* buffer size in bytes              */
    int           maxmsz;  /* max message size in bytes         */
    void         *bufptr;  /* user-allocated backing buffer     */
} PK_CMBF;

/* Internal: packed args for sys_snd_mbf (4 params) */
typedef struct {
    int         mbfid;
    const void *msg;
    int         msgsz;
    int         tmout;
} PK_SND_MBF_ARGS;

/* Create message buffer with user-provided backing buffer. Returns mbfid or -1. */
static inline int tk_cre_mbf(PK_CMBF *pk)
{
    struct { unsigned int mbfatr; int bufsz; int maxmsz; unsigned int buf_ptr; } args = {
        pk->mbfatr, pk->bufsz, pk->maxmsz, (unsigned int)(long)pk->bufptr
    };
    return __sc(0x150, (int)(long)&args, 0, 0);
}

/* Delete message buffer. */
static inline int tk_del_mbf(int mbfid)
    { return __sc(0x151, mbfid, 0, 0); }

/* Send message. msgsz must be <= maxmsz specified at creation.
 * tmout_ms: -1=forever, 0=poll. */
static inline int tk_snd_mbf(int mbfid, const void *msg, int msgsz, int tmout_ms)
{
    PK_SND_MBF_ARGS args = { mbfid, msg, msgsz, tmout_ms };
    return __sc(0x152, (int)(long)&args, 0, 0);
}

/* Receive message into buf. Returns received size (>0) or negative error. */
static inline int tk_rcv_mbf(int mbfid, void *buf, int tmout_ms)
    { return __sc(0x153, mbfid, (int)(long)buf, tmout_ms); }

/* ================================================================= */
/* T-Kernel native: variable memory pool                              */
/* ================================================================= */

typedef struct {
    unsigned int  mplatr;  /* TA_MBX_TFIFO=0 or TA_MBX_TPRI=1 */
    int           mplsz;   /* total pool size in bytes          */
    void         *bufptr;  /* user-allocated backing buffer     */
} PK_CMPL;

/* Create variable memory pool. Returns mplid or -1. */
static inline int tk_cre_mpl(PK_CMPL *pk)
{
    struct { unsigned int mplatr; int mplsz; unsigned int buf_ptr; } args = {
        pk->mplatr, pk->mplsz, (unsigned int)(long)pk->bufptr
    };
    return __sc(0x160, (int)(long)&args, 0, 0);
}

/* Delete variable memory pool. */
static inline int tk_del_mpl(int mplid)
    { return __sc(0x161, mplid, 0, 0); }

/* Allocate a block of blksz bytes.
 * Returns block pointer (non-NULL) or NULL/negative on error or timeout. */
static inline void *tk_get_mpl(int mplid, int blksz, int tmout_ms)
    { return (void *)(long)__sc(0x162, mplid, blksz, tmout_ms); }

/* Release an allocated block back to the pool. */
static inline int tk_rel_mpl(int mplid, void *blk)
    { return __sc(0x163, mplid, (int)(long)blk, 0); }

/* ================================================================= */
/* T-Kernel native: fixed memory pool                                 */
/* ================================================================= */

typedef struct {
    unsigned int  mpfatr;  /* TA_MBX_TFIFO=0 or TA_MBX_TPRI=1 */
    int           mpfcnt;  /* number of blocks in pool          */
    int           blfsz;   /* block size in bytes               */
    void         *bufptr;  /* user-allocated backing buffer     */
} PK_CMPF;

/* Create fixed memory pool. Returns mpfid or -1. */
static inline int tk_cre_mpf(PK_CMPF *pk)
{
    struct { unsigned int mpfatr; int mpfcnt; int blfsz; unsigned int buf_ptr; } args = {
        pk->mpfatr, pk->mpfcnt, pk->blfsz, (unsigned int)(long)pk->bufptr
    };
    return __sc(0x168, (int)(long)&args, 0, 0);
}

/* Delete fixed memory pool. */
static inline int tk_del_mpf(int mpfid)
    { return __sc(0x169, mpfid, 0, 0); }

/* Allocate one fixed-size block. tmout_ms: -1=forever, 0=poll.
 * Returns block pointer or NULL on error/timeout. */
static inline void *tk_get_mpf(int mpfid, int tmout_ms)
    { return (void *)(long)__sc(0x16A, mpfid, tmout_ms, 0); }

/* Release a fixed block back to the pool. */
static inline int tk_rel_mpf(int mpfid, void *blf)
    { return __sc(0x16B, mpfid, (int)(long)blf, 0); }

/* ================================================================= */
/* T-Kernel native: cyclic handler                                    */
/* ================================================================= */

/* Cyclic handler attribute flags */
#define TA_CYC_HLNG  0x01  /* high-level language handler (always required) */
#define TA_CYC_STA   0x02  /* start handler immediately after creation      */
#define TA_CYC_PHS   0x04  /* preserve phase on restart                     */

typedef struct {
    unsigned int  cycatr;     /* TA_CYC_STA and/or TA_CYC_PHS            */
    void        (*cychdr)(void); /* handler function (must not block)     */
    int           cyctim_ms;  /* cycle interval in ms (must be > 0)      */
    int           cycphs_ms;  /* initial phase offset in ms              */
} PK_CCYC;

/* Create cyclic handler. Returns cycid or -1. */
static inline int tk_cre_cyc(PK_CCYC *pk)
{
    struct { unsigned int cycatr; unsigned int cychdr;
             int cyctim_ms; int cycphs_ms; } args = {
        pk->cycatr, (unsigned int)(long)pk->cychdr,
        pk->cyctim_ms, pk->cycphs_ms
    };
    return __sc(0x170, (int)(long)&args, 0, 0);
}

/* Delete cyclic handler. */
static inline int tk_del_cyc(int cycid)
    { return __sc(0x171, cycid, 0, 0); }

/* Start (or restart) cyclic handler. */
static inline int tk_sta_cyc(int cycid)
    { return __sc(0x172, cycid, 0, 0); }

/* Stop cyclic handler (can be restarted with tk_sta_cyc). */
static inline int tk_stp_cyc(int cycid)
    { return __sc(0x173, cycid, 0, 0); }

/* ================================================================= */
/* T-Kernel native: alarm handler                                     */
/* ================================================================= */

typedef struct {
    unsigned int  almatr;     /* 0 (no special attributes)               */
    void        (*almhdr)(void); /* handler function (must not block)    */
} PK_CALM;

/* Create alarm handler. Returns almid or -1. */
static inline int tk_cre_alm(PK_CALM *pk)
{
    struct { unsigned int almatr; unsigned int almhdr; } args = {
        pk->almatr, (unsigned int)(long)pk->almhdr
    };
    return __sc(0x178, (int)(long)&args, 0, 0);
}

/* Delete alarm handler. */
static inline int tk_del_alm(int almid)
    { return __sc(0x179, almid, 0, 0); }

/* Start alarm: fires once after almtim_ms milliseconds. */
static inline int tk_sta_alm(int almid, int almtim_ms)
    { return __sc(0x17A, almid, almtim_ms, 0); }

/* Cancel a pending alarm. */
static inline int tk_stp_alm(int almid)
    { return __sc(0x17B, almid, 0, 0); }

/* ================================================================= */
/* T-Kernel native: task supplement                                   */
/* ================================================================= */

/* Force-terminate another task (must be in non-DORMANT state). */
static inline int tk_ter_tsk(int tid)
    { return __sc(0x109, tid, 0, 0); }

/* Suspend task (task remains suspended until tk_rsm_tsk/tk_frsm_tsk). */
static inline int tk_sus_tsk(int tid)
    { return __sc(0x10A, tid, 0, 0); }

/* Resume a suspended task (clears one suspend level). */
static inline int tk_rsm_tsk(int tid)
    { return __sc(0x10B, tid, 0, 0); }

/* Force-resume: clears all suspend levels. */
static inline int tk_frsm_tsk(int tid)
    { return __sc(0x10C, tid, 0, 0); }

/* Release task from wait state (task transitions to READY). */
static inline int tk_rel_wai(int tid)
    { return __sc(0x10D, tid, 0, 0); }

/* Get current running task ID. */
static inline int tk_get_tid(void)
    { return __sc(0x10E, 0, 0, 0); }

/* Cancel pending wakeup requests. Returns cancelled count or error. */
static inline int tk_can_wup(int tid)
    { return __sc(0x10F, tid, 0, 0); }

/* ================================================================= */
/* T-Kernel native: ref (reference state) APIs                        */
/* ================================================================= */

/* Semaphore state */
typedef struct { int wtsk; int semcnt; } PK_REF_SEM;

/* Event flag state */
typedef struct { int wtsk; unsigned int flgptn; } PK_REF_FLG;

/* Mutex state */
typedef struct { int htsk; int wtsk; } PK_REF_MTX;

/* Mailbox state */
typedef struct { int wtsk; } PK_REF_MBX;

/* Message buffer state */
typedef struct { int wtsk; int stsk; int msgsz; int frbufsz; int maxmsz; } PK_REF_MBF;

/* Variable pool state */
typedef struct { int wtsk; int frsz; int maxsz; } PK_REF_MPL;

/* Fixed pool state */
typedef struct { int wtsk; int frbcnt; } PK_REF_MPF;

/* Cyclic handler state (cycstat: 1=running, 0=stopped) */
typedef struct { int lfttim_ms; unsigned int cycstat; } PK_REF_CYC;

/* Alarm handler state (almstat: 1=pending, 0=idle) */
typedef struct { int lfttim_ms; unsigned int almstat; } PK_REF_ALM;

static inline int tk_ref_sem(int semid, PK_REF_SEM *out)
    { return __sc(0x114, semid, (int)(long)out, 0); }

static inline int tk_ref_flg(int flgid, PK_REF_FLG *out)
    { return __sc(0x125, flgid, (int)(long)out, 0); }

static inline int tk_ref_mtx(int mtxid, PK_REF_MTX *out)
    { return __sc(0x134, mtxid, (int)(long)out, 0); }

static inline int tk_ref_mbx(int mbxid, PK_REF_MBX *out)
    { return __sc(0x144, mbxid, (int)(long)out, 0); }

static inline int tk_ref_mbf(int mbfid, PK_REF_MBF *out)
    { return __sc(0x154, mbfid, (int)(long)out, 0); }

static inline int tk_ref_mpl(int mplid, PK_REF_MPL *out)
    { return __sc(0x164, mplid, (int)(long)out, 0); }

static inline int tk_ref_mpf(int mpfid, PK_REF_MPF *out)
    { return __sc(0x16C, mpfid, (int)(long)out, 0); }

static inline int tk_ref_cyc(int cycid, PK_REF_CYC *out)
    { return __sc(0x174, cycid, (int)(long)out, 0); }

static inline int tk_ref_alm(int almid, PK_REF_ALM *out)
    { return __sc(0x17C, almid, (int)(long)out, 0); }

/* ================================================================= */
/* T-Kernel native: time                                              */
/* ================================================================= */

/* System time: hi = upper 32 bits, lo = lower 32 bits (ms since boot) */
typedef struct { int hi; unsigned int lo; } PK_SYSTIM;

/* Get system time. */
static inline int tk_get_tim(PK_SYSTIM *out)
    { return __sc(0x180, (int)(long)out, 0, 0); }

/* Delay current task by delay_ms milliseconds. */
static inline int tk_dly_tsk(int delay_ms)
    { return __sc(0x181, delay_ms, 0, 0); }

/* ================================================================= */
/* T-Kernel native: rendezvous port                                   */
/* ================================================================= */

/* Rendezvous port attribute */
#define TA_RDV_TFIFO  0x00  /* wait queue: FIFO            */
#define TA_RDV_TPRI   0x01  /* wait queue: priority order  */

/* Port creation */
typedef struct {
    unsigned int poratr;  /* TA_RDV_TFIFO or TA_RDV_TPRI */
    int          maxcmsz; /* max call message size (bytes) */
    int          maxrmsz; /* max reply message size (bytes)*/
} PK_CPOR;

/* Port reference result */
typedef struct { int wtsk; int atsk; int maxcmsz; int maxrmsz; } PK_RPOR;

/* Create rendezvous port. Returns porid (>0) or error. */
static inline int tk_cre_por(PK_CPOR *pk)
    { return __sc(0x190, (int)(long)pk, 0, 0); }

/* Delete rendezvous port. */
static inline int tk_del_por(int porid)
    { return __sc(0x191, porid, 0, 0); }

/* Call rendezvous: send msg (cmsgsz bytes), receive reply into same msg buf.
 * calptn selects the call pattern; tmout_ms: -1=forever.
 * Returns reply message size (>=0) or error. */
static inline int tk_cal_por(int porid, unsigned int calptn,
                               void *msg, int cmsgsz, int tmout_ms)
{
    struct { int porid; unsigned int calptn; unsigned int msg_ptr;
             int cmsgsz; int tmout; } args = {
        porid, calptn, (unsigned int)(long)msg, cmsgsz, tmout_ms };
    return __sc(0x192, (int)(long)&args, 0, 0);
}

/* Accept rendezvous: receive call message, fill *p_rdvno with rendezvous ID.
 * acpptn: accepted call patterns (bitfield).
 * msg: buffer to receive call message (maxcmsz bytes).
 * Returns received message size (>=0) or error. */
static inline int tk_acp_por(int porid, unsigned int acpptn,
                               unsigned int *p_rdvno, void *msg, int tmout_ms)
{
    struct { int porid; unsigned int acpptn; unsigned int p_rdvno;
             unsigned int msg_ptr; int tmout; } args = {
        porid, acpptn, (unsigned int)(long)p_rdvno,
        (unsigned int)(long)msg, tmout_ms };
    return __sc(0x193, (int)(long)&args, 0, 0);
}

/* Forward rendezvous to another port. */
static inline int tk_fwd_por(int porid, unsigned int calptn,
                               unsigned int rdvno, void *msg, int cmsgsz)
{
    struct { int porid; unsigned int calptn; unsigned int rdvno;
             unsigned int msg_ptr; int cmsgsz; } args = {
        porid, calptn, rdvno, (unsigned int)(long)msg, cmsgsz };
    return __sc(0x194, (int)(long)&args, 0, 0);
}

/* Reply to rendezvous: send reply message of rmsgsz bytes. */
static inline int tk_rpl_rdv(unsigned int rdvno, void *msg, int rmsgsz)
    { return __sc(0x195, (int)rdvno, (int)(long)msg, rmsgsz); }

/* ================================================================= */
/* T-Kernel native: system info                                       */
/* ================================================================= */

/* Version info */
typedef struct {
    unsigned short maker;   /* OS manufacturer code */
    unsigned short prid;    /* OS ID                */
    unsigned short spver;   /* spec version         */
    unsigned short prver;   /* product version      */
    unsigned short prno[4]; /* product number       */
} PK_RVER;

/* System state */
typedef struct {
    unsigned int sysstat;    /* system state flags        */
    int          runtskid;   /* running task ID           */
    int          schedtskid; /* scheduled task ID         */
} PK_RSYS;

/* Get T-Kernel version info. */
static inline int tk_ref_ver(PK_RVER *out)
    { return __sc(0x1A0, (int)(long)out, 0, 0); }

/* Get system state. */
static inline int tk_ref_sys(PK_RSYS *out)
    { return __sc(0x1A1, (int)(long)out, 0, 0); }

/* ================================================================= */
/* T-Kernel native: rendezvous port ref (supplement)                  */
/* ================================================================= */

static inline int tk_ref_por(int porid, PK_RPOR *out)
    { return __sc(0x196, porid, (int)(long)out, 0); }

/* ================================================================= */
/* T-Kernel native: time supplement                                   */
/* ================================================================= */

/* Get operational (monotonic) time — unaffected by tk_set_tim. */
static inline int tk_get_otm(PK_SYSTIM *out)
    { return __sc(0x182, (int)(long)out, 0, 0); }

/* Set system time. */
static inline int tk_set_tim(PK_SYSTIM *in)
    { return __sc(0x183, (int)(long)in, 0, 0); }

/* ================================================================= */
/* T-Kernel native: task dispatch control                             */
/* ================================================================= */

/* Exit current task AND delete its TCB (combined ext+del). */
static inline void tk_exd_tsk(void)
{
    __sc(0x1C0, 0, 0, 0);
    for (;;) asm volatile("hlt");
}

/* Disable task dispatch (non-preemptive section begin). */
static inline int tk_dis_dsp(void)
    { return __sc(0x1C1, 0, 0, 0); }

/* Enable task dispatch (non-preemptive section end). */
static inline int tk_ena_dsp(void)
    { return __sc(0x1C2, 0, 0, 0); }

/* Rotate ready queue at priority tskpri (0 = own priority).
 * Moves the running task to the tail — cooperative yield within
 * same-priority group. */
static inline int tk_rot_rdq(int tskpri)
    { return __sc(0x1C3, tskpri, 0, 0); }

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
