/* ark_crypto.c: the ark entry's crypto parts, stage 1 (ark-entry.md 3b, RFC 8439).
 * ChaCha20 (2.3, 2.4), Poly1305 (2.5, 26-bit limbs in 64-bit products), the one-time
 * key (2.6) and AEAD_CHACHA20_POLY1305 (2.8). Written from the RFC's pseudocode; the
 * test is tests/host/ark_crypto_test.c (vectors generated from the RFC text).
 * Falsifiers: -DARKC_NC_NO_PAD (the AEAD's pad16 left out), -DARKC_NC_TAG_NOCHECK
 * (open does not compare the tag). Hosted only. */
#include <stdlib.h>
#include <string.h>
#include "ark_crypto.h"
#include "ed25519.h"

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)v); st32(p + 4, (uint32_t)(v >> 32));
}
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* ---- ChaCha20 (2.1 - 2.4) ---- */
#define ROTL(v, n) ((uint32_t)((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7); } while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32(nonce + 4 * i);
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {           /* 20 rounds: column then diagonal */
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
    wipe(x, sizeof x); wipe(s, sizeof s);
}

void arkc_chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                   const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ks[64];
    for (size_t off = 0; off < len; off += 64, counter++) {
        chacha20_block(key, counter, nonce, ks);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
    wipe(ks, sizeof ks);
}

/* ---- Poly1305 (2.5): h = (h + block) * r mod 2^130 - 5, then + s ---- */
typedef struct {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    size_t n;
} p1305;

static void p1305_init(p1305 *st, const uint8_t key[32])
{
    /* r clamped (2.5: r &= 0x0ffffffc0ffffffc0ffffffc0fffffff), in 26-bit limbs */
    st->r[0] = ld32(key + 0) & 0x3ffffff;
    st->r[1] = (ld32(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (ld32(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (ld32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = ld32(key + 16 + 4 * i);
    st->n = 0;
}

static void p1305_block(p1305 *st, const uint8_t m[16], uint32_t hibit)
{
    const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    h0 += ld32(m + 0) & 0x3ffffff;
    h1 += (ld32(m + 3) >> 2) & 0x3ffffff;
    h2 += (ld32(m + 6) >> 4) & 0x3ffffff;
    h3 += (ld32(m + 9) >> 6) & 0x3ffffff;
    h4 += (ld32(m + 12) >> 8) | hibit;
    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
    c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
    c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
    c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
    c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void p1305_update(p1305 *st, const uint8_t *m, size_t len)
{
    while (len) {
        size_t k = 16 - st->n < len ? 16 - st->n : len;
        memcpy(st->buf + st->n, m, k);
        st->n += k; m += k; len -= k;
        if (st->n == 16) { p1305_block(st, st->buf, 1u << 24); st->n = 0; }
    }
}

static void p1305_finish(p1305 *st, uint8_t tag[16])
{
    if (st->n) {                             /* the last partial block: a 1 byte, then zeros */
        st->buf[st->n] = 1;
        for (size_t i = st->n + 1; i < 16; i++) st->buf[i] = 0;
        p1305_block(st, st->buf, 0);
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* g = h + 5 - 2^130: if it does not go negative, h >= p and g is h mod p */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;          /* all ones when g4 did not borrow */
    h0 = (h0 & ~mask) | (g0 & mask); h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask); h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);
    /* to 4 words, then + s (mod 2^128) */
    uint32_t w0 = h0 | h1 << 26, w1 = h1 >> 6 | h2 << 20, w2 = h2 >> 12 | h3 << 14, w3 = h3 >> 18 | h4 << 8;
    uint64_t f;
    f = (uint64_t)w0 + st->pad[0];             st32(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + st->pad[1] + (f >> 32); st32(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + st->pad[2] + (f >> 32); st32(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + st->pad[3] + (f >> 32); st32(tag + 12, (uint32_t)f);
    wipe(st, sizeof *st);
}

void arkc_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{
    p1305 st;
    p1305_init(&st, key);
    p1305_update(&st, msg, len);
    p1305_finish(&st, tag);
}

void arkc_poly1305_keygen(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32])
{
    uint8_t b[64];
    chacha20_block(key, 0, nonce, b);
    memcpy(otk, b, 32);
    wipe(b, sizeof b);
}

/* ---- AEAD (2.8): tag over aad || pad16 || ct || pad16 || le64(aad) || le64(ct) ---- */
static void aead_tag(const uint8_t otk[32], const uint8_t *aad, size_t alen,
                     const uint8_t *ct, size_t len, uint8_t tag[16])
{
    static const uint8_t zero[16];
    uint8_t lens[16];
    p1305 st;
    p1305_init(&st, otk);
    p1305_update(&st, aad, alen);
#ifdef ARKC_NC_NO_PAD
    (void)zero;
#else
    if (alen % 16) p1305_update(&st, zero, 16 - alen % 16);
#endif
    p1305_update(&st, ct, len);
#ifndef ARKC_NC_NO_PAD
    if (len % 16) p1305_update(&st, zero, 16 - len % 16);
#endif
    st64(lens, (uint64_t)alen); st64(lens + 8, (uint64_t)len);
    p1305_update(&st, lens, 16);
    p1305_finish(&st, tag);
}

void arkc_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16])
{
    uint8_t otk[32];
    arkc_poly1305_keygen(key, nonce, otk);
    arkc_chacha20(key, 1, nonce, pt, ct, len);
    aead_tag(otk, aad, aad_len, ct, len, tag);
    wipe(otk, sizeof otk);
}

int arkc_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt)
{
    uint8_t otk[32], t[16];
    arkc_poly1305_keygen(key, nonce, otk);
    aead_tag(otk, aad, aad_len, ct, len, t);
    wipe(otk, sizeof otk);
    uint8_t diff = 0;                        /* every byte, whatever differs first */
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(t[i] ^ tag[i]);
    wipe(t, sizeof t);
#ifdef ARKC_NC_TAG_NOCHECK
    diff = 0;
#endif
    if (diff) {
        memset(pt, 0, len);
        return -1;
    }
    arkc_chacha20(key, 1, nonce, ct, pt, len);
    return 0;
}

/* ---- HMAC-SHA-512 (stage 3, RFC 2104 with SHA-512, checked on RFC 4231): the hash is
 * ed25519.c's one-shot SHA-512, so the inner message is copied after the padded key ---- */
int arkc_hmac_sha512(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[64])
{
    uint8_t k0[128], ih[64];
    memset(k0, 0, sizeof k0);
    if (klen > 128) ed25519_sha512(key, klen, k0);   /* a long key is hashed first */
    else if (klen) memcpy(k0, key, klen);
    size_t cap = 128 + (mlen > 64 ? mlen : 64);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { wipe(k0, sizeof k0); return -1; }
    for (int i = 0; i < 128; i++) buf[i] = k0[i] ^ 0x36;
    if (mlen) memcpy(buf + 128, msg, mlen);
    ed25519_sha512(buf, 128 + mlen, ih);
    for (int i = 0; i < 128; i++) buf[i] = k0[i] ^ 0x5c;
    memcpy(buf + 128, ih, 64);
    ed25519_sha512(buf, 128 + 64, out);
    wipe(buf, cap); free(buf);
    wipe(k0, sizeof k0); wipe(ih, sizeof ih);
    return 0;
}

/* ---- HKDF-SHA-512 (stage 3, RFC 5869): PRK = HMAC(salt, IKM) (2.2), then
 * T(i) = HMAC(PRK, T(i-1) || info || i) for i = 1..N, OKM = the first L bytes (2.3).
 * Falsifiers: -DARKC_NC_HKDF_NO_CHAIN (T(i-1) left out: wrong from the second block on),
 * -DARKC_NC_HKDF_NO_LIMIT (no 255 * 64 check: the counter byte wraps) ---- */
int arkc_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len)
{
    static const uint8_t zeros[64];
    uint8_t prk[64], t[64];
#ifndef ARKC_NC_HKDF_NO_LIMIT
    if (okm_len > 255u * 64u) { memset(okm, 0, okm_len); return -1; }
#endif
    if (!salt_len) { salt = zeros; salt_len = sizeof zeros; }   /* 2.2: HashLen zeros */
    if (arkc_hmac_sha512(salt, salt_len, ikm, ikm_len, prk) != 0) { memset(okm, 0, okm_len); return -1; }
    uint8_t *buf = (uint8_t *)malloc(64 + info_len + 1);
    if (!buf) { wipe(prk, sizeof prk); memset(okm, 0, okm_len); return -1; }
    size_t tl = 0, off = 0;                  /* T(0) is empty */
    for (unsigned i = 1; off < okm_len; i++) {
        size_t n = 0;
#ifndef ARKC_NC_HKDF_NO_CHAIN
        memcpy(buf, t, tl); n = tl;
#else
        (void)tl;
#endif
        if (info_len) memcpy(buf + n, info, info_len);
        n += info_len;
        buf[n++] = (uint8_t)i;
        if (arkc_hmac_sha512(prk, sizeof prk, buf, n, t) != 0) {
            wipe(buf, 64 + info_len + 1); free(buf); wipe(prk, sizeof prk); wipe(t, sizeof t);
            memset(okm, 0, okm_len); return -1;
        }
        tl = 64;
        size_t k = okm_len - off < 64 ? okm_len - off : 64;
        memcpy(okm + off, t, k);
        off += k;
    }
    wipe(buf, 64 + info_len + 1); free(buf);
    wipe(prk, sizeof prk); wipe(t, sizeof t);
    return 0;
}

/* ---- X25519 (stage 2, RFC 7748): the ladder is ed25519.c's ---- */
int arkc_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32])
{
    ed25519_x25519(out, scalar, u);
    uint8_t any = 0;                         /* 6.1: an all-zero K is refused, every byte read */
    for (int i = 0; i < 32; i++) any |= out[i];
    return any ? 0 : -1;
}

void arkc_x25519_base(uint8_t pub[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = {9};
    ed25519_x25519(pub, scalar, nine);
}
