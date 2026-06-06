/*
 *  arch/linux/x86_64/net_dispatch.c
 *
 *  Tiny shim that owns the four public arch_linux_net_* symbols the
 *  rtl8139 driver shim calls into, and delegates to either:
 *    - net_unix_*  (loopback UDP transport, default)
 *    - net_relay_* (Phase B v1/v2 wire over public relay, selected
 *                    when PKERNEL_RELAY (multi-relay HA list) or the
 *                    legacy PKERNEL_RELAY_HOST is set in the environment)
 *
 *  Decided at arch_linux_net_init() time and stable for the process
 *  lifetime. Same source compiled identically for x86_64; promote to
 *  arch/common/linux/ once the existing net_unix.c duplication is
 *  cleaned up.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int net_unix_init(void);
extern int net_unix_send(const void *frame, int len);
extern int net_unix_recv(void *buf, int maxlen);
extern int net_unix_node_id(void);

extern int net_relay_init(void);
extern int net_relay_send(const void *frame, int len);
extern int net_relay_recv(void *buf, int maxlen);
extern int net_relay_node_id(void);

static int (*g_send)(const void *, int) = NULL;
static int (*g_recv)(void *, int)       = NULL;
static int (*g_node_id)(void)           = NULL;
static int  g_node = 1;

int arch_linux_net_init(void)
{
    const char *list = getenv("PKERNEL_RELAY");
    const char *host = getenv("PKERNEL_RELAY_HOST");
    if ((list && *list) || (host && *host)) {
        int n = net_relay_init();
        if (n > 0) {
            g_send    = net_relay_send;
            g_recv    = net_relay_recv;
            g_node_id = net_relay_node_id;
            g_node    = n;
            dprintf(2, "[net] transport = relay (node %d)\n", n);
            return n;
        }
        dprintf(2, "[net] relay init failed; falling back to loopback\n");
    }
    int n = net_unix_init();
    g_send    = net_unix_send;
    g_recv    = net_unix_recv;
    g_node_id = net_unix_node_id;
    g_node    = (n > 0) ? n : g_node;
    dprintf(2, "[net] transport = loopback (node %d)\n", g_node);
    return n;
}

int arch_linux_net_send(const void *frame, int len)
{
    return g_send ? g_send(frame, len) : -1;
}

int arch_linux_net_recv(void *buf, int maxlen)
{
    return g_recv ? g_recv(buf, maxlen) : 0;
}

int arch_linux_net_node_id(void)
{
    return g_node_id ? g_node_id() : g_node;
}
