/*
 *  relay/sha256.c — pure-C SHA-256 and HMAC-SHA-256.
 *
 *  Straight FIPS 180-4. Not constant-time at the block level; HMAC
 *  comparison sites in callers must use a constant-time compare.
 *
 *  Uses only plain `unsigned int` / `unsigned long long` / `unsigned
 *  char` — see sha256.h for why this avoids a stdint.h shadow clash
 *  when linked alongside POSIX-using translation units.
 */

#include "sha256.h"
#include <string.h>

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned char      u8;

static const u32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 2) ^ ROR(x,13) ^ ROR(x,22))
#define BSIG1(x) (ROR(x, 6) ^ ROR(x,11) ^ ROR(x,25))
#define SSIG0(x) (ROR(x, 7) ^ ROR(x,18) ^ ((x) >>  3))
#define SSIG1(x) (ROR(x,17) ^ ROR(x,19) ^ ((x) >> 10))

static void compress(u32 state[8], const u8 block[64])
{
    u32 w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((u32)block[i*4 + 0] << 24)
             | ((u32)block[i*4 + 1] << 16)
             | ((u32)block[i*4 + 2] <<  8)
             | ((u32)block[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    u32 e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        u32 t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen   = 0;
    c->buflen   = 0;
}

void sha256_update(sha256_ctx *c, const void *data_, size_t len)
{
    const u8 *data = (const u8 *)data_;
    c->bitlen += (u64)len * 8u;
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

void sha256_final(sha256_ctx *c, u8 out[SHA256_DIGEST_SIZE])
{
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        while (c->buflen < SHA256_BLOCK_SIZE) c->buf[c->buflen++] = 0;
        compress(c->state, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56) c->buf[c->buflen++] = 0;
    u64 bl = c->bitlen;
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (u8)(bl >> (i * 8));
    compress(c->state, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4 + 0] = (u8)(c->state[i] >> 24);
        out[i*4 + 1] = (u8)(c->state[i] >> 16);
        out[i*4 + 2] = (u8)(c->state[i] >>  8);
        out[i*4 + 3] = (u8)(c->state[i]);
    }
}

void sha256(const void *data, size_t len, u8 out[SHA256_DIGEST_SIZE])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256(const u8 *key, size_t key_len,
                 const u8 *msg, size_t msg_len,
                 u8 *out, size_t out_len)
{
    u8 k[SHA256_BLOCK_SIZE];
    u8 ipad[SHA256_BLOCK_SIZE];
    u8 opad[SHA256_BLOCK_SIZE];
    u8 inner[SHA256_DIGEST_SIZE];
    u8 outer[SHA256_DIGEST_SIZE];
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

static int eq_bytes(const u8 *a, const u8 *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

int sha256_self_test(void)
{
    static const u8 want_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    u8 got[32];
    sha256("abc", 3, got);
    if (!eq_bytes(got, want_abc, 32)) return 1;

    u8 key1[20]; memset(key1, 0x0b, sizeof(key1));
    static const u8 want_hmac1[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7,
    };
    u8 got_h[32];
    hmac_sha256(key1, sizeof(key1),
                (const u8 *)"Hi There", 8, got_h, 32);
    if (!eq_bytes(got_h, want_hmac1, 32)) return 2;

    static const u8 want_hmac2[32] = {
        0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,
        0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
        0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,
        0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43,
    };
    hmac_sha256((const u8 *)"Jefe", 4,
                (const u8 *)"what do ya want for nothing?", 28,
                got_h, 32);
    if (!eq_bytes(got_h, want_hmac2, 32)) return 3;

    u8 key4[25];
    for (int i = 0; i < 25; i++) key4[i] = (u8)(i + 1);
    u8 data4[50]; memset(data4, 0xcd, sizeof(data4));
    static const u8 want_hmac4[32] = {
        0x82,0x55,0x8a,0x38,0x9a,0x44,0x3c,0x0e,
        0xa4,0xcc,0x81,0x98,0x99,0xf2,0x08,0x3a,
        0x85,0xf0,0xfa,0xa3,0xe5,0x78,0xf8,0x07,
        0x7a,0x2e,0x3f,0xf4,0x67,0x29,0x66,0x5b,
    };
    hmac_sha256(key4, sizeof(key4), data4, sizeof(data4), got_h, 32);
    if (!eq_bytes(got_h, want_hmac4, 32)) return 4;

    return 0;
}
