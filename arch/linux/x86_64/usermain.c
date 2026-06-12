/*
 *  arch/linux/x86_64/usermain.c
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
#include "spec.h"   /* R3b breathing params */
#include "world.h"
#include "reflex.h"
#include "pmesh.h"
#include "demo_kdds.h"
#include "pfs_block.h"
#include "pfs_repl.h"
#include "pfs_dag.h"
#include "protect.h"
#include "guard.h"
#include "dmn.h"
#include "galaxy.h"  /* galaxy v1: the observation window task */
#include "selfc.h"
#ifdef HAVE_LIBTCC
#include "selfc_proc.h"
#endif
#include "genome.h"

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
IMPORT void dtr_train_cmd(const UB *args, UW len);   /* R3a training  */
IMPORT void dtr_worker_task(INT stacd, void *exinf); /* guarded worker */
IMPORT void dtr_recover_weights(void);               /* guard recover  */
IMPORT void r3_cmd(const UB *args, UW len);           /* R3 in-context  */
IMPORT void r3_handoff_test(void);                    /* LM-4 fast->slow */
IMPORT void r3_stream_test(void);                     /* LM-5 stream     */
IMPORT void mind_cmd(const UB *args, UW len);         /* LM-6 the mouth  */
IMPORT void mind_net_open(void);                      /* LM-7 reserve topic */
IMPORT void mind_net_task(INT stacd, void *exinf);    /* LM-7 shared mind */
IMPORT void mind_merge_task(INT stacd, void *exinf);  /* LM-10 Path W merge */
IMPORT void lm_test(void);                            /* living-mind DMN */
IMPORT void lm_self_test(void);                       /* living-mind Self */
IMPORT void sign_self_test(void);                      /* signing.md sign suite */
static void print_dec_s(W v);   /* fwd: used by cmd_net for multi-digit node id */
IMPORT void degrade_init(void);
IMPORT void degrade_stat(void);
IMPORT void dkva_init(void);
IMPORT void dkva_task(INT stacd, void *exinf);
IMPORT void dkva_cmd(const UB *args, UW len);

extern char *getenv(const char *);

/* Receive-side relay authenticity counters (defined in net_relay.c):
 *   ok     = authenticated + fresh frames delivered to the stack
 *   badmac = inbound frames dropped for failing HMAC verification (G4)
 *   replay = inbound frames dropped as nonce replays (wave-11 window) */
IMPORT void net_relay_stats(unsigned long *ok, unsigned long *badmac,
                            unsigned long *replay);

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
    print("  dtr eval  - accuracy + cross-entropy on the labelled dataset\r\n");
    print("  dtr train [epochs] - REAL training: SGD + analytic backprop\r\n");
    print("  dtr save  - trained weights -> versioned p-fs object dtr/weights\r\n");
    print("  dtr load  - load weights from p-fs (works on replicas too)\r\n");
    print("  dtr grad  - verify analytic gradient vs finite differences\r\n");
    print("  dtr remember - store engrams (memory the forward pass reads) in p-fs\r\n");
    print("  dtr ret [on|off|reload] - retrieval blend: forward votes with p-fs engrams\r\n");
    print("  dtr crash - fault-inject the guarded worker (NULL write); guard recovers\r\n");
    print("  guard  - guarded-task table (fault isolation + auto-respawn)\r\n");
    print("  infer [a b c d] - run a Transformer inference on 4 int8 sensors\r\n");
    print("  moe [a b c d] - route to best expert (locality MoE); `moe test` = §7/§8 property tests\r\n");
    print("  breathe - R3b: expert specialization; join smarter / leave graceful (numbers)\r\n");
    print("  dkva [infer [a b c d]] - distributed KV attention from THIS node\r\n");
    print("  dist   - distributed degrade level (SOLO/REDUCED/FULL)\r\n");
    print("  kdds   - K-DDS topic table\r\n");
    print("  kdemo  - cross-arch K-DDS heartbeat demo (pub+sub on demo/heartbeat)\r\n");
    print("  pfs    - p-fs block store self-test (dedup)\r\n");
    print("  pfs put <text> - store <text> as a block; replicates to region peers\r\n");
    print("  pfs ls - list stored blocks (id prefix / len / origin)\r\n");
    print("  pfs save <name> <text> - new VERSION of <name> (old ones survive)\r\n");
    print("  pfs log <name> - version history (walk the prev chain)\r\n");
    print("  pfs cat <name> [@<seq>] - head (or version <seq>) content\r\n");
    print("  selfc demo - compile C INSIDE the running kernel (libtcc) + run it\r\n");
    print("  selfc save <name> - save the demo C source as p-fs object <name>\r\n");
    print("  selfc run <name> - compile+run C source from p-fs object <name>\r\n");
    print("  selfc ls - list units compiled this boot\r\n");
    print("  genome - cell DNA status (role / manifest)\r\n");
    print("  genome publish <role> - this cell's DNA (weights+code) -> p-fs manifest\r\n");
    print("  genome sprout - empty plate germinates from the swarm's manifest\r\n");
    print("  net    - bring up the AF_UNIX virtual NIC and DRPC stack\r\n");
    print("  world  - whole-network situational map (alias: map), from gossip, no central\r\n");
    print("  reflex [on|off|table|stat] - §8 reflex layer: inference -> local defence\r\n");
    print("  protect <text>|ls|on|off|test - §2/G28 protected unit: ground threat in under-replication; actuator evacuates it\r\n");
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

    /* Relay-transport authenticity counters (only meaningful when running
     * over the relay backend; all zero otherwise). */
    {
        unsigned long ok = 0, badmac = 0, replay = 0;
        net_relay_stats(&ok, &badmac, &replay);
        i = 0;
        APPEND("[rx-relay] ok="); APPEND_DEC((UW)ok);
        APPEND("  badmac="); APPEND_DEC((UW)badmac);
        APPEND("  replay="); APPEND_DEC((UW)replay);
        buf[i++] = '\r'; buf[i++] = '\n';
        sio_send_frame((const UB *)buf, i);
    }
    #undef APPEND
    #undef APPEND_DEC
}

static void cmd_ver(void)
{
    print("p-kernel UMP — User-Mode p-kernel\r\n");
    print("  host arch    : x86_64-linux\r\n");
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
    } else if (mac[3] == 0 && mac[4] == 0 && mac[5] > DNODE_MAX) {
        /* G7: a valid relay-wire node id (1..255) that exceeds the cluster
         * cap. This node joins the relay transport but its index nid=mac[5]-1
         * would overflow dnode_table[DNODE_MAX], so drpc/pmesh/kdds can't see
         * it. Say so loudly — this is the silent single-node dropout the audit
         * flagged (net_relay_init already warns on the transport side). */
        print("[net] node id ");
        print_dec_s((W)mac[5]);
        print(" > DNODE_MAX(");
        print_dec_s((W)DNODE_MAX);
        print("): joins relay transport but NOT the cluster logic; "
              "single-node mode only\r\n");
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
    /* Test hook (G12 demo): PKERNEL_WORLD_BEACON_HOLD_MS>0 suppresses this
     * node's self-beacon for the first N ms, holding world-gossip intentionally
     * unconverged so samples/21_honest_degraded can prove degraded(k/n) honesty
     * does not depend on gossip freshness. Default 0 = no change in production. */
    {
        UW hold = env_uint("PKERNEL_WORLD_BEACON_HOLD_MS", 0);
        if (hold > 0) {
            world_set_beacon_hold(hold);
            print("[net] world beacon hold enabled (G12 honesty test)\r\n");
        }
    }
    create_task((FP)world_task, 7, 4096);
    print("[net] world situational-awareness beacon task started\r\n");

    /* Reflex layer (wave 9 配線②) — wires inference completion to local
     * immediate-defence actions (§8). Same symmetric task on every node:
     * it polls peers' "reflex/alarm/<id>" topics and reacts with an
     * attenuated reflex of its own. No central commander (alarms are
     * information, not orders). */
    create_task((FP)reflex_task, 7, 4096);
    print("[net] reflex (thought->action) task started\r\n");

    /* p-fs P1 replication — polls the region-scoped announce/want/sync
     * topics so a block put on any region peer is pulled in here too.
     * Same symmetric task on every node: no master copy. */
    create_task((FP)pfs_repl_task, 7, 4096);
    print("[net] pfs block replication task started\r\n");

    /* LM-7 (living-mind.md Part VIII) — the shared mind. Polls the region-
     * scoped "mind/teach" topic; a fact taught on any region peer enters
     * THIS node's queue through r3_fact_learn and THIS node's DMN
     * consolidates it. Symmetric on every node (no teacher of record). */
    /* 16K stack: the arrival path runs the FULL R3 substrate (m_boot's
     * s_pretrain + r3_fact_learn's frozen reads), the same deep call chain
     * the DMN task uses an 8K stack for — give margin (the 4K net-task
     * default overflows it; the hosted-relay stack-overflow lesson). */
    create_task((FP)mind_net_task, 7, 16384);
    print("[net] mind shared-teach (LM-7) task started\r\n");

    /* LM-10 (living-mind.md Part XI) — Path W: the one mind. The fleet-DMN
     * slow-band weight-merge pulse: each node publishes its rw[] (84 KB, 22
     * chunks) and gl_merge()s the region into ONE shared weight-state.
     * Symmetric, no aggregator, region-scoped. 84 KB buffers are file-static
     * (.bss), not task-stack locals (the hosted-relay stack lesson). */
    create_task((FP)mind_merge_task, 7, 16384);
    print("[net] mind one-mind weight-merge (LM-10 Path W) task started\r\n");

    /* p-fs P2 ref gossip — beacons this node's name->head-manifest refs
     * on the region topic and merges peers' (LWW by version seq). Same
     * symmetric task everywhere: refs are gossip, not a registry. */
    /* 8K stack (was 4K): a CROSS-NODE ref adoption (e.g. a peer's self/prov
     * replicated in by P1 — LM-7 is the first slice to exercise it) walks
     * merge_entry -> ref_set -> refs_persist -> pfs_dur_write, whose 2x1KB
     * path[] locals overflowed the 4K stack (garbage-PC crash, the hosted-
     * relay stack lesson). 8K matches the DMN task's depth budget. */
    create_task((FP)pfs_dag_task, 7, 8192);
    print("[net] pfs ref (version DAG) gossip task started\r\n");


    /* G28 — protected-object actuator. While a declared unit is under-
     * replicated it re-announces it to drive replication into neighbours'
     * durable store; the grounded threat falls as replicas reach R.
     * Symmetric on every node (no central commander). */
    /* control hook: PKERNEL_PROTECT_OFF=1 disables the actuator. */
    if (env_uint("PKERNEL_PROTECT_OFF", 0)) protect_set_enabled(0);
    create_task((FP)protect_task, 7, 4096);
    print("[net] protect actuator task started\r\n");

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
 * candidate's utility and the chosen expert.
 * `moe test` — run the §7/§8 property self-tests (I7/I8/D0/§5). */
static void cmd_moe(const UB *line, INT n)
{
    const UB *p   = line;
    const UB *end = line + n;
    while (p < end && *p != ' ' && *p != '\t') p++;   /* skip the verb */
    while (p < end && (*p == ' ' || *p == '\t')) p++; /* skip spaces      */

    /* `moe test` — NO-CENTRAL gating / two-layer low-pass / oscillation /
     * simultaneous-event property tests. CI greps the [moe-*] PASS lines. */
    if (p < end && starts_with(p, (INT)(end - p), "test")) {
        moe_self_test();
        return;
    }

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
 *   pfs ls         -> list blocks (id prefix / len / origin)
 *   pfs save/log/cat -> P2 version DAG (pfs_dag_cmd) */
static void cmd_pfs(const UB *line, INT n)
{
    const UB *p   = line + 3;          /* skip "pfs" */
    const UB *end = line + n;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (p < end && (starts_with(p, (INT)(end - p), "save") ||
                    starts_with(p, (INT)(end - p), "log")  ||
                    starts_with(p, (INT)(end - p), "cat"))) {
        /* P2 version-DAG verbs — parsed + printed by pfs_dag.c */
        pfs_dag_cmd(p, (UW)(end - p));
    } else if (p < end && starts_with(p, (INT)(end - p), "put")) {
        p += 3;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) {
            print("usage: pfs put <text>\r\n");
            return;
        }
        pfs_repl_put_cmd(p, (UW)(end - p));
    } else if (p < end && starts_with(p, (INT)(end - p), "get")) {
        /* `pfs get <text>` — recompute the block-id = sha256(text) and fetch
         * via pfs_get. After a remount the P0 table is empty, so this
         * exercises the durable backend's fall-through (ARK / flat). */
        p += 3;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) {
            print("usage: pfs get <text>\r\n");
            return;
        }
        {
            U1 gid[PFS_ID_LEN];
            pfs_id_compute(p, (UW)(end - p), gid);
            static UB gbuf[PFS_BLOCK_MAX + 1];
            INT gr = pfs_get(gid, gbuf, (UW)PFS_BLOCK_MAX);
            if (gr < 0) {
                print("[pfs] get: NOT FOUND\r\n");
            } else {
                UW gl = (UW)gr;
                if (gl > PFS_BLOCK_MAX) gl = PFS_BLOCK_MAX;
                gbuf[gl] = '\0';
                print("[pfs] get: ");
                print((const char *)gbuf);
                print("\r\n");
            }
        }
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
    print("\r\n p-kernel  [linux / x86_64 userspace]\r\n\r\n");

    /* AI Body layer — tensor pool, ai_job queue, pipeline, MLP. */
    ai_kernel_init();
    /* pmesh routing MUST come before kdds_init(): pmesh_init() zeros
     * pmesh_socks[], which would otherwise wipe out the kdds_rx
     * binding kdds_init() installs via pmesh_bind(). */
    pmesh_init();
    /* K-DDS — pub/sub. Single-node mode without a NIC. */
    kdds_init();
    /* LM-7 — reserve the cluster-wide "mind/teach" topic slot NOW, before
     * dkva_init()'s per-node pre-opens saturate the bounded topic table. */
    mind_net_open();
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
    /* Reflex layer (§8) — thought->action wiring. The reflex_task itself
     * starts in cmd_net once the node ID is known; here we just clear state
     * so reflex_is_shielded()/reflex_threat_level() are safe pre-mesh. */
    reflex_init();
    /* p-fs P1 — region-scoped block replication. Opens its control
     * topics + chunk port and hooks pfs_put; must follow pmesh_init()
     * and kdds_init(). The poll task starts in cmd_net. */
    pfs_repl_init();
    /* p-fs P2 — version DAG + ref gossip on "pfs/ref". Manifests ride
     * the P1 block replication; only refs are mutable. */
    pfs_dag_init();
    /* G24 — make the library non-volatile. If PKERNEL_PFS_DIR is set,
     * reload content-addressed blocks (sha256-verified) and the named-ref
     * table from disk: the swarm's memory survives a total power loss and
     * comes back on reboot. No-op (memory-only) when the env is unset. */
    pfs_durable_restore(print);
    pfs_dag_restore();
    /* G28 — protected-object registry. Registers the pfs_repl announce-hook
     * so heard announces feed the holder count that grounds the threat.
     * Must follow pfs_repl_init(). protect_task starts in cmd_net. */
    protect_init();
    /* guard — ring-0 task fault isolation + auto-respawn (wave 7).
     * The dtr worker is guarded with recover_fn = reload the trained
     * weights from the p-fs object "dtr/weights": a fault kills the
     * task, never the node, and the brain comes back from p-fs. */
    guard_init();
    guard_register("dtr-worker", (FP)dtr_worker_task, 4096, 6,
                   dtr_recover_weights);

    /* DMN — the idle-time organ (Phase 13; living-mind Part VI VI.0 #3
     * / COMMANDER DECISION 1). Until LM-5 this was x86-only dead code:
     * the hosted fleet (the binary CI actually tests) never created the
     * task, so the live sleep hooks (lm_consolidate_idle_round + the
     * LM-5 r3_consolidate_idle_round) could never fire. Lowest priority
     * (13, below every 3-7 task above): it runs only when everything
     * else blocks, and consolidation runs only when engrams/facts are
     * pending. Params mirror arch/x86/usermain.c (DMN_PRIORITY/STACK). */
    dmn_init();
    if (create_task((FP)dmn_task, 13, 8192) < E_OK)
        print("[ERR] DMN task\r\n");
    else
        print("[OK]  DMN task\r\n");

    /* galaxy v1 (docs/architecture/galaxy.md) — the per-node observation
     * window: a loopback HTTP/1.0 server (port 7800+(id-1)) serving THIS
     * node's gossip-bounded view + an SSE event stream of real organism
     * events. Default ON for hosted builds; PKERNEL_GALAXY=0 opts out.
     * Priority 8: below net/swim (3/6), above the dmn (13) — a web `mind`
     * verb cannot be preempted by the prio-13 consolidation round (§3.3,
     * §6). Bind is 127.0.0.1 ONLY (galaxy_posix.c, hard-coded). */
    galaxy_init();
    if (galaxy_on) {
        if (create_task((FP)galaxy_task, 8, 8192) < E_OK)
            print("[ERR] galaxy task\r\n");
        else
            print("[OK]  galaxy observation window task\r\n");
    } else {
        print("[net] galaxy disabled (PKERNEL_GALAXY=0)\r\n");
    }

#ifdef HAVE_LIBTCC
    /* selfc-ring3 §2.1: the germ supervisor task — runs at BOOT (not just
     * after `net`) so `selfc test`/`selfc run` work standalone. It owns ALL
     * germ lifecycle: it is the task that fork()s germs, waitpid(WNOHANG)s
     * them, drains their capability frames and rolls back bad builds (§1.4).
     * CRUCIAL: germ fork()s happen in THIS task, NOT the shell task — forking
     * from the shell task corrupts the shell's stdin (the COW child can
     * re-enter the cooperative scheduler and steal console input). */
    create_task((FP)selfc_proc_task, 7, 65536);
    print("[OK]  selfc germ supervisor task\r\n");
#endif

    /* If PKERNEL_AUTONET is set, bring up the network automatically
     * so a backgrounded node-2 process doesn't have to be driven via
     * its shell. */
    if (getenv("PKERNEL_AUTONET")) {
        print("\r\n[autonet] PKERNEL_AUTONET set — bringing up net\r\n");
        cmd_net();
    }

    /* §3 self-regeneration: PKERNEL_SPROUT=1 makes an empty plate wait
     * for the swarm's genome manifest and germinate from it. Runs here
     * (shell task, before the shell loop) so the pfs_dag scratch rule
     * holds; default OFF, so every existing demo is untouched. */
    genome_autosprout();

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
        } else if (starts_with(line, n, "dkva")) {
            /* "dkva" -> stats; "dkva infer [a b c d]" -> distributed
             * attention with THIS node as the requester (any node may
             * be the origin — survival, wave 8) */
            dkva_cmd(line + 4, (UW)(n - 4));
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
        } else if (starts_with(line, n, "breathe")) {
            /* R3b: expert specialization — join smarter / leave graceful */
            breathe_cmd(line + 7, (UW)(n - 7));
        } else if (starts_with(line, n, "dmn")) {
            /* living-mind first slice (docs/architecture/living-mind.md II):
             * `dmn test` runs the DMN sleep-consolidation acceptance suite
             * (replay engrams -> distill via gl_merge; no catastrophic
             * forgetting; decentralized; survives kill+rejoin). */
            const UB *a = line + 3; UW al = (UW)(n - 3);
            while (al && (*a==' '||*a=='\t')) { a++; al--; }
            if (al >= 4 && a[0]=='t'&&a[1]=='e'&&a[2]=='s'&&a[3]=='t') lm_test();
            else print("usage: dmn test\r\n");
        } else if (starts_with(line, n, "self") && (n == 4 || line[4] == ' ')) {
            /* living-mind Self layer (docs/architecture/living-mind.md III):
             * `self test` runs the distributed-autobiographical-self suite
             * (hash-chained "self/lin" lineage that survives death, is
             * tamper-evident, and reconstructs from a peer excluding the
             * origin). Guarded against shadowing the `selfc` verb below. */
            const UB *a = line + 4; UW al = (UW)(n - 4);
            while (al && (*a==' '||*a=='\t')) { a++; al--; }
            if (al >= 4 && a[0]=='t'&&a[1]=='e'&&a[2]=='s'&&a[3]=='t') lm_self_test();
            else print("usage: self test\r\n");
        } else if (starts_with(line, n, "sign") && (n == 4 || line[4] == ' ')) {
            /* signing.md: `sign test` — Ed25519 provenance suite (RFC 8032
             * KATs + selflayer/unit/keyrotation). A signature attests an
             * ARTIFACT came from a KEY, never a human. */
            const UB *a = line + 4; UW al = (UW)(n - 4);
            while (al && (*a==' '||*a=='\t')) { a++; al--; }
            if (al >= 4 && a[0]=='t'&&a[1]=='e'&&a[2]=='s'&&a[3]=='t') sign_self_test();
            else print("usage: sign test\r\n");
        } else if (starts_with(line, n, "handoff") && (n == 7 || line[7] == ' ')) {
            /* living-mind LM-4 (docs/architecture/living-mind.md Part V):
             * `handoff test` runs the fast->slow handoff acceptance suite
             * (a fact learned ONLY in-context by R3's FAST layer is
             * self-distilled into R3's OWN weights rw[] via r_backward, so
             * after a sleep-consolidation round the mind answers it WITHOUT
             * the prompt; scrambled-teacher control proves the grounding). */
            const UB *a = line + 7; UW al = (UW)(n - 7);
            while (al && (*a==' '||*a=='\t')) { a++; al--; }
            if (al >= 4 && a[0]=='t'&&a[1]=='e'&&a[2]=='s'&&a[3]=='t') r3_handoff_test();
            else if (al >= 6 && a[0]=='s'&&a[1]=='t'&&a[2]=='r'&&a[3]=='e'&&a[4]=='a'&&a[5]=='m') r3_stream_test();
            else print("usage: handoff test|stream\r\n");
        } else if (starts_with(line, n, "r3")) {
            /* R3: non-trivial thought — in-context recall capacity cert.
             * `r3 test` proves learned >> any fixed hand-if (by construction). */
            r3_cmd(line + 2, (UW)(n - 2));
        } else if (starts_with(line, n, "mind")) {
            /* living-mind LM-6 (docs/architecture/living-mind.md Part VII):
             * the mouth — the OWNER teaches the live mind at this prompt;
             * the DMN task's own idle pulses consolidate (no harness).
             * mind teach <k> <v> | mind ask <k> | mind wait [s] | mind */
            mind_cmd(line + 4, (UW)(n - 4));
        } else if (starts_with(line, n, "dtr")) {
            /* "dtr" / "dtr stat" -> stats; eval/train/save/load/grad/
             * remember/ret -> R3a + wave-8 verbs (dtr_train.c) */
            dtr_train_cmd(line + 3, (UW)(n - 3));
        } else if (starts_with(line, n, "guard")) {
            guard_print();
        } else if (starts_with(line, n, "kdds")) {
            kdds_list();
        } else if (starts_with(line, n, "pfs")) {
            cmd_pfs(line, n);
        } else if (starts_with(line, n, "protect")) {
            /* `protect <text>|ls|stat|on|off|test` — G28 protected-object
             * registry: declare a unit, ground the threat in its under-
             * replication, and drive the actuator that closes the loop. */
            protect_cmd(line + 7, (UW)(n - 7));
        } else if (starts_with(line, n, "reflex")) {
            /* `reflex [on|off|table|stat]` — §8 reflex layer status/control */
            reflex_cmd(line + 6, (UW)(n - 6));
        } else if (starts_with(line, n, "selfc")) {
            /* SHIELD (§8): under sustained threat the reflex layer refuses to
             * take in unknown code — don't germinate new self-compiled units
             * while shielded (real defensive action, not a print). */
            if (reflex_is_shielded()) {
                print("[reflex] SHIELD active — refusing new selfc germination "
                      "(no unknown code under attack)\r\n");
            } else {
                selfc_cmd(line, n);
            }
        } else if (starts_with(line, n, "genome")) {
            /* SHIELD (§8): genome sprouting compiles foreign code into this
             * cell (via selfc) — refuse it under sustained threat too. */
            if (reflex_is_shielded()) {
                print("[reflex] SHIELD active — refusing genome germination "
                      "(no foreign DNA under attack)\r\n");
            } else {
                genome_cmd(line, n);
            }
        } else if (starts_with(line, n, "kdemo")) {
            cmd_kdemo("x86_64");
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
