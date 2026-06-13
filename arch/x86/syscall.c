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
#include "netstack.h"
#include "net_ssy.h"
#include "fs_ssy.h"
#include "vfs.h"
#include "ai_kernel.h"
#include "paging.h"
#include "fpu.h"
#include "gdt_user.h"
#include "user_range.h"   /* ISO-USERPTR — user_range_ok() kernel-range guard */
#include "kdds.h"
#include "edf.h"
#include "dtr.h"
#include "moe.h"
#include "world.h"     /* ring3-core Wave C: SYS_MIND_NOTE → world_note_firing */
#include "reflex.h"    /* ring3-core Wave C: SYS_MIND_NOTE → reflex_on_inference */
#include "drpc.h"      /* ring3-core Wave C: drpc_my_node for the reflex hook */
#include <syscall.h>       /* tk_cre_tsk, tk_sta_tsk, ... */
#include <subsystem.h>     /* SSYCB, knl_ssy_cleanup */

/* knl_svc_ientry: routes pk_para to the subsystem whose ID = fncd & 0xFF */
IMPORT ER knl_svc_ientry(void *pk_para, FN fncd);
#include <tmonitor.h>

/* idt_set_gate() lives in boot/x86/idt.c */
IMPORT void idt_set_gate(UB num, unsigned long long handler,
                          UH sel, UB flags);
#define KERNEL64_CS  0x18u

extern void syscall_isr(void);

IMPORT void sio_send_frame(const UB *buf, INT size);

/* File I/O: SYS_OPEN and SYS_CLOSE route through fs_ssy_call() so that
 * fs_ssy.c can track per-task FD ownership (cleanupfn closes leaked fds).
 * SYS_READ/WRITE/LSEEK/MKDIR/UNLINK/RENAME/READDIR go direct to VFS.
 * UDP/TCP state lives in net_ssy.c (ssid=1). */

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
/* ring3-core Wave B — user-process unwind (shared by SYS_EXIT and   */
/* the ring-3 fault reap)                                            */
/*                                                                   */
/* user_last_exit is the gate-1 class channel: core_moe.elf passes   */
/* the class it received from SYS_INFER back as its SYS_EXIT code,   */
/* so the value makes the full round trip                            */
/*   kernel moe_infer → ring3 EAX → user code → SYS_EXIT(arg0) →    */
/*   here → shell `ring3 test` (via user_last_exit_code()).          */
/* The shell verb therefore cannot be greened by a kernel-side       */
/* recording alone — the ring-3 task must really have run and        */
/* returned the value.                                               */
/* ----------------------------------------------------------------- */
#define USER_EXIT_NONE   (-9999)   /* no user task has exited yet     */
#define USER_EXIT_FAULT  (-86)     /* task was reaped by a ring3 fault */

static W user_last_exit = USER_EXIT_NONE;

W user_last_exit_code(void) { return user_last_exit; }

/* Context-independent teardown of a user process's kernel-side
 * resources: subsystem cleanup (sockets, fds), shell-relay unblock,
 * page-table destroy, FPU slot reset.
 *
 * Debt wave (RING3-B follow-up): this is the part of the old
 * user_proc_unwind() that does NOT need to run in the victim's own
 * context, factored out so the KILLER (dproc_kill_by_name) can call
 * it on a foreign task.  Contract: when called on a foreign tid the
 * victim must already be stopped (tk_ter_tsk'd) — on this UP kernel
 * the shell/killer outprioritises the launcher tasks, so the victim
 * is never mid-run here.
 *
 * paging_switch(kernel_cr3) before the destroy is required in BOTH
 * contexts: the CPU may currently be running on the victim's CR3
 * (the dispatcher never switches CR3; only user_exec does), and
 * destroying the live tables would hand them to the next exec.      */
void user_proc_teardown(ID tid)
{
    /* Release subsystem resources (sockets, fds, etc.) */
    knl_ssy_cleanup(tid);
    /* Unblock shell relay loop */
    if (stdin_active) {
        stdin_active = FALSE;
        tk_sig_sem(stdin_exit_sem, 1);
    }
    /* Free process page tables and restore kernel address space */
    {
        UW proc_cr3 = paging_get_task_cr3(tid);
        paging_switch(paging_get_kernel_cr3());
        if (proc_cr3 != paging_get_kernel_cr3()) {
            paging_proc_destroy(proc_cr3);
            paging_set_task_cr3(tid, 0);
        }
    }
    /* Drop the per-task FPU image: a reused tid starts clean */
    fpu_task_reset(tid);
}

/* Common tail of SYS_EXIT: full teardown, then exit the task.
 * Runs in the VICTIM's own context (syscall or fault-reap path).
 * NEVER returns (tk_ext_tsk dispatches the next task).              */
static void user_proc_unwind(void)
{
    user_proc_teardown(knl_ctxtsk->tskid);
    tk_ext_tsk();
}

/* Called from exception_handler (boot/x86/idt.c) when a fault came
 * from CS=USER_CS (ring 3): terminate the offending user task via the
 * exact SYS_EXIT unwind and return control to the scheduler.  The
 * kernel survives; the caller's (dead) exception frame is abandoned
 * with the task.  NEVER returns.
 *
 * Honest bound (ring3-core.md II.4): this slice's crash binary faults
 * in PURE USER CODE.  A fault *inside* a syscall (kernel lock held,
 * half-written p-fs block) is NOT handled here — that is a later
 * wave's problem (the existing fs_ssy cleanupfn path is the model). */
void user_fault_reap(void)
{
    user_last_exit = USER_EXIT_FAULT;
    user_proc_unwind();
    /* NOTREACHED */
}

/* TCP handle table has moved to net_ssy.c */

/* ----------------------------------------------------------------- */
/* Pipe table (SYS_PIPE)                                             */
/*                                                                   */
/* POSIX pipe fd numbering:                                          */
/*   read  end = PIPE_FD_BASE + 2*i                                  */
/*   write end = PIPE_FD_BASE + 2*i + 1                              */
/*                                                                   */
/* SYS_READ / SYS_WRITE check IS_PIPE_FD before routing to VFS.     */
/* ----------------------------------------------------------------- */
#define PIPE_MAX      4
#define PIPE_BUFSZ    256
#define PIPE_FD_BASE  32   /* fd 32..39: pipe fds */

typedef struct {
    BOOL in_use;
    ID   sem_data;          /* count = bytes available to read */
    ID   sem_space;         /* count = free bytes in buffer    */
    UB   buf[PIPE_BUFSZ];
    volatile UB wptr;       /* next write index (wraps at 256) */
    volatile UB rptr;       /* next read  index (wraps at 256) */
    BOOL write_end_open;    /* FALSE after write end closed (= EOF) */
} PIPE_SLOT;

static PIPE_SLOT pipes[PIPE_MAX];

#define IS_PIPE_FD(fd)     ((fd) >= PIPE_FD_BASE && (fd) < PIPE_FD_BASE + PIPE_MAX*2)
#define PIPE_IDX(fd)       (((fd) - PIPE_FD_BASE) / 2)
#define IS_PIPE_WRITE(fd)  (((fd) - PIPE_FD_BASE) % 2 == 1)

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
    for (INT i = 0; i < USR_AI_MAX;   i++) usr_ai[i].in_use   = 0;
    /* UDP/TCP state initialised by net_ssy_init() in usermain */

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

    /* Pipe slots */
    for (INT i = 0; i < PIPE_MAX; i++) pipes[i].in_use = FALSE;

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

#define IS_STD_FD(fd)    ((fd) == 0 || (fd) == 1 || (fd) == 2)
#define TO_VFS_FD(fd)    ((fd) - 3)
#define TO_POSIX_FD(vfd) ((vfd) + 3)

/* ----------------------------------------------------------------- */
/* syscall_dispatch                                                   */
/* ----------------------------------------------------------------- */
W syscall_dispatch(W nr, W arg0, W arg1, W arg2)
{
    /* Linux set_robust_list (#258 = 0x102) conflicts with p-kernel
     * SYS_TK_EXT_TSK (0x102).  Stub here to prevent musl's libc init
     * from accidentally triggering tk_ext_tsk() and killing the task. */
    if (nr == 258) return 0;

    switch (nr) {

    /* ------------------------------------------------------------- */
    /* POSIX file I/O                                                 */
    /* ------------------------------------------------------------- */

    case SYS_WRITE: {
        /* fd=1/2 → serial stdout; pipe fd → pipe buf; fd≥3 → VFS */
        const char *buf = (const char *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        /* ISO-USERPTR: the buffer is READ on the user's behalf — reject a
         * kernel-range source pointer (confused deputy). */
        if (!user_range_ok(buf, (UW)len)) return -1;
        if (IS_STD_FD(arg0)) { serial_write(buf, len); return len; }
        if (IS_PIPE_FD(arg0) && IS_PIPE_WRITE(arg0)) {
            INT pi = PIPE_IDX(arg0);
            if (!pipes[pi].in_use) return -1;
            W written = 0;
            while (written < len) {
                if (tk_wai_sem(pipes[pi].sem_space, 1, TMO_FEVR) != E_OK) break;
                pipes[pi].buf[pipes[pi].wptr++] = (UB)buf[written++];
                tk_sig_sem(pipes[pi].sem_data, 1);
            }
            return written;
        }
        if (!vfs_ready) return -1;
        return vfs_write(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_READ: {
        void *buf = (void *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        /* ISO-USERPTR: the kernel WRITES len bytes into buf — a kernel-range
         * destination would let a ring-3 caller scribble over kernel memory. */
        if (!user_range_ok(buf, (UW)len)) return -1;
        if (arg0 == 0) {
            if (!stdin_active || len == 0) return -1;
            if (tk_wai_sem(stdin_sem, 1, TMO_FEVR) != E_OK) return -1;
            UB *dst = (UB *)buf;
            INT n = 0;
            dst[n++] = stdin_rbuf[stdin_rptr++];
            while (n < len && tk_wai_sem(stdin_sem, 1, TMO_POL) == E_OK)
                dst[n++] = stdin_rbuf[stdin_rptr++];
            return (W)n;
        }
        if (IS_PIPE_FD(arg0) && !IS_PIPE_WRITE(arg0)) {
            INT pi = PIPE_IDX(arg0);
            if (!pipes[pi].in_use) return -1;
            UB *dst = (UB *)buf;
            INT n = 0;
            while (n < len) {
                ER er = tk_wai_sem(pipes[pi].sem_data, 1, TMO_POL);
                if (er != E_OK) {
                    /* no data — if write end is closed, return EOF */
                    if (!pipes[pi].write_end_open) break;
                    if (n > 0) break;  /* return what we have */
                    /* block waiting for data or EOF */
                    er = tk_wai_sem(pipes[pi].sem_data, 1, TMO_FEVR);
                    if (er != E_OK) break;
                }
                dst[n++] = pipes[pi].buf[pipes[pi].rptr++];
                tk_sig_sem(pipes[pi].sem_space, 1);
            }
            return (W)n;
        }
        if (IS_STD_FD(arg0)) return -1;
        if (!vfs_ready) return -1;
        return vfs_read(TO_VFS_FD(arg0), buf, (UW)len);
    }

    case SYS_OPEN: {
        /* fd tracking via fs_ssy_call (records ownership for cleanupfn) */
        return fs_ssy_call(SYS_OPEN, arg0, arg1, 0);
    }

    case SYS_CLOSE: {
        if (IS_STD_FD(arg0)) return 0;
        if (IS_PIPE_FD(arg0)) {
            INT pi = PIPE_IDX(arg0);
            if (!pipes[pi].in_use) return -1;
            if (IS_PIPE_WRITE(arg0)) {
                /* closing write end — signal EOF to readers */
                pipes[pi].write_end_open = FALSE;
                tk_sig_sem(pipes[pi].sem_data, 1); /* unblock any waiting reader */
            } else {
                pipes[pi].in_use = FALSE;  /* read end closed — free slot */
            }
            return 0;
        }
        /* fd release via fs_ssy_call */
        return fs_ssy_call(SYS_CLOSE, arg0, 0, 0);
    }

    case SYS_LSEEK: {
        if (IS_STD_FD(arg0)) return -1;
        if (!vfs_ready) return -1;
        UW off = (UW)arg1;
        if (arg2 == 2) off = vfs_fsize(TO_VFS_FD(arg0));
        return vfs_seek(TO_VFS_FD(arg0), off);
    }

    case SYS_MKDIR: {
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        if (!user_range_ok(path, 1)) return -1;   /* ISO-USERPTR */
        return vfs_mkdir(path);
    }

    case SYS_UNLINK: {
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        if (!user_range_ok(path, 1)) return -1;   /* ISO-USERPTR */
        return vfs_unlink(path);
    }

    case SYS_RENAME: {
        const char *old = (const char *)(UW)arg0;
        const char *nw  = (const char *)(UW)arg1;
        if (!vfs_ready) return -1;
        if (!user_range_ok(old, 1) || !user_range_ok(nw, 1)) return -1; /* ISO-USERPTR */
        return vfs_rename(old, nw);
    }

    case SYS_READDIR: {
        if (!vfs_ready) return -1;
        return fs_ssy_call(SYS_READDIR, arg0, arg1, arg2);
    }

    /* ------------------------------------------------------------- */
    /* New POSIX interfaces                                           */
    /* ------------------------------------------------------------- */

    case SYS_GETPID: {
        /* Returns the current task ID as the process ID */
        return (W)knl_ctxtsk->tskid;
    }

    case SYS_CHDIR: {
        const char *path = (const char *)(UW)arg0;
        if (!vfs_ready) return -1;
        if (!user_range_ok(path, 1)) return -1;   /* ISO-USERPTR */
        return (W)vfs_chdir(path);
    }

    case SYS_GETCWD: {
        /* arg0=buf, arg1=len */
        char *buf = (char *)(UW)arg0;
        INT   len = (INT)arg1;
        if (!buf || len <= 0) return -1;
        if (!user_range_ok(buf, (UW)len)) return -1;   /* ISO-USERPTR (kernel writes) */
        vfs_getcwd(buf, len);
        return 0;
    }

    case SYS_STAT: {
        /* arg0=path, arg1=struct p_stat* */
        const char *path = (const char *)(UW)arg0;
        PK_STAT    *st   = (PK_STAT *)(UW)arg1;
        if (!st || !vfs_ready) return -1;
        /* ISO-USERPTR: path is read, st is written by the kernel. */
        if (!user_range_ok(path, 1) || !user_range_ok(st, sizeof(PK_STAT)))
            return -1;
        UW size; BOOL is_dir;
        if (vfs_stat_path(path, &size, &is_dir) < 0) return -1;
        st->st_mode  = is_dir ? S_IFDIR : S_IFREG;
        st->st_size  = is_dir ? 0       : size;
        st->st_ino   = 0;
        st->st_mtime = 0;
        return 0;
    }

    case SYS_FSTAT: {
        /* arg0=posix_fd, arg1=struct p_stat* */
        PK_STAT *st = (PK_STAT *)(UW)arg1;
        if (!st || IS_STD_FD(arg0)) return -1;
        if (!user_range_ok(st, sizeof(PK_STAT))) return -1;   /* ISO-USERPTR (kernel writes) */
        if (IS_PIPE_FD(arg0)) {
            /* pipes are always regular files for stat purposes */
            st->st_mode = S_IFREG; st->st_size = 0;
            st->st_ino = 0; st->st_mtime = 0;
            return 0;
        }
        if (!vfs_ready) return -1;
        st->st_mode  = S_IFREG;
        st->st_size  = vfs_fsize(TO_VFS_FD(arg0));
        st->st_ino   = 0;
        st->st_mtime = 0;
        return 0;
    }

    case SYS_DUP: {
        /* arg0 = old posix fd  → returns new posix fd */
        if (IS_STD_FD(arg0)) return arg0;   /* dup(0/1/2) = itself */
        if (IS_PIPE_FD(arg0)) return -1;    /* pipe dup not supported */
        if (!vfs_ready) return -1;
        INT vfd = vfs_dup(TO_VFS_FD(arg0));
        if (vfd < 0) return -1;
        return TO_POSIX_FD(vfd);
    }

    case SYS_DUP2: {
        /* arg0=old fd, arg1=new fd  (only newfd≥3 supported) */
        if (arg1 < 3 || IS_PIPE_FD(arg1)) return -1;
        if (arg0 == arg1) return arg1;
        INT vfd = vfs_dup2(TO_VFS_FD(arg0), TO_VFS_FD(arg1));
        if (vfd < 0) return -1;
        return TO_POSIX_FD(vfd);
    }

    case SYS_PIPE: {
        /* arg0 = int[2]* — filled with [read_fd, write_fd] */
        W *fds = (W *)(UW)arg0;
        if (!fds) return -1;
        if (!user_range_ok(fds, 2 * sizeof(W))) return -1;   /* ISO-USERPTR (kernel writes 2 ints) */
        /* Find a free pipe slot */
        for (INT i = 0; i < PIPE_MAX; i++) {
            if (!pipes[i].in_use) {
                T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                              .isemcnt = 0, .maxsem = PIPE_BUFSZ };
                pipes[i].sem_data  = tk_cre_sem(&cs);
                T_CSEM cs2 = { .exinf = NULL, .sematr = TA_TFIFO,
                               .isemcnt = PIPE_BUFSZ, .maxsem = PIPE_BUFSZ };
                pipes[i].sem_space = tk_cre_sem(&cs2);
                pipes[i].wptr      = 0;
                pipes[i].rptr      = 0;
                pipes[i].write_end_open = TRUE;
                pipes[i].in_use    = TRUE;
                fds[0] = PIPE_FD_BASE + 2 * i;       /* read end  */
                fds[1] = PIPE_FD_BASE + 2 * i + 1;   /* write end */
                return 0;
            }
        }
        return -1;   /* no free pipe slot */
    }

    /* ------------------------------------------------------------- */
    /* Linux i386 syscall number aliases                             */
    /* These allow musl-compiled static binaries to run on p-kernel */
    /* ------------------------------------------------------------- */

    /* lseek: Linux=#19, p-kernel=7 */
    case 19: {
        INT fd = (INT)arg0;
        W   off = arg1;
        INT r = vfs_seek(fd, (INT)off);
        return (r < 0) ? -1 : off;
    }

    /* access: Linux=#33 — check if file is accessible */
    case 33: {
        if (!user_range_ok((const void *)(UW)arg0, 1)) return -1;  /* ISO-USERPTR */
        INT fd = vfs_open((const char *)(UW)arg0);
        if (fd < 0) return -2;   /* ENOENT */
        vfs_close(fd);
        return 0;
    }

    /* rename: Linux=#38, p-kernel=10 */
    case 38:
        if (!user_range_ok((const void *)(UW)arg0, 1) ||
            !user_range_ok((const void *)(UW)arg1, 1)) return -1;  /* ISO-USERPTR */
        return (W)vfs_rename((const char *)(UW)arg0,
                             (const char *)(UW)arg1);

    /* mkdir: Linux=#39, p-kernel=8 */
    case 39:
        if (!user_range_ok((const void *)(UW)arg0, 1)) return -1;  /* ISO-USERPTR */
        return (W)vfs_mkdir((const char *)(UW)arg0);

    /* rmdir: Linux=#40 — stub (not implemented) */
    case 40:
        return -1;

    /* munmap: Linux=#91 — ignore (we never unmap) */
    case 91:
        return 0;

    /* readlink: Linux=#85 — no symlinks */
    case 85:
        return -22;   /* EINVAL */

    /* ioctl: Linux=#54 — stub for terminal detection */
    case 54:
        return -25;   /* ENOTTY — not a terminal */

    /* mmap: Linux=#90 — return ENOMEM (force brk-only malloc) */
    case 90:
        return -12;   /* ENOMEM */

    /* mmap2: Linux=#192 — anonymous-only via brk extension */
    case 192: {
        /* We ignore flags and treat all mmap2 as anonymous MAP_PRIVATE */
        UW size  = ((UW)arg1 + 0xFFFUL) & ~0xFFFUL;   /* page-align */
        ID tid   = knl_ctxtsk->tskid;
        UW cur   = paging_get_task_brk(tid);
        UW next  = cur + size;
        /* Upper limit: end of PD[71] (0x09000000) which is the top of the
         * Linux-ELF mapped region; native ELF heap is well below this. */
        if (size == 0 || next > 0x09000000UL)
            return -12;   /* ENOMEM */
        paging_set_task_brk(tid, next);
        return (W)cur;   /* pointer to new mapping */
    }

    /* kstat64 helper macro — fill Linux stat64 struct from path or fd */
    /* fstat: Linux=#108 */
    case 108:
    /* fstat64: Linux=#197 */
    case 197: {
        typedef struct {
            unsigned long long st_dev; unsigned char _p0[4];
            unsigned long __ino; unsigned int st_mode; unsigned int st_nlink;
            unsigned int st_uid; unsigned int st_gid;
            unsigned long long st_rdev; unsigned char _p3[4];
            long long st_size; unsigned long st_blksize;
            unsigned long long st_blocks;
            unsigned long st_atime; unsigned long _an;
            unsigned long st_mtime; unsigned long _mn;
            unsigned long st_ctime; unsigned long _cn;
            unsigned long long st_ino;
        } kstat64;
        kstat64 *ks = (kstat64 *)(UW)arg1;
        if (!ks) return -14;
        if (!user_range_ok(ks, sizeof(kstat64))) return -14;  /* ISO-USERPTR (kernel writes) */
        INT posix_fd = (INT)arg0;
        UW sz = 0;
        unsigned int mode = 0100644; /* S_IFREG | 0644 */
        if (IS_STD_FD(posix_fd)) {
            mode = 020000 | 0666; /* S_IFCHR */
        } else if (!IS_PIPE_FD(posix_fd) && vfs_ready) {
            sz = (UW)vfs_fsize(TO_VFS_FD(posix_fd));
        }
        ks->st_dev = 1; ks->__ino = 1; ks->st_ino = 1;
        ks->st_mode = mode; ks->st_nlink = 1;
        ks->st_uid = 0; ks->st_gid = 0; ks->st_rdev = 0;
        ks->st_size = (long long)sz; ks->st_blksize = 512;
        ks->st_blocks = (unsigned long long)((sz + 511) / 512);
        ks->st_atime = ks->_an = ks->st_mtime = ks->_mn =
        ks->st_ctime = ks->_cn = 0;
        return 0;
    }

    /* lstat: Linux=#107 — same as stat (no symlinks) */
    case 107:
    /* stat64: Linux=#195, lstat64=#196 */
    case 195:
    case 196: {
        typedef struct {
            unsigned long long st_dev; unsigned char _p0[4];
            unsigned long __ino; unsigned int st_mode; unsigned int st_nlink;
            unsigned int st_uid; unsigned int st_gid;
            unsigned long long st_rdev; unsigned char _p3[4];
            long long st_size; unsigned long st_blksize;
            unsigned long long st_blocks;
            unsigned long st_atime; unsigned long _an;
            unsigned long st_mtime; unsigned long _mn;
            unsigned long st_ctime; unsigned long _cn;
            unsigned long long st_ino;
        } kstat64;
        kstat64 *ks = (kstat64 *)(UW)arg1;
        const char *path = (const char *)(UW)arg0;
        if (!ks || !path || !vfs_ready) return -2;
        /* ISO-USERPTR: path is read, ks is written by the kernel. */
        if (!user_range_ok(path, 1) || !user_range_ok(ks, sizeof(kstat64)))
            return -2;
        UW sz; BOOL is_dir;
        if (vfs_stat_path(path, &sz, &is_dir) < 0) return -2;
        ks->st_dev = 1; ks->__ino = 1; ks->st_ino = 1;
        ks->st_mode = is_dir ? (040000|0755) : (0100644);
        ks->st_nlink = 1; ks->st_uid = 0; ks->st_gid = 0; ks->st_rdev = 0;
        ks->st_size = is_dir ? 0 : (long long)sz;
        ks->st_blksize = 512;
        ks->st_blocks = is_dir ? 0 : (unsigned long long)((sz+511)/512);
        ks->st_atime = ks->_an = ks->st_mtime = ks->_mn =
        ks->st_ctime = ks->_cn = 0;
        return 0;
    }

    /* writev: Linux=#146 — scatter write over iovec array */
    case 146: {
        typedef struct { void *base; unsigned int len; } iovec_t;
        INT      posix_fd = (INT)arg0;
        iovec_t *iov      = (iovec_t *)(UW)arg1;
        INT      iovcnt   = (INT)arg2;
        W total = 0;
        /* ISO-USERPTR: the iovec ARRAY itself is read by the kernel. */
        if (iovcnt < 0 || iovcnt > 1024) return -1;
        if (iovcnt > 0 && !user_range_ok(iov, (UW)iovcnt * sizeof(iovec_t)))
            return -1;
        for (INT i = 0; i < iovcnt; i++) {
            if (!iov[i].base || iov[i].len == 0) continue;
            /* ISO-USERPTR: each scatter buffer is read on the user's behalf. */
            if (!user_range_ok(iov[i].base, iov[i].len))
                return total > 0 ? total : -1;
            W r;
            if (IS_STD_FD(posix_fd)) {
                sio_send_frame((const UB *)iov[i].base, (INT)iov[i].len);
                r = (W)iov[i].len;
            } else if (IS_PIPE_FD(posix_fd) && IS_PIPE_WRITE(posix_fd)) {
                INT pi = PIPE_IDX(posix_fd);
                if (!pipes[pi].in_use) return total > 0 ? total : -1;
                r = 0;
                const UB *b = (const UB *)iov[i].base;
                for (UW k = 0; k < iov[i].len; k++) {
                    if (tk_wai_sem(pipes[pi].sem_space, 1, TMO_FEVR) != E_OK) break;
                    pipes[pi].buf[pipes[pi].wptr++] = b[k];
                    tk_sig_sem(pipes[pi].sem_data, 1);
                    r++;
                }
            } else {
                if (!vfs_ready) return total > 0 ? total : -1;
                r = (W)vfs_write(TO_VFS_FD(posix_fd), iov[i].base, (UW)iov[i].len);
            }
            if (r < 0) return total > 0 ? total : r;
            total += r;
        }
        return total;
    }

    /* poll: Linux=#168 — stub (no events, immediate timeout) */
    case 168:
        return 0;

    /* futex: Linux=#240 — stub for single-threaded programs */
    case 240:
        return 0;

    /* set_thread_area: Linux=#243 — wire musl TLS into GDT[8] */
    case 243:
        /* user_desc layout: [0]=entry_number [1]=base_addr [2]=limit [3]=flags
         * musl passes the TLS block address in base_addr, then loads GS with
         * (entry_number*8)|3.  We use GDT slot 8 (selector 0x43) which is
         * already set up; just update the base to musl's TLS block address. */
        if (arg0) {
            unsigned int *ud = (unsigned int *)(UW)arg0;
            /* ISO-USERPTR: ud[0] is written, ud[1] is read (user_desc). */
            if (!user_range_ok(ud, 2 * sizeof(unsigned int))) return -1;
            UW base = (UW)ud[1];
            if (base) gdt_set_user_tls(base);   /* update GDT[8] base */
            ud[0] = 8;   /* entry_number = 8 → GS = (8*8)|3 = 0x43 */
        }
        return 0;

    /* exit_group: Linux=#252 — same as SYS_EXIT */
    case 252:
        goto do_exit;

    /* set_robust_list: Linux=#258 handled before switch() above */

    /* getdents64: Linux=#220 — stub (return 0 = no entries) */
    case 220:
        return 0;

    /* fcntl64: Linux=#221 — stub basic operations */
    case 221:
        /* F_GETFL=3: return O_RDWR */
        if (arg1 == 3) return 2;
        /* F_SETFL=4: ignore */
        if (arg1 == 4) return 0;
        return -22;   /* EINVAL */

    /* NOTE: Linux unlink=#10 conflicts with p-kernel SYS_RENAME=#10.
     * musl's hello world doesn't call unlink, so this is acceptable. */

    /* ------------------------------------------------------------- */
    /* Linux-compatible brk (heap extension) — syscall #45          */
    /* Used by musl/libc malloc to grow the heap.                   */
    /* arg0 = requested new brk; 0 = query current brk.             */
    /* Returns new brk on success, current brk on failure.          */
    /* ------------------------------------------------------------- */
    case 45: {
        ID tid      = knl_ctxtsk->tskid;
        UW cur_brk  = paging_get_task_brk(tid);
        UW new_brk  = (UW)arg0;

        if (new_brk == 0) {
            /* Query */
            return (W)cur_brk;
        }
        /* Clamp to safe range.
         * Native ELFs: heap in 0x500000–0xFFFFFF (PD[2..7]).
         * Linux ELFs:  heap in 0x08050000–0x08FFFFFF (PD[64..71]).
         * Upper bound = end of PD[71] = 0x09000000. */
        if (new_brk < 0x400000UL || new_brk > 0x09000000UL) {
            return (W)cur_brk;   /* refuse — return current brk */
        }
        paging_set_task_brk(tid, new_brk);
        return (W)new_brk;
    }

    do_exit:
    case SYS_EXIT: {
        /* arg0 = exit code */
        tm_putstring((UB *)"\r\n[proc] exited (code=");
        sout_num(arg0);
        tm_putstring((UB *)")\r\np-kernel> ");
        user_last_exit = arg0;   /* gate-1 class channel (see above) */
        user_proc_unwind();      /* never returns */
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: task management                              */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_TSK: {
        /* arg0 = PK_CRE_TSK* */
        PK_CRE_TSK *pk = (PK_CRE_TSK *)(UW)arg0;
        if (!pk || !user_range_ok(pk, sizeof(*pk))) return -1;  /* ISO-USERPTR */
        if (!pk->task) return -1;

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
        ID cur = knl_ctxtsk->tskid;
        knl_ssy_cleanup(cur);   /* release sockets etc. */
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
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
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
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
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
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
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
        if (!pk || !user_range_ok(pk, sizeof(*pk))) return -1;  /* ISO-USERPTR */
        UINT flgptn = 0;
        W r = (W)tk_wai_flg(pk->flgid, pk->waiptn, pk->wfmode,
                             &flgptn, pk->tmout);
        /* ISO-USERPTR: p_flgptn is a user out-pointer the kernel writes. */
        if (pk->p_flgptn) {
            if (!user_range_ok((void *)(UW)pk->p_flgptn, sizeof(UINT))) return -1;
            *pk->p_flgptn = flgptn;
        }
        return r;
    }

    /* ------------------------------------------------------------- */
    /* Network syscalls (0x200-0x2FF) — delegated to net_ssy (ssid=1) */
    /* ------------------------------------------------------------- */

    case SYS_UDP_BIND:
    case SYS_UDP_SEND:
    case SYS_UDP_RECV:
    case SYS_UDP_JOIN_GROUP:
    case SYS_UDP_LEAVE_GROUP:
    case SYS_TCP_CONNECT:
    case SYS_TCP_WRITE:
    case SYS_TCP_READ:
    case SYS_TCP_CLOSE:
    case SYS_MOUNT:
    case SYS_UMOUNT: {
        NET_SVC_PKT pk = { nr, arg0, arg1, arg2 };
        return (W)knl_svc_ientry(&pk, (FN)(((UW)nr << 8) | NET_SSID));
    }

    /* ------------------------------------------------------------- */
    /* AI inference syscalls                                         */
    /* ------------------------------------------------------------- */

    case SYS_INFER: {
        /* arg0 = sensor_packed (SENSOR_PACK(t,h,p,l))
         *
         * ring3-core Wave B (II.1): the ring-3 path exercises the core
         * computation moe_infer() — the same entry the ring-0 oracle in
         * shell `ring3 test` calls — instead of the handwritten
         * mlp_forward.  The math still executes in ring-0 behind this
         * syscall for THIS slice; relocating the moe.c body into the
         * ring-3 ELF is the next slice (Wave C). */
        return (W)moe_infer(SENSOR_UNPACK_T(arg0),
                            SENSOR_UNPACK_H(arg0),
                            SENSOR_UNPACK_P(arg0),
                            SENSOR_UNPACK_L(arg0));
    }

    case SYS_DTR_WEIGHTS_GET: {
        /* ring3-core Wave C (III.2, CDN-6): copy the LIVE learned dtr
         * weights into a ring-3 buffer so the user-space mind computes
         * on the same weights the ring-0 oracle uses.
         *   arg0 = user float* buffer (synchronous deref — the
         *          established struct-passing pattern, cf. SYS_TOPIC_SUB)
         *   arg1 = buffer capacity in floats
         * Returns DTR_WEIGHT_FLOATS (635) on success, -1 on bad args.
         * A single 2,540-byte copy — microseconds.  This is a fetch-time
         * SNAPSHOT (staleness honesty: III.2); re-fetch per decision. */
        float *ubuf = (float *)(UW)arg0;
        if (!ubuf) return -1;
        if (arg1 < (W)DTR_WEIGHT_FLOATS) return -1;
        /* ISO-USERPTR: the kernel WRITES DTR_WEIGHT_FLOATS floats into ubuf.
         * Post-relocation (ring3-core III) this is THE pointer that crosses
         * the privilege boundary on the AI core path — a kernel-range ubuf
         * would let a ring-3 mind scribble the live weights snapshot over
         * kernel memory. */
        if (!user_range_ok(ubuf, (UW)DTR_WEIGHT_FLOATS * sizeof(float)))
            return -1;
        dtr_weights_get(ubuf);
        return (W)DTR_WEIGHT_FLOATS;
    }

    case SYS_MIND_NOTE: {
        /* ring3-core Wave C (CDN-7): a ring-3 moe_infer stays visible
         * to the world map and the reflex/defense layer — 可視化 is a
         * mechanism, not decoration.  The reflex ACTION table stays
         * ring-0; only the hook crosses the boundary.
         *   arg0 = op: 0 = world_note_firing, 1 = reflex_on_inference
         *   arg1 = cls | conf<<8 (cls in [0..255], conf in [0..100]) */
        UB cls  = (UB)((UW)arg1 & 0xFF);
        UB conf = (UB)(((UW)arg1 >> 8) & 0xFF);
        if (arg0 == 0) {
            world_note_firing(cls);
        } else if (arg0 == 1) {
            reflex_on_inference(cls, conf, drpc_my_node);
        } else {
            return -1;
        }
        return 0;
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
    /* K-DDS トピック syscall                                        */
    /* ------------------------------------------------------------- */

    case SYS_TOPIC_OPEN: {
        /* arg0 = name (const char*), arg1 = qos */
        const char *name = (const char *)(UW)arg0;
        if (!name || !user_range_ok(name, 1)) return -1;  /* ISO-USERPTR */
        return kdds_open(name, arg1);
    }

    case SYS_TOPIC_PUB: {
        /* arg0 = handle, arg1 = data ptr, arg2 = len */
        const void *data = (const void *)(UW)arg1;
        if (!data || !user_range_ok(data, (UW)arg2)) return -1;  /* ISO-USERPTR (read) */
        return kdds_pub(arg0, data, arg2);
    }

    case SYS_TOPIC_SUB: {
        /* arg0 = PK_TOPIC_SUB* */
        PK_TOPIC_SUB *pk = (PK_TOPIC_SUB *)(UW)arg0;
        if (!pk || !user_range_ok(pk, sizeof(*pk))) return -1;  /* ISO-USERPTR */
        void *buf = (void *)(UW)pk->buf_ptr;
        /* ISO-USERPTR: kdds_sub WRITES up to buflen bytes into buf. */
        if (!buf || !user_range_ok(buf, (UW)pk->buflen)) return -1;
        return kdds_sub(pk->handle, buf, pk->buflen, pk->timeout_ms);
    }

    case SYS_TOPIC_CLOSE: {
        /* arg0 = handle */
        kdds_close(arg0);
        return 0;
    }

    case SYS_INFER_SLA: {
        /* arg0 = sensor_packed, arg1 = deadline_ms */
        return edf_infer(arg0, arg1);
    }

    /* ------------------------------------------------------------- */
    /* 分散 Transformer 推論 syscall (Phase 12)                      */
    /* ------------------------------------------------------------- */

    case SYS_DTR_SUBMIT: {
        /* arg0 = sensor_packed (SENSOR_PACK(t,h,p,l))
         * dtr_infer() を呼んで Transformer 推論を実行する。
         * 単一ノード(0xFF)は全ステージをローカル実行。
         * 分散ノード (0/1) は縮退レベルに応じて Pipeline/Tensor Parallel。
         * 最大 DTR_INFER_TMO ms ブロックして class [0,1,2] または -1 を返す。 */
        B input[4] = {
            SENSOR_UNPACK_T(arg0),
            SENSOR_UNPACK_H(arg0),
            SENSOR_UNPACK_P(arg0),
            SENSOR_UNPACK_L(arg0),
        };
        return (W)dtr_infer(input);
    }

    case SYS_DTR_WAIT: {
        /* 将来の非同期実装用 — 現バージョンでは SYS_DTR_SUBMIT が同期のため不要 */
        return -1;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: mutex                                       */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_MTX: {
        /* arg0 = PK_CRE_MTX* */
        PK_CRE_MTX *upk = (PK_CRE_MTX *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        T_CMTX pk;
        pk.exinf   = NULL;
        pk.mtxatr  = (ATR)upk->mtxatr;
        pk.ceilpri = (PRI)upk->ceilpri;
        return (W)tk_cre_mtx(&pk);
    }

    case SYS_TK_DEL_MTX:
        return (W)tk_del_mtx((ID)arg0);

    case SYS_TK_LOC_MTX:
        /* arg0=mtxid, arg1=tmout_ms */
        return (W)tk_loc_mtx((ID)arg0, (TMO)arg1);

    case SYS_TK_UNL_MTX:
        return (W)tk_unl_mtx((ID)arg0);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: mailbox                                      */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_MBX: {
        /* arg0 = mbxatr (TA_TFIFO=0, TA_TPRI=1, TA_MPRI=2) */
        T_CMBX pk;
        pk.exinf  = NULL;
        pk.mbxatr = (ATR)arg0;
        return (W)tk_cre_mbx(&pk);
    }

    case SYS_TK_DEL_MBX:
        return (W)tk_del_mbx((ID)arg0);

    case SYS_TK_SND_MBX:
        /* arg0=mbxid, arg1=T_MSG* */
        /* ISO-USERPTR: the kernel links into this user message header. */
        if (!user_range_ok((void *)(UW)arg1, sizeof(T_MSG))) return -1;
        return (W)tk_snd_mbx((ID)arg0, (T_MSG *)(UW)arg1);

    case SYS_TK_RCV_MBX: {
        /* arg0=mbxid, arg1=T_MSG** (out), arg2=tmout_ms */
        T_MSG *msg = NULL;
        /* ISO-USERPTR: arg1 is a user out-pointer the kernel writes. */
        if (!user_range_ok((void *)(UW)arg1, sizeof(T_MSG *))) return -1;
        ER er = tk_rcv_mbx((ID)arg0, &msg, (TMO)arg2);
        if (er < E_OK) return (W)er;
        *(T_MSG **)(UW)arg1 = msg;
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: message buffer                               */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_MBF: {
        /* arg0 = PK_CRE_MBF* */
        PK_CRE_MBF *upk = (PK_CRE_MBF *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        T_CMBF pk;
        pk.exinf  = NULL;
        pk.mbfatr = (ATR)upk->mbfatr | TA_USERBUF;
        pk.bufsz  = (SZ)upk->bufsz;
        pk.maxmsz = (INT)upk->maxmsz;
        pk.bufptr = (void *)(UW)upk->buf_ptr;
        return (W)tk_cre_mbf(&pk);
    }

    case SYS_TK_DEL_MBF:
        return (W)tk_del_mbf((ID)arg0);

    case SYS_TK_SND_MBF: {
        /* arg0 = PK_SND_MBF* (mbfid, msg_ptr, msgsz, tmout) */
        PK_SND_MBF *upk = (PK_SND_MBF *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        /* ISO-USERPTR: msg_ptr is a user buffer read by the kernel. */
        if (!user_range_ok((void *)(UW)upk->msg_ptr, (UW)upk->msgsz)) return -1;
        return (W)tk_snd_mbf((ID)upk->mbfid,
                               (void *)(UW)upk->msg_ptr,
                               (INT)upk->msgsz, (TMO)upk->tmout);
    }

    case SYS_TK_RCV_MBF:
        /* arg0=mbfid, arg1=buf_ptr, arg2=tmout_ms — returns received size.
         * ISO-USERPTR: the kernel writes the received message into buf_ptr.
         * The message size is bounded by the mbf's maxmsz (set at create
         * time, not visible here); reject at least a kernel-range base. */
        if (!user_range_ok((void *)(UW)arg1, 1)) return -1;
        return (W)tk_rcv_mbf((ID)arg0, (void *)(UW)arg1, (TMO)arg2);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: variable memory pool                         */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_MPL: {
        /* arg0 = PK_CRE_MPL* */
        PK_CRE_MPL *upk = (PK_CRE_MPL *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        T_CMPL pk;
        pk.exinf  = NULL;
        pk.mplatr = (ATR)upk->mplatr | TA_USERBUF;
        pk.mplsz  = (SZ)upk->mplsz;
        pk.bufptr = (void *)(UW)upk->buf_ptr;
        return (W)tk_cre_mpl(&pk);
    }

    case SYS_TK_DEL_MPL:
        return (W)tk_del_mpl((ID)arg0);

    case SYS_TK_GET_MPL: {
        /* arg0=mplid, arg1=blksz, arg2=tmout_ms — returns block ptr or err */
        void *blk = NULL;
        ER er = tk_get_mpl((ID)arg0, (SZ)arg1, &blk, (TMO)arg2);
        if (er < E_OK) return (W)er;
        return (W)(UW)blk;
    }

    case SYS_TK_REL_MPL:
        /* arg0=mplid, arg1=blk_ptr (a block GET_MPL handed back) */
        if (!user_range_ok((void *)(UW)arg1, 1)) return -1;  /* ISO-USERPTR */
        return (W)tk_rel_mpl((ID)arg0, (void *)(UW)arg1);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: fixed memory pool                            */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_MPF: {
        /* arg0 = PK_CRE_MPF* */
        PK_CRE_MPF *upk = (PK_CRE_MPF *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        T_CMPF pk;
        pk.exinf  = NULL;
        pk.mpfatr = (ATR)upk->mpfatr | TA_USERBUF;
        pk.mpfcnt = (SZ)upk->mpfcnt;
        pk.blfsz  = (SZ)upk->blfsz;
        pk.bufptr = (void *)(UW)upk->buf_ptr;
        return (W)tk_cre_mpf(&pk);
    }

    case SYS_TK_DEL_MPF:
        return (W)tk_del_mpf((ID)arg0);

    case SYS_TK_GET_MPF: {
        /* arg0=mpfid, arg1=tmout_ms — returns block ptr or err */
        void *blf = NULL;
        ER er = tk_get_mpf((ID)arg0, &blf, (TMO)arg1);
        if (er < E_OK) return (W)er;
        return (W)(UW)blf;
    }

    case SYS_TK_REL_MPF:
        /* arg0=mpfid, arg1=blf_ptr (a block GET_MPF handed back) */
        if (!user_range_ok((void *)(UW)arg1, 1)) return -1;  /* ISO-USERPTR */
        return (W)tk_rel_mpf((ID)arg0, (void *)(UW)arg1);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: cyclic handler                               */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_CYC: {
        /* arg0 = PK_CRE_CYC* */
        PK_CRE_CYC *upk = (PK_CRE_CYC *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        if (!upk->cychdr) return -1;
        T_CCYC pk;
        pk.exinf   = NULL;
        pk.cycatr  = (ATR)upk->cycatr | TA_HLNG;
        pk.cychdr  = (FP)(UW)upk->cychdr;
        pk.cyctim  = (RELTIM)upk->cyctim_ms;
        pk.cycphs  = (RELTIM)upk->cycphs_ms;
        return (W)tk_cre_cyc(&pk);
    }

    case SYS_TK_DEL_CYC:
        return (W)tk_del_cyc((ID)arg0);

    case SYS_TK_STA_CYC:
        return (W)tk_sta_cyc((ID)arg0);

    case SYS_TK_STP_CYC:
        return (W)tk_stp_cyc((ID)arg0);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: alarm handler                                */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_ALM: {
        /* arg0 = PK_CRE_ALM* */
        PK_CRE_ALM *upk = (PK_CRE_ALM *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        if (!upk->almhdr) return -1;
        T_CALM pk;
        pk.exinf  = NULL;
        pk.almatr = (ATR)upk->almatr | TA_HLNG;
        pk.almhdr = (FP)(UW)upk->almhdr;
        return (W)tk_cre_alm(&pk);
    }

    case SYS_TK_DEL_ALM:
        return (W)tk_del_alm((ID)arg0);

    case SYS_TK_STA_ALM:
        /* arg0=almid, arg1=almtim_ms */
        return (W)tk_sta_alm((ID)arg0, (RELTIM)arg1);

    case SYS_TK_STP_ALM:
        return (W)tk_stp_alm((ID)arg0);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: task supplement                              */
    /* ------------------------------------------------------------- */

    case SYS_TK_TER_TSK:
        return (W)tk_ter_tsk((ID)arg0);

    case SYS_TK_SUS_TSK:
        return (W)tk_sus_tsk((ID)arg0);

    case SYS_TK_RSM_TSK:
        return (W)tk_rsm_tsk((ID)arg0);

    case SYS_TK_FRSM_TSK:
        return (W)tk_frsm_tsk((ID)arg0);

    case SYS_TK_REL_WAI:
        return (W)tk_rel_wai((ID)arg0);

    case SYS_TK_GET_TID:
        return (W)tk_get_tid();

    case SYS_TK_CAN_WUP:
        return (W)tk_can_wup((ID)arg0);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: ref APIs                                     */
    /* ------------------------------------------------------------- */

    case SYS_TK_REF_SEM: {
        PK_REF_SEM *out = (PK_REF_SEM *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RSEM rsem;
        ER er = tk_ref_sem((ID)arg0, &rsem);
        if (er < E_OK) return (W)er;
        out->wtsk   = (W)rsem.wtsk;
        out->semcnt = (W)rsem.semcnt;
        return 0;
    }

    case SYS_TK_REF_FLG: {
        PK_REF_FLG *out = (PK_REF_FLG *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RFLG rflg;
        ER er = tk_ref_flg((ID)arg0, &rflg);
        if (er < E_OK) return (W)er;
        out->wtsk   = (W)rflg.wtsk;
        out->flgptn = (UW)rflg.flgptn;
        return 0;
    }

    case SYS_TK_REF_MTX: {
        PK_REF_MTX *out = (PK_REF_MTX *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RMTX rmtx;
        ER er = tk_ref_mtx((ID)arg0, &rmtx);
        if (er < E_OK) return (W)er;
        out->htsk = (W)rmtx.htsk;
        out->wtsk = (W)rmtx.wtsk;
        return 0;
    }

    case SYS_TK_REF_MBX: {
        PK_REF_MBX *out = (PK_REF_MBX *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RMBX rmbx;
        ER er = tk_ref_mbx((ID)arg0, &rmbx);
        if (er < E_OK) return (W)er;
        out->wtsk = (W)rmbx.wtsk;
        return 0;
    }

    case SYS_TK_REF_MBF: {
        PK_REF_MBF *out = (PK_REF_MBF *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RMBF rmbf;
        ER er = tk_ref_mbf((ID)arg0, &rmbf);
        if (er < E_OK) return (W)er;
        out->wtsk    = (W)rmbf.wtsk;
        out->stsk    = (W)rmbf.stsk;
        out->msgsz   = (W)rmbf.msgsz;
        out->frbufsz = (W)rmbf.frbufsz;
        out->maxmsz  = (W)rmbf.maxmsz;
        return 0;
    }

    case SYS_TK_REF_MPL: {
        PK_REF_MPL *out = (PK_REF_MPL *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RMPL rmpl;
        ER er = tk_ref_mpl((ID)arg0, &rmpl);
        if (er < E_OK) return (W)er;
        out->wtsk  = (W)rmpl.wtsk;
        out->frsz  = (W)rmpl.frsz;
        out->maxsz = (W)rmpl.maxsz;
        return 0;
    }

    case SYS_TK_REF_MPF: {
        PK_REF_MPF *out = (PK_REF_MPF *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RMPF rmpf;
        ER er = tk_ref_mpf((ID)arg0, &rmpf);
        if (er < E_OK) return (W)er;
        out->wtsk   = (W)rmpf.wtsk;
        out->frbcnt = (W)rmpf.frbcnt;
        return 0;
    }

    case SYS_TK_REF_CYC: {
        PK_REF_CYC *out = (PK_REF_CYC *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RCYC rcyc;
        ER er = tk_ref_cyc((ID)arg0, &rcyc);
        if (er < E_OK) return (W)er;
        out->lfttim_ms = (W)rcyc.lfttim;
        out->cycstat   = (UW)rcyc.cycstat;
        return 0;
    }

    case SYS_TK_REF_ALM: {
        PK_REF_ALM *out = (PK_REF_ALM *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RALM ralm;
        ER er = tk_ref_alm((ID)arg0, &ralm);
        if (er < E_OK) return (W)er;
        out->lfttim_ms = (W)ralm.lfttim;
        out->almstat   = (UW)ralm.almstat;
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: time                                         */
    /* ------------------------------------------------------------- */

    case SYS_TK_GET_TIM: {
        /* arg0 = PK_SYSTIM* */
        PK_SYSTIM *out = (PK_SYSTIM *)(UW)arg0;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        SYSTIM tim;
        ER er = tk_get_tim(&tim);
        if (er < E_OK) return (W)er;
        out->hi = (W)tim.hi;
        out->lo = (UW)tim.lo;
        return 0;
    }

    case SYS_TK_DLY_TSK:
        /* arg0 = delay_ms */
        return (W)tk_dly_tsk((RELTIM)arg0);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: rendezvous port                              */
    /* ------------------------------------------------------------- */

    case SYS_TK_CRE_POR: {
        /* arg0 = PK_CPOR* */
        PK_CPOR *upk = (PK_CPOR *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        T_CPOR pk;
        pk.exinf   = NULL;
        pk.poratr  = (ATR)upk->poratr;
        pk.maxcmsz = (INT)upk->maxcmsz;
        pk.maxrmsz = (INT)upk->maxrmsz;
        return (W)tk_cre_por(&pk);
    }

    case SYS_TK_DEL_POR:
        return (W)tk_del_por((ID)arg0);

    case SYS_TK_CAL_POR: {
        /* arg0 = PK_CAL_POR* */
        PK_CAL_POR *upk = (PK_CAL_POR *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        /* ISO-USERPTR: msg_ptr is a user call-message buffer (read+reply). */
        if (!user_range_ok((void *)(UW)upk->msg_ptr, (UW)upk->cmsgsz)) return -1;
        return (W)tk_cal_por((ID)upk->porid, (UINT)upk->calptn,
                              (void *)(UW)upk->msg_ptr,
                              (INT)upk->cmsgsz, (TMO)upk->tmout);
    }

    case SYS_TK_ACP_POR: {
        /* arg0 = PK_ACP_POR* */
        PK_ACP_POR *upk = (PK_ACP_POR *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        /* ISO-USERPTR: msg_ptr receives the accepted call message (kernel
         * writes up to the port's maxcmsz, not visible here → base check). */
        if (!user_range_ok((void *)(UW)upk->msg_ptr, 1)) return -1;
        RNO rdvno = 0;
        W r = (W)tk_acp_por((ID)upk->porid, (UINT)upk->acpptn,
                              &rdvno, (void *)(UW)upk->msg_ptr,
                              (TMO)upk->tmout);
        if (upk->p_rdvno) {
            if (!user_range_ok((void *)(UW)upk->p_rdvno, sizeof(RNO))) return -1;
            *(RNO *)(UW)upk->p_rdvno = rdvno;
        }
        return r;
    }

    case SYS_TK_FWD_POR: {
        /* arg0 = PK_FWD_POR* */
        PK_FWD_POR *upk = (PK_FWD_POR *)(UW)arg0;
        if (!upk || !user_range_ok(upk, sizeof(*upk))) return -1;  /* ISO-USERPTR */
        /* ISO-USERPTR: msg_ptr is a user call-message buffer (read). */
        if (!user_range_ok((void *)(UW)upk->msg_ptr, (UW)upk->cmsgsz)) return -1;
        return (W)tk_fwd_por((ID)upk->porid, (UINT)upk->calptn,
                               (RNO)upk->rdvno,
                               (void *)(UW)upk->msg_ptr,
                               (INT)upk->cmsgsz);
    }

    case SYS_TK_RPL_RDV:
        /* arg0=rdvno, arg1=msg_ptr, arg2=rmsgsz */
        /* ISO-USERPTR: msg_ptr is a user reply buffer (read). */
        if (!user_range_ok((void *)(UW)arg1, (UW)arg2)) return -1;
        return (W)tk_rpl_rdv((RNO)arg0, (void *)(UW)arg1, (INT)arg2);

    /* ------------------------------------------------------------- */
    /* T-Kernel native: system info                                  */
    /* ------------------------------------------------------------- */

    case SYS_TK_REF_VER: {
        /* arg0 = PK_RVER* */
        PK_RVER *out = (PK_RVER *)(UW)arg0;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RVER rver;
        ER er = tk_ref_ver(&rver);
        if (er < E_OK) return (W)er;
        out->maker = rver.maker;
        out->prid  = rver.prid;
        out->spver = rver.spver;
        out->prver = rver.prver;
        for (INT i = 0; i < 4; i++) out->prno[i] = rver.prno[i];
        return 0;
    }

    case SYS_TK_REF_SYS: {
        /* arg0 = PK_RSYS* */
        PK_RSYS *out = (PK_RSYS *)(UW)arg0;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RSYS rsys;
        ER er = tk_ref_sys(&rsys);
        if (er < E_OK) return (W)er;
        out->sysstat    = (UW)rsys.sysstat;
        out->runtskid   = (W)rsys.runtskid;
        out->schedtskid = (W)rsys.schedtskid;
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: rendezvous port ref                          */
    /* ------------------------------------------------------------- */

    case SYS_TK_REF_POR: {
        /* arg0=porid, arg1=PK_RPOR* */
        PK_RPOR *out = (PK_RPOR *)(UW)arg1;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        T_RPOR rpor;
        ER er = tk_ref_por((ID)arg0, &rpor);
        if (er < E_OK) return (W)er;
        out->wtsk    = (W)rpor.wtsk;
        out->atsk    = (W)rpor.atsk;
        out->maxcmsz = (W)rpor.maxcmsz;
        out->maxrmsz = (W)rpor.maxrmsz;
        return 0;
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: time supplement                              */
    /* ------------------------------------------------------------- */

    case SYS_TK_GET_OTM: {
        /* arg0 = PK_SYSTIM* — operational time (monotonic uptime) */
        PK_SYSTIM *out = (PK_SYSTIM *)(UW)arg0;
        if (!out || !user_range_ok(out, sizeof(*out))) return -1;  /* ISO-USERPTR */
        SYSTIM tim;
        ER er = tk_get_otm(&tim);
        if (er < E_OK) return (W)er;
        out->hi = (W)tim.hi;
        out->lo = (UW)tim.lo;
        return 0;
    }

    case SYS_TK_SET_TIM: {
        /* arg0 = PK_SYSTIM* */
        PK_SYSTIM *in = (PK_SYSTIM *)(UW)arg0;
        if (!in || !user_range_ok(in, sizeof(*in))) return -1;  /* ISO-USERPTR */
        SYSTIM tim;
        tim.hi = (W)in->hi;
        tim.lo = (UW)in->lo;
        return (W)tk_set_tim(&tim);
    }

    /* ------------------------------------------------------------- */
    /* T-Kernel native: task dispatch control                        */
    /* ------------------------------------------------------------- */

    case SYS_TK_EXD_TSK: {
        ID cur = knl_ctxtsk->tskid;
        knl_ssy_cleanup(cur);   /* release sockets etc. */
        for (INT i = 0; i < USR_TASK_MAX; i++) {
            if (usr_ctx[i].real_task != NULL && usr_ctx[i].tskid == cur) {
                free_user_stack(usr_ctx[i].stack_base);
                usr_ctx[i].real_task = NULL;
                break;
            }
        }
        tk_exd_tsk();
        return 0;
    }

    case SYS_TK_DIS_DSP:
        return (W)tk_dis_dsp();

    case SYS_TK_ENA_DSP:
        return (W)tk_ena_dsp();

    case SYS_TK_ROT_RDQ:
        /* arg0 = priority (0 = current task priority) */
        return (W)tk_rot_rdq((PRI)arg0);

    /* System control (0x400+) */
    case SYS_REBOOT:
        /* ACPI フルリセット — 呼び出し元には返らない */
        __asm__ volatile(
            "movw $0xCF9, %%dx\n\t"
            "movb $0x06, %%al\n\t"
            "outb %%al, %%dx\n\t"
            :: : "eax", "edx"
        );
        for (;;) __asm__("hlt");
        return 0;   /* 到達しない */

    default:
        return -1;  /* ENOSYS */
    }
}
