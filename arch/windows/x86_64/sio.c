/*
 *  arch/windows/x86_64/sio.c
 *
 *  Serial I/O for the native Windows port — wraps the Win32 console /
 *  stdin pipe so the T-Kernel monitor (tm_putstring / tm_getline) and the
 *  interactive `mind` shell see them as a UART. Public surface matches
 *  arch/linux/x86_64/sio.c:
 *    sio_init()
 *    sio_send_frame(buf, size)
 *    sio_recv_frame(buf, size)
 *    sio_data_ready()
 *    sio_read_line(buf, maxlen)
 *    sio_ungetch_byte / sio_ungetch_full
 *
 *  This TU is Win32-only — T-Kernel headers are NOT included (like the
 *  Linux sio.c) to avoid type collisions. INT/UB are int / unsigned char
 *  at the ABI level, so linker resolution against T-Kernel callers works.
 *
 *  Non-blocking reads let sio_read_line yield to other T-Kernel tasks via
 *  tk_dly_tsk while waiting for input, instead of halting the whole
 *  process (which would starve net/DRPC/DMN tasks).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef int           INT;
typedef unsigned char UB;
#define EXPORT

/* tk_dly_tsk lives in T-Kernel; forward-declare so we can yield while
 * polling stdin without blocking the whole process. */
extern int tk_dly_tsk(long msec);

/* console_ring.c (hosted-only): tee printed bytes for GET /console.txt. */
extern void console_ring_note(const unsigned char *buf, int size);

static HANDLE win_hin  = INVALID_HANDLE_VALUE;
static HANDLE win_hout = INVALID_HANDLE_VALUE;
static int    win_is_console = 0;

EXPORT void sio_init(void)
{
    DWORD mode;

    win_hin  = GetStdHandle(STD_INPUT_HANDLE);
    win_hout = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Raw console input: per-character, no echo, no line editing — the
     * shell does its own echo and line editing (sio_read_line). */
    if (win_hin != INVALID_HANDLE_VALUE && GetConsoleMode(win_hin, &mode)) {
        win_is_console = 1;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        SetConsoleMode(win_hin, mode);
    }
}

EXPORT void sio_send_frame(const UB *buf, INT size)
{
    INT off = 0;
    console_ring_note(buf, size);
    if (win_hout == INVALID_HANDLE_VALUE) return;
    while (off < size) {
        DWORD wrote = 0;
        if (!WriteFile(win_hout, buf + off, (DWORD)(size - off), &wrote, NULL))
            return;
        if (wrote == 0) return;
        off += (INT)wrote;
    }
}

/*
 * Read one input byte without blocking.
 *   >= 0 : the byte
 *   -1   : no data available right now (EAGAIN)
 *   -2   : end of input (broken pipe / EOF)
 */
static int win_read_byte(void)
{
    if (win_hin == INVALID_HANDLE_VALUE) return -2;

    if (win_is_console) {
        INPUT_RECORD rec;
        DWORD n = 0;
        if (WaitForSingleObject(win_hin, 0) != WAIT_OBJECT_0) return -1;
        if (!ReadConsoleInputA(win_hin, &rec, 1, &n) || n == 0) return -1;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            char c = rec.Event.KeyEvent.uChar.AsciiChar;
            if (c != 0) return (unsigned char)c;
        }
        return -1;   /* non-key / dead-key / key-up event */
    } else {
        /* Pipe or redirected file. */
        DWORD avail = 0;
        unsigned char c;
        DWORD got = 0;
        if (PeekNamedPipe(win_hin, NULL, 0, NULL, &avail, NULL)) {
            if (avail == 0) return -1;   /* pipe open, no data yet */
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            return -2;                    /* write end closed → EOF */
        }
        /* File (peek not applicable) or pipe with data: read one byte. */
        if (!ReadFile(win_hin, &c, 1, &got, NULL)) {
            return (GetLastError() == ERROR_BROKEN_PIPE) ? -2 : -1;
        }
        if (got == 0) return -2;         /* EOF */
        return (int)c;
    }
}

UB  sio_ungetch_byte = 0;
int sio_ungetch_full = 0;

EXPORT void sio_recv_frame(UB *buf, INT size)
{
    INT off = 0;
    if (off < size && sio_ungetch_full) {
        buf[off++] = sio_ungetch_byte;
        sio_ungetch_full = 0;
    }
    while (off < size) {
        int c = win_read_byte();
        if (c == -1) { tk_dly_tsk(10); continue; }
        if (c == -2) return;             /* EOF */
        buf[off++] = (UB)c;
    }
}

EXPORT int sio_data_ready(void)
{
    int c;
    if (sio_ungetch_full) return 1;
    c = win_read_byte();
    if (c >= 0) {
        sio_ungetch_byte = (UB)c;
        sio_ungetch_full = 1;
        return 1;
    }
    return 0;   /* -1 (no data) or -2 (EOF) */
}

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
            int c = win_read_byte();
            if (c == -1) {
                /* No data — yield to other tasks for 10 ms then retry. */
                tk_dly_tsk(10);
                continue;
            }
            if (c == -2) {
                /* EOF on stdin. Don't exit immediately — give other
                 * tasks time to run. After enough idle polls, give up. */
                eof_polls++;
                if (eof_polls > 100) {
                    if (pos == 0) {
                        /* Keep the node alive even with no stdin. */
                        for (;;) tk_dly_tsk(1000);
                    }
                    buf[pos] = '\0';
                    return pos;
                }
                tk_dly_tsk(10);
                continue;
            }
            eof_polls = 0;
            ch = (UB)c;
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
