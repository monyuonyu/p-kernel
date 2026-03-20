/*
 *  ai_stats.c (x86)
 *  Global AI statistics object + ai_kernel_init()
 */

#include "kernel.h"
#include "ai_kernel.h"
#include <tmonitor.h>

/* Global stats — read by any module */
AI_STATS ai_stats = { 0, 0, 0, {0,0,0}, 0, 0, 0, 0 };

/* ------------------------------------------------------------------ */
/* ai_kernel_init — call once from usermain() after NIC is up         */
/* ------------------------------------------------------------------ */

void ai_kernel_init(void)
{
    tensor_init();
    ai_job_init();
    pipeline_init();

    tm_putstring((UB *)"[ai]   Tensor pool   : 16 slots × 16 KB\r\n");
    tm_putstring((UB *)"[ai]   AI job queue  : 8 slots (software NPU)\r\n");
    tm_putstring((UB *)"[ai]   Pipeline      : 16 frames zero-copy\r\n");
    tm_putstring((UB *)"[ai]   MLP model     : 4→8→8→3 sensor classifier\r\n");
}

/* ------------------------------------------------------------------ */
/* Stats print                                                         */
/* ------------------------------------------------------------------ */

static void put_dec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v > 0 && i > 0) { buf[--i] = (char)('0' + v%10); v /= 10; } }
    tm_putstring((UB *)(buf+i));
}

void ai_stats_print(void)
{
    tm_putstring((UB *)"[ai stats]\r\n");
    tm_putstring((UB *)"  inferences total : "); put_dec(ai_stats.inferences_total);  tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"    local          : "); put_dec(ai_stats.inferences_local);  tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"    remote (DRPC)  : "); put_dec(ai_stats.inferences_remote); tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  class normal     : "); put_dec(ai_stats.class_count[0]);    tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  class alert      : "); put_dec(ai_stats.class_count[1]);    tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  class critical   : "); put_dec(ai_stats.class_count[2]);    tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  pipeline in/out  : "); put_dec(ai_stats.pipeline_in);
    tm_putstring((UB *)" / "); put_dec(ai_stats.pipeline_out); tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  FL rounds        : "); put_dec(ai_stats.fl_rounds);         tm_putstring((UB *)"\r\n");
    tm_putstring((UB *)"  AI jobs done     : "); put_dec(ai_stats.ai_jobs_done);      tm_putstring((UB *)"\r\n");
}
