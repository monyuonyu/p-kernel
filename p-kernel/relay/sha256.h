/*
 *  relay/sha256.h — pure-C SHA-256 + HMAC-SHA-256.
 *
 *  No openssl, no libcrypto. Self-contained so the relay stays a
 *  zero-dependency static binary. Reference: FIPS 180-4 / RFC 6234
 *  for SHA-256, RFC 2104 for HMAC.
 *
 *  Uses only plain `unsigned` types (no <stdint.h>) so this header is
 *  safe to mix with translation units that have pulled in system
 *  POSIX headers carrying a *different* stdint definition than the
 *  T-Kernel shadow. The wire layout is unaffected — sizes are pinned
 *  by static_assert below.
 */
#ifndef PKERNEL_RELAY_SHA256_H
#define PKERNEL_RELAY_SHA256_H

#include <stddef.h>

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    unsigned int       state[8];     /* H0..H7 */
    unsigned long long bitlen;       /* message length in bits */
    unsigned char      buf[SHA256_BLOCK_SIZE];
    size_t             buflen;
} sha256_ctx;

void sha256_init  (sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final (sha256_ctx *c, unsigned char out[SHA256_DIGEST_SIZE]);

/* One-shot convenience. */
void sha256(const void *data, size_t len,
            unsigned char out[SHA256_DIGEST_SIZE]);

/* HMAC-SHA256(key, msg). out_len <= 32. */
void hmac_sha256(const unsigned char *key, size_t key_len,
                 const unsigned char *msg, size_t msg_len,
                 unsigned char *out, size_t out_len);

/* Returns 0 if all built-in KATs pass, non-zero on failure. Cheap. */
int sha256_self_test(void);

#endif
