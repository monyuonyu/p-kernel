/*
 * arch/linux/node_id.c — stable, distinct per-install default node id.
 *
 *  THE PROBLEM (N-0, BACKLOG / docs/architecture/20-architecture/p2p-overlay.md):
 *  two fresh nodes that both default to node_id = 1 filter each other
 *  out of SWIM as a self-echo (src == my_id is ignored), so they never
 *  mesh. The fix: give every install a DISTINCT, STABLE id in the
 *  usable range [1 .. DNODE_MAX-1] (0 is the sentinel/reserved slot).
 *
 *  RESOLUTION ORDER (pkernel_default_node_id):
 *    1. $PKERNEL_NODE_ID, if set and in [1..255]  -> honoured verbatim.
 *       (tests, the relay round-trip harness, and humans who want a
 *        chosen id all rely on this override; never break it.)
 *    2. else a per-install secret SEED, hashed into [1..DNODE_MAX-1].
 *       The seed is 16 random bytes generated ONCE and PERSISTED so the
 *       same install keeps the same id across reboots and across
 *       reinstall-preserving-data:
 *         - file "$PKERNEL_PFS_DIR/node_seed"     (Android: <files>/ark)
 *         - or  "$PKERNEL_NODE_SEED" as 32 hex chars (an explicit secret)
 *    3. else (no persistent home at all: bare `./p-kernel` with no env)
 *       fall back to 1, exactly as before — a single anonymous node has
 *       no peer to collide with, and the override is the documented way
 *       to run a second one.
 *
 *  HONEST BOUND: with DNODE_MAX = 64 there are only 63 usable ids, so a
 *  hash collision is possible for a large fleet (~birthday: 2 installs
 *  ~1.6%, 3 ~4.7%). Acceptable for now — the real fix is the bigger id
 *  space / Ed25519 identity (a named future wave). The point of N-0 is
 *  that 2-3 phones reliably get DISTINCT ids; a derived id from a random
 *  per-install secret achieves that far better than the old constant 1.
 *
 *  POSIX-only TU (like net_unix.c / console_ring.c): no T-Kernel headers.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "sha256.h"   /* from relay/, on the include path (see Makefile). */

#define DNODE_MAX  64              /* keep in sync with arch/common/include/drpc.h */
#define SEED_LEN   16

/* map a 32-byte digest to a node id in [1 .. DNODE_MAX-1] (never 0). */
static int digest_to_id(const unsigned char digest[SHA256_DIGEST_SIZE])
{
    /* take 4 bytes, fold into [0 .. DNODE_MAX-2], then +1 -> [1..DNODE_MAX-1]. */
    unsigned long v = ((unsigned long)digest[0] << 24) |
                      ((unsigned long)digest[1] << 16) |
                      ((unsigned long)digest[2] <<  8) |
                      ((unsigned long)digest[3]);
    return (int)(v % (unsigned long)(DNODE_MAX - 1)) + 1;   /* 1..63 */
}

/* hex-decode exactly SEED_LEN bytes from `hex` (>= 2*SEED_LEN chars).
 * returns 0 on success, -1 on a malformed char / short string. */
static int seed_from_hex(const char *hex, unsigned char out[SEED_LEN])
{
    for (int i = 0; i < SEED_LEN; i++) {
        int hi = -1, lo = -1;
        char c;
        c = hex[2*i];     if (!c) return -1;
        if      (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;
        c = hex[2*i + 1]; if (!c) return -1;
        if      (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

/* fill `out` with SEED_LEN cryptographically-random bytes. */
static int gen_random_seed(unsigned char out[SEED_LEN])
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        int got = 0;
        while (got < SEED_LEN) {
            ssize_t r = read(fd, out + got, (size_t)(SEED_LEN - got));
            if (r <= 0) break;
            got += (int)r;
        }
        close(fd);
        if (got == SEED_LEN) return 0;
    }
    return -1;
}

/* Read the seed file at <pfs_dir>/node_seed (32 hex chars).
 * If it doesn't exist, generate a fresh random seed and persist it there
 * so the id is stable across reboots / reinstall-preserving-data.
 * returns 0 and fills `out` on success; -1 if no pfs_dir / unwritable. */
static int seed_from_pfs_dir(const char *pfs_dir, unsigned char out[SEED_LEN])
{
    if (!pfs_dir || !*pfs_dir) return -1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/node_seed", pfs_dir);

    /* try to read an existing seed first. */
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char hex[2*SEED_LEN + 1] = {0};
        ssize_t n = read(fd, hex, 2*SEED_LEN);
        close(fd);
        if (n == 2*SEED_LEN && seed_from_hex(hex, out) == 0)
            return 0;
        /* a short/garbled file: fall through and re-mint. */
    }

    /* mint a new seed and persist it (best-effort; mkdir is the caller's
     * job — Android's nativeSetDataDir + pfs already create the ark dir). */
    if (gen_random_seed(out) != 0) return -1;

    char hex[2*SEED_LEN + 1];
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < SEED_LEN; i++) {
        hex[2*i]     = H[(out[i] >> 4) & 0xF];
        hex[2*i + 1] = H[out[i] & 0xF];
    }
    hex[2*SEED_LEN] = '\0';

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ssize_t w = write(fd, hex, 2*SEED_LEN);
        (void)w;
        close(fd);
    }
    /* even if the write failed we still return the freshly-minted seed:
     * the id is stable for THIS process at least, and the next boot will
     * re-mint (rare — the ark dir is normally writable). */
    return 0;
}

/*
 * pkernel_default_node_id — the single source of truth for "what id am I
 * when PKERNEL_NODE_ID is not set". All three hosted transports
 * (net_unix / net_lan / net_relay) and galaxy.c call this so they agree.
 *
 * Always returns a value in [1 .. DNODE_MAX-1]; never 0.
 */
int pkernel_default_node_id(void)
{
    /* 1. explicit override wins (tests + chosen ids). */
    const char *env = getenv("PKERNEL_NODE_ID");
    if (env && *env) {
        int v = atoi(env);
        if (v >= 1 && v <= 255) return v;   /* range-checked again by caller */
    }

    unsigned char seed[SEED_LEN];
    int have_seed = -1;

    /* 2a. an explicit hex secret, if provided. */
    const char *hexseed = getenv("PKERNEL_NODE_SEED");
    if (hexseed && *hexseed)
        have_seed = seed_from_hex(hexseed, seed);

    /* 2b. else the per-install seed file under the ark / pfs dir. */
    if (have_seed != 0)
        have_seed = seed_from_pfs_dir(getenv("PKERNEL_PFS_DIR"), seed);

    if (have_seed == 0) {
        unsigned char digest[SHA256_DIGEST_SIZE];
        sha256(seed, SEED_LEN, digest);
        return digest_to_id(digest);
    }

    /* 3. no persistent identity at all: the historical default. */
    return 1;
}
