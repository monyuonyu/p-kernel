/*
 * tests/windows/fiber_fpctl.c — does a task's FP control state (MXCSR and the
 * x87 control word) survive a Fiber switch, the way the Windows port switches?
 *
 * The Windows port (arch/windows/x86_64/win_prim.c) uses ConvertThreadToFiber
 * and CreateFiber with no flags. Microsoft documents FIBER_FLAG_FLOAT_SWITCH
 * ("the floating-point state is switched for the fiber"); without it the
 * control state may be shared by every fiber. On hosted Linux the same bug
 * (HOSTED-FP-CTL-NOT-SAVED) was fixed in dispatch.S; this is the Windows check.
 *
 * Fiber A sets round-toward-zero (MXCSR RC=11) and x87 PC=single/RC=up,
 * switches to B; B sets the defaults (MXCSR 0x1F80, CW 0x037F) and switches
 * back. A must read its own values back.
 *
 *   fiber_fpctl.exe          the port's form (no flags)
 *   fiber_fpctl.exe float    FIBER_FLAG_FLOAT_SWITCH on all three fibers
 *   fiber_fpctl.exe leak     negative control (shared state) - must FAIL
 *
 * Measured 2026-11-09 on Windows 11 24H2 (10.0.26200, AMD64): no-flags PASS
 * 2/2, float PASS 2/2, leak FAIL - SwitchToFiber on x64 keeps MXCSR and the
 * x87 CW per fiber even without the flag. Not covered: older Windows, Wine,
 * and the port's own tasks (this mimics win_prim.c, it is not the kernel).
 *
 * Build: x86_64-w64-mingw32-gcc -O1 -o fiber_fpctl.exe fiber_fpctl.c
 * Prints one line [win-fiber-fpctl] PASS|FAIL ... and exits 0 / 1.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void *fa, *fb, *fmain;

static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }
static unsigned short get_cw(void) { unsigned short v; __asm__ volatile("fnstcw %0" : "=m"(v)); return v; }
static void set_cw(unsigned short v) { __asm__ volatile("fldcw %0" : : "m"(v)); }

#define A_MXCSR 0x7F80u   /* default masks + RC=11 (toward zero) */
#define A_CW    0x0B7Fu   /* masks, PC=00 (single), RC=10 (up) */
#define B_MXCSR 0x1F80u
#define B_CW    0x037Fu

static unsigned got_mx;
static unsigned short got_cw;

static void WINAPI fiber_b(void *arg)
{
    (void)arg;
    set_mxcsr(B_MXCSR);
    set_cw(B_CW);
    SwitchToFiber(fa);
    for (;;) SwitchToFiber(fmain);
}

static void WINAPI fiber_a(void *arg)
{
    (void)arg;
    set_mxcsr(A_MXCSR);
    set_cw(A_CW);
    SwitchToFiber(fb);
    got_mx = get_mxcsr();
    got_cw = get_cw();
    set_mxcsr(B_MXCSR);   /* leave the main fiber's defaults alone either way */
    set_cw(B_CW);
    for (;;) SwitchToFiber(fmain);
}

int main(int argc, char **argv)
{
    int flt = (argc > 1 && strcmp(argv[1], "float") == 0);
    int leak = (argc > 1 && strcmp(argv[1], "leak") == 0);
    DWORD fl = flt ? FIBER_FLAG_FLOAT_SWITCH : 0;
    int ok;

    if (leak) {
        /* negative control: what a switch that shares the control state looks
         * like - A sets its values, B's values are loaded on the same context,
         * A reads. The check below must say FAIL. */
        set_mxcsr(A_MXCSR); set_cw(A_CW);
        set_mxcsr(B_MXCSR); set_cw(B_CW);
        got_mx = get_mxcsr(); got_cw = get_cw();
        ok = ((got_mx & 0xFFC0u) == A_MXCSR) && ((got_cw & 0x1F3Fu) == (A_CW & 0x1F3Fu));
        printf("[win-fiber-fpctl] %s mode=leak(negative) mxcsr=0x%04X cw=0x%04X\n",
               ok ? "PASS" : "FAIL", got_mx & 0xFFC0u, got_cw & 0x1F3Fu);
        return ok ? 0 : 1;
    }

    fmain = flt ? ConvertThreadToFiberEx(NULL, fl) : ConvertThreadToFiber(NULL);
    fa = flt ? CreateFiberEx(0, 0, fl, fiber_a, NULL) : CreateFiber(0, fiber_a, NULL);
    fb = flt ? CreateFiberEx(0, 0, fl, fiber_b, NULL) : CreateFiber(0, fiber_b, NULL);
    if (!fmain || !fa || !fb) {
        printf("[win-fiber-fpctl] FAIL could not create fibers (err %lu)\n", GetLastError());
        return 1;
    }
    SwitchToFiber(fa);
    ok = ((got_mx & 0xFFC0u) == A_MXCSR) && ((got_cw & 0x1F3Fu) == (A_CW & 0x1F3Fu));
    printf("[win-fiber-fpctl] %s mode=%s A after switch: mxcsr=0x%04X (want 0x%04X) cw=0x%04X (want 0x%04X)\n",
           ok ? "PASS" : "FAIL", flt ? "FLOAT_SWITCH" : "no-flags(port)",
           got_mx & 0xFFC0u, A_MXCSR, got_cw & 0x1F3Fu, A_CW & 0x1F3Fu);
    return ok ? 0 : 1;
}
