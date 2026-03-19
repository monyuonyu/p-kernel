/*
 *  shell.c (x86)
 *  Interactive shell for p-kernel
 *
 *  Runs as a T-Kernel task.
 *  Input : COM1 serial (always available) OR PS/2 keyboard via IRQ1
 *          Serial is read first; keyboard IRQ feeds the same semaphore.
 *  Output: VGA text buffer + COM1 serial (mirrored)
 *
 *  Commands: help, ver, mem, ps, clear
 */

#include "vga.h"
#include "keyboard.h"
#include "rtl8139.h"
#include "netstack.h"
#include "kernel.h"

#define SHELL_LINE_MAX  128
#define PS_MAX_TSKID    CFN_MAX_TSKID

/* ------------------------------------------------------------------ */
/* Output helpers (VGA + serial mirror)                                */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT void sio_recv_frame(UB *buf, INT size);

static void sout(const char *s)
{
    vga_puts(s);
    INT len = 0;
    while (s[len]) len++;
    sio_send_frame((const UB *)s, len);
}

static void soutc(char c)
{
    vga_putchar(c);
    sio_send_frame((const UB *)&c, 1);
}

/* Unsigned decimal */
static void sout_dec(UW v)
{
    char buf[12];
    INT i = 11;
    buf[i] = '\0';
    if (v == 0) { sout("0"); return; }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    sout(&buf[i]);
}

/* Unsigned hex (8 digits) */
static void sout_hex(UW v)
{
    char buf[9];
    buf[8] = '\0';
    for (INT i = 7; i >= 0; i--) {
        INT d = (INT)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    sout(buf);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(void)
{
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("Available commands:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  help   - show this message\r\n");
    sout("  ver    - kernel version info\r\n");
    sout("  mem    - memory layout\r\n");
    sout("  ps     - list tasks\r\n");
    sout("  net    - NIC status (RTL8139 + stats)\r\n");
    sout("  arp    - ARP cache + send request for gateway\r\n");
    sout("  ping <IP>        - send ICMP echo request\r\n");
    sout("  dns <host>       - DNS A-record lookup\r\n");
    sout("  udp <IP> <p> <m> - send UDP datagram\r\n");
    sout("  clear            - clear screen\r\n");
}

static void cmd_ver(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout("micro T-Kernel 2.0  /  p-kernel x86/QEMU port\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

IMPORT void *knl_lowmem_top;
IMPORT void *knl_lowmem_limit;

static void cmd_mem(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("Memory layout:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    sout("  kernel heap base : 0x"); sout_hex((UW)knl_lowmem_top);   sout("\r\n");
    sout("  kernel heap limit: 0x"); sout_hex((UW)knl_lowmem_limit); sout("\r\n");

    UW avail = (UW)knl_lowmem_limit - (UW)knl_lowmem_top;
    sout("  heap available   : "); sout_dec(avail / 1024); sout(" KB\r\n");
    sout("  heap limit       : ");
    sout_dec((UW)knl_lowmem_limit / 1024 / 1024); sout(" MB\r\n");
}

/* Task state name */
static const char *tsk_state(UINT st)
{
    switch (st & 0xFF) {
    case TTS_RUN: return "RUN    ";
    case TTS_RDY: return "READY  ";
    case TTS_WAI: return "WAIT   ";
    case TTS_SUS: return "SUSPEND";
    case TTS_WAS: return "WAI+SUS";
    case TTS_DMT: return "DORMANT";
    default:      return "?      ";
    }
}

static void cmd_ps(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("TID  PRI  STATE\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (INT id = 1; id <= PS_MAX_TSKID; id++) {
        T_RTSK rtsk;
        if (tk_ref_tsk((ID)id, &rtsk) != E_OK) continue;
        /* skip truly empty/invalid entries */
        if ((rtsk.tskstat & 0xFF) == 0) continue;

        sout("  "); sout_dec((UW)id);
        sout("    "); sout_dec((UW)rtsk.tskpri);
        sout("  "); sout(tsk_state(rtsk.tskstat));
        sout("\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* Line input                                                          */
/* ------------------------------------------------------------------ */

static INT str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void cmd_net(void)
{
    if (!rtl_initialized) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("RTL8139 not found. Launch QEMU with:\r\n");
        sout("  -netdev user,id=n0 -device rtl8139,netdev=n0\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    UB mac[6];
    rtl8139_get_mac(mac);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("RTL8139 NIC status:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    sout("  MAC  : ");
    for (INT i = 0; i < 6; i++) {
        if (i) sout(":");
        /* print hex byte */
        const char *h = "0123456789ABCDEF";
        char buf[3] = { h[mac[i] >> 4], h[mac[i] & 0xF], '\0' };
        sout(buf);
    }
    sout("\r\n");

    sout("  RX   : "); sout_dec(rtl_rx_count);    sout(" frames\r\n");
    sout("  TX   : "); sout_dec(rtl_tx_count);    sout(" frames\r\n");
    sout("  ARP  : "); sout_dec(net_rx_arp);       sout(" rx / ");
                       sout_dec(net_tx_arp);       sout(" tx\r\n");
    sout("  ICMP : "); sout_dec(net_rx_icmp_req);  sout(" req rx / ");
                       sout_dec(net_rx_icmp_rep);  sout(" rep rx / ");
                       sout_dec(net_tx_icmp);      sout(" tx\r\n");
    sout("  UDP  : "); sout_dec(net_rx_udp);       sout(" rx / ");
                       sout_dec(net_tx_udp);       sout(" tx\r\n");
    sout("  My IP: "); sout(ip_str(NET_MY_IP));    sout("\r\n");
    sout("  GW   : "); sout(ip_str(NET_GW_IP));    sout("\r\n");
    sout("  DNS  : "); sout(ip_str(NET_DNS_IP));   sout("\r\n");
}

/* Parse "A.B.C.D" → IP4 value.  Returns 1 on success. */
static INT parse_ip(const char *s, UW *out)
{
    UW ip = 0;
    for (INT oct = 0; oct < 4; oct++) {
        UW v = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (UW)(*s++ - '0'); }
        if (v > 255) return 0;
        ip |= (v << (oct * 8));         /* IP4 format: byte 0 = octet A */
        if (oct < 3) { if (*s != '.') return 0; s++; }
    }
    *out = ip;
    return 1;
}

IMPORT INT icmp_ping_shell(UW dst_ip);

static void cmd_ping(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: ping <A.B.C.D>\r\n");
        return;
    }

    UW dst;
    if (!parse_ip(arg, &dst)) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("Invalid IP address\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    sout("PING "); sout(ip_str(dst)); sout(" ...\r\n");

    /* Retry up to 10×100ms waiting for ARP to resolve */
    INT r = -1;
    for (INT retry = 0; retry < 10; retry++) {
        r = icmp_ping_shell(dst);
        if (r == 0) break;
        tk_dly_tsk(100);    /* sleep → net_task gets to process ARP reply */
    }
    if (r < 0) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        sout("ARP timed out — no reply from gateway\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        sout("(watch for [icmp] echo REPLY in the log)\r\n");
    }
}

static void cmd_arp(void)
{
    UB mac[6];
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("Sending ARP request for gateway ");
    sout(ip_str(NET_GW_IP)); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    arp_request(NET_GW_IP);

    /* Show what's already cached */
    if (arp_lookup(NET_GW_IP, mac)) {
        sout("Gateway MAC (cached): ");
        for (INT i = 0; i < 6; i++) {
            if (i) sout(":");
            const char *h = "0123456789ABCDEF";
            char buf[3] = { h[mac[i]>>4], h[mac[i]&0xF], '\0' };
            sout(buf);
        }
        sout("\r\n");
    } else {
        sout("(ARP reply pending — watch for [arp] log)\r\n");
    }
}

IMPORT INT dns_query(const char *hostname, UW *out_ip);
IMPORT INT udp_send(UW dst_ip, UH src_port, UH dst_port,
                    const UB *data, UH data_len);

static void cmd_dns(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: dns <hostname>\r\n");
        return;
    }

    if (!rtl_initialized) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("NIC not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    sout("Resolving '"); sout(arg); sout("' ...\r\n");

    UW ip;
    INT r = dns_query(arg, &ip);
    if (r == 0) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        sout(arg); sout(" -> "); sout(ip_str(ip)); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("DNS failed (timeout or NXDOMAIN)\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_udp(const char *arg)
{
    /* udp <ip> <port> <message> */
    while (*arg == ' ') arg++;

    UW dst;
    if (!parse_ip(arg, &dst)) {
        sout("Usage: udp <ip> <port> <message>\r\n");
        return;
    }
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;

    UW port = 0;
    while (*arg >= '0' && *arg <= '9') { port = port * 10 + (UW)(*arg++ - '0'); }
    while (*arg == ' ') arg++;

    if (!*arg || port == 0 || port > 65535) {
        sout("Usage: udp <ip> <port> <message>\r\n");
        return;
    }

    INT mlen = 0;
    while (arg[mlen]) mlen++;

    sout("UDP -> "); sout(ip_str(dst)); sout(":"); sout_dec(port); sout("\r\n");

    INT r = udp_send(dst, 5301, (UH)port, (const UB *)arg, (UH)mlen);
    if (r < 0) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        sout("ARP not resolved — retry after ARP completes\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        sout("Sent "); sout_dec((UW)mlen); sout(" bytes\r\n");
    }
}

static void execute(const char *cmd)
{
    while (*cmd == ' ') cmd++;      /* strip leading spaces */
    if (*cmd == '\0') return;

    /* Commands that take arguments (prefix match) */
    if (cmd[0]=='p' && cmd[1]=='i' && cmd[2]=='n' && cmd[3]=='g')
        { cmd_ping(cmd + 4); return; }
    if (cmd[0]=='d' && cmd[1]=='n' && cmd[2]=='s')
        { cmd_dns(cmd + 3); return; }
    if (cmd[0]=='u' && cmd[1]=='d' && cmd[2]=='p')
        { cmd_udp(cmd + 3); return; }

    if      (str_eq(cmd, "help"))   cmd_help();
    else if (str_eq(cmd, "ver"))    cmd_ver();
    else if (str_eq(cmd, "mem"))    cmd_mem();
    else if (str_eq(cmd, "ps"))     cmd_ps();
    else if (str_eq(cmd, "net"))    cmd_net();
    else if (str_eq(cmd, "arp"))    cmd_arp();
    else if (str_eq(cmd, "clear"))  vga_clear();
    else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("Unknown command: '"); sout(cmd); sout("'  (try 'help')\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ------------------------------------------------------------------ */
/* Shell task entry point                                              */
/* ------------------------------------------------------------------ */

void shell_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    vga_init();

    /* Banner */
    vga_set_color(VGA_GREEN, VGA_BLACK);
    sout("\r\n");
    sout("  +-----------------------------------------+\r\n");
    sout("  |  p-kernel  /  micro T-Kernel 2.0 x86   |\r\n");
    sout("  |  Interactive Shell                      |\r\n");
    sout("  +-----------------------------------------+\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  Type 'help' for available commands.\r\n\r\n");

    char line[SHELL_LINE_MAX];

    for (;;) {
        /* Prompt */
        vga_set_color(VGA_CYAN, VGA_BLACK);
        sout("p-kernel> ");
        vga_set_color(VGA_WHITE, VGA_BLACK);

        /* Read a line from serial (COM1) — works with -serial stdio */
        INT pos = 0;
        for (;;) {
            UB raw;
            sio_recv_frame(&raw, 1);
            char c = (char)raw;

            if (c == '\n' || c == '\r') {
                soutc('\r'); soutc('\n');
                line[pos] = '\0';
                break;
            } else if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    soutc('\b'); soutc(' '); soutc('\b');
                }
            } else if (pos < SHELL_LINE_MAX - 1) {
                line[pos++] = c;
                soutc(c);
            }
        }

        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        execute(line);
    }
}
