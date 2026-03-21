/*
 *  syscall.c (x86)
 *  System call handler — INT 0x80 dispatcher
 *
 *  Syscalls:
 *    POSIX-compatible file I/O  (1, 3-10)
 *    T-Kernel native API        (0x100+)
 */

#include "kernel.h"
#include "task.h"
#include "p_syscall.h"
#include "vfs.h"
#include "netstack.h"
#include "ai_kernel.h"
#include <syscall.h>       /* tk_cre_tsk, tk_sta_tsk, ... */
#include <tmonitor.h>

/* idt_set_gate() lives in boot/x86/idt.c */
IMPORT void idt_set_gate(UB num, unsigned long long handler,
                          UH sel, UB flags);
#define KERNEL64_CS  0x18u

extern void syscall_isr(void);

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ----------------------------------------------------------------- */
/* UDP receive buffer pool (SYS_UDP_BIND / SYS_UDP_RECV)            */
/*                                                                   */
/* When user-space calls SYS_UDP_BIND, a kernel-side slot is        */
/* allocated. Arriving packets are copied into the slot buffer and  */
/* the rx_sem is signalled. SYS_UDP_RECV waits on rx_sem and then  */
/* copies to user-space.                                             */
/* ----------------------------------------------------------------- */
#define UDP_BIND_MAX    4
#define UDP_RECV_BUFSZ  512

typedef struct {
    UH   port;
    UB   in_use;
    ID   rx_sem;
    UW   src_ip;
    UH   src_port;
    UH   data_len;
    UB   data[UDP_RECV_BUFSZ];
} USR_UDP_SLOT;

static USR_UDP_SLOT usr_udp[UDP_BIND_MAX];

/* Forward declarations for the per-slot UDP callbacks */
static void usr_udp_rx(INT slot, UW src_ip, UH src_port,
                        const UB *data, UH len);
static void usr_udp_rx0(UW i, UH p, const UB *d, UH l) { usr_udp_rx(0,i,p,d,l); }
static void usr_udp_rx1(UW i, UH p, const UB *d, UH l) { usr_udp_rx(1,i,p,d,l); }
static void usr_udp_rx2(UW i, UH p, const UB *d, UH l) { usr_udp_rx(2,i,p,d,l); }
static void usr_udp_rx3(UW i, UH p, const UB *d, UH l) { usr_udp_rx(3,i,p,d,l); }

static const udp_recv_fn usr_udp_cbs[UDP_BIND_MAX] = {
    usr_udp_rx0, usr_udp_rx1, usr_udp_rx2, usr_udp_rx3
};

static void usr_udp_rx(INT slot, UW src_ip, UH src_port,
                        const UB *data, UH len)
{
    USR_UDP_SLOT *s = &usr_udp[slot];
    if (!s->in_use) return;
    if (len > UDP_RECV_BUFSZ) len = (UH)UDP_RECV_BUFSZ;
    s->src_ip   = src_ip;
    s->src_port = src_port;
    s->data_len = len;
    for (UH i = 0; i < len; i++) s->data[i] = data[i];
    tk_sig_sem(s->rx_sem, 1);
}

/* ----------------------------------------------------------------- */
/* Async AI job pool (SYS_AI_SUBMIT / SYS_AI_WAIT)                  */
/* ----------------------------------------------------------------- */
#define USR_AI_MAX  4

typedef struct {
    UB   in_use;
    ID   jid;       /* kernel AI job ID                              */
    ID   in_tid;    /* input  tensor (4-byte int8 FLAT)             */
    ID   out_tid;   /* output tensor (1-byte int8 = class)          */
} USR_AI_JOB;

static USR_AI_JOB usr_ai[USR_AI_MAX];

/* ----------------------------------------------------------------- */
/* stdin relay buffer (SYS_READ fd=0)                               */
/*                                                                   */
/* When a user ELF is executing, shell_task forwards each serial     */
/* character to stdin_feed().  SYS_READ(fd=0) waits on stdin_sem    */
/* and copies bytes out of the ring buffer.                          */
/* SYS_EXIT signals stdin_exit_sem to unblock the relay loop.       */
/* ----------------------------------------------------------------- */
#define STDIN_BUFSZ  256

static ID   stdin_sem;         /* count = bytes available in ring buffer */
static ID   stdin_exit_sem;    /* signalled once when user ELF calls SYS_EXIT */
static BOOL stdin_active;      /* TRUE while a user ELF is running         */
static UB   stdin_rbuf[STDIN_BUFSZ];
static volatile UB stdin_wptr; /* next write index (wraps at 256)          */
static volatile UB stdin_rptr; /* next read  index (wraps at 256)          */

/* Called from shell.c before elf_exec() */
void stdin_activate(void)
{
    /* Drain any stale semaphore tokens */
    while (tk_wai_sem(stdin_sem,      1, TMO_POL) == E_OK) {}
    while (tk_wai_sem(stdin_exit_sem, 1, TMO_POL) == E_OK) {}
    stdin_wptr = 0;
    stdin_rptr = 0;
    stdin_active = TRUE;
}

/* Called from shell.c on elf_exec() failure */
void stdin_deactivate(void) { stdin_active = FALSE; }

/* Called from shell.c relay loop to push one serial char into stdin */
void stdin_feed(UB c)
{
    if (!stdin_active) return;
    stdin_rbuf[stdin_wptr++] = c;   /* wptr wraps naturally (UB, 256) */
    tk_sig_sem(stdin_sem, 1);
}

/* Returns the exit semaphore ID so shell.c can poll it */
ID stdin_get_exit_sem(void) { return stdin_exit_sem; }

/* ----------------------------------------------------------------- */
/* TCP connection handle table (SYS_TCP_CONNECT / WRITE / READ / CLOSE) */
/* ----------------------------------------------------------------- */
#define USR_TCP_MAX  4

static TCP_CONN *usr_tcp[USR_TCP_MAX];

/* ----------------------------------------------------------------- */
/* Stack pool for user-created tasks (8 slots × 4 KiB)              */
/* ----------------------------------------------------------------- */
#define USR_TASK_MAX   8
#define USR_TASK_STKSZ 4096

static UB   usr_stacks[USR_TASK_MAX][USR_TASK_STKSZ];
static BOOL usr_stack_inuse[USR_TASK_MAX];

static void *alloc_user_stack(void)
{
    for (INT i = 0; i < USR_TASK_MAX; i++) {
        if (!usr_stack_inuse[i]) {
            usr_stack_inuse[i] = TRUE;
            return usr_stacks[i];
        }
    }
    return NULL;
}

/* Called when a user-created task exits to free its stack slot */
static void free_user_stack(void *stk)
{
    for (INT i = 0; i < USR_TASK_MAX; i++) {
        if (usr_stacks[i] == (UB *)stk) {
            usr_stack_inuse[i] = FALSE;
            return;
        }
    }
}

/* Wrapper that frees stack then calls tk_ext_tsk */
static void user_task_wrapper(INT stacd, void *exinf);

typedef struct {
    FP    real_task;
    void *real_exinf;
    void *stack_base;
    ID    tskid;        /* kernel task ID — used for cleanup on ext_tsk */
} USR_TASK_CTX;

static USR_TASK_CTX usr_ctx[USR_TASK_MAX];

static void user_task_wrapper(INT stacd, void *exinf)
{
    USR_TASK_CTX *ctx = (USR_TASK_CTX *)exinf;
    ctx->real_task(stacd, ctx->real_exinf);
    /* real_task returned normally (did NOT call tk_ext_tsk via syscall) */
    void *stk = ctx->stack_base;
    ctx->real_task = NULL;   /* free ctx slot */
    free_user_stack(stk);
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* syscall_init                                                       */
/* ----------------------------------------------------------------- */
void syscall_init(void)
{
    for (INT i = 0; i < USR_TASK_MAX; i++) usr_stack_inuse[i] = FALSE;
    for (INT i = 0; i < UDP_BIND_MAX;  i++) usr_udp[i].in_use = 0;
    for (INT i = 0; i < USR_AI_MAX;    i++) usr_ai[i].in_use  = 0;
    for (INT i = 0; i < USR_TCP_MAX;   i++) usr_tcp[i]        = NULL;

    /* stdin semaphores */
    {
        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                      .isemcnt = 0, .maxsem = STDIN_BUFSZ };
        stdin_sem = tk_cre_sem(&cs);
        T_CSEM cs2 = { .exinf = NULL, .sematr = TA_TFIFO,
                       .isemcnt = 0, .maxsem = 1 };
        stdin_exit_sem = tk_cre_sem(&cs2);
    }
    stdin_active = FALSE;

    idt_set_gate(0x80,
                 (unsigned long long)(UW)syscall_isr,
                 (UH)KERNEL64_CS,
                 0xEF);
    tm_putstring((UB *)"[syscall] int 0x80 registered (ring3 trap gate)\r\n");
}

/* ----------------------------------------------------------------- */
/* Serial helpers                                                     */
/* ----------------------------------------------------------------- */
static void serial_write(const char *buf, W len)
{
    for (W i = 0; i < len; i++)
        sio_send_frame((const UB *)&buf[i], 1);
}

static void sout_num(W n)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (n == 0) { buf[--i] = '0'; }
    else {
        if (n < 0) { tm_putstring((UB *)"-"); n = -n; }
        while (n > 0 && i > 0) { buf[--i] = (char)('0' + n%10); n /= 10; }
    }
    tm_putstring((UB *)(buf+i));
}

/* ----------------------------------------------------------------- */
/* FD translation: POSIX fd 3..10 → VFS fd 0..7                     */
/* fd 0 = stdin (not impl), fd 1/2 = serial stdout/stderr           */
/* ----------------------------------------------------------------- */
#define POSIX_FD_OFFSET   3
#define IS_STD_FD(fd)     ((fd) == 0 || (fd) == 1 || (fd) == 2)
#define TO_VFS_FD(fd)     ((fd) - POSIX_FD_OFFSET)
#define TO_POSIX_FD(vfd)  ((vfd) + POSIX_FD_OFFSET)

/* ----------------------------------------------------------------- */
/* syscall_dispatch                                                   */
/* ----------------------------------------------------------------- */
W syscall_dispatch(W nr, W arg0, W arg1, W arg2)
{
    switch (nr) {

    /* ------------------------------------------------------------- */
    /* POSIX file I/O                                                 */
    /* ------------------------------------------------------------- */

    case SYS_WRITE: {
        /* arg0=fd, arg1=buf_ptr, arg2=len */
        const char *buf = (const char *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        if (IS_STD_FD(arg0)) {
            serial_write(buf, len);
            return len;
        }
        if (!vfs_ready) return -1;
        return vfs_write(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_READ: {
        /* arg0=fd, arg1=buf_ptr, arg2=len */
        void *buf = (void *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        if (arg0 == 0) {
            /* stdin: block until at least one byte is available */
            if (!stdin_active || len == 0) return -1;
            if (tk_wai_sem(stdin_sem, 1, TMO_FEVR) != E_OK) return -1;
            UB *dst = (UB *)buf;
            INT n = 0;
            dst[n++] = stdin_rbuf[stdin_rptr++];  /* first byte */
            /* Greedily read additional bytes that are already available */
            while (n < len && tk_wai_sem(stdin_sem, 1, TMO_POL) == E_OK)
                dst[n++] = stdin_rbuf[stdin_rptr++];
            return (W)n;
        }
        if (IS_STD_FD(arg0)) return -1;
        if (!vfs_ready) return -1;
        return vfs_read(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_OPEN: {
        /* arg0=path_ptr, arg1=flags */
        const char *path = (const char *)(UW)arg0;
        W flags = arg1;
        if (!vfs_ready) return -1;
        INT vfd;
        if (flags & 0x0041) {  /* O_WRONLY | O_CREAT */
            vfd = vfs_create(path);
        } else {
            vfd = vfs_open(path);
        }
        if (vfd < 0) return -1;
        return TO_POSIX_FD(vfd);
    }

    case SYS_CLOSE: {
        /* arg0=fd */
        if (IS_STD_FD(arg0)) return 0;
        if (!vfs_ready) return -1;
        vfs_close(TO_VFS_FD(arg0));
        return 0;
    }

    case SYS_LSEEK: {
        /* arg0=fd, arg1=offset, arg2=whence (0=SET,1=CUR,2=END) */
        if (IS_STD_FD(arg0)) return -1;
        if (!vfs_ready) return -1;
        /* Only SEEK_SET (0) and SEEK_END (2) supported via vfs_seek */
        UW off = (UW)arg1;
        if (arg2 == 2) off = vfs_fsize(TO_VFS_FD(arg0));  /* SEEK_END */
        return vfs_seek(TO_VFS_FD(arg0), off);
    }

    case SYS_MKDIR: {
        /* arg0=path_ptr */
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        return vfs_mkdir(path);
    }

    case SYS_UNLINK: {
        /* arg0=path_ptr */
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        return vfs_unlink(path);
    }

    case SYS_RENAME: {
        /* arg0=old_ptr, arg1=new_ptr */
        const char *old = (const char *)(UW)arg0;
        const char *nw  = (const char *)(UW)arg1;
        if (!vfs_ready) return -1;
        return vfs_rename(old, nw);
    }

    case SYS_EXIT: {
        /* arg0 = exit code */
        tm_putstring((UB *)"\r\n[proc] exited (code=");
        sout_num(arg0);
        tm_putstring((UB *)")\r\np-kernel> ");
        /* Unblock shell relay loop */
        if (stdin_active) {
            stdin_active = FALSE;
            tk_sig_sem(stdin_exit_sem, 1);
        }
        tk_ext_tsk();
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: task management                              */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_TSK: {
        /* arg0 = PK_CRE_TSK* */
        PK_CRE_TSK *pk = (PK_CRE_TSK *)(UW)arg0;
        if (!pk || !pk->task) return -1;

        /* find a free stack slot */
        void *stk = alloc_user_stack();
        if (!stk) return -1;

        /* find a free ctx slot */
        INT ci = -1;
        for (INT i = 0; i < USR_TASK_MAX; i++) {
            if (usr_ctx[i].real_task == NULL) { ci = i; break; }
        }
        if (ci < 0) { free_user_stack(stk); return -1; }

        usr_ctx[ci].real_task  = pk->task;
        usr_ctx[ci].real_exinf = pk->exinf;
        usr_ctx[ci].stack_base = stk;

        INT pri = (pk->pri > 0 && pk->pri < NUM_PRI) ? pk->pri : NUM_PRI/2;
        INT stksz = (pk->stksz > 0) ? pk->stksz : USR_TASK_STKSZ;
        if (stksz > USR_TASK_STKSZ) stksz = USR_TASK_STKSZ;

        T_CTSK ctsk;
        ctsk.exinf    = &usr_ctx[ci];
        ctsk.tskatr   = TA_HLNG | TA_USERBUF;
        ctsk.task     = user_task_wrapper;
        ctsk.itskpri  = (PRI)pri;
        ctsk.stksz    = (W)stksz;
        ctsk.bufptr   = stk;
#if USE_OBJECT_NAME
        ctsk.dsname[0] = '\0';
#endif

        ID tid = tk_cre_tsk(&ctsk);
        if (tid < 0) {
            usr_ctx[ci].real_task = NULL;
            free_user_stack(stk);
            return -1;
        }
        usr_ctx[ci].tskid = tid;

        /* Apply scheduling policy */
        TCB *tcb = get_tcb(tid);
        INT policy = (pk->policy == SCHED_RR) ? SCHED_RR : SCHED_FIFO;
        tcb->sched_policy = (UB)policy;
        if (policy == SCHED_RR) {
            INT ticks = (pk->slice_ms > 0)
                        ? (pk->slice_ms + 9) / 10   /* ms→ticks (10ms/tick) */
                        : DEFAULT_TIME_SLICE;
            if (ticks < 1)  ticks = 1;
            if (ticks > 255) ticks = 255;
            tcb->time_slice      = (UH)ticks;
            tcb->remaining_slice = (UH)ticks;
        }

        return (W)tid;
    }

    case SYS_TK_STA_TSK: {
        /* arg0=tid, arg1=stacd */
        return (W)tk_sta_tsk((ID)arg0, (INT)arg1);
    }

    case SYS_TK_EXT_TSK: {
        /* Free stack/ctx for user-created tasks before exiting */
        ID cur = knl_ctxtsk->tskid;
        for (INT i = 0; i < USR_TASK_MAX; i++) {
            if (usr_ctx[i].real_task != NULL && usr_ctx[i].tskid == cur) {
                free_user_stack(usr_ctx[i].stack_base);
                usr_ctx[i].real_task = NULL;
                break;
            }
        }
        tk_ext_tsk();
        return 0;
    }

    case SYS_TK_SLP_TSK: {
        /* arg0=timeout_ms (-1=forever) */
        return (W)tk_slp_tsk((TMO)arg0);
    }

    case SYS_TK_WUP_TSK: {
        /* arg0=tid */
        return (W)tk_wup_tsk((ID)arg0);
    }

    case SYS_TK_CHG_PRI: {
        /* arg0=tid, arg1=new_priority */
        return (W)tk_chg_pri((ID)arg0, (PRI)arg1);
    }

    case SYS_TK_CHG_SLT: {
        /* arg0=tid, arg1=slice_ms */
        ID tid = (ID)arg0;
        INT ticks = ((INT)arg1 + 9) / 10;
        if (ticks < 1)   ticks = 1;
        if (ticks > 255) ticks = 255;
        TCB *tcb = get_tcb(tid);
        tcb->time_slice      = (UH)ticks;
        tcb->remaining_slice = (UH)ticks;
        return 0;
    }

    case SYS_TK_DEL_TSK: {
        /* arg0=tid — delete a DORMANT task and free its TCB slot */
        return (W)tk_del_tsk((ID)arg0);
    }

    case SYS_TK_REF_TSK: {
        /* arg0=tid, arg1=PK_REF_TSK* */
        ID tid = (ID)arg0;
        PK_REF_TSK *out = (PK_REF_TSK *)(UW)arg1;
        if (!out) return -1;
        T_RTSK rtsk;
        W r = (W)tk_ref_tsk(tid, &rtsk);
        if (r < 0) return r;
        TCB *tcb = get_tcb(tid);
        out->pri      = (INT)rtsk.tskpri;
        out->state    = (INT)rtsk.tskstat;
        out->policy   = (INT)tcb->sched_policy;
        out->slice_ms = (INT)tcb->time_slice * 10;
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: semaphore                                    */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_SEM: {
        /* arg0 = PK_CSEM* { exinf, isemcnt, maxsem }
         * Build a proper T_CSEM (has sematr field between exinf and isemcnt).
         * TA_WMUL (0x08) allows cnt > 1 in tk_wai_sem / tk_sig_sem. */
        struct { void *exinf; INT isemcnt; INT maxsem; } *upk =
            (void *)(UW)arg0;
        T_CSEM pk;
        pk.exinf   = upk->exinf;
        pk.sematr  = TA_TFIFO;  /* 0: FIFO queue, cnt>1 allowed without TA_WMUL */
        pk.isemcnt = upk->isemcnt;
        pk.maxsem  = upk->maxsem;
        return (W)tk_cre_sem(&pk);
    }

    case SYS_TK_DEL_SEM: {
        return (W)tk_del_sem((ID)arg0);
    }

    case SYS_TK_SIG_SEM: {
        /* arg0=semid, arg1=cnt */
        return (W)tk_sig_sem((ID)arg0, (INT)arg1);
    }

    case SYS_TK_WAI_SEM: {
        /* arg0=semid, arg1=cnt, arg2=timeout_ms */
        return (W)tk_wai_sem((ID)arg0, (INT)arg1, (TMO)arg2);
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: event flag                                   */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_FLG: {
        /* arg0 = PK_CFLG* { exinf, iflgptn }
         * Build proper T_CFLG (has flgatr field between exinf and iflgptn). */
        struct { void *exinf; UINT iflgptn; } *upk =
            (void *)(UW)arg0;
        T_CFLG pk;
        pk.exinf   = upk->exinf;
        pk.flgatr  = TA_TFIFO | TA_WMUL;
        pk.iflgptn = upk->iflgptn;
        return (W)tk_cre_flg(&pk);
    }

    case SYS_TK_DEL_FLG: {
        return (W)tk_del_flg((ID)arg0);
    }

    case SYS_TK_SET_FLG: {
        /* arg0=flgid, arg1=setptn */
        return (W)tk_set_flg((ID)arg0, (UINT)arg1);
    }

    case SYS_TK_CLR_FLG: {
        /* arg0=flgid, arg1=clrptn */
        return (W)tk_clr_flg((ID)arg0, (UINT)arg1);
    }

    case SYS_TK_WAI_FLG: {
        /* arg0 = PK_WAI_FLG* */
        PK_WAI_FLG *pk = (PK_WAI_FLG *)(UW)arg0;
        if (!pk) return -1;
        UINT flgptn = 0;
        W r = (W)tk_wai_flg(pk->flgid, pk->waiptn, pk->wfmode,
                             &flgptn, pk->tmout);
        if (pk->p_flgptn) *pk->p_flgptn = flgptn;
        return r;
    }

    /* ------------------------------------------------------------- */
    /* UDP network syscalls                                          */
    /* ------------------------------------------------------------- */

    case SYS_UDP_BIND: {
        /* arg0 = port number */
        UH port = (UH)arg0;

        /* Find a free slot */
        INT slot = -1;
        for (INT i = 0; i < UDP_BIND_MAX; i++) {
            if (!usr_udp[i].in_use) { slot = i; break; }
        }
        if (slot < 0) return -1;

        /* Create receive semaphore */
        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                      .isemcnt = 0, .maxsem = 16 };
        ID sem = tk_cre_sem(&cs);
        if (sem < E_OK) return -1;

        /* Register with netstack */
        if (udp_bind(port, usr_udp_cbs[slot]) != 0) {
            tk_del_sem(sem);
            return -1;
        }

        usr_udp[slot].port     = port;
        usr_udp[slot].rx_sem   = sem;
        usr_udp[slot].data_len = 0;
        usr_udp[slot].in_use   = 1;
        return 0;
    }

    case SYS_UDP_SEND: {
        /* arg0 = PK_UDP_SEND* */
        PK_UDP_SEND *pk = (PK_UDP_SEND *)(UW)arg0;
        if (!pk) return -1;
        return udp_send(pk->dst_ip, pk->src_port, pk->dst_port,
                        (const UB *)pk->buf_ptr, pk->len);
    }

    case SYS_UDP_RECV: {
        /* arg0 = PK_UDP_RECV* (IN/OUT) */
        PK_UDP_RECV *pk = (PK_UDP_RECV *)(UW)arg0;
        if (!pk) return -1;

        /* Find bound slot for this port */
        INT slot = -1;
        for (INT i = 0; i < UDP_BIND_MAX; i++) {
            if (usr_udp[i].in_use && usr_udp[i].port == pk->port) {
                slot = i; break;
            }
        }
        if (slot < 0) return -1;

        /* Wait for a packet */
        ER er = tk_wai_sem(usr_udp[slot].rx_sem, 1, (TMO)pk->timeout_ms);
        if (er != E_OK) return (W)er;   /* E_TMOUT = -50 */

        /* Copy to user buffer */
        UH dlen = usr_udp[slot].data_len;
        if (dlen > pk->buflen) dlen = pk->buflen;
        UB *dst = (UB *)(UW)pk->buf_ptr;
        for (UH i = 0; i < dlen; i++) dst[i] = usr_udp[slot].data[i];

        pk->src_ip   = usr_udp[slot].src_ip;
        pk->src_port = usr_udp[slot].src_port;
        pk->data_len = dlen;
        return (W)dlen;
    }

    /* ------------------------------------------------------------- */
    /* AI inference syscalls                                         */
    /* ------------------------------------------------------------- */

    case SYS_INFER: {
        /* arg0 = sensor_packed (SENSOR_PACK(t,h,p,l)) */
        B input[MLP_IN] = {
            SENSOR_UNPACK_T(arg0),
            SENSOR_UNPACK_H(arg0),
            SENSOR_UNPACK_P(arg0),
            SENSOR_UNPACK_L(arg0),
        };
        return (W)mlp_forward(input);
    }

    case SYS_AI_SUBMIT: {
        /* arg0 = sensor_packed — submits a job to the AI worker task */
        /* Find a free user AI slot */
        INT slot = -1;
        for (INT i = 0; i < USR_AI_MAX; i++) {
            if (!usr_ai[i].in_use) { slot = i; break; }
        }
        if (slot < 0) return -1;

        /* Create input tensor: shape [4], dtype int8 */
        UW shape_in[1] = { (UW)MLP_IN };
        ID in_tid = tk_cre_tensor(1, shape_in, TENSOR_DTYPE_I8,
                                  TENSOR_LAYOUT_FLAT);
        if (in_tid < E_OK) return -1;

        /* Write sensor data */
        B input[MLP_IN] = {
            SENSOR_UNPACK_T(arg0),
            SENSOR_UNPACK_H(arg0),
            SENSOR_UNPACK_P(arg0),
            SENSOR_UNPACK_L(arg0),
        };
        tk_tensor_write(in_tid, 0, input, MLP_IN);

        /* Create output tensor: shape [1], dtype int8 */
        UW shape_out[1] = { 1 };
        ID out_tid = tk_cre_tensor(1, shape_out, TENSOR_DTYPE_I8,
                                   TENSOR_LAYOUT_FLAT);
        if (out_tid < E_OK) {
            tk_del_tensor(in_tid);
            return -1;
        }

        /* Submit AI job */
        AI_JOB_SPEC spec;
        spec.op         = AI_OP_MLP_FWD;
        spec.model_id   = MODEL_SENSOR_CLS;
        spec.input_tid  = in_tid;
        spec.output_tid = out_tid;
        spec.param[0]   = 0;
        spec.param[1]   = 0;

        ID jid = tk_cre_ai_job(&spec);
        if (jid < E_OK) {
            tk_del_tensor(in_tid);
            tk_del_tensor(out_tid);
            return -1;
        }

        usr_ai[slot].jid     = jid;
        usr_ai[slot].in_tid  = in_tid;
        usr_ai[slot].out_tid = out_tid;
        usr_ai[slot].in_use  = 1;
        return slot;
    }

    case SYS_AI_WAIT: {
        /* arg0 = slot (returned by SYS_AI_SUBMIT), arg1 = timeout_ms */
        INT slot = (INT)arg0;
        if (slot < 0 || slot >= USR_AI_MAX) return -1;
        if (!usr_ai[slot].in_use) return -1;

        ER er = tk_wai_ai_job(usr_ai[slot].jid, (TMO)arg1);

        W result = -1;
        if (er == E_OK) {
            /* Read class from output tensor */
            B cls = 0;
            tk_tensor_read(usr_ai[slot].out_tid, 0, &cls, 1);
            result = (W)(UB)cls;
        }

        /* Cleanup */
        tk_del_ai_job(usr_ai[slot].jid);
        tk_del_tensor(usr_ai[slot].in_tid);
        tk_del_tensor(usr_ai[slot].out_tid);
        usr_ai[slot].in_use = 0;

        return (er == E_OK) ? result : (W)er;
    }

    /* ------------------------------------------------------------- */
    /* TCP network syscalls                                          */
    /* ------------------------------------------------------------- */

    case SYS_TCP_CONNECT: {
        /* arg0 = PK_TCP_CONNECT* */
        PK_TCP_CONNECT *pk = (PK_TCP_CONNECT *)(UW)arg0;
        if (!pk) return -1;

        /* Find a free TCP handle slot */
        INT slot = -1;
        for (INT i = 0; i < USR_TCP_MAX; i++) {
            if (!usr_tcp[i]) { slot = i; break; }
        }
        if (slot < 0) return -1;

        TCP_CONN *conn = NULL;
        if (tcp_connect(pk->dst_ip, pk->dst_port, &conn) < 0 || !conn)
            return -1;

        usr_tcp[slot] = conn;
        return slot;
    }

    case SYS_TCP_WRITE: {
        /* arg0=handle, arg1=buf_ptr, arg2=len */
        INT h = (INT)arg0;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h]) return -1;
        return tcp_write(usr_tcp[h], (const UB *)(UW)arg1, (UH)arg2);
    }

    case SYS_TCP_READ: {
        /* arg0 = PK_TCP_READ* */
        PK_TCP_READ *pk = (PK_TCP_READ *)(UW)arg0;
        if (!pk) return -1;
        INT h = pk->handle;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h]) return -1;
        return tcp_read(usr_tcp[h], (UB *)(UW)pk->buf_ptr,
                        pk->buflen, pk->timeout_ms);
    }

    case SYS_TCP_CLOSE: {
        /* arg0 = handle */
        INT h = (INT)arg0;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h]) return -1;
        tcp_close(usr_tcp[h]);
        tcp_free(usr_tcp[h]);
        usr_tcp[h] = NULL;
        return 0;
    }

    default:
        return -1;  /* ENOSYS */
    }
}
