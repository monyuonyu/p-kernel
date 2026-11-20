/*
 * arch/linux/node_ident.c — N2c first slice: the node keeps its signing key
 * and its SWIM incarnation across restarts (hosted only; bare metal keeps the
 * old behaviour, crown). Design: docs/architecture/30-module/research/n2c-identity.md
 * §2 1 and 4.
 *
 *  node.key    the Ed25519 seed, 32 bytes, 0600, next to node_seed in
 *              $PKERNEL_PFS_DIR. Written with pfs_dur_write (tmp, fsync, rename,
 *              dir fsync). A node.key that is there but does not read as 32 bytes
 *              is NEVER overwritten: it is moved to node.key.bad and a new key is
 *              made (one line says so). If node.key.bad is already there too,
 *              nothing on disk is touched and this run uses a key it does not
 *              keep (one line says so) — a person decides which file is right.
 *              A key made from weak entropy is not kept either.
 *  node.incarn the incarnation, 4 bytes little endian. At start it is read,
 *              +1, written (fsync) BEFORE the first beacon, so a node that was
 *              declared DEAD comes back with a number above the rumour. The wire
 *              still carries one byte (the low 8 bits); see the design §4.
 *
 *  Without PKERNEL_PFS_DIR nothing is kept (exactly the old behaviour).
 *  hosted CFLAGS, host headers, no kernel.h (like pfs_durable.c).
 */
#include <stdio.h>
#include <string.h>

extern void sio_send_frame(const unsigned char *buf, int size);
extern int  sign_entropy(unsigned char *out, int n);
extern const char *pfs_dur_dir(void);
extern int  pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int  pfs_dur_read(const char *fname, void *buf, unsigned maxlen);
extern int  pfs_dur_exists(const char *fname);

static void say(const char *s) { sio_send_frame((const unsigned char *)s, (int)strlen(s)); }

/* rename <dir>/<from> to <dir>/<to>; 0 on success. Refuses when <to> is there
 * (rename would replace it on POSIX). */
static int move_aside(const char *from, const char *to)
{
    const char *dir = pfs_dur_dir();
    char a[1024], b[1024];
    if (!dir || pfs_dur_exists(to)) return -1;
    if (snprintf(a, sizeof a, "%s/%s", dir, from) >= (int)sizeof a) return -1;
    if (snprintf(b, sizeof b, "%s/%s", dir, to) >= (int)sizeof b) return -1;
    return rename(a, b) == 0 ? 0 : -1;
}

/* Fill seed[0..32). Returns bit 0 = strong entropy (or read from disk),
 * bit 1 = read from node.key, bit 2 = made now and kept in node.key. */
int pk_node_key_seed(unsigned char seed[32])
{
    unsigned char kb[33];
    int n, strong;

#ifndef N2C_NC_KEY_EPHEMERAL
    if (pfs_dur_dir()) {
        n = pfs_dur_read("node.key", kb, sizeof kb);
        if (n == 32) {
            memcpy(seed, kb, 32); memset(kb, 0, sizeof kb);
            return 1 | 2;
        }
        memset(kb, 0, sizeof kb);
        if (n >= 0 || pfs_dur_exists("node.key")) {
            char m[160];
#ifndef N2C_NC_KEY_OVERWRITE
            if (move_aside("node.key", "node.key.bad") != 0) {
                snprintf(m, sizeof m, "[sign] node.key: not 32 bytes (%d) and node.key.bad is there "
                         "too; nothing changed on disk, this run uses a key it does not keep\r\n", n);
                say(m);
                return sign_entropy(seed, 32) ? 1 : 0;
            }
            snprintf(m, sizeof m, "[sign] node.key: not 32 bytes (%d); kept as node.key.bad, "
                     "a new key\r\n", n);
            say(m);
#endif
        }
        strong = sign_entropy(seed, 32);
        if (!strong) {
            say("[sign] node.key: weak entropy, the key is not kept\r\n");
            return 0;
        }
        if (pfs_dur_write("node.key", seed, 32) != 0) {
            say("[sign] node.key: could not be written; this run uses a key it does not keep\r\n");
            return 1;
        }
        return 1 | 4;
    }
#endif
    return sign_entropy(seed, 32) ? 1 : 0;
}

/* The incarnation for this start: node.incarn + 1, written before it is used.
 * 0 when nothing is kept (no PKERNEL_PFS_DIR). */
unsigned pk_node_incarn_boot(void)
{
#ifndef N2C_NC_INC_RESET
    unsigned char b[5];
    unsigned v = 0;
    int n;
    char m[160];
    if (!pfs_dur_dir()) return 0;
    n = pfs_dur_read("node.incarn", b, sizeof b);
    if (n == 4) {
        v = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
    } else if (n >= 0 || pfs_dur_exists("node.incarn")) {
        /* the count is lost; keep the bytes for a person, start again from 0 */
        if (move_aside("node.incarn", "node.incarn.bad") == 0)
            snprintf(m, sizeof m, "[swim] node.incarn: not 4 bytes (%d); kept as node.incarn.bad, "
                     "counting again from 0\r\n", n);
        else
            snprintf(m, sizeof m, "[swim] node.incarn: not 4 bytes (%d) and node.incarn.bad is "
                     "there too; counting again from 0\r\n", n);
        say(m);
        v = 0;
    }
    v++;
    b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF); b[3] = (unsigned char)((v >> 24) & 0xFF);
    if (pfs_dur_write("node.incarn", b, 4) != 0)
        say("[swim] node.incarn: could not be written\r\n");
    return v;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------------
 * N2c stage 3 (research/n2c-identity.md §2 2, 3 and §11): a wide id drawn from
 * the key, PKERNEL_NODE_WIDE=1 only (the narrow default stays as it was).
 *
 *  node.redraw  the redraw count, 4 bytes little endian, written (fsync) before
 *               the id it gives is used. Missing = 0.
 *  id           257 + (two bytes of sha256(public key || count, LE 4 bytes)
 *               mod 65278) = 257..65534.
 *  fingerprint  the first 8 bytes of sha256(public key); the v2 beacon carries it.
 * ------------------------------------------------------------------------ */
#include <stdlib.h>
#include "sha256.h"
#if !defined(_WIN32) && !defined(__ANDROID__)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

extern int sign_node_key_ensure(void);
extern const unsigned char *sign_node_pubkey(void);

/* the redraw count: node.redraw, or (falsifier N2C_NC_REDRAW_VOLATILE) only the
 * environment the re-exec hands on, so a real restart forgets it */
static unsigned wide_count(void)
{
    const char *e = getenv("PKERNEL_N2C_REDRAW_MEM");
    if (e && *e) return (unsigned)strtoul(e, 0, 10);
#ifndef N2C_NC_REDRAW_VOLATILE
    {
        unsigned char b[5];
        if (pfs_dur_dir() && pfs_dur_read("node.redraw", b, sizeof b) == 4)
            return (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
    }
#endif
    return 0;
}

static unsigned wide_now_count;   /* the count the id of this run came from */
static int      wide_now_id;      /* 0 = this run did not draw a wide id   */

/* The wire id (257..65534) for this run, or 0 when PKERNEL_NODE_WIDE is not 1
 * or there is no key. net_relay_init calls it when PKERNEL_NODE_ID is unset. */
int pk_node_wide_id(void)
{
    const char *w = getenv("PKERNEL_NODE_WIDE");
    const unsigned char *pk;
    unsigned char in[36], h[SHA256_DIGEST_SIZE];
    unsigned c;
    int id;
    char m[160];
    if (!w || strcmp(w, "1") != 0) return 0;
    if (wide_now_id) return wide_now_id;   /* the UDP and the TCP transport agree */
    if (!sign_node_key_ensure() || !(pk = sign_node_pubkey())) return 0;
    c = wide_count();
    memcpy(in, pk, 32);
    in[32] = (unsigned char)(c & 0xFF); in[33] = (unsigned char)((c >> 8) & 0xFF);
    in[34] = (unsigned char)((c >> 16) & 0xFF); in[35] = (unsigned char)((c >> 24) & 0xFF);
    sha256(in, sizeof in, h);
    id = 257 + (int)((((unsigned)h[0] << 8) | h[1]) % 65278u);
    if (c == 0) {   /* tests only: two keys behind one id (run_n2c_wide.sh) */
        const char *t = getenv("PKERNEL_N2C_TEST_WIDE0");
        if (t && *t) { int v = atoi(t); if (v >= 257 && v <= 65534) id = v; }
    }
    wide_now_count = c; wide_now_id = id;
    snprintf(m, sizeof m, "[n2c] wide id %d (key, redraw %u)\r\n", id, c);
    say(m);
    return id;
}

/* the first 8 bytes of sha256(public key); 0 if there is no key */
int pk_node_fp(unsigned char fp[8])
{
    const unsigned char *pk;
    unsigned char h[SHA256_DIGEST_SIZE];
    if (!sign_node_key_ensure() || !(pk = sign_node_pubkey())) return 0;
    sha256(pk, 32, h);
    memcpy(fp, h, 8);
    return 1;
}

/* Another key holds this node's wide id and this key is the larger (swim.c).
 * Count + 1 is written (fsync) first, then the process starts itself again with
 * the same arguments and environment, so it comes up under the new id (the id is
 * fixed in the transport, drpc and the address at start). Android and Windows
 * cannot start themselves again: one line, the new id from the next start. */
void pk_node_wide_redraw(void)
{
    static int done;
    unsigned c;
    char m[200];
    if (done || !wide_now_id) return;
    done = 1;
    c = wide_now_count + 1;
#ifndef N2C_NC_REDRAW_VOLATILE
    {
        unsigned char b[4];
        b[0] = (unsigned char)(c & 0xFF); b[1] = (unsigned char)((c >> 8) & 0xFF);
        b[2] = (unsigned char)((c >> 16) & 0xFF); b[3] = (unsigned char)((c >> 24) & 0xFF);
        if (!pfs_dur_dir() || pfs_dur_write("node.redraw", b, 4) != 0)
            say("[n2c] node.redraw: could not be written (the new id lasts this run only)\r\n");
    }
#endif
    {
        char cs[16];
        snprintf(cs, sizeof cs, "%u", c);
        setenv("PKERNEL_N2C_REDRAW_MEM", cs, 1);   /* the next image reads this first */
    }
#if defined(_WIN32) || defined(__ANDROID__)
    snprintf(m, sizeof m, "[n2c] redraw %u: another key holds wide id %d; the new id from the next start\r\n",
             c, wide_now_id);
    say(m);
#else
    snprintf(m, sizeof m, "[n2c] redraw %u: another key holds wide id %d; starting again\r\n", c, wide_now_id);
    say(m);
    {
        static char buf[8192];
        char *av[128];
        int fd = open("/proc/self/cmdline", O_RDONLY), n = 0, k = 0, i;
        sigset_t none;
        if (fd >= 0) { n = (int)read(fd, buf, sizeof buf - 1); close(fd); }
        if (n <= 0) { say("[n2c] redraw: cannot read the command line; the new id from the next start\r\n"); return; }
        buf[n] = 0;
        for (i = 0; i < n && k < 127; ) { av[k++] = buf + i; i += (int)strlen(buf + i) + 1; }
        av[k] = 0;
        /* the new image must not inherit this one's timer signal or mask */
        signal(SIGALRM, SIG_IGN);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, 0);
        for (i = 3; i < 1024; i++) (void)fcntl(i, F_SETFD, FD_CLOEXEC);
        execv("/proc/self/exe", av);
        say("[n2c] redraw: exec failed; the new id from the next start\r\n");
    }
#endif
}

/* N2c stage 2 (research/n2c-identity.md §10 4): a refute raised the 1-byte
 * wire incarnation to `wire`; keep the smallest count >= the stored one whose
 * low byte is `wire`, so the next start (count + 1) is past the refute too.
 * Nothing is written without PKERNEL_PFS_DIR or when the count is unreadable. */
void pk_node_incarn_note(unsigned char wire)
{
#ifndef N2C_NC_INC_RESET
    unsigned char b[5];
    unsigned v, w;
    if (!pfs_dur_dir()) return;
    if (pfs_dur_read("node.incarn", b, sizeof b) != 4) return;
    v = (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
    w = (v & ~0xFFu) | wire;
    if (w < v) w += 0x100u;
    if (w == v) return;
    b[0] = (unsigned char)(w & 0xFF); b[1] = (unsigned char)((w >> 8) & 0xFF);
    b[2] = (unsigned char)((w >> 16) & 0xFF); b[3] = (unsigned char)((w >> 24) & 0xFF);
    if (pfs_dur_write("node.incarn", b, 4) != 0)
        say("[swim] node.incarn: could not be written\r\n");
#else
    (void)wire;
#endif
}
