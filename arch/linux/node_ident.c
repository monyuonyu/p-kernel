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
