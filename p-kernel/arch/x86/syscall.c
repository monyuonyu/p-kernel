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
#include <syscall.h>       /* tk_cre_tsk, tk_sta_tsk, ... */
#include <tmonitor.h>

/* idt_set_gate() lives in boot/x86/idt.c */
IMPORT void idt_set_gate(UB num, unsigned long long handler,
                          UH sel, UB flags);
#define KERNEL64_CS  0x18u

extern void syscall_isr(void);

IMPORT void sio_send_frame(const UB *buf, INT size);

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
} USR_TASK_CTX;

static USR_TASK_CTX usr_ctx[USR_TASK_MAX];

static void user_task_wrapper(INT stacd, void *exinf)
{
    USR_TASK_CTX *ctx = (USR_TASK_CTX *)exinf;
    ctx->real_task(stacd, ctx->real_exinf);
    void *stk = ctx->stack_base;
    free_user_stack(stk);
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* syscall_init                                                       */
/* ----------------------------------------------------------------- */
void syscall_init(void)
{
    for (INT i = 0; i < USR_TASK_MAX; i++) usr_stack_inuse[i] = FALSE;

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
        if (arg0 == 0) return -1;  /* stdin not implemented */
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

    default:
        return -1;  /* ENOSYS */
    }
}
