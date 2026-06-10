/*
 *  12_ring3/03_core_mind/core_mind.c — ring3-core Wave C
 *  (docs/architecture/ring3-core.md III.1.4)
 *
 *  THE MIND'S MATH IN RING 3.  This ELF links the REAL arch/common/moe.c
 *  + arch/common/dtr.c (whole-file dual-compile, --gc-sections); the
 *  moe_infer / dtr_decide / dtr_forward_probs / train_forward path below
 *  is the SAME .text the kernel runs — computed here, in ring 3, on a
 *  user-space copy of the live weights fetched via SYS_DTR_WEIGHTS_GET.
 *
 *  Anti-fork rule (III.1.1): this file contains ZERO math — only the
 *  driver (argv modes, weight fetch, exit-code channel).
 *
 *  Three modes, selected by argv (the loader builds a Linux-style
 *  argc/argv frame; argc at [esp+0], argv pointers from [esp+4]):
 *
 *    (none)    fetch weights → dtr_weights_set → cls = moe_infer(V0)
 *              in ring-3 → sys_exit(cls).            (gates M1/M7)
 *    -poison   c_pre = moe_infer(V0); perturb ONLY the user copy:
 *              wbuf[632 + c_pre] -= 50.0f  (b_cls[c_pre] — the last 3
 *              floats of the 635-float dtr_weights_get layout); install;
 *              c_post = moe_infer(V0); sys_exit((c_pre << 4) | c_post).
 *              Only a computation actually running on the user-space
 *              copy can feel the poison.              (gate M3)
 *    -crash    one successful ring-3 moe_infer, then *(int*)0 = 0 —
 *              the Wave B inducer inside the FAT ELF, proving the
 *              bigger mind is still reapable.         (gate M5)
 *
 *  V0 MUST match arch/x86/shell.c (R3_V0_T/H/P/L) and the Wave B ELFs
 *  (single-definition discipline; the expected class is NEVER
 *  hard-coded — it is whatever the live ring-0 oracle says).
 */

#include "moe.h"        /* moe_infer — the REAL one, linked from moe.c  */
#include "dtr.h"        /* dtr_weights_set, DTR_WEIGHT_FLOATS           */
#include "p_syscall.h"  /* SYS_EXIT, SYS_WRITE, SYS_DTR_WEIGHTS_GET     */

#define V0_T  30
#define V0_H  10
#define V0_P  5
#define V0_L  90

/* flat index of b_cls[0] in the dtr_weights_get layout (III.3b):
 * 635 floats total, b_cls = the last DOUT(3) → 635 - 3 = 632. */
#define WB_B_CLS0  (DTR_WEIGHT_FLOATS - 3)

/* ---- driver-only glue (no plibc.h: kernel headers own the types) -- */
static inline int cm_sc(int nr, int a0, int a1, int a2)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a0), "c"(a1), "d"(a2)
                     : "memory");
    return ret;
}

static void cm_exit(int code)
{
    cm_sc(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

static void cm_puts(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    cm_sc(SYS_WRITE, 1, (int)s, n);
}

static void cm_putdec(unsigned v)
{
    char buf[12];
    int  i = 11;
    buf[i] = '\0';
    if (v == 0) { cm_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    cm_puts(&buf[i]);
}

static int cm_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The weight buffer: 635 floats = 2,540 B.  STATIC, not stack — the
 * hosted-relay lesson: KB-scale locals overflow task stacks silently. */
static float wbuf[DTR_WEIGHT_FLOATS];

/* Fetch the LIVE kernel weights and install them into THIS ELF's own
 * static dtr arrays (the dual-compiled dtr.c's private copy). */
static int fetch_and_install(void)
{
    int n = cm_sc(SYS_DTR_WEIGHTS_GET, (int)wbuf, DTR_WEIGHT_FLOATS, 0);
    if (n != DTR_WEIGHT_FLOATS) {
        cm_puts("[core_mind] SYS_DTR_WEIGHTS_GET failed\r\n");
        return -1;
    }
    dtr_weights_set(wbuf);
    return 0;
}

void core_mind_main(unsigned sp0)
{
    int          argc = *(int *)sp0;
    char       **argv = (char **)(sp0 + 4);
    const char  *mode = (argc >= 2) ? argv[1] : "";

    if (fetch_and_install() != 0)
        cm_exit(-1);

    if (cm_streq(mode, "-poison")) {
        /* III.3b: prove the ring-3 COPY is what computes.  c_pre FIRST
         * (must match the live oracle), THEN poison only the user copy:
         * a -50 logit floor on b_cls[c_pre] dominates trained logits
         * (O(±10)), so the argmax MUST leave c_pre.  The kernel weights
         * are untouched — gate M7 (restart == oracle) proves it. */
        int c_pre = (int)moe_infer(V0_T, V0_H, V0_P, V0_L);   /* ring-3 */
        wbuf[WB_B_CLS0 + c_pre] -= 50.0f;
        dtr_weights_set(wbuf);
        int c_post = (int)moe_infer(V0_T, V0_H, V0_P, V0_L);  /* ring-3 */
        cm_puts("[core_mind] poison: pre=");
        cm_putdec((unsigned)c_pre);
        cm_puts(" post=");
        cm_putdec((unsigned)c_post);
        cm_puts("\r\n");
        /* exit encoding (III.4 M3): (c_pre << 4) | c_post */
        cm_exit((c_pre << 4) | c_post);
    }

    if (cm_streq(mode, "-crash")) {
        /* one successful ring-3 forward, then the Wave B inducer */
        int cls = (int)moe_infer(V0_T, V0_H, V0_P, V0_L);     /* ring-3 */
        (void)cls;
        cm_puts("[core_mind] infer ok - now writing *(int*)0 from ring3\r\n");
        *(volatile int *)0 = 0;    /* #PF from ring3 */
        cm_puts("[core_mind] STILL ALIVE - fault did not fire\r\n");
        cm_exit(99);               /* NOT reached if the fault fired */
    }

    /* normal: the REAL moe_infer, in ring 3, on the fetched weights */
    int cls = (int)moe_infer(V0_T, V0_H, V0_P, V0_L);         /* ring-3 */
    cm_puts("[core_mind] ring3 moe_infer class=");
    if (cls >= 0 && cls <= 9) cm_putdec((unsigned)cls);
    else                      cm_puts("ERR");
    cm_puts("\r\n");
    cm_exit(cls);                  /* gate channel: exit code = class */
}

/* _start: capture the entry ESP (argc at [esp+0], argv at [esp+4] —
 * elf_loader.c build_argv_stack) and hand it to C.  Top-level asm so
 * no compiler prologue moves ESP before we read it. */
__asm__(
    "    .text\n"
    "    .globl _start\n"
    "_start:\n"
    "    mov  %esp, %eax\n"
    "    push %eax\n"
    "    call core_mind_main\n"
    "1:  hlt\n"
    "    jmp  1b\n"
);
