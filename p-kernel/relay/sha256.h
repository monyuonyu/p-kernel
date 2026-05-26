/*
 *  relay/sha256.h — pure-C SHA-256 + HMAC-SHA-256.
 *
 *  No openssl, no libcrypto. Self-contained so the relay stays a
 *  zero-dependency static binary. Reference: FIPS 180-4 / RFC 6234
 *  for SHA-256, RFC 2104 for HMAC.
 */
#ifndef PKERNEL_RELAY_SHA256_H
#define PKERNEL_RELAY_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[SHA256_BLOCK_SIZE];
    size_t   buflen;
} sha256_ctx;

void sha256_init  (sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final (sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* HMAC-SHA256(key, msg). out_len <= 32. */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t *out, size_t out_len);

/* Returns 0 if all built-in KATs pass, non-zero on failure. Cheap. */
int sha256_self_test(void);

#endif
