/*
 * console_ring.c — the node's own voice, kept (hosted ports only).
 *
 *  Every byte the kernel prints (tm_putstring → sio_send_frame) is teed
 *  into this fixed ring so the galaxy can serve GET /console.txt — the
 *  remote-diagnosis API mk_pino asked for ("環境構築が重要です"): instead
 *  of hand-copying the advanced screen's log, anyone (including the AI
 *  commander over the shared loopback) can curl the node's recent
 *  console output, [mind] gate prints included.
 *
 *  This TU is POSIX-only like sio.c — no T-Kernel headers (va_list /
 *  wchar_t collisions); plain C types match the ABI widths.
 *
 *  Concurrency honesty: writers reserve their region with an atomic
 *  fetch-add and then fill it; a reader snapshots the head and walks
 *  backward. A writer that laps the reader mid-copy can garble the
 *  OLDEST bytes of one read — this is a diagnostics ring, not a flight
 *  recorder, and a torn line is visible as such. No locks on the print
 *  path (it must stay as cheap as the write() it shadows).
 */

#include <string.h>

#define CR_SIZE 32768u                 /* power of two */

static char                       cr_buf[CR_SIZE];
static volatile unsigned long     cr_head = 0;   /* total bytes ever     */

void console_ring_note(const unsigned char *buf, int size)
{
    if (size <= 0) return;
    unsigned long start = __sync_fetch_and_add(&cr_head, (unsigned long)size);
    for (int i = 0; i < size; i++)
        cr_buf[(start + (unsigned long)i) & (CR_SIZE - 1)] = (char)buf[i];
}

/* copy the newest <= max bytes into out; returns the byte count. */
int console_ring_read(char *out, int max)
{
    if (max <= 0) return 0;
    unsigned long head = cr_head;
    unsigned long n = head < CR_SIZE ? head : CR_SIZE;
    if (n > (unsigned long)max) n = (unsigned long)max;
    unsigned long from = head - n;
    for (unsigned long i = 0; i < n; i++)
        out[i] = cr_buf[(from + i) & (CR_SIZE - 1)];
    return (int)n;
}

unsigned long console_ring_total(void) { return cr_head; }
