/*
 *  blk_ssy.c (x86)
 *  Block device subsystem — ssid=2
 *
 *  Maintains a registry of up to BLK_DEV_MAX block devices.
 *  Drivers call blk_ssy_register() at init; consumers call
 *  blk_ssy_lookup() to get the ops pointer for a named device.
 *
 *  No per-task resources → cleanupfn = NULL.
 *  svchdr is a stub placeholder; user-space block I/O syscalls
 *  (SYS_BLK_READ/WRITE) can be added here in a future pass.
 */

#include "kernel.h"
#include "blk_ssy.h"
#include <syscall.h>
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* Device registry                                                      */
/* ------------------------------------------------------------------ */

static const BLK_OPS *blk_devs[BLK_DEV_MAX];
static INT            blk_ndev = 0;

INT blk_ssy_register(const BLK_OPS *ops)
{
    if (!ops || !ops->name) return -1;
    if (blk_ndev >= BLK_DEV_MAX) return -1;

    /* Reject duplicate names */
    for (INT i = 0; i < blk_ndev; i++) {
        const char *a = blk_devs[i]->name;
        const char *b = ops->name;
        INT j = 0;
        while (a[j] && a[j] == b[j]) j++;
        if (a[j] == '\0' && b[j] == '\0') return -1; /* duplicate */
    }

    blk_devs[blk_ndev++] = ops;
    return 0;
}

const BLK_OPS *blk_ssy_lookup(const char *name)
{
    if (!name) return NULL;
    for (INT i = 0; i < blk_ndev; i++) {
        const char *a = blk_devs[i]->name;
        INT j = 0;
        while (a[j] && a[j] == name[j]) j++;
        if (a[j] == '\0' && name[j] == '\0') return blk_devs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* svchdr — placeholder for future SYS_BLK_* user-space syscalls     */
/* ------------------------------------------------------------------ */

static INT blk_svchdr(void *pk_para, FN fncd)
{
    (void)pk_para; (void)fncd;
    return -1;  /* not yet implemented */
}

/* ------------------------------------------------------------------ */
/* blk_ssy_init                                                         */
/* ------------------------------------------------------------------ */

void blk_ssy_init(void)
{
    blk_ndev = 0;
    for (INT i = 0; i < BLK_DEV_MAX; i++) blk_devs[i] = NULL;

    T_DSSY dssy;
    dssy.ssyatr    = TA_NULL;
    dssy.ssypri    = 2;
    dssy.svchdr    = (FP)blk_svchdr;
    dssy.breakfn   = NULL;
    dssy.startupfn = NULL;
    dssy.cleanupfn = NULL;   /* no per-task resources */
    dssy.eventfn   = NULL;
    dssy.resblksz  = 0;

    ER er = tk_def_ssy(BLK_SSID, &dssy);
    if (er == E_OK)
        tm_putstring((UB *)"[blk_ssy] registered (ssid=2)\r\n");
    else
        tm_putstring((UB *)"[blk_ssy] tk_def_ssy FAILED\r\n");
}
