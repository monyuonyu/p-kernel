/*
 *  sign_entropy.c — first-boot keygen entropy for the Ed25519 node key,
 *  HOSTED (arch/linux, shared by aarch64 + x86_64 Linux ports).
 *
 *  signing.md §3.1 / D3(b): the node keypair is generated once at first boot;
 *  Ed25519 signing itself is deterministic (no per-signature RNG), so the ONLY
 *  entropy the system needs is the 32-byte seed here. On a hosted node the OS
 *  RNG is the right source: /dev/urandom (getrandom-quality, non-blocking after
 *  the pool is seeded, which it always is on a booted Linux/Android userspace).
 *
 *  Returns 1 on STRONG entropy (the 32 bytes are from the OS RNG), 0 on a weak
 *  fallback. Hosted should always return 1; if /dev/urandom is somehow
 *  unavailable we fail CLOSED to a loudly-flagged weak path (caller prints the
 *  warning) rather than silently shipping a predictable key.
 *
 *  arch/linux discipline (see pfs_durable.c): hosted CFLAGS, host headers,
 *  NO kernel.h. Plain C types only.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

/* Fill out[0..n) with entropy. Returns 1 = strong (OS RNG), 0 = weak. */
int sign_entropy(unsigned char *out, int n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        int got = 0;
        while (got < n) {
            ssize_t r = read(fd, out + got, (size_t)(n - got));
            if (r <= 0) break;
            got += (int)r;
        }
        close(fd);
        if (got == n) return 1;            /* STRONG */
    }
    /* Weak fallback — should never happen on a booted hosted node, but make
     * it VISIBLE (return 0) rather than silent. Mix wall-clock + monotonic +
     * pid; clearly NOT cryptographic. */
    {
        struct timespec ts1, ts2;
        unsigned long x;
        int i;
        clock_gettime(CLOCK_REALTIME, &ts1);
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        x = (unsigned long)ts1.tv_nsec ^ ((unsigned long)ts2.tv_nsec << 13)
          ^ ((unsigned long)getpid() << 27) ^ (unsigned long)ts1.tv_sec;
        for (i = 0; i < n; i++) {
            x = x * 6364136223846793005UL + 1442695040888963407UL;
            out[i] = (unsigned char)(x >> 33);
        }
    }
    return 0;                              /* WEAK */
}
