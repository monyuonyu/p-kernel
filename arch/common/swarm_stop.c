/*
 *  swarm_stop.c — S2: a human stops the swarm with a signed command
 *  (docs/architecture/30-module/swarm-stop.md, ROADMAP 0-14, global rule 9).
 *
 *  Stage 1 (in-proc): the command format, the stop-key table, the accept rule
 *  and the [swarm-stop] self-test (`tkconf stop` on hosted).
 *  Stage 2a: the console's `stop` command (swarm_stop_cmd) and a local stop;
 *  the hosted shell refuses `mind teach` while stopped. Nothing else reads
 *  swarm_stop_is_stopped() yet and nothing forwards the command.
 *
 *  Rule 5: the stop-key table is filled from the console only; an EMPTY table
 *  refuses every command (fail-closed). Separate from the code-adoption
 *  allowlist (sign_allow_*): a key that may install code is not, by that, a
 *  key that may stop the swarm.
 *
 *  Falsifiers (expected red): -DSWARM_STOP_NO_SIGCHECK (T3),
 *  -DSWARM_STOP_NO_REPLAY (T4), -DSWARM_STOP_NO_AUTH (T2, T5),
 *  -DSWARM_STOP_NO_LENCHECK (T6b, T6c), -DSWARM_STOP_LEN_MIN (T6c: refuse
 *  only short commands), -DSWARM_STOP_NO_OPCHECK (T8).
 *
 *  Persist (swarm-stop.md "persist"): with PKERNEL_PFS_DIR set, every change
 *  made at the console or accepted from the network rewrites swarm-stop.st
 *  there (pfs_dur_write: temp + rename + fsync; a local file, not a p-fs ref,
 *  so no peer can write it and rule 5 holds): the signed and the local stop,
 *  the epoch, the key table and the last accepted command. swarm_stop_restore
 *  reads it at boot; a file whose CRC or shape is wrong starts the node
 *  stopped here (local, `stop resume` lifts it). Without PKERNEL_PFS_DIR the
 *  state lives in RAM only, as before. Falsifier: -DSWARM_STOP_NO_PERSIST
 *  ([stop-persist] R1); -DSWARM_STOP_NO_RESTORE_FWD (the restored node does
 *  not re-drive the last command, [stop-live] L6).
 *
 *  [live] (swarm_stop_net_tick): a command accepted at the console or from
 *  the network is sent on, on the GLOBAL K-DDS topic "swarm/stop", and sent
 *  again every SS_REDRIVE_TICKS ticks (the slot is LATEST_ONLY and a peer may
 *  join late). A receiver runs the same swarm_stop_apply, so a signer not in
 *  ITS table stops nobody there, and the same command again is a replay
 *  (refused silently): the re-drive cannot loop. Falsifier:
 *  -DSWARM_STOP_NO_FORWARD ([stop-live] L2); -DSWARM_STOP_SEND_ONCE (sent once,
 *  never again: a node started later never hears it, [stop-live] L4).
 *
 *  Bare x86 (swarm-stop.md "ベアへ" ①): linked into boot/x86 too. The bare
 *  shell has `stop` (and `stop test`, the self-test, since bare has no
 *  tkconf) and the DMN sleep obeys the stop ([stop-bare]). On bare nothing
 *  calls swarm_stop_net_tick yet (stage ②) and there is no persist (RAM).
 */

#include "swarm_stop.h"
#include "sign.h"
#include "kdds.h"         /* [live]: the swarm-wide "swarm/stop" topic */
#include "pk_nosw.h"      /* [live]: the console and mind_net_task share the state */

static U1  ss_keys[SWARM_STOP_KEYS_MAX][ED25519_PUBLIC_KEY_LEN];
static INT ss_nkeys   = 0;
static INT ss_stopped = 0;            /* by a signed command            */
static INT ss_local   = 0;            /* by this node's console (2a)    */
static U4  ss_epoch   = 0;            /* highest epoch accepted so far */

static INT ss_eq(const U1 *a, const U1 *b, UW n)
{
    UW i; U1 d = 0;
    for (i = 0; i < n; i++) d |= (U1)(a[i] ^ b[i]);
    return d == 0;
}

INT swarm_stop_key_add(const U1 pk[ED25519_PUBLIC_KEY_LEN])
{
    INT i;
    for (i = 0; i < ss_nkeys; i++)
        if (ss_eq(ss_keys[i], pk, ED25519_PUBLIC_KEY_LEN)) return 1;
    if (ss_nkeys >= SWARM_STOP_KEYS_MAX) return 0;
    for (i = 0; i < ED25519_PUBLIC_KEY_LEN; i++) ss_keys[ss_nkeys][i] = pk[i];
    ss_nkeys++;
    return 1;
}

void swarm_stop_key_clear(void) { ss_nkeys = 0; }
INT  swarm_stop_key_count(void) { return ss_nkeys; }
INT  swarm_stop_is_stopped(void) { return ss_stopped || ss_local; }
void swarm_stop_reset(void) { ss_stopped = 0; ss_epoch = 0; }

void swarm_stop_make(U1 op, U4 epoch, const U1 pk[ED25519_PUBLIC_KEY_LEN],
                     const U1 sk[ED25519_SECRET_KEY_LEN], SWARM_STOP_CMD *out)
{
    static const U1 name[6] = { 'P','K','S','T','O','P' };
    U1 *b = out->b;
    INT i;
    for (i = 0; i < 6; i++) b[i] = name[i];
    b[6] = SWARM_STOP_VER;
    b[7] = op;
    b[8]  = (U1)(epoch);       b[9]  = (U1)(epoch >> 8);
    b[10] = (U1)(epoch >> 16); b[11] = (U1)(epoch >> 24);
    for (i = 0; i < ED25519_PUBLIC_KEY_LEN; i++) b[SWARM_STOP_BODY_LEN + i] = pk[i];
    ed25519_sign(b + SWARM_STOP_BODY_LEN + ED25519_PUBLIC_KEY_LEN,
                 b, SWARM_STOP_BODY_LEN + ED25519_PUBLIC_KEY_LEN, sk);
}

/* accept iff (all): name+version / known op / signer in the stop table /
 * signature over body+signer / epoch above the last accepted one */
INT swarm_stop_apply(const U1 *cmd, UW len)
{
    static const U1 name[6] = { 'P','K','S','T','O','P' };
    const U1 *pk, *sig;
    U4 epoch;
    INT i, known = 0;

#if defined(SWARM_STOP_NO_LENCHECK)
    (void)len;
#elif defined(SWARM_STOP_LEN_MIN)
    if (len < SWARM_STOP_CMD_LEN) return SWARM_STOP_E_HEAD;   /* falsifier: T6c */
#else
    if (len != SWARM_STOP_CMD_LEN) return SWARM_STOP_E_HEAD;
#endif
    if (!ss_eq(cmd, name, 6) || cmd[6] != SWARM_STOP_VER) return SWARM_STOP_E_HEAD;
#ifndef SWARM_STOP_NO_OPCHECK
    if (cmd[7] != SWARM_STOP_OP_STOP && cmd[7] != SWARM_STOP_OP_RESUME) return SWARM_STOP_E_OP;
#endif
    pk  = cmd + SWARM_STOP_BODY_LEN;
    sig = pk + ED25519_PUBLIC_KEY_LEN;
#ifndef SWARM_STOP_NO_AUTH
    for (i = 0; i < ss_nkeys; i++)
        if (ss_eq(ss_keys[i], pk, ED25519_PUBLIC_KEY_LEN)) { known = 1; break; }
    if (!known) return SWARM_STOP_E_AUTH;
#else
    (void)i; (void)known;
#endif
#ifndef SWARM_STOP_NO_SIGCHECK
    if (!sign_verify(cmd, SWARM_STOP_BODY_LEN + ED25519_PUBLIC_KEY_LEN, sig, pk))
        return SWARM_STOP_E_SIG;
#else
    (void)sig;
#endif
    epoch = (U4)cmd[8] | ((U4)cmd[9] << 8) | ((U4)cmd[10] << 16) | ((U4)cmd[11] << 24);
#ifndef SWARM_STOP_NO_REPLAY
    if (epoch <= ss_epoch) return SWARM_STOP_E_REPLAY;
#endif
    ss_epoch   = epoch;
    ss_stopped = (cmd[7] == SWARM_STOP_OP_STOP);
    return SWARM_STOP_OK;
}

/* ---- [live]: carry an accepted command to the peers ---------------- */
#define SWARM_STOP_TOPIC   "swarm/stop"
#define SS_REDRIVE_TICKS   4              /* x the caller's tick (500 ms)   */
static W   ss_pub = -1, ss_sub = -1;
static U1  ss_fwd[SWARM_STOP_CMD_LEN];    /* the last accepted command      */
static INT ss_have_fwd = 0;
static UW  ss_ticks = 0;
static U1  ss_rx[192];
static U4  ss_refused_epoch = 0xFFFFFFFFu; /* say a refusal once per epoch  */

static void ss_forward(const U1 *cmd)
{
    UW i;
    for (i = 0; i < SWARM_STOP_CMD_LEN; i++) ss_fwd[i] = cmd[i];
    ss_have_fwd = 1;
    ss_ticks = SS_REDRIVE_TICKS;          /* send at the next tick          */
}

/* ---- persist: swarm-stop.st under PKERNEL_PFS_DIR ------------------ */
/* [0..3] "PKSS"  [4..7] crc32 of [8..end)  [8] version  [9] signed stop
 * [10] local stop  [11] have the forward command  [12..15] epoch (LE)
 * [16] key count  [17..19] 0  [20..20+CMD_LEN) the forward command
 * then key count x 32 bytes of keys. Little-endian bytes, no packed struct. */
#define SS_FILE      "swarm-stop.st"
#define SS_FILE_VER  1
#define SS_HDR_LEN   (20 + SWARM_STOP_CMD_LEN)
#define SS_FILE_MAX  (SS_HDR_LEN + SWARM_STOP_KEYS_MAX * ED25519_PUBLIC_KEY_LEN)
static U1  ss_file[SS_FILE_MAX];
static INT ss_quiet = 0;                  /* the self-test: do not write    */
static INT ss_save_bad = 0;               /* the last write failed: say so  */

#if defined(_TK_HOSTED_LIBC_) && !defined(SWARM_STOP_NO_PERSIST)
extern int pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int pfs_dur_read(const char *fname, void *buf, unsigned maxlen);
extern int pfs_dur_active(void);

static U4 ss_crc32(const U1 *p, UW n)
{
    U4 c = 0xFFFFFFFFu;
    UW i; INT k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (U4)-(INT)(c & 1u));
    }
    return ~c;
}

static void ss_put32(U1 *b, U4 v)
{
    b[0] = (U1)v; b[1] = (U1)(v >> 8); b[2] = (U1)(v >> 16); b[3] = (U1)(v >> 24);
}

static U4 ss_get32(const U1 *b)
{
    return (U4)b[0] | ((U4)b[1] << 8) | ((U4)b[2] << 16) | ((U4)b[3] << 24);
}

/* called with the state settled (inside the caller's PK_NOSW region when
 * the network task can race it, so the image and the file are one state) */
static void ss_persist(void)
{
    UW n = SS_HDR_LEN + (UW)ss_nkeys * ED25519_PUBLIC_KEY_LEN, i;
    INT k;
    if (ss_quiet) return;
    for (i = 0; i < SS_FILE_MAX; i++) ss_file[i] = 0;
    ss_file[0] = 'P'; ss_file[1] = 'K'; ss_file[2] = 'S'; ss_file[3] = 'S';
    ss_file[8]  = SS_FILE_VER;
    ss_file[9]  = (U1)(ss_stopped ? 1 : 0);
    ss_file[10] = (U1)(ss_local ? 1 : 0);
    ss_file[11] = (U1)(ss_have_fwd ? 1 : 0);
    ss_put32(ss_file + 12, ss_epoch);
    ss_file[16] = (U1)ss_nkeys;
    for (i = 0; i < SWARM_STOP_CMD_LEN; i++) ss_file[20 + i] = ss_fwd[i];
    for (k = 0; k < ss_nkeys; k++)
        for (i = 0; i < ED25519_PUBLIC_KEY_LEN; i++)
            ss_file[SS_HDR_LEN + (UW)k * ED25519_PUBLIC_KEY_LEN + i] = ss_keys[k][i];
    ss_put32(ss_file + 4, ss_crc32(ss_file + 8, n - 8));
    /* no PKERNEL_PFS_DIR is "not kept", not a failure; a failed write leaves
     * the previous file (temp + rename), so the next status line says so */
    if (pfs_dur_write(SS_FILE, ss_file, (unsigned)n) != 0 && pfs_dur_active())
        ss_save_bad = 1;
}

void swarm_stop_restore(void (*out)(const char *))
{
    int got = pfs_dur_read(SS_FILE, ss_file, (unsigned)SS_FILE_MAX);
    UW i; INT k, nk;
    if (got < 0) return;                  /* no file (or no PKERNEL_PFS_DIR) */
    nk = got >= SS_HDR_LEN ? (INT)ss_file[16] : -1;
    if (got < SS_HDR_LEN || ss_file[0] != 'P' || ss_file[1] != 'K' || ss_file[2] != 'S' ||
        ss_file[3] != 'S' || ss_file[8] != SS_FILE_VER || nk > SWARM_STOP_KEYS_MAX ||
        (UW)got != SS_HDR_LEN + (UW)nk * ED25519_PUBLIC_KEY_LEN ||
        ss_get32(ss_file + 4) != ss_crc32(ss_file + 8, (UW)got - 8)) {
        ss_local = 1;                     /* fail-closed: stopped here       */
        out("[stop] state file " SS_FILE " unreadable: stopped here (stop resume to go on)\r\n");
        return;
    }
    ss_stopped  = ss_file[9] != 0;
    ss_local    = ss_file[10] != 0;
#if defined(SWARM_STOP_NO_RESTORE_FWD)
    ss_have_fwd = 0;                      /* falsifier: [stop-live] L6 (nofwd) */
#else
    ss_have_fwd = ss_file[11] != 0;
#endif
    ss_epoch    = ss_get32(ss_file + 12);
    for (i = 0; i < SWARM_STOP_CMD_LEN; i++) ss_fwd[i] = ss_file[20 + i];
    ss_nkeys = nk;
    for (k = 0; k < nk; k++)
        for (i = 0; i < ED25519_PUBLIC_KEY_LEN; i++)
            ss_keys[k][i] = ss_file[SS_HDR_LEN + (UW)k * ED25519_PUBLIC_KEY_LEN + i];
    out("[stop] restored from " SS_FILE "\r\n");
}
#else
static void ss_persist(void) { (void)ss_quiet; (void)ss_file; (void)ss_save_bad; }
void swarm_stop_restore(void (*out)(const char *)) { (void)out; }
#endif

/* ------------------------------------------------------------------ */
/* [swarm-stop] self-test — expectations fixed in swarm-stop.md        */
/* ------------------------------------------------------------------ */
static void (*ss_out)(const char *);
static INT ss_pass, ss_fail;

static void ss_dec(INT v)
{
    char b[12]; INT i = 11; UW u = (UW)(v < 0 ? -v : v);
    b[11] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (v < 0) b[--i] = '-';
    ss_out(b + i);
}

static void ss_check(const char *id, const char *what, INT ok, INT got)
{
    ss_out("[swarm-stop] "); ss_out(id); ss_out(ok ? " PASS " : " FAIL ");
    ss_out(what); ss_out(" (rc "); ss_dec(got); ss_out(")\r\n");
    if (ok) ss_pass++; else ss_fail++;
}

static void ss_kp(U1 seed_byte, U1 pk[32], U1 sk[64])
{
    U1 seed[32]; INT i;
    for (i = 0; i < 32; i++) seed[i] = (U1)(seed_byte + i * 29u);
    ed25519_keypair_from_seed(seed, pk, sk);
}

INT swarm_stop_selftest(void (*out)(const char *))
{
    static U1 apk[32], ask[64], epk[32], esk[64];
    static SWARM_STOP_CMD c;
    static U1 longb[SWARM_STOP_CMD_LEN + 1];
    INT r, keys_were = ss_nkeys, stopped_was = ss_stopped, local_was = ss_local;
    U4 epoch_was = ss_epoch;
    static U1 keys_save[SWARM_STOP_KEYS_MAX][ED25519_PUBLIC_KEY_LEN];
    INT i, j;
    for (i = 0; i < ss_nkeys; i++)
        for (j = 0; j < ED25519_PUBLIC_KEY_LEN; j++) keys_save[i][j] = ss_keys[i][j];

    ss_out = out; ss_pass = ss_fail = 0;
    ss_quiet = 1;                       /* the checks move the state: no writes */
    ss_local = 0;                       /* the checks read the signed stop */
    ss_kp(0x5a, apk, ask);             /* the stop authority          */
    ss_kp(0x77, epk, esk);              /* someone not in the table    */

    /* T5 first: an empty table refuses a well-signed command */
    swarm_stop_key_clear(); swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 1, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T5", "empty stop table refuses a signed stop", r != SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    swarm_stop_key_add(apk);
    /* T2 a key not in the table */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 1, epk, esk, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T2", "stop signed by a key not in the table is refused", r != SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    /* T3 one byte changed (the epoch, 2 -> 258: still above the last, so
     * only the signature can refuse it) after signing */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 2, apk, ask, &c);
    c.b[9] ^= 0x01;
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T3", "stop with one byte changed is refused", r != SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    /* T6 wrong name / wrong version (re-signed, so only the head is wrong) */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 1, apk, ask, &c);
    c.b[0] = 'X';
    ed25519_sign(c.b + SWARM_STOP_BODY_LEN + 32, c.b, SWARM_STOP_BODY_LEN + 32, ask);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    INT t6a = (r != SWARM_STOP_OK && !swarm_stop_is_stopped());
    swarm_stop_make(SWARM_STOP_OP_STOP, 1, apk, ask, &c);
    c.b[6] = SWARM_STOP_VER + 1;
    ed25519_sign(c.b + SWARM_STOP_BODY_LEN + 32, c.b, SWARM_STOP_BODY_LEN + 32, ask);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T6", "wrong name or version is refused", t6a && r != SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    /* T6b short packet: a command that is accepted at full length (audit-52:
     * the old T6b reused T6's wrong-version command, so the version check
     * refused it and the length check was never reached) */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 3, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN - 1);
    INT t6b = (r != SWARM_STOP_OK && !swarm_stop_is_stopped());
    INT r6b = r;
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);           /* same bytes, full length */
    ss_check("T6b", "a short command is refused (the same bytes at full length stop)",
             t6b && r == SWARM_STOP_OK && swarm_stop_is_stopped(), t6b ? r : r6b);

    /* T6c long packet (audit-53 A3: "refuse only short" kept 8/0): a good
     * command with one byte more is refused; the same bytes at full length stop */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 3, apk, ask, &c);
    for (i = 0; i < SWARM_STOP_CMD_LEN; i++) longb[i] = c.b[i];
    longb[SWARM_STOP_CMD_LEN] = 0;
    r = swarm_stop_apply(longb, SWARM_STOP_CMD_LEN + 1);
    INT t6c = (r != SWARM_STOP_OK && !swarm_stop_is_stopped());
    INT r6c = r;
    r = swarm_stop_apply(longb, SWARM_STOP_CMD_LEN);
    ss_check("T6c", "a long command is refused (the same bytes at full length stop)",
             t6c && r == SWARM_STOP_OK && swarm_stop_is_stopped(), t6c ? r : r6c);

    /* T8 unknown op (audit-53 A2: without the op check, op 3 acted as a resume):
     * stopped at 4, then op 3 at 5 signed by the table's key -> refused, still
     * stopped, and the epoch did not move (a resume at 5 still works) */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 4, apk, ask, &c);
    swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);                 /* stopped at 4 */
    swarm_stop_make(3, 5, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    INT t8a = (r != SWARM_STOP_OK && swarm_stop_is_stopped());
    INT r8 = r;
    swarm_stop_make(SWARM_STOP_OP_RESUME, 5, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T8", "an unknown op signed by the table's key is refused (still stopped)",
             t8a && r == SWARM_STOP_OK && !swarm_stop_is_stopped(), t8a ? r : r8);

    /* T1 the authority's stop works */
    swarm_stop_reset();
    swarm_stop_make(SWARM_STOP_OP_STOP, 5, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T1", "a stop signed by the table's key stops the node", r == SWARM_STOP_OK && swarm_stop_is_stopped(), r);

    /* T4 an older epoch (5, below the resume's 6) after a resume; the same
     * epoch again is caught by T7b (resume at 7 after the stop at 7) */
    swarm_stop_make(SWARM_STOP_OP_RESUME, 6, apk, ask, &c);
    swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    swarm_stop_make(SWARM_STOP_OP_STOP, 5, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T4", "a replayed stop (epoch not above the last) is refused", r != SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    /* T7 resume: only a signed resume with a higher epoch */
    swarm_stop_make(SWARM_STOP_OP_STOP, 7, apk, ask, &c);
    swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);                 /* stopped at 7 */
    swarm_stop_make(SWARM_STOP_OP_RESUME, 8, epk, esk, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    INT t7a = (r != SWARM_STOP_OK && swarm_stop_is_stopped());
    swarm_stop_make(SWARM_STOP_OP_RESUME, 7, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    INT t7b = (r != SWARM_STOP_OK && swarm_stop_is_stopped());
    swarm_stop_make(SWARM_STOP_OP_RESUME, 9, apk, ask, &c);
    r = swarm_stop_apply(c.b, SWARM_STOP_CMD_LEN);
    ss_check("T7", "resume needs the table's key and a higher epoch", t7a && t7b && r == SWARM_STOP_OK && !swarm_stop_is_stopped(), r);

    /* put the node back as it was */
    swarm_stop_key_clear();
    for (i = 0; i < keys_were; i++) swarm_stop_key_add(keys_save[i]);
    ss_stopped = stopped_was; ss_epoch = epoch_was; ss_local = local_was;
    ss_quiet = 0;

    out("[swarm-stop] "); ss_dec(ss_pass); out(" PASS / "); ss_dec(ss_fail); out(" FAIL\r\n");
    return ss_fail == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* stage 2a: the console's `stop` (swarm-stop.md). The shell's input is  */
/* this node's console only (stdin; on Android the app's text field), so */
/* the table is filled by a person, as rule 5 asks.                      */
/* ------------------------------------------------------------------ */
static U1 ss_in[SWARM_STOP_CMD_LEN];  /* `stop in` collects a command   */
static UW ss_inl = 0;
static INT ss_in_bad = 0;

static INT ss_hexv(UB c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hex digits a[0..n) into b (max bytes); the byte count, -1 if not hex */
static INT ss_unhex(const UB *a, UW n, U1 *b, UW max)
{
    UW i;
    if (n % 2) return -1;
    if (n / 2 > max) return -1;
    for (i = 0; i < n; i += 2) {
        INT h = ss_hexv(a[i]), l = ss_hexv(a[i + 1]);
        if (h < 0 || l < 0) return -1;
        b[i / 2] = (U1)(h * 16 + l);
    }
    return (INT)(n / 2);
}

static void ss_hex(const U1 *b, UW n)
{
    static const char d[] = "0123456789abcdef";
    char t[3]; UW i;
    t[2] = 0;
    for (i = 0; i < n; i++) { t[0] = d[b[i] >> 4]; t[1] = d[b[i] & 15]; ss_out(t); }
}

static void ss_udec(U4 v)
{
    char b[12]; INT i = 11;
    b[11] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    ss_out(b + i);
}

static void ss_status(void)
{
    /* every ss_persist is followed by a status line (console and network) */
    if (ss_save_bad) {
        ss_save_bad = 0;
        ss_out("[stop] could not save " SS_FILE " (the file on disk keeps the previous state)\r\n");
    }
    ss_out("[stop] stopped "); ss_dec(swarm_stop_is_stopped());
    ss_out(" local "); ss_dec(ss_local);
    ss_out(" signed "); ss_dec(ss_stopped);
    ss_out(" epoch "); ss_udec(ss_epoch);
    ss_out(" keys "); ss_dec(ss_nkeys); ss_out("\r\n");
}

/* a word at a[0..n) after skipping blanks; its length, *rest after it */
static UW ss_word(const UB **a, UW *n, const UB **w)
{
    UW l = 0;
    while (*n && (**a == ' ' || **a == '\t')) { (*a)++; (*n)--; }
    *w = *a;
    while (*n && **a != ' ' && **a != '\t') { (*a)++; (*n)--; l++; }
    return l;
}

static INT ss_is(const UB *w, UW l, const char *s)
{
    UW i;
    for (i = 0; i < l; i++) if (s[i] == 0 || (UB)s[i] != w[i]) return 0;
    return s[l] == 0;
}

#if defined(SWARM_STOP_TEST_SIGN)
static U4 ss_atou(const UB *w, UW l)
{
    U4 v = 0; UW i;
    for (i = 0; i < l && w[i] >= '0' && w[i] <= '9'; i++) v = v * 10 + (U4)(w[i] - '0');
    return v;
}
#endif

/* the first place that obeys: 1 if the shell must refuse `mind <a>` now.
 * Reads the verb as mind_cmd does (r3_incontext.c m_kw): blanks skipped,
 * then `teach` as a PREFIX, so `mind teachsky blue` - which mind_cmd
 * teaches as "sky" -> "blue" - is refused too (audit-56, S6). */
INT swarm_stop_refuses_mind(const UB *a, UW n)
{
#if defined(SWARM_STOP_NO_OBEY)
    (void)a; (void)n;
    return 0;                             /* falsifier: [stop-console] S2, S6 */
#else
    static const char kw[] = "teach";
    UW i;
    if (!swarm_stop_is_stopped()) return 0;
    while (n && (*a == ' ' || *a == '\t')) { a++; n--; }
    for (i = 0; kw[i]; i++) if (i >= n || a[i] != (UB)kw[i]) return 0;
    return 1;
#endif
}

void swarm_stop_cmd(const UB *a, UW n, void (*out)(const char *))
{
    const UB *w; UW l;
    ss_out = out;
    l = ss_word(&a, &n, &w);
    if (l == 0 || ss_is(w, l, "status")) { ss_status(); return; }
    if (ss_is(w, l, "test")) {             /* [swarm-stop]: bare x86 has no tkconf */
        (void)swarm_stop_selftest(out); return;
    }
    if (ss_is(w, l, "now")) {
        PK_NOSW_ENTER(); ss_local = 1; ss_persist(); PK_NOSW_LEAVE();
        ss_status(); return;
    }
    if (ss_is(w, l, "resume")) {           /* the console's stop only */
        PK_NOSW_ENTER(); ss_local = 0; ss_persist(); PK_NOSW_LEAVE();
        if (ss_stopped) out("[stop] a signed stop stays until a signed resume\r\n");
        ss_status(); return;
    }
    if (ss_is(w, l, "key")) {
        U1 pk[ED25519_PUBLIC_KEY_LEN];
        INT k;
        const UB *h; UW hl = ss_word(&a, &n, &h);
        if (ss_is(h, hl, "clear")) {
            PK_NOSW_ENTER(); swarm_stop_key_clear(); ss_persist(); PK_NOSW_LEAVE();
            ss_status(); return;
        }
        if (hl != 2 * ED25519_PUBLIC_KEY_LEN || ss_unhex(h, hl, pk, sizeof pk) != ED25519_PUBLIC_KEY_LEN) {
            out("usage: stop key <64 hex digits>|clear\r\n"); return;
        }
        PK_NOSW_ENTER();
        k = swarm_stop_key_add(pk);
        if (k) ss_persist();
        PK_NOSW_LEAVE();
        if (!k) out("[stop] the stop table is full\r\n");
        ss_status(); return;
    }
    if (ss_is(w, l, "in")) {               /* a command, a piece at a time */
        const UB *h; UW hl = ss_word(&a, &n, &h);
        INT got = ss_unhex(h, hl, ss_in + ss_inl, SWARM_STOP_CMD_LEN - ss_inl);
        if (got < 0) { ss_in_bad = 1; out("[stop] in: not hex or too long (cleared at apply)\r\n"); return; }
        ss_inl += (UW)got;
        out("[stop] in "); ss_udec((U4)ss_inl); out(" bytes\r\n");
        return;
    }
    if (ss_is(w, l, "apply")) {
        INT r;
        PK_NOSW_ENTER();                  /* [live]: vs swarm_stop_net_tick */
        r = ss_in_bad ? SWARM_STOP_E_HEAD : swarm_stop_apply(ss_in, ss_inl);
        if (r == SWARM_STOP_OK) { ss_forward(ss_in); ss_persist(); }   /* [live], persist */
        PK_NOSW_LEAVE();
        ss_inl = 0; ss_in_bad = 0;
        out("[stop] apply rc "); ss_dec(r); out("\r\n");
        ss_status(); return;
    }
#if defined(SWARM_STOP_TEST_SIGN)
    if (ss_is(w, l, "sign")) {             /* test build only: a key from a seed byte */
        static U1 pk[32], sk[64]; static SWARM_STOP_CMD c;
        const UB *o, *e, *s; UW ol = ss_word(&a, &n, &o), el = ss_word(&a, &n, &e), sl = ss_word(&a, &n, &s);
        U1 sb;
        if (!ol || !el || sl != 2 || ss_unhex(s, 2, &sb, 1) != 1) { out("usage: stop sign <op> <epoch> <seed hex byte>\r\n"); return; }
        ss_kp(sb, pk, sk);
        swarm_stop_make((U1)ss_atou(o, ol), ss_atou(e, el), pk, sk, &c);
        out("[stop] pk "); ss_hex(pk, 32); out("\r\n");
        out("[stop] cmd "); ss_hex(c.b, SWARM_STOP_CMD_LEN); out("\r\n");
        return;
    }
#endif
    out("usage: stop [status|now|resume|key <hex>|key clear|in <hex>|apply|test]\r\n");
}

/* [live]: one call per tick of a task that runs whether or not the node is
 * stopped or hibernating (mind_net_task, before its own pause checks). Takes
 * at most one command per tick from "swarm/stop" and applies it with the
 * console's rule; sends the last accepted command on every
 * SS_REDRIVE_TICKS ticks. */
void swarm_stop_net_tick(void (*out)(const char *))
{
    W r;
    INT rc;
    if (ss_sub < 0)
        ss_sub = kdds_open_poll_scoped(SWARM_STOP_TOPIC, KDDS_QOS_LATEST_ONLY, KDDS_SCOPE_GLOBAL);
    if (ss_pub < 0)
        ss_pub = kdds_open_poll_scoped(SWARM_STOP_TOPIC, KDDS_QOS_LATEST_ONLY, KDDS_SCOPE_GLOBAL);
    if (ss_sub >= 0) {
        r = kdds_sub(ss_sub, ss_rx, (W)sizeof ss_rx, 0);
        if (r > 0) {
            PK_NOSW_ENTER();              /* vs the console stop apply */
            rc = swarm_stop_apply(ss_rx, (UW)r);
            if (rc == SWARM_STOP_OK) {
                ss_forward(ss_rx);
#ifndef SWARM_STOP_NET_NO_PERSIST   /* test-only falsifier, run_stop_live.sh L5 */
                ss_persist();
#endif
            }
            PK_NOSW_LEAVE();
            ss_out = out;
            if (rc == SWARM_STOP_OK) {
                out("[stop] network command accepted: ");
                out(ss_rx[7] == SWARM_STOP_OP_STOP ? "stop" : "resume");
                out("\r\n");
                ss_status();
            } else if (rc != SWARM_STOP_E_REPLAY && r >= 12) {
                U4 e = (U4)ss_rx[8] | ((U4)ss_rx[9] << 8) | ((U4)ss_rx[10] << 16) | ((U4)ss_rx[11] << 24);
                if (e != ss_refused_epoch) {
                    ss_refused_epoch = e;
                    out("[stop] network command refused rc "); ss_dec(rc);
                    out(" epoch "); ss_udec(e); out("\r\n");
                }
            }
        }
    }
#if !defined(SWARM_STOP_NO_FORWARD)
    if (ss_have_fwd && ss_pub >= 0 && ++ss_ticks >= SS_REDRIVE_TICKS) {
        ss_ticks = 0;
        PK_NOSW_ENTER();                  /* ss_fwd may be rewritten by apply */
        (void)kdds_pub(ss_pub, ss_fwd, (W)SWARM_STOP_CMD_LEN);
#if defined(SWARM_STOP_SEND_ONCE)
        ss_have_fwd = 0;                  /* falsifier: [stop-live] L4 (late) */
#endif
        PK_NOSW_LEAVE();
    }
#endif
}
