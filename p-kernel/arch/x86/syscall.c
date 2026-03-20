/*
 *  syscall.c (x86)
 *  System call handler (INT 0x80)
 *
 *  The IDT gate is registered with DPL=3 so ring-3 code can trigger it.
 *  In IA-32e mode the gate must use CS=0x18 (64-bit code segment) so the
 *  CPU switches to 64-bit mode on entry.  The handler (syscall_isr in
 *  isr.S) saves registers, far-jumps to 32-bit mode to call
 *  syscall_dispatch(), far-jumps back to 64-bit, then iretq to ring3.
 *
 *  Ring-3 calling convention (set before INT 0x80):
 *    EAX = syscall number
 *    EBX = arg0,  ECX = arg1,  EDX = arg2
 *    Return value → EAX
 */

#include "kernel.h"
#include "p_syscall.h"
#include <tmonitor.h>

/*
 * Forward declarations to avoid stdint.h conflicts:
 * idt_set_gate() lives in boot/x86/idt.c (compiled with -I.)
 * KERNEL64_CS = 0x18 (64-bit code segment, IA-32e mode)
 */
IMPORT void idt_set_gate(UB num, unsigned long long handler,
                          UH sel, UB flags);
#define KERNEL64_CS  0x18u

/* syscall_isr entry point declared in isr.S */
extern void syscall_isr(void);

/* Serial frame send (from sio.c) */
IMPORT void sio_send_frame(const UB *buf, INT size);

void syscall_init(void)
{
    /*
     * INT 0x80: DPL=3 trap gate (0xEF), target CS = KERNEL64_CS (0x18).
     * DPL=3  → ring-3 code may issue INT 0x80 without a GPF.
     * trap gate (type=0xF) → IF is NOT cleared on entry (unlike int gate).
     * CS=0x18 → CPU enters 64-bit mode; handler uses iretq to return.
     */
    idt_set_gate(0x80,
                 (unsigned long long)(UW)syscall_isr,
                 (UH)KERNEL64_CS,
                 0xEF);   /* P=1, DPL=3, type=0xF (64-bit trap gate) */

    tm_putstring((UB *)"[syscall] int 0x80 registered (ring3 trap gate)\r\n");
}

/* ----------------------------------------------------------------- */
/* Serial output helper (for sys_write)                              */
/* ----------------------------------------------------------------- */

static void serial_write(const char *buf, W len)
{
    for (W i = 0; i < len; i++) {
        sio_send_frame((const UB *)&buf[i], 1);
    }
}

/* ----------------------------------------------------------------- */
/* Syscall dispatcher                                                 */
/* ----------------------------------------------------------------- */

W syscall_dispatch(W nr, W arg0, W arg1, W arg2)
{
    switch (nr) {

    case SYS_WRITE: {
        /* arg0=fd, arg1=buf_ptr, arg2=len */
        if (arg0 != 1 && arg0 != 2) return -1;
        const char *buf = (const char *)(UW)arg1;
        W len = arg2;
        if (len < 0 || len > 65536) return -1;
        serial_write(buf, len);
        return len;
    }

    case SYS_EXIT: {
        /* arg0 = exit code */
        tm_putstring((UB *)"\r\n[proc] exited (code=");
        W code = arg0;
        if (code < 0) { tm_putstring((UB *)"-"); code = -code; }
        char buf[12]; INT i = 11; buf[i] = '\0';
        if (code == 0) { buf[--i] = '0'; }
        else { while (code > 0 && i > 0) { buf[--i] = (char)('0' + code%10); code /= 10; } }
        tm_putstring((UB *)(buf+i));
        tm_putstring((UB *)")\r\np-kernel> ");
        tk_ext_tsk();
        /* not reached */
        return 0;
    }

    default:
        return -1;  /* ENOSYS */
    }
}
