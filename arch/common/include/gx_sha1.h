/*
 *  gx_sha1.h — pure-C SHA-1 + base64 for the galaxy WebSocket handshake.
 *
 *  RFC 6455 §1.3 only: Sec-WebSocket-Accept = base64(SHA1(key + GUID)).
 *  SHA-1 here is a PROTOCOL primitive, not a security claim (the galaxy
 *  socket is loopback-only). Single-purpose, no libc, LP64-uniform: only
 *  `unsigned` (32-bit words) and `unsigned long long` (the 64-bit bit
 *  length), so nothing widens on a 64-bit host.
 */
#ifndef GX_SHA1_H
#define GX_SHA1_H

#define GX_SHA1_BLOCK   64
#define GX_SHA1_DIGEST  20

typedef struct {
    unsigned       state[5];     /* H0..H4                                  */
    unsigned long long bitlen;   /* message length in bits                  */
    unsigned char  buf[GX_SHA1_BLOCK];
    unsigned       buflen;
} GX_SHA1_CTX;

_Static_assert(GX_SHA1_DIGEST == 20, "SHA-1 digest is 20 bytes");

void gx_sha1_init  (GX_SHA1_CTX *c);
void gx_sha1_update(GX_SHA1_CTX *c, const void *data, unsigned len);
void gx_sha1_final (GX_SHA1_CTX *c, unsigned char out[GX_SHA1_DIGEST]);

/* one-shot SHA-1. */
void gx_sha1(const void *data, unsigned len, unsigned char out[GX_SHA1_DIGEST]);

/* base64 of `n` bytes into `out` (NUL-terminated, bounded by outmax).
 * returns the encoded length (excluding the NUL). */
unsigned gx_base64(const unsigned char *in, unsigned n, char *out, unsigned outmax);

/* base64(SHA1(data, len)) — the RFC 6455 accept recipe in one call.
 * `out` must hold >= 29 bytes (28 b64 chars + NUL). */
unsigned gx_sha1_base64(const void *data, unsigned len, char *out, unsigned outmax);

/* boot-time KAT (two SHA-1 vectors + the RFC 6455 §1.3 accept example).
 * returns 0 on all-pass, nonzero on any miscompile. */
int gx_sha1_self_test(void);

#endif
