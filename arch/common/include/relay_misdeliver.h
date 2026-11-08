/*
 *  relay_misdeliver.h — the receiving side of a one-node frame over the relay
 *  (hosted, net_relay.c and net_relay_tcp.c; test-holes H20).
 *
 *  A REL_DATA packet is a frame the sender meant for one node: it read the
 *  node from the frame's MAC 52:54:00:00:hi:lo (N2: id = hi << 8 | lo, wide ids
 *  use the high byte). If that MAC names another node, the sender (or the
 *  relay) got the node wrong. Nothing else sees it: the stack drops the frame
 *  and the round trips and the gossip reach the right node by other ways
 *  (audit-163: over 200 frames for 260 went to node 4 and [n2-tcp] stayed
 *  green). So the receiver counts it and says so on stderr:
 *    [<who>] misdelivered: a frame for node <id> came to node <me> (<count>)
 *  for the first 3 and every 100th. Observation only: the frame is still
 *  handed up as before. [n2-tcp] T5 wants no such line.
 *
 *  The rx paths run on ~4 KB task stacks: no dprintf (see log_relay in
 *  net_relay.c); the line is formatted by hand into a static buffer.
 */
#ifndef PKERNEL_RELAY_MISDELIVER_H
#define PKERNEL_RELAY_MISDELIVER_H
#include <unistd.h>

static void relay_md_put(char *b, int cap, int *p, const char *s)
{
    while (*s && *p < cap - 1) b[(*p)++] = *s++;
}

static void relay_md_dec(char *b, int cap, int *p, unsigned v)
{
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && *p < cap - 1) b[(*p)++] = t[--n];
}

/* f, len: the frame handed up; me: this node's id */
static void relay_note_misdelivered(const char *who, int me, const unsigned char *f, int len)
{
    static unsigned count;
    static char line[112];
    if (len < 6 || f[0] != 0x52 || f[1] != 0x54 || f[2] != 0x00 || f[3] != 0x00) return;
    int id = (f[4] << 8) | f[5];
    if (id == 0 || id == me) return;
    count++;
    if (count > 3 && count % 100 != 0) return;
    int p = 0;
    relay_md_put(line, (int)sizeof line, &p, "[");
    relay_md_put(line, (int)sizeof line, &p, who);
    relay_md_put(line, (int)sizeof line, &p, "] misdelivered: a frame for node ");
    relay_md_dec(line, (int)sizeof line, &p, (unsigned)id);
    relay_md_put(line, (int)sizeof line, &p, " came to node ");
    relay_md_dec(line, (int)sizeof line, &p, (unsigned)me);
    relay_md_put(line, (int)sizeof line, &p, " (");
    relay_md_dec(line, (int)sizeof line, &p, count);
    relay_md_put(line, (int)sizeof line, &p, ")\n");
    ssize_t w = write(2, line, (size_t)p);
    (void)w;
}

#endif
