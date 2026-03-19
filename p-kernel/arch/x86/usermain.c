/*
 *  usermain.c (x86)
 *  p-kernel initial task: starts keyboard driver and interactive shell
 */

#include "kernel.h"
#include "keyboard.h"
#include <tmonitor.h>

/* Shell task entry point (defined in shell.c) */
IMPORT void shell_task(INT stacd, void *exinf);

#define SHELL_STACK_SIZE  8192  /* 8KB stack for shell */
#define SHELL_PRIORITY    2     /* lower than initial task (1) */

EXPORT INT usermain(void)
{
    tm_putstring((UB *)"[T-Kernel] Initial task started\r\n");

    /* --- Semaphore for keyboard driver -------------------------------- */
    T_CSEM csem = {
        .exinf   = NULL,
        .sematr  = TA_TFIFO,
        .isemcnt = 0,
        .maxsem  = 64,
    };
    ID kbd_sem = tk_cre_sem(&csem);
    if (kbd_sem < E_OK) {
        tm_putstring((UB *)"[ERR] kbd semaphore create failed\r\n");
        return 0;
    }

    /* --- Initialize keyboard driver ----------------------------------- */
    kbd_init(kbd_sem);
    tm_putstring((UB *)"[OK]  Keyboard driver initialized (IRQ1)\r\n");

    /* --- Create and start shell task ---------------------------------- */
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = shell_task,
        .itskpri = SHELL_PRIORITY,
        .stksz   = SHELL_STACK_SIZE,
    };
    ID shell_id = tk_cre_tsk(&ctsk);
    if (shell_id < E_OK) {
        tm_putstring((UB *)"[ERR] shell task create failed\r\n");
        return 0;
    }
    tk_sta_tsk(shell_id, 0);

    tm_putstring((UB *)"[OK]  Shell task started\r\n");

    /* Initial task can exit — shell task keeps running */
    return 0;
}
