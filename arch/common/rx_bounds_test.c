/*
 *  rx_bounds_test.c — `tkconf rx`: input-validation cert for the UDP/pmesh
 *  receive entry points (ROADMAP 0-9). Hosted builds only (linux x86_64 /
 *  aarch64, Windows); this file is not linked into the bare-metal kernels, but
 *  the guards it tests live in shared TUs and do change the bare .text
 *  (R-RXB in the baton).
 *
 *  Each case hands one fixed packet to a production rx function and checks the
 *  module's reject counter: a well-formed control packet must leave it alone,
 *  a short or out-of-range packet must bump it by one. The verdict is the
 *  counter only. The buffers are the full packet size with only `len` passed,
 *  so a read past `len` stays inside the buffer and no sanitizer would see it
 *  (the hosted builds are -static anyway, without ASan): a guard that reads
 *  the wrong bytes but still drops shows up here only if the counter differs.
 *  For raft the cert also reads raft_log_size(), which must never exceed
 *  RAFT_LOG_MAX.
 *
 *  Falsifiers (each reverts one guard; the cert then goes RED for that case):
 *    -DRXB_NO_SPAWN_GUARD  spawn src_node range check
 *    -DRXB_NO_SPAWN_LEN    spawn STATE_PUSH length check
 *    -DRXB_NO_PMESH_GUARD  pmesh DATA dst_node range check
 *    -DRXB_NO_RAFT_GUARD   raft append bound (checks the wire field, not size)
 *    -DRXB_NO_KLOAD_CLEN   kloader chunk_len vs buffer / packet length
 *    -DRXB_NO_KLOAD_IDX    kloader chunk_idx ceiling (offset forming)
 *    -DRXB_OLD_KLOAD_FLOOR kloader header floor back to 12 bytes (K6; K2 stays
 *                          green, the chunk_len check also drops it)
 *
 *  This leaves junk raft entries and an open kloader transfer behind, so run
 *  it on a throwaway node (`tkconf rx`).
 */

#include "kernel.h"
#include "drpc.h"
#include "spawn.h"
#include "raft.h"
#include "pmesh.h"
#include "kloader_task.h"

extern UW spawn_drop_bad;
extern UW pmesh_drop_bad;
extern UW kloader_drop_bad;

static void (*rxb_out)(const char *);
static INT rxb_pass, rxb_fail;

static void rxb_dec(UW v)
{
    char b[12]; INT i = 11; b[i] = 0;
    if (v == 0) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    rxb_out(&b[i]);
}

/* ok = the module behaved; report PASS/FAIL with the observed delta. */
static void rxb_check(const char *id, const char *what, INT ok, UW got, UW want)
{
    rxb_out("[rxb] "); rxb_out(id); rxb_out(ok ? " PASS " : " FAIL ");
    rxb_out(what); rxb_out("  reject+"); rxb_dec(got);
    rxb_out(" (want +"); rxb_dec(want); rxb_out(")\r\n");
    if (ok) rxb_pass++; else rxb_fail++;
}

static void p32(UB *p, UW v) { p[0]=(UB)v; p[1]=(UB)(v>>8); p[2]=(UB)(v>>16); p[3]=(UB)(v>>24); }
static void p16(UB *p, UH v) { p[0]=(UB)v; p[1]=(UB)(v>>8); }

/* ---- spawn ---------------------------------------------------------- */

#define SP_HDR ((UH)(sizeof(SPAWN_PKT) - SPAWN_CHUNK_SIZE))

static void sp_build(UB *b, UH n, UB type, UB src)
{
    for (UH i = 0; i < n; i++) b[i] = 0;
    p32(b, SPAWN_MAGIC);
    b[4] = SPAWN_VERSION; b[5] = type; b[6] = src; b[7] = pk_node_v1(drpc_my_node);
}

static void sp_case(const char *id, const char *what, UH n, UB type, UB src,
                    UW want)
{
    UB b[sizeof(SPAWN_PKT)];
    UW before = spawn_drop_bad;
    sp_build(b, n, type, src);
    spawn_rx(0, SPAWN_PORT, b, n);
    UW d = spawn_drop_bad - before;
    rxb_check(id, what, d == want, d, want);
}

/* ---- raft ----------------------------------------------------------- */

/* RAFT_PKT packed offsets: magic0 version4 type5 src6 cand7 term8 log_idx12
 * log_term16 commit_idx20 granted24 success25 entry_key26 entry_val27
 * entry_idx28 entry_term32. term/entry_term non-zero so the append branch is
 * taken; a large entry_idx keeps "entry_idx >= log_size" true every packet. */
static void rf_build(UB *b)
{
    for (UH i = 0; i < sizeof(RAFT_PKT); i++) b[i] = 0;
    p32(b, RAFT_MAGIC);
    b[4] = RAFT_VERSION; b[5] = RAFT_MSG_APPEND;
    p32(b + 8,  1);                       /* term       */
    b[26] = 7; b[27] = 9;                 /* entry_key / entry_val */
    p32(b + 28, 0xFFFFFFFFUL);            /* entry_idx  */
    p32(b + 32, 1);                       /* entry_term */
}

/* ---- pmesh ---------------------------------------------------------- */

static void pm_data(UB *b, UB dst)
{
    for (UH i = 0; i < 12; i++) b[i] = 0;
    p32(b, PMESH_MAGIC);
    b[4] = PMESH_VERSION; b[5] = PMESH_DATA; b[6] = 0 /*src*/; b[7] = dst;
    p16(b + 8, 0); p16(b + 10, 0);        /* dst_port, data_len = 0 */
}

static void pm_case(const char *id, const char *what, UB dst, UW want)
{
    UB b[12];
    UW before = pmesh_drop_bad;
    pm_data(b, dst);
    pmesh_rx(0, PMESH_PORT, b, 12);
    UW d = pmesh_drop_bad - before;
    rxb_check(id, what, d == want, d, want);
}

/* ---- kloader -------------------------------------------------------- */

#define KL_HDR ((UH)(sizeof(KLOAD_PKT) - KLOAD_CHUNK_SIZE))

static INT kl_pushed;
static void kl_count_push(UW ip) { (void)ip; kl_pushed++; }

/* KLOAD_PKT packed offsets: magic0 version4 type5 src6 _pad7 total_size8
 * chunk_idx12 chunk_len16 data18. */
static void kl_build(UB *b, UB type, UW idx, UH clen)
{
    for (UH i = 0; i < sizeof(KLOAD_PKT); i++) b[i] = 0;
    p32(b, KLOAD_MAGIC);
    b[4] = KLOAD_VERSION; b[5] = type;
    p32(b + 8,  (1 << 20));               /* total_size */
    p32(b + 12, idx);                     /* chunk_idx  */
    p16(b + 16, clen);                    /* chunk_len  */
}

static void kl_case(const char *id, const char *what, UW idx, UH clen, UH len,
                    UW want)
{
    UB b[sizeof(KLOAD_PKT)];
    UW before = kloader_drop_bad;
    kl_build(b, KLOAD_CHUNK, idx, clen);
    kloader_rx(0, KLOAD_PORT, b, len);
    UW d = kloader_drop_bad - before;
    rxb_check(id, what, d == want, d, want);
}

/* ---- runner --------------------------------------------------------- */

INT rx_bounds_run(void (*out)(const char *))
{
    rxb_out = out; rxb_pass = 0; rxb_fail = 0;

    out("[rxb] rx-bounds cert (ROADMAP 0-9): malformed-packet rejection\r\n");

    /* spawn */
    sp_case("S1", "spawn DONE (control)",          (UH)sizeof(SPAWN_PKT), SPAWN_DONE,       3,          0);
    sp_case("S2", "spawn READY src=DNODE_MAX",     SP_HDR,                SPAWN_READY,      DNODE_MAX,   1);
    sp_case("S3", "spawn READY src=255",           SP_HDR,                SPAWN_READY,      255,         1);
    sp_case("S4", "spawn STATE_PUSH header-only",  SP_HDR,                SPAWN_STATE_PUSH, 1,           1);
    sp_case("S5", "spawn STATE_PUSH full (ctrl)",  (UH)sizeof(SPAWN_PKT), SPAWN_STATE_PUSH, 1,           0);

    /* raft: feed RAFT_LOG_MAX+8 append packets; log must cap at RAFT_LOG_MAX */
    {
        UB b[sizeof(RAFT_PKT)];
        rf_build(b);
        for (INT k = 0; k < RAFT_LOG_MAX + 8; k++)
            raft_rx(0, 0 /*port*/, b, (UH)sizeof(RAFT_PKT));
        UW sz = raft_log_size();
        rxb_check("R1", "raft log stays within RAFT_LOG_MAX",
                  sz <= RAFT_LOG_MAX, sz, RAFT_LOG_MAX);
    }

    /* pmesh */
    pm_case("P1", "pmesh DATA dst=DNODE_MAX",      DNODE_MAX, 1);
    pm_case("P2", "pmesh DATA dst=255",            255,       1);
    pm_case("P3", "pmesh DATA dst=5 (control)",    5,         0);

    /* kloader: the receiver is shut by default (KLOAD-UNAUTH, 0-14 S3), so a
     * well-formed START from the mesh is turned away; red with
     * -DKLOAD_NO_RX_GATE. The owner may have typed `kload open` before
     * `tkconf rx`: shut it for K0 and put it back at the end (audit-51) */
    INT kl_was_open = kloader_rx_is_open();
    kloader_set_rx_open(0);
    {
        UB s[sizeof(KLOAD_PKT)];
        UW before = kloader_drop_closed;
        kl_build(s, KLOAD_START, 0, 0);
        kloader_rx(0, KLOAD_PORT, s, KL_HDR);
        UW d = kloader_drop_closed - before;
        rxb_check("K0", "kload START refused while closed", d == 1, d, 1);
    }
    /* then open it (as `kload open` does), open a transfer, then controls
     * and malformed chunks */
    kloader_set_rx_open(1);
    {
        UB s[sizeof(KLOAD_PKT)];
        kl_build(s, KLOAD_START, 0, 0);
        kloader_rx(0, KLOAD_PORT, s, KL_HDR);
    }
    kl_case("K1", "kload CHUNK idx0 len16 (ctrl)", 0,                 16,   (UH)(KL_HDR + 16),   0);
    kl_case("K2", "kload CHUNK 12-byte packet",    0,                 16,   12,                  1);
    kl_case("K3", "kload CHUNK chunk_len>1024",    0,                 2000, (UH)(KL_HDR + 100),  1);
    /* idx 0x400000 is past the ceiling (KLOAD_BUF_MAX/KLOAD_CHUNK_SIZE=8192);
     * idx*KLOAD_CHUNK_SIZE = 0x400000*1024 also wraps 32 bits to 0, the case
     * the offset-before-bound ordering mishandled. */
    kl_case("K4", "kload CHUNK idx past ceiling",  0x400000UL,        1024, (UH)(KL_HDR + 1024), 1);
    kl_case("K5", "kload CHUNK clen100 len50",     0,                 100,  (UH)(KL_HDR + 50),   1);
    /* last: under the old 12-byte floor this START is taken (and resets the
     * transfer); with the header floor it is dropped */
    {
        UB s[sizeof(KLOAD_PKT)];
        UW before = kloader_drop_bad;
        kl_build(s, KLOAD_START, 0, 0);
        kloader_rx(0, KLOAD_PORT, s, 12);
        UW d = kloader_drop_bad - before;
        rxb_check("K6", "kload START 12-byte packet", d == 1, d, 1);
    }
    kloader_set_rx_open(kl_was_open);

    /* K7 (kload finding 3): a beacon queued while auto-push was on must not
     * be pushed once the owner turns auto-push off. The drain gets a counting
     * stub, never the real push. (The kloader task drains the same queue
     * every 5 s; it could take the beacon between the two calls below - then
     * K7 reads queued 0 and is red, never a false green.) */
    {
        UB b[12];
        INT ap_was = kloader_auto_push_on();
        kl_pushed = 0;
        p32(b, KLOAD_MAGIC); b[4] = KLOAD_VERSION; b[5] = KLOAD_BEACON;
        b[6] = 9; b[7] = 0; p32(b + 8, 0x0A0100FEUL);
        kloader_set_auto_push(1);
        kloader_beacon_rx(0x0A0100FEUL, KLOAD_PORT_BCN, b, 12);
        kloader_set_auto_push(0);
        INT q = kloader_beacon_queued();
        INT pushed = kloader_beacon_drain(kl_count_push);
        rxb_check("K7", "beacon queued before autopush off is not pushed",
                  q == 1 && pushed == 0 && kloader_beacon_queued() == 0, (UW)pushed, 0);
        kloader_set_auto_push(ap_was);
    }

    out("[rxb] "); rxb_dec((UW)rxb_pass);
    out(" PASS / "); rxb_dec((UW)rxb_fail); out(" FAIL\r\n");
    return rxb_fail == 0 ? 0 : -1;
}
