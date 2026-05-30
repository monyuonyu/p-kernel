/*
 *  arch/linux/aarch64/sio.c
 *  Serial I/O for the Linux userspace port — wraps stdin/stdout in
 *  termios raw mode so the T-Kernel monitor (tm_putstring / tm_getline)
 *  sees them as a real UART.
 *
 *  Surface matches arch/aarch64/sio.c:
 *    sio_init()
 *    sio_send_frame(buf, size)         — used by tm_putstring
 *    sio_recv_frame(buf, size)         — used by tm_getchar
 *    sio_data_ready()
 *    sio_read_line(buf, maxlen)        — used by interactive shell
 *
 *  This TU is POSIX-only — T-Kernel headers are NOT included to avoid
 *  va_list / wchar_t collisions. The integer typedefs (INT, UB) are
 *  the same width as int / unsigned char at the ABI level, so linker
 *  resolution against T-Kernel callers works without sharing typedef.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__ANDROID__)
/* Bionic's <termios.h> inlines bits/termios_inlines.h whose helpers do
 * `errno = EINVAL` textually — they need both names visible at parse
 * time. We can't just `#include <errno.h>` because the T-Kernel errno.h
 * shadow (used by the kernel-side sources) sits earlier on the include
 * path and provides neither symbol. Declare the Bionic accessor and the
 * one POSIX errno constant the inlines reference. */
extern volatile int *__errno(void);
#define errno  (*__errno())
#define EINVAL 22
#endif
#include <termios.h>
#include <fcntl.h>

typedef int          INT;
typedef unsigned char UB;
typedef int          BOOL;
#define TRUE 1
#define FALSE 0
#define EXPORT

/* tk_dly_tsk lives in T-Kernel; forward-declare so we can yield while
 * polling stdin without blocking the whole Linux process (which would
 * starve every other T-Kernel task — including net_task). */
extern int tk_dly_tsk(long msec);

static struct termios original_termios;
static int            termios_saved = 0;

static void sio_restore_termios(void)
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    }
}

EXPORT void sio_init(void)
{
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
            termios_saved = 1;
            atexit(sio_restore_termios);
            struct termios raw = original_termios;
            raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
            raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP);
            raw.c_cflag |= CS8;
            raw.c_oflag &= ~OPOST;
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
    }
    /* Make stdin non-blocking so sio_recv_frame / sio_read_line can
     * yield to other T-Kernel tasks while waiting for input. Without
     * this, a blocking read() halts the whole Linux process and
     * net_task / DRPC task / etc. never get scheduled. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

EXPORT void sio_send_frame(const UB *buf, INT size)
{
    INT off = 0;
    while (off < size) {
        ssize_t n = write(STDOUT_FILENO, buf + off, (size_t)(size - off));
        if (n < 0) return;       /* SA_RESTART means we only see real errors */
        off += (INT)n;
    }
}

EXPORT void sio_recv_frame(UB *buf, INT size)
{
    INT off = 0;
    while (off < size) {
        ssize_t n = read(STDIN_FILENO, buf + off, (size_t)(size - off));
        if (n <= 0) return;
        off += (INT)n;
    }
}

EXPORT BOOL sio_data_ready(void)
{
    /* Non-destructive peek: temporarily switch stdin to non-blocking,
     * try to read one byte, restore. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0) return FALSE;
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return FALSE;

    UB peek;
    ssize_t n = read(STDIN_FILENO, &peek, 1);
    fcntl(STDIN_FILENO, F_SETFL, flags);

    if (n == 1) {
        /* Push the byte back to be re-read by the next sio_recv_frame.
         * The simplest portable way is to keep a one-byte ungetch
         * buffer. We use a static slot here. */
        extern UB sio_ungetch_byte;
        extern int sio_ungetch_full;
        sio_ungetch_byte = peek;
        sio_ungetch_full = 1;
        return TRUE;
    }
    return FALSE;  /* n == 0 (EOF) or n < 0 (EAGAIN or real error) */
}

UB  sio_ungetch_byte = 0;
int sio_ungetch_full = 0;

EXPORT INT sio_read_line(UB *buf, INT maxlen)
{
    INT pos = 0;
    int eof_polls = 0;
    while (pos < maxlen - 1) {
        UB ch;
        if (sio_ungetch_full) {
            ch = sio_ungetch_byte;
            sio_ungetch_full = 0;
        } else {
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n < 0) {
                /* EAGAIN — no data right now. Yield to other tasks
                 * for 10 ms then poll again. */
                tk_dly_tsk(10);
                continue;
            }
            if (n == 0) {
                /* True EOF on stdin (piped input drained). Don't
                 * exit immediately — give other tasks time to run
                 * (so a backgrounded node-2 keeps living after its
                 * stdin closes). After enough idle polls give up. */
                eof_polls++;
                if (eof_polls > 100) {
                    /* > 1 sec of no stdin and no input ever buffered
                     * → safe to call this a real EOF. */
                    if (pos == 0) {
                        /* Keep running forever so the node stays
                         * alive even with no stdin. */
                        for (;;) tk_dly_tsk(1000);
                    }
                    buf[pos] = '\0';
                    return pos;
                }
                tk_dly_tsk(10);
                continue;
            }
            eof_polls = 0;
        }
        if (ch == '\r' || ch == '\n') {
            sio_send_frame((const UB *)"\r\n", 2);
            buf[pos] = '\0';
            return pos;
        }
        if (ch == 0x7F || ch == 0x08) {
            if (pos > 0) {
                pos--;
                sio_send_frame((const UB *)"\b \b", 3);
            }
            continue;
        }
        sio_send_frame(&ch, 1);
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}
