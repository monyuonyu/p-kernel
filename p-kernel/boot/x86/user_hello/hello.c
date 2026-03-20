/*
 *  hello.c — ring-3 user-mode test program for p-kernel
 *
 *  Uses INT 0x80 syscalls (Linux-compatible numbers):
 *    SYS_WRITE (4): write to fd 1 (serial stdout)
 *    SYS_EXIT  (1): terminate process
 *
 *  Calling convention:
 *    EAX = syscall number
 *    EBX = arg0,  ECX = arg1,  EDX = arg2
 *    Return value in EAX.
 *
 *  Built as a bare ELF32 (no libc) linked at USER_CODE_BASE = 0x00400000.
 */

/* --- Syscall helpers ----------------------------------------------- */

static int sys_write(int fd, const char *buf, int len)
{
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static void sys_exit(int code)
{
    asm volatile(
        "int $0x80"
        :
        : "a"(1), "b"(code)
        : "memory"
    );
    /* not reached */
    for (;;) asm volatile("hlt");
}

/* --- Minimal string helpers --------------------------------------- */

static int mystrlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void mywrite(const char *s)
{
    sys_write(1, s, mystrlen(s));
}

/* --- Entry point -------------------------------------------------- */

void _start(void)
{
    mywrite("Hello from ring-3!\r\n");
    mywrite("p-kernel user-space is working.\r\n");

    /* Simple arithmetic test */
    int a = 6, b = 7, c = a * b;
    char buf[4];
    buf[0] = (char)('0' + c / 10);
    buf[1] = (char)('0' + c % 10);
    buf[2] = '\r';
    buf[3] = '\n';
    mywrite("6 * 7 = ");
    sys_write(1, buf, 4);

    mywrite("Calling sys_exit(0)...\r\n");
    sys_exit(0);
}
