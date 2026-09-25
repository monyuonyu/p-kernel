/* yield_pull_hook.c — a STRONG cradle_poll_and_pull for the cooperative-yield
 * cert (run_yield.sh), overriding the weak no-op in student_shell.c.
 *
 * It models the one transport property Cert D needs: a lesson the transport has
 * seen is re-offered on every poll until an ingest returns >0 (cradle_net.c
 * advances its per-origin high-water only then). The cert sets
 * g_yield_pending; each poll (the net task's, or the student's start-of-batch
 * pull) offers it once. With g_yield_pending == NULL (Certs A-C) it is a no-op,
 * exactly like the weak stub. */
#include <stdint.h>

int cradle_lesson_ingest(const uint8_t *body, int len);

const uint8_t *g_yield_pending     = 0;
int            g_yield_pending_len = 0;
int            g_yield_last_ingest = 0;

void cradle_poll_and_pull(void)
{
    if (!g_yield_pending) return;
    g_yield_last_ingest = cradle_lesson_ingest(g_yield_pending, g_yield_pending_len);
    if (g_yield_last_ingest > 0) g_yield_pending = 0;   /* high-water advanced */
}
