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
#include "swim.h"
#include "kdds.h"
#include "heal.h"
#include "edf.h"
#include "replica.h"
#include "vital.h"
#include "persist.h"
#include "dtr.h"
#include "ai_kernel.h"
#include "vfs.h"
#include "elf_loader.h"
#include "kernel.h"

#define SHELL_LINE_MAX  128
#define PS_MAX_TSKID    CFN_MAX_TSKID

/* ------------------------------------------------------------------ */
/* Output helpers (VGA + serial mirror)                                */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

/* stdin relay API (syscall.c) — forwards serial chars to user ELF stdin */
IMPORT void stdin_activate(void);
IMPORT void stdin_deactivate(void);
IMPORT void stdin_feed(UB c);
IMPORT ID   stdin_get_exit_sem(void);
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
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("Filesystem commands:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  ls [path]          - list directory (default: /)\r\n");
    sout("  cat <file>         - print file contents\r\n");
    sout("  write <file> <txt> - create/overwrite file with text\r\n");
    sout("  rm <file>          - delete file\r\n");
    sout("  mkdir <dir>        - create directory\r\n");
    sout("  cp <src> <dst>     - copy file\r\n");
    sout("  mv <src> <dst>     - rename/move file\r\n");
    sout("  exec <file>        - load and run ELF32 binary\r\n");
    sout("  mount              - show mount table\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("K-DDS commands:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  topic list               - トピック一覧表示\r\n");
    sout("  topic pub <name> <data>  - トピックへ発行\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("Self-Healing commands:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  heal list                - ガードタスク一覧表示\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("EDF スケジューリング:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  edf stat                 - SLA 統計 + ノード負荷表示\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("生存本能 (Phase 6):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  replica stat             - 複製統計表示\r\n");
    sout("  vital stat               - クラスタ生命兆候一覧\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("永続化 (Phase 7):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  persist list             - ディスク上の保存トピック一覧\r\n");
    sout("  persist save             - 全トピックを今すぐ保存\r\n");
    sout("  persist clear            - 保存済みトピックを全削除\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("分散 AI 推論 (Phase 8):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  dtr stat                         - パイプライン統計\r\n");
    sout("  dtr infer <t> <h> <p> <l>        - 分散推論実行\r\n");
    sout("    Node 0: Embed+Layer0 -> dtr/l0 -> Node 1: FFN+Output\r\n");
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
    swim_nodes_print();
}

static void cmd_topic(const char *arg)
{
    while (*arg == ' ') arg++;

    if (str_starts(arg, "list") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kdds_list();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* topic pub <name> <data> */
    if (str_starts(arg, "pub ")) {
        arg += 4;
        while (*arg == ' ') arg++;
        /* トピック名を切り出す */
        char name[32];
        INT ni = 0;
        while (*arg && *arg != ' ' && ni < 31) name[ni++] = *arg++;
        name[ni] = '\0';
        while (*arg == ' ') arg++;
        if (!*arg) { sout("Usage: topic pub <name> <data>\r\n"); return; }

        W h = kdds_open(name, KDDS_QOS_LATEST_ONLY);
        if (h < 0) { sout("[topic] open failed\r\n"); return; }
        INT len = 0; while (arg[len]) len++;
        W r = kdds_pub(h, arg, len + 1);   /* null 含む */
        if (r < 0) sout("[topic] pub failed\r\n");
        else {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            sout("[topic] published to \""); sout(name); sout("\"\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        kdds_close(h);
        return;
    }

    sout("Usage: topic list | topic pub <name> <data>\r\n");
}

static void cmd_edf(const char *arg)
{
    while (*arg == ' ') arg++;
    if (str_starts(arg, "stat") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        edf_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    sout("Usage: edf stat\r\n");
}

static void cmd_heal(const char *arg)
{
    while (*arg == ' ') arg++;
    if (str_starts(arg, "list") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        heal_list();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    sout("Usage: heal list\r\n");
}

static void cmd_replica(const char *arg)
{
    while (*arg == ' ') arg++;
    if (str_starts(arg, "stat") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        replica_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    sout("Usage: replica stat\r\n");
}

static void cmd_vital(const char *arg)
{
    while (*arg == ' ') arg++;
    if (str_starts(arg, "stat") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vital_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    sout("Usage: vital stat\r\n");
}

static void cmd_persist(const char *arg)
{
    while (*arg == ' ') arg++;
    if (str_starts(arg, "list") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        persist_list();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    if (str_starts(arg, "save")) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        persist_checkpoint();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    if (str_starts(arg, "clear")) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        persist_clear();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    sout("Usage: persist list | save | clear\r\n");
}

static void cmd_dtr(const char *arg)
{
    while (*arg == ' ') arg++;

    if (str_starts(arg, "stat") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        dtr_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    if (str_starts(arg, "infer")) {
        arg += 5;
        while (*arg == ' ') arg++;

        /* <t> <h> <p> <l> を読む */
        W vals[4] = {25, 50, 1013, 500};   /* デフォルト値 */
        for (INT vi = 0; vi < 4 && *arg; vi++) {
            while (*arg == ' ') arg++;
            W neg = 0, v = 0;
            if (*arg == '-') { neg = 1; arg++; }
            while (*arg >= '0' && *arg <= '9') v = v * 10 + (*arg++ - '0');
            vals[vi] = neg ? -v : v;
        }

        B inp[4];
        inp[0] = sensor_norm_temp  ((W)vals[0]);
        inp[1] = sensor_norm_hum   ((W)vals[1]);
        inp[2] = sensor_norm_press ((W)vals[2]);
        inp[3] = sensor_norm_light ((W)vals[3]);

        sout("DTR infer: temp="); sout_dec((UW)vals[0]);
        sout(" hum=");  sout_dec((UW)vals[1]);
        sout(" press="); sout_dec((UW)vals[2]);
        sout(" light="); sout_dec((UW)vals[3]); sout("\r\n");

        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        W cls = dtr_infer(inp);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

        if (cls < 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            sout("[dtr] inference failed\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        return;
    }

    sout("Usage: dtr stat | dtr infer <temp> <hum> <press> <light>\r\n");
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

/* ------------------------------------------------------------------ */
/* Filesystem commands (ls / cat / exec)                               */
/* ------------------------------------------------------------------ */

IMPORT BOOL vfs_ready;

static void cmd_ls(const char *arg)
{
    while (*arg == ' ') arg++;
    const char *path = (*arg == '\0') ? "/" : arg;

    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready (no disk?)\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    VFS_DIRENT entries[32];
    INT n = vfs_readdir(path, entries, 32);
    if (n < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[ls] directory not found: "); sout(path); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout(path); sout(":\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (INT i = 0; i < n; i++) {
        if (entries[i].is_dir) {
            vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            sout("  ["); sout(entries[i].name); sout("]/\r\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            sout("  "); sout(entries[i].name);
            sout("  ("); sout_dec(entries[i].size); sout(" B)\r\n");
        }
    }
    if (n == 0) sout("  (empty)\r\n");
}

static void cmd_cat(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: cat <file>\r\n");
        return;
    }

    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    INT fd = vfs_open(arg);
    if (fd < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[cat] not found: "); sout(arg); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    static UB catbuf[512];
    INT total = 0;
    for (;;) {
        INT n = vfs_read(fd, catbuf, sizeof(catbuf));
        if (n <= 0) break;
        for (INT i = 0; i < n; i++) {
            char c = (char)catbuf[i];
            if (c == '\n') soutc('\r');
            if ((unsigned char)c >= 0x20 || c == '\n' || c == '\t') soutc(c);
        }
        total += n;
        if (total > 65536) { sout("\r\n[... truncated]\r\n"); break; }
    }
    sout("\r\n");
    vfs_close(fd);
}

static void cmd_exec(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: exec <file.elf>\r\n");
        return;
    }

    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[exec] loading: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    stdin_activate();

    ID tid = elf_exec(arg);
    if (tid < E_OK) {
        stdin_deactivate();
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[exec] failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* stdin relay loop: forward serial chars to user ELF stdin.
     * Exits when the ELF calls SYS_EXIT (which signals stdin_exit_sem).
     *
     * The command line ends with '\r'; the terminal typically sends "\r\n".
     * The '\n' that immediately follows '\r' stays in the serial buffer and
     * arrives as the very first character in the relay loop.  We discard it
     * so that the ELF's first readline() call is not terminated by a stray
     * newline before the user has typed anything. */
    ID exit_sem = stdin_get_exit_sem();
    BOOL skip_lf = TRUE;   /* discard the first '\n' only */
    for (;;) {
        UB raw;
        sio_recv_frame(&raw, 1);          /* block until a char arrives     */
        /* Check (non-blocking) if the ELF has exited */
        if (tk_wai_sem(exit_sem, 1, TMO_POL) == E_OK) break;
        /* Skip the one '\n' left over from the "\r\n" command terminator */
        if (skip_lf) {
            skip_lf = FALSE;
            if (raw == (UB)'\n') continue;
        }
        stdin_feed(raw);
    }
}

/* ------------------------------------------------------------------ */
/* New filesystem write commands                                       */
/* ------------------------------------------------------------------ */

IMPORT INT  vfs_create(const char *path);
IMPORT INT  vfs_write(INT fd, const void *buf, UW len);
IMPORT INT  vfs_unlink(const char *path);
IMPORT INT  vfs_mkdir(const char *path);
IMPORT INT  vfs_rename(const char *oldpath, const char *newpath);

static void fs_err(const char *cmd, const char *detail)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    sout("["); sout(cmd); sout("] "); sout(detail); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_write(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: write <file> <text>\r\n"); return; }
    if (!vfs_ready) { fs_err("write", "VFS not ready"); return; }

    /* Split into path and content */
    const char *path = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg == '\0') { fs_err("write", "missing text argument"); return; }
    /* NUL-terminate path in a local buffer */
    char path_buf[128];
    INT plen = (INT)(arg - path);
    if (plen >= 128) plen = 127;
    for (INT i = 0; i < plen; i++) path_buf[i] = path[i];
    path_buf[plen] = '\0';
    while (*arg == ' ') arg++;  /* skip spaces before content */

    INT fd = vfs_create(path_buf);
    if (fd < 0) { fs_err("write", "cannot create file"); return; }

    /* Write text + newline */
    UW len = 0;
    while (arg[len]) len++;
    vfs_write(fd, arg, len);
    vfs_write(fd, "\r\n", 2);
    vfs_close(fd);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[write] ok: "); sout(path_buf); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_rm(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: rm <file>\r\n"); return; }
    if (!vfs_ready) { fs_err("rm", "VFS not ready"); return; }

    if (vfs_unlink(arg) < 0) { fs_err("rm", "failed (not found or is a dir?)"); return; }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[rm] deleted: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_mkdir(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: mkdir <dir>\r\n"); return; }
    if (!vfs_ready) { fs_err("mkdir", "VFS not ready"); return; }

    if (vfs_mkdir(arg) < 0) { fs_err("mkdir", "failed (exists or no space?)"); return; }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[mkdir] created: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_cp(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: cp <src> <dst>\r\n"); return; }
    if (!vfs_ready) { fs_err("cp", "VFS not ready"); return; }

    /* Parse src and dst */
    const char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg == '\0') { fs_err("cp", "missing dst"); return; }
    char src_buf[128];
    INT slen = (INT)(arg - src);
    if (slen >= 128) slen = 127;
    for (INT i = 0; i < slen; i++) src_buf[i] = src[i];
    src_buf[slen] = '\0';
    while (*arg == ' ') arg++;
    const char *dst = arg;

    INT sfd = vfs_open(src_buf);
    if (sfd < 0) { fs_err("cp", "src not found"); return; }
    INT dfd = vfs_create(dst);
    if (dfd < 0) { vfs_close(sfd); fs_err("cp", "cannot create dst"); return; }

    static UB cp_buf[512];
    INT total = 0;
    for (;;) {
        INT n = vfs_read(sfd, cp_buf, sizeof(cp_buf));
        if (n <= 0) break;
        vfs_write(dfd, cp_buf, (UW)n);
        total += n;
    }
    vfs_close(sfd);
    vfs_close(dfd);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[cp] "); sout(src_buf); sout(" -> "); sout(dst);
    sout("  ("); sout_dec((UW)total); sout(" B)\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ------------------------------------------------------------------ */
/* /etc/init.rc — boot-time script (called from shell_task at startup) */
/* ------------------------------------------------------------------ */
static void run_initrc(void)
{
    INT fd = vfs_open("/etc/init.rc");
    if (fd < 0) return;

    sout("[init.rc] /etc/init.rc found\r\n");

    static UB rc_buf[2048];
    INT n = vfs_read(fd, rc_buf, sizeof(rc_buf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    rc_buf[n] = '\0';

    INT pos = 0;
    while (pos < n) {
        INT start = pos;
        while (pos < n && rc_buf[pos] != '\n' && rc_buf[pos] != '\r') pos++;
        INT end = pos;
        while (pos < n && (rc_buf[pos] == '\n' || rc_buf[pos] == '\r')) pos++;
        rc_buf[end] = '\0';

        const UB *line = &rc_buf[start];
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') continue;

        /* exec <file.elf> */
        if (line[0]=='e' && line[1]=='x' && line[2]=='e' &&
            line[3]=='c' && line[4]==' ') {
            const UB *path = line + 5;
            while (*path == ' ') path++;
            sout("[init.rc] exec: ");
            sout((const char *)path);
            sout("\r\n");

            stdin_activate();
            ID tid = elf_exec((const char *)path);
            if (tid < E_OK) {
                stdin_deactivate();
                sout("[init.rc] exec failed\r\n");
                continue;
            }
            ID esem = stdin_get_exit_sem();
            tk_wai_sem(esem, 1, 30000);
            stdin_deactivate();

        /* mkdir <path> */
        } else if (line[0]=='m' && line[1]=='k' && line[2]=='d' &&
                   line[3]=='i' && line[4]=='r' && line[5]==' ') {
            const UB *path = line + 6;
            while (*path == ' ') path++;
            vfs_mkdir((const char *)path);
        }
    }

    sout("[init.rc] done\r\n");
}

static void cmd_mount(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("Mount table:\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  /  ->  FAT32 / IDE  [");
    sout(vfs_ready ? "ready" : "not ready");
    sout("]\r\n");
    if (vfs_ready) {
        sout("  init.rc: /etc/init.rc  (exec at boot)\r\n");
    }
}

static void cmd_mv(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: mv <src> <dst>\r\n"); return; }
    if (!vfs_ready) { fs_err("mv", "VFS not ready"); return; }

    const char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg == '\0') { fs_err("mv", "missing dst"); return; }
    char src_buf[128];
    INT slen = (INT)(arg - src);
    if (slen >= 128) slen = 127;
    for (INT i = 0; i < slen; i++) src_buf[i] = src[i];
    src_buf[slen] = '\0';
    while (*arg == ' ') arg++;
    const char *dst = arg;

    if (vfs_rename(src_buf, dst) < 0) {
        fs_err("mv", "failed"); return;
    }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[mv] "); sout(src_buf); sout(" -> "); sout(dst); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
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
    if (cmd[0]=='l' && cmd[1]=='s')
        { cmd_ls(cmd + 2); return; }
    if (cmd[0]=='c' && cmd[1]=='a' && cmd[2]=='t')
        { cmd_cat(cmd + 3); return; }
    if (cmd[0]=='e' && cmd[1]=='x' && cmd[2]=='e' && cmd[3]=='c')
        { cmd_exec(cmd + 4); return; }
    if (cmd[0]=='w' && cmd[1]=='r' && cmd[2]=='i' && cmd[3]=='t' && cmd[4]=='e')
        { cmd_write(cmd + 5); return; }
    if (cmd[0]=='r' && cmd[1]=='m')
        { cmd_rm(cmd + 2); return; }
    if (cmd[0]=='m' && cmd[1]=='k' && cmd[2]=='d' && cmd[3]=='i' && cmd[4]=='r')
        { cmd_mkdir(cmd + 5); return; }
    if (cmd[0]=='c' && cmd[1]=='p')
        { cmd_cp(cmd + 2); return; }
    if (cmd[0]=='m' && cmd[1]=='v')
        { cmd_mv(cmd + 2); return; }
    if (cmd[0]=='t' && cmd[1]=='o' && cmd[2]=='p' && cmd[3]=='i' && cmd[4]=='c')
        { cmd_topic(cmd + 5); return; }
    if (cmd[0]=='h' && cmd[1]=='e' && cmd[2]=='a' && cmd[3]=='l')
        { cmd_heal(cmd + 4); return; }
    if (cmd[0]=='e' && cmd[1]=='d' && cmd[2]=='f')
        { cmd_edf(cmd + 3); return; }
    if (cmd[0]=='r' && cmd[1]=='e' && cmd[2]=='p' && cmd[3]=='l' &&
        cmd[4]=='i' && cmd[5]=='c' && cmd[6]=='a')
        { cmd_replica(cmd + 7); return; }
    if (cmd[0]=='v' && cmd[1]=='i' && cmd[2]=='t' && cmd[3]=='a' && cmd[4]=='l')
        { cmd_vital(cmd + 5); return; }
    if (cmd[0]=='p' && cmd[1]=='e' && cmd[2]=='r' && cmd[3]=='s' &&
        cmd[4]=='i' && cmd[5]=='s' && cmd[6]=='t')
        { cmd_persist(cmd + 7); return; }
    if (cmd[0]=='d' && cmd[1]=='t' && cmd[2]=='r')
        { cmd_dtr(cmd + 3); return; }

    if      (str_eq(cmd, "help"))   cmd_help();
    else if (str_eq(cmd, "mount"))  cmd_mount();
    else if (str_eq(cmd, "nodes"))  cmd_nodes();
    else if (str_eq(cmd, "umount")) sout("umount: single root mount — nothing to unmount\r\n");
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

    /* Run /etc/init.rc now that all tasks and VFS are ready */
    run_initrc();

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
