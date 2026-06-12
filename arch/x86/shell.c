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
#include "degrade.h"
#include "dmn.h"
#include "ga.h"
#include "vital.h"
#include "persist.h"
#include "dtr.h"
#include "dproc.h"
#include "sfs.h"
#include "pmesh.h"
#include "raft.h"
#include "spawn.h"
#include "kloader_task.h"
#include "moe.h"
#include "dkva.h"
#include "world.h"
#include "ai_kernel.h"
#include "vfs.h"
#include "mem_store.h"
#include "chat.h"
#include "elf_loader.h"
#include "kernel.h"
#include "paging.h"   /* paging_pool_used — `dproc test` leak gate */

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
IMPORT BOOL sio_data_ready(void);

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
    sout("  status - クラスタ全体のヘルスダッシュボード\r\n");
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
    sout("  exec <file>        - load and run ELF32 binary (blocking)\r\n");
    sout("  spawn <file>       - load and run ELF32 binary (background)\r\n");
    sout("  guard <file>       - spawn + heal watchdog (auto-restart on crash)\r\n");
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
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("プロセス管理 (Phase 9):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  dproc                  - クラスタ全体のプロセス一覧\r\n");
    sout("  kill <name|path>       - プロセスを停止 (全クラスタへ伝播)\r\n");
    sout("  topic del <name>       - K-DDS トピックを全クラスタから削除\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("共有フォルダ同期 (Phase 9.5 SFS):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  sfs list               - /shared/ ファイル一覧\r\n");
    sout("  sfs stat               - 同期統計 + tombstone\r\n");
    sout("  sfs push <path>        - /shared/ ファイルを手動でプッシュ\r\n");
    sout("  sfs sync               - 全ノードへ SYNC_REQ (起動時同期)\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("メッシュルーティング (Phase 10):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  mesh route             - ルーティングテーブル表示\r\n");
    sout("  mesh stat              - メッシュ統計表示\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("縮退モード (Phase 11):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  degrade                - 縮退レベル + 統計表示\r\n");
    sout("    FULL(3+)→REDUCED(2)→SOLO(1) ノード数で自動遷移\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("Phase 11 (記憶 / AI会話):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  chat                   - AI会話モード (記憶付き)\r\n");
    sout("  memstat                - 記憶ストア統計表示\r\n");
    sout("  chatstat               - チャット統計表示\r\n");
    sout("  spawn claude_bridge.elf - Claude API ブリッジ起動\r\n");
    sout("  evolve                 - AI自律進化: 状態収集→Claude分析→改善実行\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("Phase 10 (Raft / MoE / 自己増殖):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  raft                   - Raftコンセンサス状態表示\r\n");
    sout("  moe                    - MoE推論ルーター統計表示\r\n");
    sout("  ring3 test             - ring3生存ゲート (フォールト→reap→再起動)\r\n");
    sout("  ring3 mind             - ring3-mindゲート (推論の数学そのものをring3で)\r\n");
    sout("  dproc test             - killパス teardown リークゲート (page-table pool)\r\n");
    sout("  fpu test               - x87/FPUコンテキスト保存ゲート (並行float)\r\n");
    sout("  world                  - 全網状況マップ表示 (alias: map) — ゴシップ由来、中央なし\r\n");
    sout("  spawn-stat             - 自己増殖統計表示\r\n");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("カーネルローダー (Phase 10 kloader):\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    sout("  kpush <node_id>        - カーネルバイナリをノードへ送信→自動再起動\r\n");
    sout("  write /shared/F text   - 書き込み→自動同期\r\n");
    sout("  rm    /shared/F        - 削除→自動tombstone伝播\r\n");
    sout("  cp src /shared/F       - コピー→自動同期\r\n");
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

    /* topic del <name> — クラスタ全体から削除 (tombstone gossip) */
    if (str_starts(arg, "del ")) {
        arg += 4;
        while (*arg == ' ') arg++;
        char name[32];
        INT ni = 0;
        while (*arg && *arg != ' ' && ni < 31) name[ni++] = *arg++;
        name[ni] = '\0';
        if (!name[0]) { sout("Usage: topic del <name>\r\n"); return; }
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kdds_delete_cluster(name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    sout("Usage: topic list | topic pub <name> <data> | topic del <name>\r\n");
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

IMPORT void lm_test(void);   /* living-mind DMN sleep-consolidation suite */
IMPORT void lm_self_test(void); /* living-mind Self-layer autobiography suite */

static void cmd_self(const char *arg)
{
    /* "self test" -> the Self-layer acceptance suite (living-mind.md III) */
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        lm_self_test();
        return;
    }
    sout("Usage: self test\r\n");
}

IMPORT void sign_self_test(void);  /* signing.md Ed25519 provenance suite */

static void cmd_sign(const char *arg)
{
    /* "sign test" -> the Ed25519 provenance suite (signing.md). [sign-roundtrip]
     * with the RFC 8032 KATs runs here on bare metal too; a signature attests an
     * ARTIFACT came from a KEY, never a human. */
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        sign_self_test();
        return;
    }
    sout("Usage: sign test\r\n");
}

static void cmd_dmn(const char *arg)
{
    /* "dmn set idle <n>" or "dmn set log <n>" or "dmn test" */
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        lm_test();   /* living-mind first slice (living-mind.md II) */
        return;
    }
    if (arg[0]=='s' && arg[1]=='e' && arg[2]=='t' && arg[3]==' ') {
        const char *p = arg + 4;
        while (*p == ' ') p++;
        if (p[0]=='i' && p[1]=='d' && p[2]=='l' && p[3]=='e') {
            p += 4; while (*p == ' ') p++;
            UW v = 0; while (*p >= '0' && *p <= '9') v = v*10 + (*p++ - '0');
            if (v) dmn_set_idle_threshold(v);
            else   sout("Usage: dmn set idle <n>\r\n");
        } else if (p[0]=='l' && p[1]=='o' && p[2]=='g') {
            p += 3; while (*p == ' ') p++;
            UW v = 0; while (*p >= '0' && *p <= '9') v = v*10 + (*p++ - '0');
            if (v) dmn_set_log_interval(v);
            else   sout("Usage: dmn set log <n>\r\n");
        } else {
            sout("Usage: dmn set idle <n> | dmn set log <n>\r\n");
        }
        return;
    }
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    dmn_stat();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_ga(const char *arg)
{
    (void)arg;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    ga_stat();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_degrade(const char *arg)
{
    (void)arg;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    degrade_stat();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
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

static void cmd_kill(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: kill <name|path>  (例: kill hello.elf)\r\n");
        return;
    }
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    W r = dproc_kill_by_name(arg);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (r < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[kill] not found: "); sout(arg); sout("\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void dproc_test(void);   /* kill-path teardown gate — defined after r3_run_elf */
static void dproc_churn(void);  /* KILL-CHURN-CRASH stress gate — defined below */

static void cmd_dproc(const char *arg)
{
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        dproc_test();
        return;
    }
    if (arg[0]=='c' && arg[1]=='h' && arg[2]=='u' && arg[3]=='r' && arg[4]=='n') {
        dproc_churn();
        return;
    }
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    dproc_list();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
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
     * If first token is a node id 0..DNODE_MAX-1 and NIC is distributed,
     * treat as node_id. Otherwise run local inference. */
    const char *p = arg;
    while (*p == ' ') p++;

    UB node_id = drpc_my_node;
    BOOL remote = FALSE;

    /* Peek: if first token is a decimal node id (0..DNODE_MAX-1) followed by
     * a space/end, treat it as a target node. */
    if (drpc_my_node != 0xFF && *p >= '0' && *p <= '9') {
        const char *q = p;
        UW v = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (UW)(*q - '0'); q++; }
        if ((*q == ' ' || *q == '\0') && v < DNODE_MAX) {
            node_id = (UB)v;
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

/* ------------------------------------------------------------------ */
/* Self-Evolution Loop (Phase 12)                                      */
/* Claude API を通じてカーネル自身が状態を分析・改善コマンドを実行   */
/* ------------------------------------------------------------------ */

static void execute(const char *cmd);  /* forward decl for evolve */

static void ev_app(char *buf, INT *pos, INT max, const char *s)
{
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void ev_app_dec(char *buf, INT *pos, INT max, UW v)
{
    char tmp[12]; INT i = 11; tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = (char)('0' + v % 10); v /= 10; } }
    ev_app(buf, pos, max, &tmp[i]);
}

/* [ANALYSIS] セクションを表示 */
static void ev_print_analysis(const char *resp, INT rlen)
{
    const char *tag = "[ANALYSIS]";
    INT tlen = 10;
    for (INT i = 0; i < rlen - tlen; i++) {
        INT match = 1;
        for (INT j = 0; j < tlen; j++) if (resp[i+j] != tag[j]) { match = 0; break; }
        if (!match) continue;
        /* found — print until next [ or end */
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        sout("\r\n[Claude]\r\n");
        i += tlen;
        while (i < rlen && resp[i] != '[') {
            if (resp[i] == '\n') sout("\r\n");
            else { char sc[2] = {resp[i], 0}; sout(sc); }
            i++;
        }
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        sout("\r\n");
        return;
    }
}

/* [CMD] 行を解析して execute() に渡す */
static void ev_exec_cmds(const char *resp, INT rlen)
{
    const char *ctag = "[CMD] ";
    INT ctlen = 6;
    static char cmdline[128];

    /* 許可コマンドプレフィックス (危険操作を弾く) */
    static const char *allowed[] = {
        "exec ", "spawn ", "write ", "mkdir ", "persist", "raft", "ls", "ps", NULL
    };

    for (INT i = 0; i < rlen - ctlen; i++) {
        INT match = 1;
        for (INT j = 0; j < ctlen; j++) if (resp[i+j] != ctag[j]) { match = 0; break; }
        if (!match) continue;
        i += ctlen;

        /* コマンド行を取り出す */
        INT cpos = 0;
        while (i < rlen && resp[i] != '\n' && resp[i] != '\r' && cpos < 127)
            cmdline[cpos++] = resp[i++];
        cmdline[cpos] = '\0';
        if (cpos == 0) continue;

        /* 許可チェック */
        INT ok = 0;
        for (INT k = 0; allowed[k]; k++) {
            INT al = 0; while (allowed[k][al]) al++;
            if (cpos >= al) {
                INT m = 1;
                for (INT x = 0; x < al; x++) if (cmdline[x] != allowed[k][x]) { m = 0; break; }
                if (m) { ok = 1; break; }
            }
        }
        if (!ok) {
            sout("[evolve] blocked cmd: "); sout(cmdline); sout("\r\n");
            continue;
        }

        sout("[evolve] exec cmd: "); sout(cmdline); sout("\r\n");
        execute(cmdline);
    }
}

/* [CODE filename.c]...[/CODE] を /user/code_gen.c に保存 */
static void ev_save_code(const char *resp, INT rlen)
{
    if (!vfs_ready) return;
    const char *otag = "[/CODE]";
    INT otlen = 7;

    /* [CODE で始まるブロックを探す */
    for (INT i = 0; i < rlen - 6; i++) {
        if (resp[i]!='[' || resp[i+1]!='C' || resp[i+2]!='O' ||
            resp[i+3]!='D' || resp[i+4]!='E') continue;
        /* ] まで読み飛ばしてコード開始位置を求める */
        INT start = i + 5;
        while (start < rlen && resp[start] != ']') start++;
        if (start >= rlen) continue;
        start++;  /* ] の次 */
        if (start < rlen && resp[start] == '\n') start++;

        /* [/CODE] を探す */
        INT end = start;
        while (end < rlen - otlen) {
            INT m = 1;
            for (INT j = 0; j < otlen; j++) if (resp[end+j] != otag[j]) { m = 0; break; }
            if (m) break;
            end++;
        }
        if (end >= rlen - otlen) continue;

        /* コードを /user/code_gen.c に保存 */
        vfs_mkdir("/user");
        INT fd = vfs_create("/user/code_gen.c");
        if (fd < 0) { sout("[evolve] failed to save code\r\n"); return; }
        vfs_write(fd, resp + start, (UW)(end - start));
        vfs_close(fd);
        sout("[evolve] [CODE] saved to /user/code_gen.c\r\n");
        sout("[evolve] cat /user/code_gen.c で確認 / claude_proxy.py --compile で自動コンパイル\r\n");
        return;
    }
}

static void cmd_evolve(void)
{
    if (!vfs_ready) { sout("[evolve] VFS not ready\r\n"); return; }
    if (!chat_api_check()) {
        sout("[evolve] Claude bridge が起動していません。\r\n");
        sout("[evolve]   -> spawn claude_bridge.elf\r\n");
        return;
    }

    sout("[evolve] カーネル状態を収集中...\r\n");

    static char ep[1024];
    INT pos = 0, max = (INT)sizeof(ep);

    ev_app(ep, &pos, max,
        "EVOLVE_REQUEST:\n"
        "あなたは p-kernel の自律進化AIです。"
        "以下のカーネル現在状態を分析し、改善提案を行ってください。\n\n");

    /* Raft 状態 */
    ev_app(ep, &pos, max, "RAFT:\n  ");
    if (drpc_my_node != 0xFF) {
        static const char *rnames[] = {"FOLLOWER","CANDIDATE","LEADER"};
        UB r = raft_role();
        ev_app(ep, &pos, max, "node="); ev_app_dec(ep, &pos, max, drpc_my_node);
        ev_app(ep, &pos, max, " role="); ev_app(ep, &pos, max, r < 3 ? rnames[r] : "?");
        ev_app(ep, &pos, max, " term="); ev_app_dec(ep, &pos, max, raft_term());
    } else {
        ev_app(ep, &pos, max, "single-node (distributed mode off)");
    }
    ev_app(ep, &pos, max, "\n");

    /* メモリ */
    IMPORT void *knl_lowmem_top; IMPORT void *knl_lowmem_limit;
    UW avail = (UW)knl_lowmem_limit - (UW)knl_lowmem_top;
    ev_app(ep, &pos, max, "MEMORY:\n  heap_avail=");
    ev_app_dec(ep, &pos, max, avail / 1024); ev_app(ep, &pos, max, "KB\n");

    /* タスク数 */
    ev_app(ep, &pos, max, "TASKS:\n  count=");
    UW tcnt = 0;
    for (ID id = 1; id <= 32; id++) {
        T_RTSK rtsk; if (tk_ref_tsk(id, &rtsk) == E_OK) tcnt++;
    }
    ev_app_dec(ep, &pos, max, tcnt); ev_app(ep, &pos, max, "\n");

    /* AI 推論統計 */
    ev_app(ep, &pos, max, "AI:\n  ");
    ev_app(ep, &pos, max, "mlp+transformer ready\n");

    ev_app(ep, &pos, max,
        "\nINSTRUCTIONS:\n"
        "返答フォーマット (必ずこの形式で):\n"
        "[ANALYSIS]\n(分析 3行以内)\n\n"
        "[CMD] exec /filename.elf    (←実行したいコマンド、省略可)\n"
        "[CMD] write /path content\n\n"
        "[CODE gen.c]\n"
        "#include \"plibc.h\"\nvoid _start(void){...sys_exit(0);}\n"
        "[/CODE]\n\n"
        "許可: exec/spawn/write/mkdir/persist/raft/ls/ps のみ\n"
        "保守的に。今回1つだけ提案してください。\n");

    /* prompt.txt に書き込む */
    vfs_mkdir("/user");
    INT fd = vfs_create("/user/prompt.txt");
    if (fd < 0) { sout("[evolve] prompt write failed\r\n"); return; }
    vfs_write(fd, ep, (UW)pos);
    vfs_close(fd);

    sout("[evolve] Claude に送信中... (最大60秒)\r\n");

    /* response.txt を待つ */
    static char resp[2048];
    INT waited = 0, rlen = 0;
    while (waited < 60000) {
        tk_dly_tsk(500);
        waited += 500;
        fd = vfs_open("/user/response.txt");
        if (fd >= 0) {
            rlen = vfs_read(fd, resp, (INT)sizeof(resp) - 1);
            vfs_close(fd);
            if (rlen > 0) { resp[rlen] = '\0'; vfs_unlink("/user/response.txt"); break; }
        }
    }

    if (rlen <= 0) { sout("[evolve] タイムアウト\r\n"); return; }

    ev_print_analysis(resp, rlen);
    ev_exec_cmds(resp, rlen);
    ev_save_code(resp, rlen);
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
        sout("Usage: exec <file.elf> [args...]\r\n");
        return;
    }

    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* Split first token (path) from the rest (args) */
    char pathbuf[128];
    const char *p = arg;
    while (*p && *p != ' ') p++;
    INT plen = (INT)(p - arg);
    if (plen >= (INT)sizeof(pathbuf)) plen = (INT)sizeof(pathbuf) - 1;
    for (INT i = 0; i < plen; i++) pathbuf[i] = arg[i];
    pathbuf[plen] = '\0';

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[exec] loading: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    stdin_activate();

    /* Pass full command line as cmdline (elf_loader builds argc/argv) */
    ID tid = elf_exec(pathbuf, arg);
    if (tid < E_OK) {
        stdin_deactivate();
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[exec] failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* Phase 9 (dproc): RUNNING として全クラスタに通知 */
    dproc_register(arg, tid);

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
        /* Poll exit status every 50ms — allows ELF to exit without serial input */
        if (tk_wai_sem(exit_sem, 1, 50) == E_OK) break;
        /* Forward any pending serial chars to ELF stdin (non-blocking) */
        if (sio_data_ready()) {
            UB raw; sio_recv_frame(&raw, 1);
            if (skip_lf) {
                skip_lf = FALSE;
                if (raw == (UB)'\n') continue;
            }
            stdin_feed(raw);
        }
    }

    /* ELF 終了 — EXITED として全クラスタへ通知 (フェイルオーバーしない) */
    stdin_deactivate();
    dproc_exit_by_tid(tid);
}

/* ------------------------------------------------------------------ */
/* spawn — 非ブロッキング ELF 起動 (デーモン用)                      */
/* ------------------------------------------------------------------ */

static void cmd_spawn(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: spawn <file.elf>\r\n");
        return;
    }
    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[spawn] loading: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    ID tid = elf_exec(arg, arg);
    if (tid < E_OK) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[spawn] failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    dproc_register(arg, tid);
    sout("[spawn] OK (background)\r\n");
}

/* ------------------------------------------------------------------ */
/* guard — spawn + heal ELF watchdog 登録                            */
/* ------------------------------------------------------------------ */

static void cmd_guard(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
        sout("Usage: guard <file.elf>\r\n");
        return;
    }
    if (!vfs_ready) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[fs] VFS not ready\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[guard] loading: "); sout(arg); sout("\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    ID tid = elf_exec(arg, arg);
    if (tid < E_OK) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[guard] exec failed\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    dproc_register(arg, tid);
    heal_elf_register(arg, 5);
    heal_elf_update_tid(arg, tid);
    sout("[guard] OK (watchdog active)\r\n");
}

/* ------------------------------------------------------------------ */
/* ring3 — ring3-core Wave B acceptance gate                           */
/* (docs/architecture/ring3-core.md II.3 — falsifiable, fake-resistant)*/
/*                                                                     */
/* What this slice proves (honest bound, II.4): the moe class-         */
/* inference computation is exercised FROM a ring-3 task (behind       */
/* SYS_INFER — the math body still runs ring-0; relocating it into     */
/* the user ELF is Wave C), and a deliberate ring-3 fault is SURVIVED  */
/* by the kernel: the task is reaped, the scheduler keeps running,     */
/* and a restart reproduces the live ring-0 oracle.                    */
/* ------------------------------------------------------------------ */

/* Survival counters — boot/x86/idt.c (the survival branch itself). */
IMPORT volatile UW ring3_faults_reaped;
IMPORT volatile UW last_fault_from_ring;
/* gate-1 class channel — arch/x86/syscall.c.  core_moe.elf returns the
 * class it got from SYS_INFER as its SYS_EXIT code; the kernel records
 * it.  A fault-reap records -86 (never a valid class), so a crashed
 * run can never masquerade as a clean inference. */
IMPORT W user_last_exit_code(void);
#define R3_EXIT_FAULT  (-86)   /* must match USER_EXIT_FAULT in syscall.c */

/* V0 — the fixed test vector.  MUST match
 * samples/12_ring3/01_core_moe/core_moe.c and 02_core_crash/core_crash.c.
 * The expected class C0 is NOT hard-coded anywhere: it is whatever the
 * live ring-0 moe_infer(V0) returns at run time (gate 1).             */
#define R3_V0_T  30
#define R3_V0_H  10
#define R3_V0_P  5
#define R3_V0_L  90

/* gate-3 sentinel: a ring-0 task that sleeps across the crash and bumps
 * a counter.  "Scheduler alive" = the counter ADVANCES after the crash
 * (a counter comparison, not a print).                                */
static volatile UW  r3s_ticks;
static volatile INT r3s_stop;
static void r3_sentinel_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    while (!r3s_stop) {
        tk_dly_tsk(20);
        r3s_ticks++;
    }
    tk_ext_tsk();
}

/* Run a user ELF and wait (bounded) until SYS_EXIT or the fault reap
 * signals the exit semaphore.  E_OK = the task terminated.
 * cmdline is tokenised into the user argc/argv frame (elf_loader.c) —
 * `ring3 mind` selects core_mind.elf modes (-poison/-crash) with it.  */
static ER r3_run_elf(const char *path, const char *cmdline)
{
    stdin_activate();
    ID tid = elf_exec(path, cmdline);
    if (tid < E_OK) {
        stdin_deactivate();
        return (ER)tid;
    }
    ER er = tk_wai_sem(stdin_get_exit_sem(), 1, 15000);
    stdin_deactivate();
    return er;
}

static void ring3_mind(void);   /* Wave C gate — defined below */

static void ring3_test(void)
{
    if (!vfs_ready) {
        sout("ring3: FAIL no-vfs (boot with the FAT32 disk: make run-disk)\r\n");
        sout("[ring3-survival] FAIL\r\n");
        return;
    }

    /* ---- quiesce the single user address space (CDN-4a) ----------
     * All user ELFs share USER_CODE_BASE 0x400000; a resident daemon
     * (init.rc: `guard /infer_d.elf`) would be clobbered by our execs
     * and fault at an unpredictable time, poisoning the reaped==1
     * exact check.  Pause the watchdog and kill the daemon; resumed
     * at the end (heal then revives it). */
    heal_elf_pause(TRUE);
    dproc_kill_by_name("infer_d.elf");   /* ignore result if absent */
    tk_dly_tsk(100);

    /* gate-1 oracle: ONE live ring-0 moe_infer(V0) call.  The ring-3
     * result is compared against THIS value, so the gate cannot be
     * greened by returning a constant. */
    UB r0_class = moe_infer(R3_V0_T, R3_V0_H, R3_V0_P, R3_V0_L);

    UW reaped0 = ring3_faults_reaped;

    /* start the gate-3 sentinel */
    r3s_ticks = 0;
    r3s_stop  = 0;
    T_CTSK ct;
    ct.exinf   = NULL;
    ct.tskatr  = TA_HLNG | TA_RNG0;
    ct.task    = r3_sentinel_task;
    ct.itskpri = 10;
    ct.stksz   = 2048;
    ID sent_tid = tk_cre_tsk(&ct);
    const char *fail = NULL;
    if (sent_tid < E_OK || tk_sta_tsk(sent_tid, 0) < E_OK)
        fail = "g3-sentinel-create";

    W  r3_class = -1, restart_class = -1, crash_exit = 0;
    UW reaped_delta = 0, from_ring = 0;
    UW ticks_at_crash = 0, ticks_after = 0;

    /* ---- gate 1 (R3-INFER): ring3 SYS_INFER == ring0 oracle ------- */
    if (!fail) {
        if (r3_run_elf("core_moe.elf", "core_moe.elf") != E_OK) fail = "g1-no-exit";
        else {
            r3_class = user_last_exit_code();
            if (r3_class != (W)r0_class) fail = "g1-class-mismatch";
        }
    }

    /* ---- gate 2 (CRASH-CAUGHT): exactly one reap, from ring 3 ----- */
    if (!fail) {
        /* The exit semaphore being signalled at all == exception_handler
         * returned control to the kernel instead of hlt-ing (the reap
         * path signals it); this very shell task observing it is the
         * "handler returned, no halt" condition. */
        if (r3_run_elf("core_crash.elf", "core_crash.elf") != E_OK) fail = "g2-no-reap";
        ticks_at_crash = r3s_ticks;
        reaped_delta   = ring3_faults_reaped - reaped0;
        from_ring      = last_fault_from_ring;
        crash_exit     = user_last_exit_code();
        if (!fail && reaped_delta != 1) fail = "g2-reaped!=1";
        if (!fail && from_ring != 3)    fail = "g2-from-ring!=3";
        /* a clean SYS_EXIT here would mean the fault never fired */
        if (!fail && crash_exit != R3_EXIT_FAULT) fail = "g2-no-fault-exit";
    }

    /* ---- gate 3 (SCHED-ALIVE): sentinel advances AFTER the crash -- */
    if (!fail) {
        tk_dly_tsk(100);
        ticks_after = r3s_ticks;
        if (ticks_after <= ticks_at_crash) fail = "g3-sched-dead";
    }

    /* ---- gate 4 (RESTART-OK): restart reproduces the oracle ------- */
    if (!fail) {
        if (r3_run_elf("core_moe.elf", "core_moe.elf") != E_OK) fail = "g4-no-exit";
        else {
            restart_class = user_last_exit_code();
            if (restart_class != (W)r0_class) fail = "g4-class-mismatch";
        }
    }

    /* stop + reclaim the sentinel; resume the ELF watchdog */
    r3s_stop = 1;
    if (sent_tid >= E_OK) {
        tk_dly_tsk(60);
        tk_del_tsk(sent_tid);
    }
    heal_elf_pause(FALSE);

    if (!fail) {
        sout("ring3: PASS  infer=");
        sout_dec((UW)r0_class);
        sout(" ring3==ring0:Y  reaped=1 from_ring=3  sched_post=Y  restart=");
        sout_dec((UW)restart_class);
        sout(":Y\r\n");
        sout("[ring3-survival] PASS\r\n");
    } else {
        sout("ring3: FAIL ");
        sout(fail);
        sout("  (r0=");      sout_dec((UW)r0_class);
        sout(" r3=");        sout_dec((UW)r3_class);
        sout(" restart=");   sout_dec((UW)restart_class);
        sout(" reaped=");    sout_dec(reaped_delta);
        sout(" from_ring="); sout_dec(from_ring);
        sout(" ticks=");     sout_dec(ticks_at_crash);
        sout("/");           sout_dec(ticks_after);
        sout(")\r\n");
        sout("[ring3-survival] FAIL\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* ring3 mind — ring3-core Wave C acceptance gate (ring3-core.md III.4)*/
/*                                                                     */
/* THE claim: the class-inference computation (moe_infer's full dtr    */
/* forward) executes inside a ring-3 user task, on a user-space copy   */
/* of the weights, with ZERO kernel-side inference during the run.     */
/* core_mind.elf links the REAL arch/common/moe.c + dtr.c (whole-file  */
/* dual-compile, --gc-sections); the kernel supplies only weights      */
/* (SYS_DTR_WEIGHTS_GET), console, the CDN-7 hooks, and the fault net. */
/*                                                                     */
/* M1-M8, every comparison EXACT (==, never >=):                       */
/*   M1 normal run class == live ring-0 oracle                         */
/*   M2 kernel_infer_count delta == 0 across the run                   */
/*   M3 poison run: c_pre == oracle AND c_post != oracle (the user     */
/*      COPY is what computes — a ring-0 answer cannot feel the poison)*/
/*   M4 kdelta == 0 across the poison run                              */
/*   M5 -crash on the FAT elf: reaped delta == 1, from_ring == 3,      */
/*      exit == -86 (the Wave B crash triple re-proven)                */
/*   M6 sentinel ticks strictly advance post-crash                     */
/*   M7 restart (normal) == oracle (poison did not leak back)          */
/*   M8 final kdelta == 0 overall (excluding the oracle call)          */
/* ------------------------------------------------------------------ */
static void ring3_mind(void)
{
    if (!vfs_ready) {
        sout("ring3-mind: FAIL no-vfs (boot with the FAT32 disk: make run-disk)\r\n");
        sout("[ring3-mind] FAIL\r\n");
        return;
    }

    /* ---- quiesce — Wave B preamble verbatim (CDN-4a single space) -- */
    heal_elf_pause(TRUE);
    dproc_kill_by_name("infer_d.elf");   /* ignore result if absent */
    tk_dly_tsk(100);

    /* ---- ring-0 oracle FIRST (it bumps the counter), THEN snapshot.
     * The expected class is NEVER a constant: it is whatever the live
     * trained/merged weights say right now.                           */
    UB r0_class = moe_infer(R3_V0_T, R3_V0_H, R3_V0_P, R3_V0_L);
    UW k0       = kernel_infer_count;    /* snapshot AFTER the oracle  */
    UW reaped0  = ring3_faults_reaped;

    /* ---- sentinel (gate M6) — same task as Wave B ------------------ */
    r3s_ticks = 0;
    r3s_stop  = 0;
    T_CTSK ct;
    ct.exinf   = NULL;
    ct.tskatr  = TA_HLNG | TA_RNG0;
    ct.task    = r3_sentinel_task;
    ct.itskpri = 10;
    ct.stksz   = 2048;
    ID sent_tid = tk_cre_tsk(&ct);
    const char *fail = NULL;
    if (sent_tid < E_OK || tk_sta_tsk(sent_tid, 0) < E_OK)
        fail = "m6-sentinel-create";

    W  m1 = -1, pexit = -1, crash_exit = 0, m7 = -1;
    UW c_pre = 0xFF, c_post = 0xFF;
    UW k1 = k0, k2 = k0, k3 = k0;
    UW reaped_delta = 0, from_ring = 0;
    UW ticks_at_crash = 0, ticks_after = 0;

    /* FPU note (debt wave closed the III.5 caveat): the dispatcher now
     * does eager per-task FXSAVE/FXRSTOR (arch/x86/fpu.c), so the fat
     * ELF's ring-3 float math survives concurrent FP users.  The
     * dedicated gate is `fpu test` (register-residency ping-pong +
     * a concurrent disturber against this very ELF).                  */

    /* ---- M1 (R3-COMPUTE): ring-3 moe_infer == ring-0 oracle -------- */
    if (!fail) {
        if (r3_run_elf("core_mind.elf", "core_mind.elf") != E_OK)
            fail = "m1-no-exit";
        else {
            m1 = user_last_exit_code();
            if (m1 != (W)r0_class) fail = "m1-class-mismatch";
        }
    }

    /* ---- M2 (KERNEL-FROZEN): zero kernel computes during the run --- */
    if (!fail) {
        k1 = kernel_infer_count;
        if (k1 - k0 != 0) fail = "m2-kdelta!=0";
    }

    /* ---- M3 (POISON): the user-space COPY is what computes --------- */
    if (!fail) {
        if (r3_run_elf("core_mind.elf", "core_mind.elf -poison") != E_OK)
            fail = "m3-no-exit";
        else {
            pexit  = user_last_exit_code();
            c_pre  = ((UW)pexit >> 4) & 0xF;
            c_post = (UW)pexit & 0xF;
            if (pexit < 0)                 fail = "m3-elf-error";
            else if (c_pre != r0_class)    fail = "m3-pre-mismatch";
            else if (c_post == r0_class)   fail = "m3-poison-not-felt";
        }
    }

    /* ---- M4 (KERNEL-FROZEN under poison) ---------------------------- */
    if (!fail) {
        k2 = kernel_infer_count;
        if (k2 - k1 != 0) fail = "m4-kdelta!=0";
    }

    /* ---- M5 (CRASH-CAUGHT): the FAT elf is still reapable ----------- */
    if (!fail) {
        if (r3_run_elf("core_mind.elf", "core_mind.elf -crash") != E_OK)
            fail = "m5-no-reap";
        ticks_at_crash = r3s_ticks;
        reaped_delta   = ring3_faults_reaped - reaped0;
        from_ring      = last_fault_from_ring;
        crash_exit     = user_last_exit_code();
        if (!fail && reaped_delta != 1) fail = "m5-reaped!=1";
        if (!fail && from_ring != 3)    fail = "m5-from-ring!=3";
        if (!fail && crash_exit != R3_EXIT_FAULT) fail = "m5-no-fault-exit";
    }

    /* ---- M6 (SCHED-ALIVE): sentinel advances AFTER the crash -------- */
    if (!fail) {
        tk_dly_tsk(100);
        ticks_after = r3s_ticks;
        if (ticks_after <= ticks_at_crash) fail = "m6-sched-dead";
    }

    /* ---- M7 (RESTART): fresh run == oracle (poison did not leak) ---- */
    if (!fail) {
        if (r3_run_elf("core_mind.elf", "core_mind.elf") != E_OK)
            fail = "m7-no-exit";
        else {
            m7 = user_last_exit_code();
            if (m7 != (W)r0_class) fail = "m7-class-mismatch";
        }
    }

    /* ---- M8 (KERNEL-FROZEN overall): crash run + restart bumped
     *      NOTHING kernel-side ------------------------------------- */
    if (!fail) {
        k3 = kernel_infer_count;
        if (k3 - k2 != 0) fail = "m8-kdelta!=0";
    }

    /* stop + reclaim the sentinel; resume the ELF watchdog */
    r3s_stop = 1;
    if (sent_tid >= E_OK) {
        tk_dly_tsk(60);
        tk_del_tsk(sent_tid);
    }
    heal_elf_pause(FALSE);

    if (!fail) {
        sout("ring3-mind: PASS  infer=");
        sout_dec((UW)r0_class);
        sout(" ring3==ring0:Y  kdelta=");
        sout_dec(k1 - k0); sout("/");
        sout_dec(k2 - k1); sout("/");
        sout_dec(k3 - k2);
        sout("  poison=");
        sout_dec(c_pre);  sout("->");
        sout_dec(c_post); sout(":Y  reaped=1 from_ring=3  sched_post=Y  restart=");
        sout_dec((UW)m7);
        sout(":Y\r\n");
        sout("[ring3-mind] PASS\r\n");
    } else {
        sout("ring3-mind: FAIL ");
        sout(fail);
        sout("  (r0=");      sout_dec((UW)r0_class);
        sout(" m1=");        sout_dec((UW)m1);
        sout(" pexit=");     sout_dec((UW)pexit);
        sout(" pre=");       sout_dec(c_pre);
        sout(" post=");      sout_dec(c_post);
        sout(" kdelta=");    sout_dec(k1 - k0);
        sout("/");           sout_dec(k2 - k1);
        sout("/");           sout_dec(k3 - k2);
        sout(" m7=");        sout_dec((UW)m7);
        sout(" reaped=");    sout_dec(reaped_delta);
        sout(" from_ring="); sout_dec(from_ring);
        sout(" ticks=");     sout_dec(ticks_at_crash);
        sout("/");           sout_dec(ticks_after);
        sout(")\r\n");
        sout("[ring3-mind] FAIL\r\n");
    }
}

static void cmd_ring3(const char *arg)
{
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        ring3_test();   /* Wave B regression — byte-for-byte clauses */
        return;
    }
    if (arg[0]=='m' && arg[1]=='i' && arg[2]=='n' && arg[3]=='d') {
        ring3_mind();   /* Wave C gate — the math itself in ring 3   */
        return;
    }
    sout("Usage: ring3 test|mind\r\n");
}


/* ------------------------------------------------------------------ */
/* dproc test — kill-path teardown leak gate (debt wave, RING3-B debt) */
/*                                                                     */
/* DISEASE (pre-fix): dproc_kill_by_name() terminated the victim with  */
/* tk_ter_tsk+tk_del_tsk but released NONE of its kernel-side          */
/* resources (no ssy/fd cleanup, no exit-sem, no page-table destroy):  */
/* every `kill` leaked the victim's 3 page tables (PML4+PDPT+PD) from  */
/* the 24-slot pool, exhausting it after 8 leaked processes.           */
/*                                                                     */
/* GATE: N exec→kill cycles of infer_d.elf; paging_pool_used() must    */
/* return to the pre-cycle baseline after EVERY cycle.  Falsifiable:   */
/* the unfixed kill path prints pool=base+3,+6,... and FAILs the first */
/* cycle.                                                              */
/* ------------------------------------------------------------------ */
#define DPT_CYCLES 4

static void dproc_test(void)
{
    if (!vfs_ready) {
        sout("dproc-test: FAIL no-vfs (boot with the FAT32 disk: make run-disk)\r\n");
        sout("[dproc-teardown] FAIL\r\n");
        return;
    }

    /* quiesce — same preamble as the ring3 gates (single user space) */
    heal_elf_pause(TRUE);
    dproc_kill_by_name("infer_d.elf");   /* ignore result if absent */
    tk_dly_tsk(100);

    W base = paging_pool_used();
    W after = base;
    const char *fail = NULL;

    sout("dproc-test: baseline pool="); sout_dec((UW)base); sout("\r\n");

    for (INT c = 0; c < DPT_CYCLES && !fail; c++) {
        ID tid = elf_exec("/infer_d.elf", "/infer_d.elf");
        if (tid < E_OK) { fail = "exec-failed"; break; }
        dproc_register("/infer_d.elf", tid);
        tk_dly_tsk(50);
        if (dproc_kill_by_name("infer_d.elf") < 0) { fail = "kill-not-found"; break; }
        tk_dly_tsk(50);
        after = paging_pool_used();
        sout("dproc-test: cycle "); sout_dec((UW)(c + 1));
        sout("  pool="); sout_dec((UW)after);
        sout(" (base="); sout_dec((UW)base); sout(")\r\n");
        if (after != base) fail = "pool-leak";
    }

    heal_elf_pause(FALSE);

    if (!fail) {
        sout("dproc-test: PASS  cycles="); sout_dec((UW)DPT_CYCLES);
        sout("  pool "); sout_dec((UW)base);
        sout(" -> ");    sout_dec((UW)after);
        sout(" (kill teardown leak-free)\r\n");
        sout("[dproc-teardown] PASS\r\n");
    } else {
        sout("dproc-test: FAIL "); sout(fail);
        sout("  pool base="); sout_dec((UW)base);
        sout(" end=");        sout_dec((UW)after);
        sout("\r\n");
        sout("[dproc-teardown] FAIL\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* dproc churn — KILL-CHURN-CRASH stress + closure gate (gap-ledger)   */
/*                                                                     */
/* DISEASE (pre-fix): kill/heal churn of a ring3 daemon (infer_d.elf)  */
/* that blocks in a timed sem wait races the foreign-kill teardown     */
/* (tk_ter_tsk + user_proc_teardown + tk_del_tsk — NOT one critical    */
/* section).  A DORMANT victim that is still/again knl_ctxtsk or       */
/* knl_schedtsk gets its TCB freed by tk_del_tsk; the dispatcher then  */
/* loads its stale ssp and "ret"s into freed+reused memory == the      */
/* historic garbage-PC #PF in knl_make_wait_reltim (CS=0x08).          */
/* Reproduced ~43% (3/7) across churn boots on master.                 */
/*                                                                     */
/* This verb drives that churn HARD and DETERMINISTICALLY in one boot: */
/* CHURN_CYCLES tight exec->(tiny varying settle)->kill cycles with    */
/* the ELF watchdog LIVE (so heal re-execs infer_d concurrently, the   */
/* real churn).  The fix (tk_del_tsk ctxtsk/schedtsk guard) keeps the  */
/* freed TCB off the dispatch pointers; the poison net                 */
/* (knl_dispatch_poison_check) HALTS with [kill-churn] CAUGHT if a      */
/* freed TCB ever reaches the dispatcher.  PASS == survived all cycles */
/* with the scheduler still advancing and no CAUGHT/#PF.               */
/* ------------------------------------------------------------------ */
#ifndef CHURN_CYCLES
#define CHURN_CYCLES 40
#endif

static volatile UW churn_sentinel_ticks;
static volatile W  churn_sentinel_stop;

static void churn_sentinel_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    while (!churn_sentinel_stop) {
        churn_sentinel_ticks++;
        tk_dly_tsk(10);
    }
    tk_ext_tsk();
}

static void dproc_churn(void)
{
    if (!vfs_ready) {
        sout("dproc-churn: FAIL no-vfs (boot with the FAT32 disk: make run-disk)\r\n");
        sout("[kill-churn] FAIL\r\n");
        return;
    }

    /* A live scheduler witness: if a freed TCB ever wedged the
     * dispatcher (the pre-fix failure short of a clean #PF), this
     * counter would stop advancing. */
    churn_sentinel_ticks = 0;
    churn_sentinel_stop  = 0;
    T_CTSK cs = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = churn_sentinel_task, .itskpri = 9, .stksz = 2048 };
    ID sent = tk_cre_tsk(&cs);
    if (sent < E_OK || tk_sta_tsk(sent, 0) < E_OK) {
        sout("dproc-churn: FAIL sentinel-create\r\n");
        sout("[kill-churn] FAIL\r\n");
        return;
    }

    sout("dproc-churn: START cycles="); sout_dec((UW)CHURN_CYCLES);
    sout(" (watchdog LIVE — real heal/kill churn)\r\n");

    /* NOTE: the ELF watchdog stays LIVE on purpose — heal re-execs the
     * killed infer_d concurrently with our kills, which is exactly the
     * churn that produced the UAF.  init.rc `guard /infer_d.elf` arms
     * it; if the daemon isn't guarded we still exec/kill it ourselves. */
    W started = 0, killed = 0;
    UW base_ticks = churn_sentinel_ticks;

    for (INT c = 0; c < CHURN_CYCLES; c++) {
        ID tid = elf_exec("/infer_d.elf", "/infer_d.elf");
        if (tid >= E_OK) { dproc_register("/infer_d.elf", tid); started++; }
        /* Vary the settle window 0..7 ms so the kill lands at every
         * phase of the victim's sem-wait / dispatch cycle. */
        tk_dly_tsk((RELTIM)(c & 7));
        if (dproc_kill_by_name("infer_d.elf") >= 0) killed++;
        /* let heal notice + the dispatcher churn before the next exec */
        tk_dly_tsk((RELTIM)(3 + (c & 3)));
        if ((c & 7) == 7) {
            sout("dproc-churn: cycle "); sout_dec((UW)(c + 1));
            sout("  started="); sout_dec((UW)started);
            sout(" killed=");   sout_dec((UW)killed);
            sout(" ticks=");    sout_dec(churn_sentinel_ticks);
            sout("\r\n");
        }
    }

    /* drain any in-flight heal restart, then quiesce */
    tk_dly_tsk(200);
    dproc_kill_by_name("infer_d.elf");

    UW end_ticks = churn_sentinel_ticks;
    churn_sentinel_stop = 1;
    tk_dly_tsk(40);
    if (sent >= E_OK) tk_del_tsk(sent);

    BOOL sched_alive = (end_ticks > base_ticks);

    sout("dproc-churn: cycles="); sout_dec((UW)CHURN_CYCLES);
    sout("  started="); sout_dec((UW)started);
    sout("  killed=");  sout_dec((UW)killed);
    sout("  sched_ticks "); sout_dec(base_ticks);
    sout("->"); sout_dec(end_ticks); sout("\r\n");

    /* Reaching here at all == no garbage-PC #PF and no poison halt
     * (those never return to the shell).  Plus the scheduler kept
     * advancing through the whole storm. */
    if (sched_alive && started > 0 && killed > 0) {
        sout("dproc-churn: PASS  (no UAF #PF, no poison-catch, sched alive)\r\n");
        sout("[kill-churn] PASS\r\n");
    } else {
        sout("dproc-churn: FAIL  (sched_alive=");
        sout_dec((UW)sched_alive);
        sout(" started="); sout_dec((UW)started);
        sout(" killed=");  sout_dec((UW)killed); sout(")\r\n");
        sout("[kill-churn] FAIL\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* fpu test — x87/SSE context-save gate (debt wave, RING3-C debt)      */
/*                                                                     */
/* DISEASE (pre-fix): the dispatcher (cpu_support.S) switched ONLY the */
/* callee-saved integer registers; the x86 port had ZERO fxsave/fnsave */
/* /CR0.TS handling.  Two tasks computing floats concurrently corrupt  */
/* each other's x87 state silently.                                    */
/*                                                                     */
/* Phase 1 (deterministic, diskless): two ring-0 tasks ping-pong via   */
/* semaphores.  Each leaves a known float REGISTER-RESIDENT on the x87 */
/* stack across the forced context switch, while the peer clobbers the */
/* FPU (fninit + fld of its own value).  Without per-task fxsave the   */
/* read-back is the peer's value (or an empty-stack QNaN) on EVERY     */
/* iteration — bit-exact compare, 100%% deterministic FAIL.  With the  */
/* dispatcher fix: 0 mismatches.                                       */
/*                                                                     */
/* Phase 2 (disk present): a higher-priority FP disturber clobbers the */
/* FPU on a 2ms cadence WHILE core_mind.elf computes the dtr forward   */
/* in ring 3.  Gate: ring-3 class == live ring-0 oracle AND the        */
/* disturber's own register-resident value survives every sleep.  This */
/* is the concurrent-ring3-mind claim the RING3-C epitaph deferred.    */
/* ------------------------------------------------------------------ */
#define FPT_ITERS 16

typedef union { float f; UW u; } FPT_BITS;

static ID fpt_semA, fpt_semB;
static volatile W  fpt_errsA, fpt_errsB;
static volatile W  fpt_doneA, fpt_doneB;
static volatile UW fpt_badA_exp, fpt_badA_got;   /* first A mismatch */

static void fpt_taskA(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT i = 0; i < FPT_ITERS; i++) {
        FPT_BITS in, out; out.u = 0;
        in.f = 1.5f + (float)i;
        /* load the value and leave it ON the x87 register stack ...  */
        asm volatile("fninit; flds %0" :: "m"(in.f));
        /* ... force a switch to task B while it is register-resident */
        tk_sig_sem(fpt_semB, 1);
        tk_wai_sem(fpt_semA, 1, 2000);
        /* ... and read it back (B ran fninit + fld in between)       */
        asm volatile("fstps %0; fninit" : "=m"(out.f));
        if (out.u != in.u) {
            if (fpt_errsA == 0) { fpt_badA_exp = in.u; fpt_badA_got = out.u; }
            fpt_errsA++;
        }
    }
    fpt_doneA = 1;
    tk_ext_tsk();
}

static void fpt_taskB(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    FPT_BITS in, out;
    in.u = 0;
    for (INT i = 0; i < FPT_ITERS; i++) {
        tk_wai_sem(fpt_semB, 1, 2000);
        /* check the value WE left register-resident last iteration
         * (A loaded its own value in between)                       */
        if (i > 0) {
            out.u = 0;
            asm volatile("fstps %0; fninit" : "=m"(out.f));
            if (out.u != in.u) fpt_errsB++;
        }
        in.f = 1000.25f + (float)i * 2.0f;
        asm volatile("fninit; flds %0" :: "m"(in.f));
        tk_sig_sem(fpt_semA, 1);
    }
    asm volatile("fninit");
    fpt_doneB = 1;
    tk_ext_tsk();
}

/* Phase-2 disturber: keeps a value register-resident across tk_dly    */
/* sleeps while the ring-3 mind computes — both must stay exact.       */
static volatile W fpt_dist_stop, fpt_dist_errs, fpt_dist_loops, fpt_dist_done;

static void fpt_disturber(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    while (!fpt_dist_stop) {
        FPT_BITS in, out; out.u = 0;
        in.f = 3.14159f;
        asm volatile("fninit; flds %0" :: "m"(in.f));
        tk_dly_tsk(2);   /* switch away with the value live on the x87 stack */
        asm volatile("fstps %0; fninit" : "=m"(out.f));
        if (out.u != in.u) fpt_dist_errs++;
        fpt_dist_loops++;
    }
    fpt_dist_done = 1;
    tk_ext_tsk();
}

static void fpu_test(void)
{
    const char *fail = NULL;

    /* ---- Phase 1: deterministic register-residency ping-pong ------- */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    fpt_semA = tk_cre_sem(&cs);
    fpt_semB = tk_cre_sem(&cs);
    fpt_errsA = fpt_errsB = 0;
    fpt_doneA = fpt_doneB = 0;
    fpt_badA_exp = fpt_badA_got = 0;

    if (fpt_semA < E_OK || fpt_semB < E_OK) fail = "p1-sem-create";

    if (!fail) {
        T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                      .itskpri = 10, .stksz = 4096 };
        ct.task = fpt_taskA;
        ID ta = tk_cre_tsk(&ct);
        ct.task = fpt_taskB;
        ID tb = tk_cre_tsk(&ct);
        if (ta < E_OK || tb < E_OK ||
            tk_sta_tsk(ta, 0) < E_OK || tk_sta_tsk(tb, 0) < E_OK)
            fail = "p1-task-create";
        else {
            for (INT w = 0; w < 100 && !(fpt_doneA && fpt_doneB); w++)
                tk_dly_tsk(50);
            if (!(fpt_doneA && fpt_doneB)) fail = "p1-timeout";
        }
        if (ta >= E_OK) tk_del_tsk(ta);
        if (tb >= E_OK) tk_del_tsk(tb);
    }
    if (fpt_semA >= E_OK) tk_del_sem(fpt_semA);
    if (fpt_semB >= E_OK) tk_del_sem(fpt_semB);

    sout("fpu-test: phase1 pingpong  itersA="); sout_dec((UW)FPT_ITERS);
    sout(" errsA="); sout_dec((UW)fpt_errsA);
    sout(" errsB="); sout_dec((UW)fpt_errsB);
    if (fpt_errsA > 0) {
        sout("  first A mismatch: expected=0x"); sout_hex(fpt_badA_exp);
        sout(" got=0x"); sout_hex(fpt_badA_got);
    }
    sout("\r\n");
    if (!fail && (fpt_errsA != 0 || fpt_errsB != 0)) fail = "p1-x87-corrupt";

    /* ---- Phase 2: concurrent ring-3 mind + FP disturber ------------ */
    W  mind_class = -1;
    UB oracle = 0;
    if (!fail && vfs_ready) {
        heal_elf_pause(TRUE);
        dproc_kill_by_name("infer_d.elf");   /* quiesce, as ring3 verbs do */
        tk_dly_tsk(100);

        oracle = moe_infer(R3_V0_T, R3_V0_H, R3_V0_P, R3_V0_L);

        fpt_dist_stop = 0; fpt_dist_errs = 0;
        fpt_dist_loops = 0; fpt_dist_done = 0;
        T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                      .task = fpt_disturber, .itskpri = 7, .stksz = 4096 };
        ID td = tk_cre_tsk(&ct);
        if (td < E_OK || tk_sta_tsk(td, 0) < E_OK) fail = "p2-disturber-create";

        if (!fail) {
            if (r3_run_elf("core_mind.elf", "core_mind.elf") != E_OK)
                fail = "p2-no-exit";
            else
                mind_class = user_last_exit_code();
        }

        fpt_dist_stop = 1;
        for (INT w = 0; w < 50 && !fpt_dist_done; w++) tk_dly_tsk(10);
        if (td >= E_OK) tk_del_tsk(td);
        heal_elf_pause(FALSE);

        sout("fpu-test: phase2 concurrent-mind  oracle="); sout_dec((UW)oracle);
        sout(" ring3="); sout_dec((UW)mind_class);
        sout(" disturber loops="); sout_dec((UW)fpt_dist_loops);
        sout(" errs=");            sout_dec((UW)fpt_dist_errs);
        sout("\r\n");
        if (!fail && mind_class != (W)oracle)  fail = "p2-mind-class-mismatch";
        if (!fail && fpt_dist_errs != 0)       fail = "p2-disturber-corrupt";
        if (!fail && fpt_dist_loops == 0)      fail = "p2-disturber-idle";
    } else if (!fail) {
        sout("fpu-test: phase2 SKIPPED (no disk — only phase1 proven this run)\r\n");
    }

    if (!fail) {
        sout("fpu-test: PASS  pingpong=0errs  concurrent-mind=");
        if (vfs_ready) { sout_dec((UW)mind_class); sout("==oracle"); }
        else           sout("skipped");
        sout("\r\n");
        sout("[fpu-ctx] PASS\r\n");
    } else {
        sout("fpu-test: FAIL "); sout(fail); sout("\r\n");
        sout("[fpu-ctx] FAIL\r\n");
    }
}

static void cmd_fpu(const char *arg)
{
    while (*arg == ' ') arg++;
    if (arg[0]=='t' && arg[1]=='e' && arg[2]=='s' && arg[3]=='t') {
        fpu_test();
        return;
    }
    sout("Usage: fpu test\r\n");
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

    /* Phase 9.5 (SFS): /shared/ 以下なら全ノードへ同期 */
    if (sfs_is_shared(path_buf)) sfs_push(path_buf);
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

    /* Phase 9.5 (SFS): /shared/ 以下なら削除を全ノードへ伝播 */
    if (sfs_is_shared(arg)) sfs_delete(arg);
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

    /* Phase 9.5 (SFS): コピー先が /shared/ 以下なら全ノードへ同期 */
    if (sfs_is_shared(dst)) sfs_push(dst);
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

        /* guard <file.elf> — spawn + heal watchdog */
        if (line[0]=='g' && line[1]=='u' && line[2]=='a' &&
            line[3]=='r' && line[4]=='d' && line[5]==' ') {
            const UB *path = line + 6;
            while (*path == ' ') path++;
            sout("[init.rc] guard: ");
            sout((const char *)path);
            sout("\r\n");
            ID tid = elf_exec((const char *)path, (const char *)path);
            if (tid < E_OK) {
                sout("[init.rc] guard: exec failed\r\n");
            } else {
                dproc_register((const char *)path, tid);
                heal_elf_register((const char *)path, 5);
                heal_elf_update_tid((const char *)path, tid);
                sout("[init.rc] guard OK (watchdog active)\r\n");
            }

        /* spawn <file.elf> — non-blocking (daemon) */
        } else if (line[0]=='s' && line[1]=='p' && line[2]=='a' &&
            line[3]=='w' && line[4]=='n' && line[5]==' ') {
            const UB *path = line + 6;
            while (*path == ' ') path++;
            sout("[init.rc] spawn: ");
            sout((const char *)path);
            sout("\r\n");
            ID tid = elf_exec((const char *)path, (const char *)path);
            if (tid < E_OK) {
                sout("[init.rc] spawn failed\r\n");
            } else {
                dproc_register((const char *)path, tid);
                sout("[init.rc] spawn OK\r\n");
            }

        /* exec <file.elf> — blocking (foreground) */
        } else if (line[0]=='e' && line[1]=='x' && line[2]=='e' &&
            line[3]=='c' && line[4]==' ') {
            const UB *path = line + 5;
            while (*path == ' ') path++;
            sout("[init.rc] exec: ");
            sout((const char *)path);
            sout("\r\n");

            stdin_activate();
            ID tid = elf_exec((const char *)path, (const char *)path);
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

/* ------------------------------------------------------------------ */
/* SFS — Shared Folder Sync                                           */
/* ------------------------------------------------------------------ */

static void cmd_sfs(const char *arg)
{
    while (*arg == ' ') arg++;

    if (str_starts(arg, "list") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        sfs_list();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    if (str_starts(arg, "stat")) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        sfs_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* sfs push <path> */
    if (str_starts(arg, "push ")) {
        arg += 5;
        while (*arg == ' ') arg++;
        if (!*arg) { sout("Usage: sfs push <path>\r\n"); return; }
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        sfs_push(arg);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* sfs sync */
    if (str_starts(arg, "sync")) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        sfs_boot_sync();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    sout("Usage: sfs list | sfs stat | sfs push <path> | sfs sync\r\n");
}

/* ------------------------------------------------------------------ */
/* kpush — 実行中カーネルをターゲットノードへ pmesh 経由で送信        */
/* (kloader_task.c の kloader_push() に委譲)                          */
/* ------------------------------------------------------------------ */

static void cmd_kpush(const char *arg)
{
    while (*arg == ' ') arg++;
    if (*arg == '\0') { sout("Usage: kpush <node_id>\r\n"); return; }
    if (drpc_my_node == 0xFF) {
        sout("[kpush] not in distributed mode\r\n"); return;
    }

    UW nid = 0;
    while (*arg >= '0' && *arg <= '9') nid = nid * 10 + (UW)(*arg++ - '0');

    if (nid >= DNODE_MAX || dnode_table[nid].state != DNODE_ALIVE) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        sout("[kpush] node "); sout_dec(nid); sout(" not alive\r\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout("[kpush] pushing to node "); sout_dec(nid); sout(" ...\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    kloader_push((UB)nid);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    sout("[kpush] done\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ------------------------------------------------------------------ */
/* mesh — メッシュルーティング                                        */
/* ------------------------------------------------------------------ */

static void cmd_mesh(const char *arg)
{
    while (*arg == ' ') arg++;

    if (str_starts(arg, "route") || *arg == '\0') {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        pmesh_route_list();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    if (str_starts(arg, "stat")) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        pmesh_stat();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    sout("Usage: mesh route | mesh stat\r\n");
}

/* ------------------------------------------------------------------ */
/* status — クラスタ全体のヘルスダッシュボード                        */
/* ------------------------------------------------------------------ */

static void cmd_status(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    sout("=== p-kernel cluster status ===\r\n");

    /* ---- [Nodes] ---- */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("\r\n[Nodes]\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (drpc_my_node == 0xFF) {
        sout("  (single-node mode)\r\n");
    } else {
        for (UB n = 0; n < DNODE_MAX; n++) {
            UB st = dnode_table[n].state;
            if (st == DNODE_UNKNOWN && n != drpc_my_node) continue;

            if (n == drpc_my_node) {
                vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
                sout("  * node ");
            } else {
                vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
                sout("    node ");
            }
            sout_dec(n);
            sout("  ");
            /* IP */
            if (n == drpc_my_node) {
                /* own IP from drpc */
                UW ip = dnode_table[n].ip;
                for (INT b = 0; b < 4; b++) {
                    if (b) sout(".");
                    sout_dec((ip >> (b * 8)) & 0xFF);
                }
            } else {
                UW ip = dnode_table[n].ip;
                for (INT b = 0; b < 4; b++) {
                    if (b) sout(".");
                    sout_dec((ip >> (b * 8)) & 0xFF);
                }
            }
            sout("  ");
            if (n == drpc_my_node) {
                vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
                sout("ALIVE (me)");
            } else if (st == DNODE_ALIVE) {
                vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
                sout("ALIVE");
            } else if (st == DNODE_SUSPECT) {
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                sout("SUSPECT");
            } else {
                vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
                sout("DEAD");
            }
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            sout("\r\n");
        }
    }

    /* ---- [Mesh] ---- */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("\r\n[Mesh]\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (drpc_my_node == 0xFF) {
        sout("  (not in distributed mode)\r\n");
    } else {
        sout("  beacon tx="); sout_dec(pmesh_stats.beacon_tx);
        sout("  rx=");        sout_dec(pmesh_stats.beacon_rx);
        sout("  data tx=");   sout_dec(pmesh_stats.data_tx);
        sout("  rx=");        sout_dec(pmesh_stats.data_rx);
        sout("  relay=");     sout_dec(pmesh_stats.data_relay);
        sout("\r\n");
        /* relay routes (non-direct) */
        INT nroutes = 0;
        for (INT i = 0; i < DNODE_MAX; i++)
            if (pmesh_routes[i].active) nroutes++;
        sout("  relay routes="); sout_dec(nroutes); sout("\r\n");
    }

    /* ---- [Topics] ---- */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("\r\n[Topics]\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    INT ntopic = 0;
    for (INT i = 0; i < KDDS_TOPIC_MAX; i++)
        if (kdds_topics[i].open) ntopic++;
    sout("  active="); sout_dec(ntopic); sout("\r\n");

    /* ---- [Tasks] ---- */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("\r\n[Tasks]\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    INT nrun = 0, nwait = 0;
    for (INT id = 1; id <= PS_MAX_TSKID; id++) {
        T_RTSK rtsk;
        if (tk_ref_tsk((ID)id, &rtsk) != E_OK) continue;
        UINT s = rtsk.tskstat & 0xFF;
        if (s == 0 || s == TTS_DMT) continue;
        if (s == TTS_RUN || s == TTS_RDY) nrun++;
        else nwait++;
    }
    sout("  running/ready="); sout_dec(nrun);
    sout("  waiting=");       sout_dec(nwait);
    sout("\r\n");

    /* ---- [Memory] ---- */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    sout("\r\n[Memory]\r\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    UW avail = (UW)knl_lowmem_limit - (UW)knl_lowmem_top;
    sout("  heap free: "); sout_dec(avail / 1024); sout(" KB\r\n");

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
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
    if (cmd[0]=='s' && cmd[1]=='p' && cmd[2]=='a' && cmd[3]=='w' && cmd[4]=='n')
        { cmd_spawn(cmd + 5); return; }
    if (cmd[0]=='g' && cmd[1]=='u' && cmd[2]=='a' && cmd[3]=='r' && cmd[4]=='d')
        { cmd_guard(cmd + 5); return; }
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
    if (cmd[0]=='d' && cmd[1]=='m' && cmd[2]=='n')
        { cmd_dmn(cmd + 3); return; }
    if (cmd[0]=='r' && cmd[1]=='i' && cmd[2]=='n' && cmd[3]=='g' && cmd[4]=='3')
        { cmd_ring3(cmd + 5); return; }
    if (cmd[0]=='f' && cmd[1]=='p' && cmd[2]=='u')
        { cmd_fpu(cmd + 3); return; }
    if (cmd[0]=='s' && cmd[1]=='e' && cmd[2]=='l' && cmd[3]=='f' &&
        (cmd[4]==' ' || cmd[4]=='\0'))
        { cmd_self(cmd + 4); return; }
    if (cmd[0]=='s' && cmd[1]=='i' && cmd[2]=='g' && cmd[3]=='n' &&
        (cmd[4]==' ' || cmd[4]=='\0'))
        { cmd_sign(cmd + 4); return; }
    if (cmd[0]=='g' && cmd[1]=='a')
        { cmd_ga(cmd + 2); return; }
    if (cmd[0]=='d' && cmd[1]=='e' && cmd[2]=='g' && cmd[3]=='r' &&
        cmd[4]=='a' && cmd[5]=='d' && cmd[6]=='e')
        { cmd_degrade(cmd + 7); return; }
    if (cmd[0]=='k' && cmd[1]=='i' && cmd[2]=='l' && cmd[3]=='l')
        { cmd_kill(cmd + 4); return; }
    if (cmd[0]=='d' && cmd[1]=='p' && cmd[2]=='r' && cmd[3]=='o' && cmd[4]=='c')
        { cmd_dproc(cmd + 5); return; }
    if (cmd[0]=='s' && cmd[1]=='f' && cmd[2]=='s')
        { cmd_sfs(cmd + 3); return; }
    if (cmd[0]=='m' && cmd[1]=='e' && cmd[2]=='s' && cmd[3]=='h')
        { cmd_mesh(cmd + 4); return; }
    if (cmd[0]=='k' && cmd[1]=='p' && cmd[2]=='u' && cmd[3]=='s' && cmd[4]=='h')
        { cmd_kpush(cmd + 5); return; }
    /* Phase 11: 記憶 / AI会話 */
    if (cmd[0]=='c' && cmd[1]=='h' && cmd[2]=='a' && cmd[3]=='t' && cmd[4]=='\0')
        { chat_run(0); return; }
    if (str_eq(cmd, "memstat"))    { mem_stat();  return; }
    if (str_eq(cmd, "chatstat"))   { chat_stat(); return; }
    if (str_eq(cmd, "evolve"))     { cmd_evolve(); return; }

    if (cmd[0]=='r' && cmd[1]=='a' && cmd[2]=='f' && cmd[3]=='t')
        { raft_stat(); return; }
    if (cmd[0]=='m' && cmd[1]=='o' && cmd[2]=='e')
        { moe_stat(); return; }
    if (cmd[0]=='d' && cmd[1]=='k' && cmd[2]=='v' && cmd[3]=='a')
        { dkva_stat(); return; }
    if (str_eq(cmd, "world") || str_eq(cmd, "map"))
        { world_print(); return; }
    if (cmd[0]=='s' && cmd[1]=='p' && cmd[2]=='a' && cmd[3]=='w' && cmd[4]=='n'
        && cmd[5]==' ')
        { /* spawn <file> handled below */ }

    if      (str_eq(cmd, "status")) cmd_status();
    else if (str_eq(cmd, "spawn-stat")) { spawn_stat(); return; }
    else if (str_eq(cmd, "help"))   cmd_help();
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
