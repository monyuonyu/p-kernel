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
#include "drpc.h"
#include "ai_kernel.h"
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
    sout("  ping <IP>           - send ICMP echo request\r\n");
    sout("  dns <host>          - DNS A-record lookup\r\n");
    sout("  udp <IP> <p> <msg>  - send UDP datagram\r\n");
    sout("  http <host>[/path]  - HTTP GET (port 80)\r\n");
    sout("  clear               - clear screen\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("AI commands:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  sensor <t> <h> <p> <l>      - push sensor frame (°C, %, hPa, lux)\r\n");
    sout("  infer <t> <h> <p> <l>       - local MLP inference (no pipeline)\r\n");
    sout("  aistat                      - AI statistics\r\n");
    sout("  fl train                    - federated learning local train step\r\n");
    sout("  fl status                   - FL round count + last loss\r\n");
    if (drpc_my_node != 0xFF) {
        sout("  infer <n> <t> <h> <p> <l>   - remote inference on node n\r\n");
    }
    if (drpc_my_node != 0xFF) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        sout("Distributed (node "); sout_dec(drpc_my_node); sout("):\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        sout("  nodes                 - list cluster nodes\r\n");
        sout("  dtask <n> <fn>        - create task on node n (fn: hello, counter)\r\n");
        sout("  dsem new              - create distributed semaphore\r\n");
        sout("  dsem wai <0xID>       - wait on distributed semaphore\r\n");
        sout("  dsem sig <0xID>       - signal distributed semaphore\r\n");
    }
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
    sout("  TCP  : "); sout_dec(net_rx_tcp);       sout(" rx / ");
                       sout_dec(net_tx_tcp);       sout(" tx\r\n");
    sout("  My IP: "); sout(ip_str(NET_MY_IP));    sout("\r\n");
    sout("  GW   : "); sout(ip_str(NET_GW_IP));    sout("\r\n");
    sout("  DNS  : "); sout(ip_str(NET_DNS_IP));   sout("\r\n");
}

/* ------------------------------------------------------------------ */
/* Distributed commands                                                */
/* ------------------------------------------------------------------ */

static INT str_starts(const char *s, const char *pfx)
{
    while (*pfx && *s == *pfx) { s++; pfx++; }
    return *pfx == '\0';
}

static UW parse_hex_arg(const char *s)
{
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    UW v = 0;
    while (*s) {
        char c = *s++;
        INT d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (UW)d;
    }
    return v;
}

static void cmd_nodes(void)
{
    if (drpc_my_node == 0xFF) {
        sout("Not in distributed mode (use make run-node0 / run-node1)\r\n");
        return;
    }
    drpc_nodes_list();
}

static void cmd_dtask(const char *arg)
{
    while (*arg == ' ') arg++;
    if (drpc_my_node == 0xFF) {
        sout("Not in distributed mode\r\n"); return;
    }
    if (!*arg) {
        sout("Usage: dtask <node_id> <func>  (func: hello, counter)\r\n"); return;
    }

    UB node_id = 0;
    while (*arg >= '0' && *arg <= '9') node_id = (UB)(node_id * 10 + (*arg++ - '0'));
    while (*arg == ' ') arg++;

    UH func_id = 0;
    if      (str_starts(arg, "hello"))   func_id = 0x0001;
    else if (str_starts(arg, "counter")) func_id = 0x0002;
    else { sout("Unknown func (hello, counter)\r\n"); return; }

    sout("[dtask] -> node "); sout_dec(node_id);
    sout("  func="); sout(arg); sout("\r\n");

    W r = dtk_cre_tsk(node_id, func_id, 4);
    if (r >= 0) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        sout("[dtask] OK  tskid="); sout_dec((UW)r); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[dtask] failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_dsem(const char *arg)
{
    while (*arg == ' ') arg++;
    if (drpc_my_node == 0xFF) {
        sout("Not in distributed mode\r\n"); return;
    }

    if (str_starts(arg, "new")) {
        UW gsemid = dtk_cre_sem(0);
        if (gsemid == (UW)-1) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("[dsem] failed\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[dsem] global semaphore: 0x"); sout_hex(gsemid); sout("\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
    } else if (str_starts(arg, "wai")) {
        arg += 3;
        UW gsemid = parse_hex_arg(arg);
        sout("[dsem] waiting on 0x"); sout_hex(gsemid); sout("...\r\n");
        ER er = dtk_wai_sem(gsemid, 1, TMO_FEVR);
        if (er == E_OK) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[dsem] woke!\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            sout("[dsem] error\r\n");
        }
    } else if (str_starts(arg, "sig")) {
        arg += 3;
        UW gsemid = parse_hex_arg(arg);
        ER er = dtk_sig_sem(gsemid, 1);
        if (er == E_OK) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[dsem] signal OK\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            sout("[dsem] signal failed\r\n");
        }
    } else {
        sout("Usage: dsem new | dsem wai <0xID> | dsem sig <0xID>\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* AI commands                                                         */
/* ------------------------------------------------------------------ */

/* Parse a signed integer from string; advances *pp past digits */
static W parse_sint(const char **pp)
{
    while (**pp == ' ') (*pp)++;
    W sign = 1;
    if (**pp == '-') { sign = -1; (*pp)++; }
    W v = 0;
    while (**pp >= '0' && **pp <= '9') { v = v * 10 + (**pp - '0'); (*pp)++; }
    return sign * v;
}

static void cmd_sensor(const char *arg)
{
    /* sensor <temp_C> <hum_%> <press_hPa> <light_lux> */
    const char *p = arg;
    W t = parse_sint(&p);
    W h = parse_sint(&p);
    W pr = parse_sint(&p);
    W l  = parse_sint(&p);

    SENSOR_FRAME f;
    f.temp     = sensor_norm_temp(t);
    f.humidity = sensor_norm_hum(h);
    f.pressure = sensor_norm_press(pr);
    f.light    = sensor_norm_light(l);
    SYSTIM st; tk_get_tim(&st);
    f.tick     = (UW)st.lo;

    ER er = pipeline_push(&f);
    if (er == E_OK) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        sout("[sensor] pushed (t="); sout_dec((UW)(t < 0 ? (UW)(-t) : (UW)t));
        sout("C h="); sout_dec((UW)h);
        sout("% p="); sout_dec((UW)pr);
        sout("hPa l="); sout_dec((UW)l); sout("lux)\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[sensor] pipeline full — frame dropped\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static const char *cls_name(UB c)
{
    if (c == 0) return "normal  ";
    if (c == 1) return "ALERT   ";
    return "CRITICAL";
}

static void cmd_infer(const char *arg)
{
    /* infer [node] <temp_C> <hum_%> <press_hPa> <light_lux>
     * If first token is 0-7 and NIC is distributed, treat as node_id.
     * Otherwise run local inference. */
    const char *p = arg;
    while (*p == ' ') p++;

    UB node_id = drpc_my_node;
    BOOL remote = FALSE;

    /* Peek: if first token is a single digit 0-7 and next char is space/end */
    if (drpc_my_node != 0xFF && *p >= '0' && *p <= '7') {
        const char *q = p + 1;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == ' ' || *q == '\0') {
            node_id = (UB)(*p - '0');
            p = q;
            remote = (node_id != drpc_my_node);
        }
    }

    W t  = parse_sint(&p);
    W h  = parse_sint(&p);
    W pr = parse_sint(&p);
    W l  = parse_sint(&p);

    if (remote) {
        W packed = SENSOR_PACK(sensor_norm_temp(t),
                               sensor_norm_hum(h),
                               sensor_norm_press(pr),
                               sensor_norm_light(l));
        UB cls = 0;
        ER er = dtk_infer(node_id, packed, &cls, 3000);
        if (er == E_OK) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[infer] node "); sout_dec(node_id);
            sout(" -> "); sout(cls_name(cls)); sout("\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("[infer] remote error\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
    } else {
        B input[MLP_IN];
        input[0] = sensor_norm_temp(t);
        input[1] = sensor_norm_hum(h);
        input[2] = sensor_norm_press(pr);
        input[3] = sensor_norm_light(l);
        UB cls = mlp_forward(input);
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        sout("[infer] local -> "); sout(cls_name(cls)); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_aistat(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    ai_stats_print();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Tiny labelled training dataset — covers all three classes */
static const B fl_samples[6][MLP_IN] = {
    /* normal: t=22C h=50% p=1013hPa l=500lux */
    {  4, 0, 0, 0 },
    {  0, 0, 0, 0 },
    /* alert: t=38C h=80% p=950hPa l=2000lux */
    { 36, 60, -31, 62 },
    { 30, 50, -20, 50 },
    /* critical: t=50C h=95% p=900hPa l=5000lux */
    { 60, 90, -56, 112 },
    { 55, 85, -50, 100 },
};
static const UB fl_labels[6] = { 0, 0, 1, 1, 2, 2 };

static void cmd_fl(const char *arg)
{
    while (*arg == ' ') arg++;

    if (str_starts(arg, "train")) {
        sout("[FL] local train step...\r\n");

        float delta_w1[MLP_IN*MLP_H1], delta_b1[MLP_H1];
        float delta_w2[MLP_H1*MLP_H2], delta_b2[MLP_H2];
        float delta_w3[MLP_H2*MLP_OUT], delta_b3[MLP_OUT];

        ER er = fl_local_train(fl_samples, fl_labels, 6,
                               delta_w1, delta_b1,
                               delta_w2, delta_b2,
                               delta_w3, delta_b3);
        if (er != E_OK) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("[FL] train failed\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }

        /* Pack all deltas into one flat array for dtk_fl_aggregate */
        static float flat_delta[MLP_IN*MLP_H1 + MLP_H1 +
                                 MLP_H1*MLP_H2 + MLP_H2 +
                                 MLP_H2*MLP_OUT + MLP_OUT];
        UW i = 0, j;
        for (j = 0; j < MLP_IN*MLP_H1;  j++) flat_delta[i++] = delta_w1[j];
        for (j = 0; j < MLP_H1;         j++) flat_delta[i++] = delta_b1[j];
        for (j = 0; j < MLP_H1*MLP_H2;  j++) flat_delta[i++] = delta_w2[j];
        for (j = 0; j < MLP_H2;         j++) flat_delta[i++] = delta_b2[j];
        for (j = 0; j < MLP_H2*MLP_OUT; j++) flat_delta[i++] = delta_w3[j];
        for (j = 0; j < MLP_OUT;        j++) flat_delta[i++] = delta_b3[j];

        ER er2 = dtk_fl_aggregate(0, flat_delta, 6, 3000);
        if (er2 == E_OK) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[FL] aggregate OK\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("[FL] aggregate failed\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

    } else if (str_starts(arg, "status")) {
        fl_status();
    } else {
        sout("Usage: fl train | fl status\r\n");
    }
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
IMPORT INT  tcp_connect(UW ip, UH port, TCP_CONN **out);
IMPORT INT  tcp_write(TCP_CONN *c, const UB *data, UH len);
IMPORT INT  tcp_read(TCP_CONN *c, UB *buf, INT maxlen, INT timeout_ms);
IMPORT void tcp_close(TCP_CONN *c);
IMPORT void tcp_free(TCP_CONN *c);

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

static void cmd_http(const char *arg)
{
    while (*arg == ' ') arg++;
    if (!*arg) {
        sout("Usage: http <host>[/path]\r\n");
        sout("  e.g.  http example.com/\r\n");
        return;
    }

    if (!rtl_initialized) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("NIC not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* Split "host/path" → host and path */
    char host[64];
    INT  hi = 0;
    while (*arg && *arg != '/' && hi < 63) host[hi++] = *arg++;
    host[hi] = '\0';
    const char *path = *arg ? arg : "/";

    /* Resolve hostname (skip DNS if already an IP) */
    UW ip;
    if (!parse_ip(host, &ip)) {
        sout("DNS: "); sout(host); sout(" ...\r\n");
        if (dns_query(host, &ip) != 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("DNS failed\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
        sout(host); sout(" -> "); sout(ip_str(ip)); sout("\r\n");
    }

    /* TCP connect to port 80 */
    sout("TCP -> "); sout(ip_str(ip)); sout(":80 ...\r\n");
    TCP_CONN *conn;
    if (tcp_connect(ip, 80, &conn) != 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("TCP connect failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* Build HTTP/1.0 GET request */
    static char req[256];
    INT rlen = 0;
    const char *s;
    s = "GET ";           for (; *s; ) req[rlen++] = *s++;
    s = path;             for (; *s; ) req[rlen++] = *s++;
    s = " HTTP/1.0\r\nHost: ";  for (; *s; ) req[rlen++] = *s++;
    s = host;             for (; *s; ) req[rlen++] = *s++;
    s = "\r\nConnection: close\r\n\r\n"; for (; *s; ) req[rlen++] = *s++;

    tcp_write(conn, (const UB *)req, (UH)rlen);

    /* Read and display response */
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout("--- HTTP Response ---\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    static UB rbuf[512];
    INT total = 0;
    for (;;) {
        INT n = tcp_read(conn, rbuf, (INT)sizeof(rbuf), 4000);
        if (n <= 0) break;
        for (INT i = 0; i < n; i++) {
            char c = (char)rbuf[i];
            if (c == '\r') continue;
            if (c == '\n') soutc('\r');
            if ((unsigned char)c >= 0x20 || c == '\n' || c == '\t') soutc(c);
        }
        total += n;
        if (total > 8192) { sout("\r\n[... truncated]\r\n"); break; }
    }

    sout("\r\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout("--- "); sout_dec((UW)total); sout(" bytes ---\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    tcp_close(conn);
    tcp_free(conn);
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
    if (cmd[0]=='h' && cmd[1]=='t' && cmd[2]=='t' && cmd[3]=='p')
        { cmd_http(cmd + 4); return; }
    if (cmd[0]=='d' && cmd[1]=='t' && cmd[2]=='a' && cmd[3]=='s' && cmd[4]=='k')
        { cmd_dtask(cmd + 5); return; }
    if (cmd[0]=='d' && cmd[1]=='s' && cmd[2]=='e' && cmd[3]=='m')
        { cmd_dsem(cmd + 4); return; }
    if (cmd[0]=='s' && cmd[1]=='e' && cmd[2]=='n' && cmd[3]=='s' &&
        cmd[4]=='o' && cmd[5]=='r')
        { cmd_sensor(cmd + 6); return; }
    if (cmd[0]=='i' && cmd[1]=='n' && cmd[2]=='f' && cmd[3]=='e' && cmd[4]=='r')
        { cmd_infer(cmd + 5); return; }
    if (cmd[0]=='f' && cmd[1]=='l')
        { cmd_fl(cmd + 2); return; }

    if      (str_eq(cmd, "help"))   cmd_help();
    else if (str_eq(cmd, "nodes"))  cmd_nodes();
    else if (str_eq(cmd, "ver"))    cmd_ver();
    else if (str_eq(cmd, "mem"))    cmd_mem();
    else if (str_eq(cmd, "ps"))     cmd_ps();
    else if (str_eq(cmd, "net"))    cmd_net();
    else if (str_eq(cmd, "arp"))    cmd_arp();
    else if (str_eq(cmd, "aistat")) cmd_aistat();
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
