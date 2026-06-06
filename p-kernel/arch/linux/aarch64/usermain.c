/*
 *  arch/linux/aarch64/usermain.c
 *
 *  Initial task for the UMP build. Brings up the AI primitives, the
 *  pub/sub layer, and the Distributed Transformer in single-node
 *  mode, then drops into an interactive shell that the user drives
 *  over the host terminal (termios stdin/stdout).
 *
 *  Distributed-layer task creation (SWIM / Raft / DRPC echo etc.) is
 *  intentionally absent — those need a real NIC, and arch/linux only
 *  stubs that out. Phase B (TUN/TAP or Android NDK with a relay)
 *  re-enables them.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "arch_reboot.h"
#include "rtl8139.h"
#include "netstack.h"
#include "drpc.h"
#include "swim.h"
#include "region.h"
#include "lookup.h"
#include "kdds.h"
#include "moe.h"
#include "world.h"
#include "pmesh.h"
#include "demo_kdds.h"
#include "pfs_block.h"
#include "pfs_repl.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT INT  sio_read_line(UB *buf, INT maxlen);

IMPORT void ai_kernel_init(void);
IMPORT void ai_stats_print(void);
IMPORT void kdds_init(void);
IMPORT void kdds_list(void);
IMPORT void dtr_init(void);
IMPORT void dtr_stat(void);
IMPORT void dtr_task(INT stacd, void *exinf);
IMPORT W    dtr_infer(const B input[4]);
static void print_dec_s(W v);   /* fwd: used by cmd_net for multi-digit node id */
IMPORT void degrade_init(void);
IMPORT void degrade_stat(void);
IMPORT void dkva_init(void);
IMPORT void dkva_task(INT stacd, void *exinf);

extern char *getenv(const char *);

static void print(const char *s)
{
    sio_send_frame((const UB *)s, (INT)__builtin_strlen(s));
}

/* Case-sensitive prefix match: true if `a` starts with `b` and `b`
 * is a complete token (followed by '\0' / space / end-of-line). */
static int starts_with(const UB *a, INT alen, const char *b)
{
    INT i = 0;
    for (; b[i] != '\0'; i++) {
        if (i >= alen) return 0;
        if (a[i] != (UB)b[i]) return 0;
    }
    return 1;
}

static void cmd_help(void)
{
    print("commands:\r\n");
    print("  help   - this list\r\n");
    print("  ai     - AI primitive statistics (inferences, jobs, FL rounds)\r\n");
    print("  dtr    - Distributed Transformer status\r\n");
    print("  infer [a b c d] - run a Transformer inference on 4 int8 sensors\r\n");
    print("  dist   - distributed degrade level (SOLO/REDUCED/FULL)\r\n");
    print("  kdds   - K-DDS topic table\r\n");
    print("  kdemo  - cross-arch K-DDS heartbeat demo (pub+sub on demo/heartbeat)\r\n");
    print("  pfs    - p-fs block store self-test (dedup)\r\n");
    print("  pfs put <text> - store <text> as a block; replicates to region peers\r\n");
    print("  pfs ls - list stored blocks (id prefix / len / origin)\r\n");
    print("  net    - bring up the AF_UNIX virtual NIC and DRPC stack\r\n");
    print("  world  - whole-network situational map (alias: map), from gossip, no central\r\n");
    print("  hrw    - lookup L0 HRW responsible(k,r) self-test (deterministic, cross-ABI)\r\n");
    print("  rx     - RX/TX frame counters\r\n");
    print("  ver    - build identity\r\n");
    print("  exit   - terminate the UMP process\r\n");
    print("  (any other text is echoed back)\r\n");
}

static void cmd_rx(void)
{
    char buf[80]; INT i = 0;
    #define APPEND(s) do { const char *p = s; while (*p) buf[i++] = *p++; } while (0)
    #define APPEND_DEC(v) do { UW vv = (v); if (vv == 0) buf[i++] = '0'; \
        else { char tmp[12]; INT t = 0; while (vv > 0) { tmp[t++] = '0' + (vv % 10); vv /= 10; } \
        while (t > 0) buf[i++] = tmp[--t]; } } while (0)
    APPEND("[rx] frames="); APPEND_DEC(rtl_rx_count);
    APPEND("  tx="); APPEND_DEC(rtl_tx_count);
    APPEND("  initialized="); APPEND_DEC((UW)rtl_initialized);
    buf[i++] = '\r'; buf[i++] = '\n';
    sio_send_frame((const UB *)buf, i);
    #undef APPEND
    #undef APPEND_DEC
}

static void cmd_ver(void)
{
    print("p-kernel UMP — User-Mode p-kernel\r\n");
    print("  host arch    : aarch64-linux\r\n");
    print("  T-Kernel core: micro T-Kernel 2.0 + p-kernel layers\r\n");
    print("  context-switch: raw asm (cooperative)\r\n");
    print("  IRQ source   : SIGALRM @ 100 Hz\r\n");
}

static ID create_sem(INT isemcnt, INT maxsem)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isemcnt, .maxsem = maxsem };
    return tk_cre_sem(&cs);
}

static ID create_task(FP fn, INT pri, INT stksz)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = fn, .itskpri = pri, .stksz = stksz };
    ID id = tk_cre_tsk(&ct);
    if (id >= E_OK) tk_sta_tsk(id, 0);
    return id;
}

static INT net_up = 0;

/* env を符号なし10進としてパース。未設定/空なら dflt。 */
static UW env_uint(const char *name, UW dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    UW r = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++)
        r = r * 10 + (UW)(*p - '0');
    return r;
}

static void cmd_net(void)
{
    if (net_up) {
        print("[net] already up\r\n");
        return;
    }

    ID rx_sem = create_sem(0, 64);
    if (rx_sem < E_OK) {
        print("[net] sem create failed\r\n");
        return;
    }

    if (rtl8139_init(rx_sem) != E_OK) {
        print("[net] rtl8139_init failed (PKERNEL_NODE_ID issue?)\r\n");
        return;
    }

    UB mac[6];
    rtl8139_get_mac(mac);
    print("[net] node MAC = 52:54:00:00:00:0");
    {
        char d[2] = { (char)('0' + mac[5]), '\0' };
        print(d);
    }
    print("\r\n");

    /* Init the IP/UDP/ARP stack so it knows our identity.
     * node id = mac[5] (1..DNODE_MAX); internal index nid = mac[5]-1 must be
     * < DNODE_MAX. The cap tracks DNODE_MAX (was hard-wired to 8 before the
     * 8->32 scale-up — that bound, not any kdds resource limit, was what
     * stranded nodes 9+ in single-node mode at runtime). */
    if (mac[3] == 0 && mac[4] == 0 && mac[5] >= 1 && mac[5] <= DNODE_MAX) {
        UB  nid = (UB)(mac[5] - 1);
        UW  nip = ((UW)mac[5] << 24) | 0x0000010AUL;   /* 10.1.0.N */
        drpc_init(nid, nip);
        print("[net] DRPC initialised (10.1.0.");
        print_dec_s((W)mac[5]);
        print(")\r\n");
    } else {
        print("[net] no cluster MAC; single-node mode only\r\n");
    }

    netstack_start();
    create_task((FP)net_task, 3, 4096);

    /* Bring up SWIM so the two nodes actively discover each other via
     * periodic gossip — without it the only traffic would be the
     * one-shot ARP at netstack_start, which is one-way until someone
     * happens to send a reply. */
    swim_init();
    create_task((FP)swim_task, 6, 4096);
    print("[net] SWIM gossip task started\r\n");

    /* RTT シミュレーション (テスト用): PKERNEL_RTT_ZONE_SIZE>0 で
     * ノードを zone = id/size に分割し、異 zone へ合成 RTT を上乗せして
     * localhost でも複数 region が形成されるようにする (region 検証用)。 */
    {
        UW zs = env_uint("PKERNEL_RTT_ZONE_SIZE", 0);
        if (zs > 0) {
            swim_set_sim_zone((UB)zs, env_uint("PKERNEL_RTT_ZONE_PENALTY", 200));
            print("[net] RTT sim zones enabled (region partitioning test)\r\n");
        }
    }

    /* pmesh_init() already ran at boot (before kdds_init() so its
     * pmesh_socks[] zero-clear didn't wipe K-DDS's bind). Here we just
     * spawn the housekeeping task that periodically beacons routes
     * across the mesh — needs drpc_my_node to be set first. */
    create_task((FP)pmesh_task, 7, 2048);
    print("[net] pmesh routing task started\r\n");

    /* Distributed Transformer worker. Needs drpc_my_node set (done by
     * drpc_init above) so it can pick its role: node 0 (even id) drives
     * inference from the shell, node 1 (odd id) answers as the K-DDS
     * worker. Once SWIM sees a peer, degrade flips SOLO -> REDUCED and
     * `infer` runs tensor-parallel across the mesh. */
    create_task((FP)dtr_task, 6, 4096);
    print("[net] dtr distributed-inference worker started\r\n");

    /* DKVA responder — every node answers distributed-KV-attention queries
     * over its local cache. With 3+ nodes degrade goes FULL and dtr_infer's
     * DKVA path broadcasts Q, aggregating partial attention from the mesh. */
    create_task((FP)dkva_task, 7, 4096);
    print("[net] dkva attention responder started\r\n");

    /* MoE score gossip — broadcasts this node's per-class accuracy and
     * collects peers'. `moe` then routes a query to the best expert by
     * locality-aware utility (accuracy minus RTT penalty, regions R1). */
    create_task((FP)moe_task, 7, 4096);
    print("[net] moe score-gossip task started\r\n");

    /* World map — each node beacons a compact self-descriptor on its own
     * per-source topic "world/beacon/<id>" and assembles its own whole-
     * network view from received beacons. No central collector: the same
     * symmetric task runs on every node, so killing any node never
     * destroys the map (see world.h NO-CENTRAL invariant). */
    create_task((FP)world_task, 7, 4096);
    print("[net] world situational-awareness beacon task started\r\n");

    /* p-fs P1 replication — polls the region-scoped announce/want/sync
     * topics so a block put on any region peer is pulled in here too.
     * Same symmetric task on every node: no master copy. */
    create_task((FP)pfs_repl_task, 7, 4096);
    print("[net] pfs block replication task started\r\n");

    print("[net] up. Run a second ./p-kernel with PKERNEL_NODE_ID=2 to mesh.\r\n");
    net_up = 1;
}

/* Print a signed decimal over the sio frame channel. */
static void print_dec_s(W v)
{
    UW uv;
    if (v < 0) { print("-"); uv = (UW)(-v); } else uv = (UW)v;
    if (uv == 0) { print("0"); return; }
    char tmp[12]; INT t = 0;
    while (uv > 0) { tmp[t++] = (char)('0' + (uv % 10)); uv /= 10; }
    char out[13]; INT i = 0;
    while (t > 0) out[i++] = tmp[--t];
    out[i] = '\0';
    print(out);
}

/* Parse one signed decimal from [*pp, end); advance *pp past it.
 * Returns 1 if a number was consumed, 0 otherwise. */
static int parse_int(const UB **pp, const UB *end, INT *out)
{
    const UB *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) return 0;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (p >= end || *p < '0' || *p > '9') return 0;
    INT v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (INT)(*p - '0'); p++; }
    *out = neg ? -v : v;
    *pp  = p;
    return 1;
}

/* `infer [a b c d]` — run a Transformer inference on 4 int8 sensor
 * values. With no peers this is local (SOLO); once a second node has
 * meshed in, dtr_infer transparently distributes it (REDUCED/FULL).
 * The detailed scores are printed by dtr_infer itself. */
static void cmd_infer(const UB *line, INT n)
{
    const UB *p   = line;
    const UB *end = line + n;
    while (p < end && *p != ' ' && *p != '\t') p++;   /* skip the verb */

    B input[4] = { 40, 80, 30, 10 };   /* default demo sensor vector */
    for (INT i = 0; i < 4; i++) {
        INT v;
        if (!parse_int(&p, end, &v)) break;
        if (v >  127) v =  127;
        if (v < -128) v = -128;
        input[i] = (B)v;
    }

    print("[infer] sensors = [");
    for (INT i = 0; i < 4; i++) { if (i) print(" "); print_dec_s(input[i]); }
    print("]\r\n");

    W cls = dtr_infer(input);
    if (cls < 0) {
        print("[infer] no result (timeout or busy)\r\n");
    } else {
        static const char *cn[] = { "normal", "alert", "critical" };
        print("[infer] => class "); print_dec_s(cls);
        print(" ("); print(cn[cls < 3 ? cls : 0]); print(")\r\n");
    }
}

/* `moe [a b c d]` — route a query to the best expert node via locality-aware
 * MoE gating (accuracy minus RTT penalty, regions R1). moe_infer logs each
 * candidate's utility and the chosen expert. */
static void cmd_moe(const UB *line, INT n)
{
    const UB *p   = line;
    const UB *end = line + n;
    while (p < end && *p != ' ' && *p != '\t') p++;   /* skip the verb */

    B in[4] = { 40, 80, 30, 10 };
    for (INT i = 0; i < 4; i++) {
        INT v;
        if (!parse_int(&p, end, &v)) break;
        if (v >  127) v =  127;
        if (v < -128) v = -128;
        in[i] = (B)v;
    }
    UB cls = moe_infer(in[0], in[1], in[2], in[3]);
    print("[moe] => class "); print_dec_s((W)cls); print("\r\n");
}

/* `pfs` family — p-fs block store + P1 replication:
 *   pfs            -> P0 self-test (dedup / round-trip / miss)
 *   pfs put <text> -> store <text>; new blocks announce to region peers
 *   pfs ls         -> list blocks (id prefix / len / origin) */
static void cmd_pfs(const UB *line, INT n)
{
    const UB *p   = line + 3;          /* skip "pfs" */
    const UB *end = line + n;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (p < end && starts_with(p, (INT)(end - p), "put")) {
        p += 3;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) {
            print("usage: pfs put <text>\r\n");
            return;
        }
        pfs_repl_put_cmd(p, (UW)(end - p));
    } else if (p < end && starts_with(p, (INT)(end - p), "ls")) {
        pfs_repl_ls();
    } else {
        pfs_self_test(print);
    }
}

/* `rgnpub` — regions R0 demo: publish to a GLOBAL topic and a REGION-scoped
 * topic and report each pub's fanout (peers actually sent to). In a
 * partitioned cluster the region pub reaches only same-region peers. */
static void cmd_rgnpub(void)
{
    static W hg = -1, hr = -1;
    if (hg < 0) hg = kdds_open("demo/global", KDDS_QOS_BEST_EFFORT);
    if (hr < 0) hr = kdds_open_scoped("demo/region", KDDS_QOS_BEST_EFFORT,
                                      KDDS_SCOPE_REGION);
    UB payload = 0x42;
    kdds_pub(hg, &payload, 1);
    print("[rgnpub] global fanout = "); print_dec_s((W)kdds_pub_fanout());
    print(" peers\r\n");
    kdds_pub(hr, &payload, 1);
    print("[rgnpub] region fanout = "); print_dec_s((W)kdds_pub_fanout());
    print(" peers (same-region only)\r\n");
}

EXPORT INT usermain(void)
{
    print("\r\n p-kernel  [linux / aarch64 userspace]\r\n\r\n");

    /* AI Body layer — tensor pool, ai_job queue, pipeline, MLP. */
    ai_kernel_init();
    /* pmesh routing MUST come before kdds_init(): pmesh_init() zeros
     * pmesh_socks[], which would otherwise wipe out the kdds_rx
     * binding kdds_init() installs via pmesh_bind(). */
    pmesh_init();
    /* K-DDS — pub/sub. Single-node mode without a NIC. */
    kdds_init();
    /* DTR — distributed Transformer (the AI brain layer). */
    dtr_init();
    /* DKVA — distributed KV attention topics (FULL-mode responder). */
    dkva_init();
    /* Degrade controller — derives SOLO/REDUCED/FULL from the live node
     * count SWIM observes. Starts at SOLO until a peer appears. */
    degrade_init();
    /* MoE router — per-class accuracy scoreboard + locality-aware expert
     * selection (regions R1). The score-gossip task starts in cmd_net. */
    moe_init();
    /* World map — decentralized whole-network situational awareness. The
     * beacon task starts in cmd_net once the node ID is known. */
    world_init();
    /* p-fs P1 — region-scoped block replication. Opens its control
     * topics + chunk port and hooks pfs_put; must follow pmesh_init()
     * and kdds_init(). The poll task starts in cmd_net. */
    pfs_repl_init();

    /* If PKERNEL_AUTONET is set, bring up the network automatically
     * so a backgrounded node-2 process doesn't have to be driven via
     * its shell. */
    if (getenv("PKERNEL_AUTONET")) {
        print("\r\n[autonet] PKERNEL_AUTONET set — bringing up net\r\n");
        cmd_net();
    }

    print("\r\n  T-Kernel is alive inside a Linux process.\r\n");
    print("  Type 'help' for commands.\r\n\r\n");

    UB line[128];
    for (;;) {
        print("p-kernel> ");
        INT n = sio_read_line(line, sizeof(line));
        if (n <= 0) {
            print("\r\n");
            continue;
        }

        if (starts_with(line, n, "help")) {
            cmd_help();
        } else if (starts_with(line, n, "ai")) {
            ai_stats_print();
        } else if (starts_with(line, n, "infer")) {
            cmd_infer(line, n);
        } else if (starts_with(line, n, "dist")) {
            degrade_stat();
        } else if (starts_with(line, n, "nodes")) {
            swim_nodes_print();
        } else if (starts_with(line, n, "region")) {
            region_print();
        } else if (starts_with(line, n, "hrw")) {
            lookup_self_test(print);
            lookup_l1_self_test(print);
        } else if (starts_with(line, n, "rgnpub")) {
            cmd_rgnpub();
        } else if (starts_with(line, n, "world") || starts_with(line, n, "map")) {
            world_print();
        } else if (starts_with(line, n, "moe")) {
            cmd_moe(line, n);
        } else if (starts_with(line, n, "dtr")) {
            dtr_stat();
        } else if (starts_with(line, n, "kdds")) {
            kdds_list();
        } else if (starts_with(line, n, "pfs")) {
            cmd_pfs(line, n);
        } else if (starts_with(line, n, "kdemo")) {
            cmd_kdemo("aarch64");
        } else if (starts_with(line, n, "net")) {
            cmd_net();
        } else if (starts_with(line, n, "rx")) {
            cmd_rx();
        } else if (starts_with(line, n, "ver")) {
            cmd_ver();
        } else if (starts_with(line, n, "exit")) {
            print("bye.\r\n");
            arch_reboot();   /* exit(0) on Linux */
        } else {
            print("[echo] ");
            sio_send_frame(line, n);
            print("\r\n");
        }
    }
    return 0;
}
