/*
 *  relay/sha256.c — pure-C SHA-256 and HMAC-SHA-256.
 *
 *  Straight FIPS 180-4. Not constant-time at the block level; HMAC
 *  comparison sites in callers must use a constant-time compare.
 */

#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 2) ^ ROR(x,13) ^ ROR(x,22))
#define BSIG1(x) (ROR(x, 6) ^ ROR(x,11) ^ ROR(x,25))
#define SSIG0(x) (ROR(x, 7) ^ ROR(x,18) ^ ((x) >>  3))
#define SSIG1(x) (ROR(x,17) ^ ROR(x,19) ^ ((x) >> 10))

static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4 + 0] << 24)
             | ((uint32_t)block[i*4 + 1] << 16)
             | ((uint32_t)block[i*4 + 2] <<  8)
             | ((uint32_t)block[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitlen   = 0;
    c->buflen   = 0;
}

void sha256_update(sha256_ctx *c, const void *data_, size_t len)
{
    const uint8_t *data = (const uint8_t *)data_;
    c->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = SHA256_BLOCK_SIZE - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len  -= take;
        if (c->buflen == SHA256_BLOCK_SIZE) {
            compress(c->state, c->buf);
            c->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE])
{
    /* Append 0x80, then zeros, then 64-bit big-endian bit length. */
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        while (c->buflen < SHA256_BLOCK_SIZE) c->buf[c->buflen++] = 0;
        compress(c->state, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56) c->buf[c->buflen++] = 0;
    uint64_t bl = c->bitlen;
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (uint8_t)(bl >> (i * 8));
    compress(c->state, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4 + 0] = (uint8_t)(c->state[i] >> 24);
        out[i*4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i*4 + 2] = (uint8_t)(c->state[i] >>  8);
        out[i*4 + 3] = (uint8_t)(c->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t *out, size_t out_len)
{
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    uint8_t outer[SHA256_DIGEST_SIZE];
    sha256_ctx c;

    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k);
        memset(k + SHA256_DIGEST_SIZE, 0, SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        memcpy(k, key, key_len);
        memset(k + key_len, 0, SHA256_BLOCK_SIZE - key_len);
    }
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&c);
    sha256_update(&c, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, SHA256_BLOCK_SIZE);
    sha256_update(&c, inner, SHA256_DIGEST_SIZE);
    sha256_final(&c, outer);

    if (out_len > SHA256_DIGEST_SIZE) out_len = SHA256_DIGEST_SIZE;
    memcpy(out, outer, out_len);
}

/* --- KAT self-test ------------------------------------------------------- */

static int eq_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

int sha256_self_test(void)
{
    /* SHA-256("abc") */
    static const uint8_t want_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    uint8_t got[32];
    sha256("abc", 3, got);
    if (!eq_bytes(got, want_abc, 32)) return 1;

    /* RFC 4231 test case 1: key = 20x 0x0b, data = "Hi There" */
    uint8_t key1[20]; memset(key1, 0x0b, sizeof(key1));
    static const uint8_t want_hmac1[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7,
    };
    uint8_t got_h[32];
    hmac_sha256(key1, sizeof(key1),
                (const uint8_t *)"Hi There", 8, got_h, 32);
    if (!eq_bytes(got_h, want_hmac1, 32)) return 2;

    /* RFC 4231 test case 2: key = "Jefe", data = "what do ya want for nothing?" */
    static const uint8_t want_hmac2[32] = {
        0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,
        0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
        0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,
        0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43,
    };
    hmac_sha256((const uint8_t *)"Jefe", 4,
                (const uint8_t *)"what do ya want for nothing?", 28,
                got_h, 32);
    if (!eq_bytes(got_h, want_hmac2, 32)) return 3;

    /* RFC 4231 test case 4: key = 0x01..0x19, data = 50x 0xcd
     * (exercises >block message path) */
    uint8_t key4[25];
    for (int i = 0; i < 25; i++) key4[i] = (uint8_t)(i + 1);
    uint8_t data4[50]; memset(data4, 0xcd, sizeof(data4));
    static const uint8_t want_hmac4[32] = {
        0x82,0x55,0x8a,0x38,0x9a,0x44,0x3c,0x0e,
        0xa4,0xcc,0x81,0x98,0x99,0xf2,0x08,0x3a,
        0x85,0xf0,0xfa,0xa3,0xe5,0x78,0xf8,0x07,
        0x7a,0x2e,0x3f,0xf4,0x67,0x29,0x66,0x5b,
    };
    hmac_sha256(key4, sizeof(key4), data4, sizeof(data4), got_h, 32);
    if (!eq_bytes(got_h, want_hmac4, 32)) return 4;

    return 0;
}
