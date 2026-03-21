/*
 *  demo.c — p-kernel POSIX + RTOS scheduler demo (ring-3 ELF)
 *
 *  Demonstrates:
 *    1. POSIX file I/O   : open / write / lseek / read / close
 *    2. Semaphore sync   : two RR tasks signal main via semaphore
 *    3. Event flag       : main waits for both tasks to finish
 *    4. RR scheduling    : TaskA and TaskB at same priority, 50ms slice
 *    5. FIFO scheduling  : TaskC at higher priority, preempts RR tasks
 */

#include "plibc.h"

/* ================================================================= */
/* Semaphore and event flag IDs (set by main, used by tasks)         */
/* ================================================================= */
static int g_sem  = -1;   /* signalled by workers when done */
static int g_flg  = -1;   /* bit0=TaskA done, bit1=TaskB done, bit2=TaskC done */

/* ================================================================= */
/* Worker tasks                                                       */
/* ================================================================= */

static void task_rr_a(int stacd, void *exinf)
{
    (void)stacd;
    plib_puts("[TaskA:RR ] started  pri=10 slice=50ms\r\n");

    for (int i = 0; i < 3; i++) {
        plib_puts("[TaskA:RR ] tick ");
        plib_puti(i);
        plib_puts("\r\n");
        tk_slp_tsk(30);   /* sleep 30ms — yields CPU */
    }

    plib_puts("[TaskA:RR ] done\r\n");
    tk_set_flg(g_flg, 0x01);   /* set bit0 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

static void task_rr_b(int stacd, void *exinf)
{
    (void)stacd;
    plib_puts("[TaskB:RR ] started  pri=10 slice=50ms\r\n");

    for (int i = 0; i < 3; i++) {
        plib_puts("[TaskB:RR ] tick ");
        plib_puti(i);
        plib_puts("\r\n");
        tk_slp_tsk(30);
    }

    plib_puts("[TaskB:RR ] done\r\n");
    tk_set_flg(g_flg, 0x02);   /* set bit1 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

static void task_fifo_c(int stacd, void *exinf)
{
    (void)stacd;
    /* Higher priority (5) FIFO task — runs to completion first */
    plib_puts("[TaskC:FIF] started  pri=5  FIFO\r\n");
    plib_puts("[TaskC:FIF] I preempt lower-priority RR tasks!\r\n");
    plib_puts("[TaskC:FIF] done\r\n");
    tk_set_flg(g_flg, 0x04);   /* set bit2 */
    tk_sig_sem(g_sem, 1);
    tk_ext_tsk();
}

/* ================================================================= */
/* Entry point                                                        */
/* ================================================================= */

void _start(void)
{
    plib_puts("\r\n");
    plib_puts("=== p-kernel POSIX + RTOS scheduler demo ===\r\n");
    plib_puts("\r\n");

    /* ---- 1. POSIX file I/O ------------------------------------ */
    plib_puts("--- [1] POSIX file I/O ---\r\n");

    int fd = sys_open("/demo.txt", O_WRONLY | O_CREAT);
    if (fd < 0) {
        plib_puts("open failed\r\n");
    } else {
        const char *msg = "Hello from ring-3 via POSIX write!\n";
        sys_write(fd, msg, plib_strlen(msg));
        sys_close(fd);
        plib_puts("write /demo.txt : ok\r\n");
    }

    /* read back */
    fd = sys_open("/demo.txt", O_RDONLY);
    if (fd >= 0) {
        char buf[64];
        int n = sys_read(fd, buf, 63);
        if (n > 0) {
            buf[n] = '\0';
            plib_puts("read  /demo.txt : \"");
            sys_write(1, buf, n);
            plib_puts("\"\r\n");
        }
        sys_close(fd);
    }

    plib_puts("\r\n");

    /* ---- 2. Create semaphore & event flag --------------------- */
    plib_puts("--- [2] Semaphore + Event flag ---\r\n");

    PK_CSEM csem = { 0, 0, 8 };   /* initial=0, max=8 */
    g_sem = tk_cre_sem(&csem);
    plib_puts("semaphore id = ");
    plib_puti(g_sem);
    plib_puts("\r\n");

    PK_CFLG cflg = { 0, 0x00 };   /* initial pattern = 0 */
    g_flg = tk_cre_flg(&cflg);
    plib_puts("event flag id = ");
    plib_puti(g_flg);
    plib_puts("\r\n\r\n");

    /* ---- 3. Create & start tasks ------------------------------ */
    plib_puts("--- [3] Task scheduling (FIFO preempts RR) ---\r\n");

    /* TaskC: FIFO pri=5  (higher priority, runs first) */
    PK_CRE_TSK tC = {
        .task     = task_fifo_c,
        .pri      = 5,
        .policy   = SCHED_FIFO,
        .slice_ms = 0,
        .stksz    = 0,
        .exinf    = 0
    };
    int tidC = tk_cre_tsk(&tC);
    plib_puts("TaskC created: tid=");
    plib_puti(tidC);
    plib_puts("\r\n");

    /* TaskA: RR pri=10 slice=50ms */
    PK_CRE_TSK tA = {
        .task     = task_rr_a,
        .pri      = 10,
        .policy   = SCHED_RR,
        .slice_ms = 50,
        .stksz    = 0,
        .exinf    = 0
    };
    int tidA = tk_cre_tsk(&tA);
    plib_puts("TaskA created: tid=");
    plib_puti(tidA);
    plib_puts("\r\n");

    /* TaskB: RR pri=10 slice=50ms */
    PK_CRE_TSK tB = {
        .task     = task_rr_b,
        .pri      = 10,
        .policy   = SCHED_RR,
        .slice_ms = 50,
        .stksz    = 0,
        .exinf    = 0
    };
    int tidB = tk_cre_tsk(&tB);
    plib_puts("TaskB created: tid=");
    plib_puti(tidB);
    plib_puts("\r\n\r\n");

    /* Start all tasks */
    tk_sta_tsk(tidC, 0);
    tk_sta_tsk(tidA, 0);
    tk_sta_tsk(tidB, 0);

    /* ---- 4. Wait for all tasks via semaphore ------------------- */
    plib_puts("--- [4] Waiting for all tasks (sem × 3) ---\r\n");
    tk_wai_sem(g_sem, 3, TMO_FEVR);
    plib_puts("all tasks signalled semaphore\r\n");

    /* ---- 5. Verify event flag ---------------------------------- */
    plib_puts("\r\n--- [5] Event flag check ---\r\n");
    unsigned int flgptn = 0;
    tk_wai_flg(g_flg, 0x07, TWF_ANDW, &flgptn, TMO_POL);
    plib_puts("flag pattern = 0x");
    plib_putu(flgptn);
    plib_puts(" (expect 0x7)\r\n");

    /* ---- 6. Cleanup -------------------------------------------- */
    tk_del_sem(g_sem);
    tk_del_flg(g_flg);
    sys_unlink("/demo.txt");

    plib_puts("\r\n=== demo complete ===\r\n");
    sys_exit(0);
}
