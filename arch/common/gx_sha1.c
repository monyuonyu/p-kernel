/*
 *  gx_sha1.c — pure-C SHA-1 + base64, from scratch, for the galaxy
 *  WebSocket handshake ONLY (galaxy.c §3.7 chat-ws transport).
 *
 *  WHY a NEW hash here and not relay/sha256.c: RFC 6455 §1.3 pins the
 *  opening handshake to SHA-1 of (Sec-WebSocket-Key + the magic GUID),
 *  base64-encoded into Sec-WebSocket-Accept. SHA-1 is used here for the
 *  protocol handshake — NOT for any security claim (the listen socket is
 *  already loopback-only, galaxy_posix.c §3.5). It is deliberately a
 *  separate, single-purpose TU so the auditor can see it touches nothing
 *  but the 20-byte digest and the 28-char base64 it feeds the handshake.
 *
 *  No libc, no openssl. Reference: FIPS 180-1 / RFC 3174 for SHA-1,
 *  RFC 4648 for base64. Uses only `unsigned` (32-bit) + `unsigned long
 *  long` (bit length) so the wire/byte layout is LP64-uniform and never
 *  bloats on a 64-bit host (the long/UW LP64 typedef trap). The output
 *  sizes are pinned by _Static_assert in the header.
 *
 *  Honesty: a known-answer self-test (gx_sha1_self_test) checks the two
 *  canonical SHA-1 vectors AND the exact RFC 6455 §1.3 example accept
 *  value, so a miscompiled hash is caught at boot, never silently shipped.
 */

#include "gx_sha1.h"

/* left-rotate a 32-bit word; the cast keeps it 32-bit on LP64. */
static unsigned gx_rol(unsigned x, unsigned n)
{
    return (unsigned)((x << n) | (x >> (32u - n)));
}

void gx_sha1_init(GX_SHA1_CTX *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xEFCDAB89u;
    c->state[2] = 0x98BADCFEu;
    c->state[3] = 0x10325476u;
    c->state[4] = 0xC3D2E1F0u;
    c->bitlen   = 0;
    c->buflen   = 0;
}

/* the SHA-1 compression of one 64-byte block. */
static void gx_sha1_block(GX_SHA1_CTX *c, const unsigned char *p)
{
    unsigned w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)p[i*4] << 24) | ((unsigned)p[i*4+1] << 16) |
               ((unsigned)p[i*4+2] << 8) | (unsigned)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = gx_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    unsigned a = c->state[0], b = c->state[1], d = c->state[2];
    unsigned e = c->state[3], f = c->state[4];

    for (int i = 0; i < 80; i++) {
        unsigned k, t;
        if (i < 20)      { t = (b & d) | ((~b) & e);          k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDCu; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6u; }
        unsigned tmp = (unsigned)(gx_rol(a, 5) + t + f + k + w[i]);
        f = e; e = d; d = gx_rol(b, 30); b = a; a = tmp;
    }

    c->state[0] = (unsigned)(c->state[0] + a);
    c->state[1] = (unsigned)(c->state[1] + b);
    c->state[2] = (unsigned)(c->state[2] + d);
    c->state[3] = (unsigned)(c->state[3] + e);
    c->state[4] = (unsigned)(c->state[4] + f);
}

void gx_sha1_update(GX_SHA1_CTX *c, const void *data, unsigned len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->bitlen += (unsigned long long)len * 8ull;
    while (len > 0) {
        unsigned room = GX_SHA1_BLOCK - c->buflen;
        unsigned take = (len < room) ? len : room;
        for (unsigned i = 0; i < take; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += take; p += take; len -= take;
        if (c->buflen == GX_SHA1_BLOCK) { gx_sha1_block(c, c->buf); c->buflen = 0; }
    }
}

void gx_sha1_final(GX_SHA1_CTX *c, unsigned char out[GX_SHA1_DIGEST])
{
    unsigned long long bits = c->bitlen;
    /* append 0x80 then zero-pad to 56 mod 64, then the 64-bit big-endian
     * length. */
    unsigned char one = 0x80;
    gx_sha1_update(c, &one, 1);
    /* gx_sha1_update bumped bitlen; undo that 8-bit bump for the padding
     * byte (the length we append must be the ORIGINAL message length). */
    c->bitlen = bits;
    unsigned char zero = 0;
    while (c->buflen != 56) { gx_sha1_update(c, &zero, 1); c->bitlen = bits; }

    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i*8));
    gx_sha1_update(c, lenb, 8);   /* this completes the final block        */

    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(c->state[i] >> 24);
        out[i*4+1] = (unsigned char)(c->state[i] >> 16);
        out[i*4+2] = (unsigned char)(c->state[i] >> 8);
        out[i*4+3] = (unsigned char)(c->state[i]);
    }
}

void gx_sha1(const void *data, unsigned len, unsigned char out[GX_SHA1_DIGEST])
{
    GX_SHA1_CTX c;
    gx_sha1_init(&c);
    gx_sha1_update(&c, data, len);
    gx_sha1_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* base64 (RFC 4648, standard alphabet, '=' pad). Encodes the 20-byte  */
/* SHA-1 digest into 28 chars (+ NUL). Bounded, no allocation.          */
/* ------------------------------------------------------------------ */

static const char gx_b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned gx_base64(const unsigned char *in, unsigned n, char *out, unsigned outmax)
{
    unsigned o = 0;
    unsigned i = 0;
    while (i < n) {
        unsigned b0 = in[i++];
        unsigned b1 = (i < n) ? in[i++] : 0;
        unsigned b2 = (i < n) ? in[i++] : 0;
        if (o + 4 >= outmax) break;                 /* leave room for NUL    */
        out[o++] = gx_b64tab[b0 >> 2];
        out[o++] = gx_b64tab[((b0 & 0x03) << 4) | (b1 >> 4)];
        out[o++] = gx_b64tab[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out[o++] = gx_b64tab[b2 & 0x3F];
    }
    /* pad: rewrite the trailing chars per the remainder. */
    unsigned rem = n % 3;
    if (rem == 1 && o >= 2) { out[o-1] = '='; out[o-2] = '='; }
    else if (rem == 2 && o >= 1) { out[o-1] = '='; }
    out[o] = 0;
    return o;
}

/* base64 of SHA-1(data) — the exact RFC 6455 Sec-WebSocket-Accept recipe
 * when `data` is (Sec-WebSocket-Key + GUID). out must hold >=29 bytes. */
unsigned gx_sha1_base64(const void *data, unsigned len, char *out, unsigned outmax)
{
    unsigned char dig[GX_SHA1_DIGEST];
    gx_sha1(data, len, dig);
    return gx_base64(dig, GX_SHA1_DIGEST, out, outmax);
}

/* ------------------------------------------------------------------ */
/* self-test (boot-time honesty): the two canonical SHA-1 vectors AND   */
/* the exact RFC 6455 §1.3 worked example. Returns 0 on all-pass.       */
/* ------------------------------------------------------------------ */

static int gx_dig_eq(const unsigned char *a, const unsigned char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int gx_streqz(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

int gx_sha1_self_test(void)
{
    unsigned char d[GX_SHA1_DIGEST];

    /* FIPS 180-1: SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d */
    static const unsigned char abc[] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
    gx_sha1("abc", 3, d);
    if (!gx_dig_eq(d, abc, GX_SHA1_DIGEST)) return 1;

    /* SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709 (empty msg) */
    static const unsigned char empty[] = {
        0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
        0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09 };
    gx_sha1("", 0, d);
    if (!gx_dig_eq(d, empty, GX_SHA1_DIGEST)) return 2;

    /* RFC 6455 §1.3: key "dGhlIHNhbXBsZSBub25jZQ==" + GUID ->
     * accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=". This certifies the whole
     * handshake recipe (SHA-1 + base64) end-to-end, the bytes the browser
     * implementer's client checks. */
    char acc[40];
    static const char sample[] =
        "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    gx_sha1_base64(sample, (unsigned)(sizeof(sample) - 1), acc, sizeof acc);
    if (!gx_streqz(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) return 3;

    return 0;
}
