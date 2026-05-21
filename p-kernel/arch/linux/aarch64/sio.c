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
#include <termios.h>
#include <fcntl.h>
/* Not including <errno.h>: T-Kernel's <errno.h> (in include/kernel/tkernel)
 * shadows the system one and defines only T-Kernel error codes, no
 * POSIX errno. Our signal handlers use SA_RESTART so EINTR is not
 * an expected return from read/write. */

typedef int          INT;
typedef unsigned char UB;
typedef int          BOOL;
#define TRUE 1
#define FALSE 0
#define EXPORT

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
    if (!isatty(STDIN_FILENO)) {
        /* Piped input or test environment — leave stdin untouched. */
        return;
    }
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
    while (pos < maxlen - 1) {
        UB ch;
        if (sio_ungetch_full) {
            ch = sio_ungetch_byte;
            sio_ungetch_full = 0;
        } else {
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) break;
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
        sio_send_frame(&ch, 1);   /* local echo */
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}
