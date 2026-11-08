/* ark_crypto.h: the ark entry's crypto parts (docs/architecture/30-module/ark-entry.md 3b).
 * Stage 1: ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (RFC 8439 sections 2.4, 2.5, 2.6, 2.8).
 * Stage 2: X25519 (RFC 7748, the ladder in ed25519.c). Stage 3: HMAC-SHA-512 (RFC 4231);
 * HKDF-SHA-512 on it (RFC 5869, Wycheproof's vectors); the one-use key box is next.
 * Hosted only: no bare-metal Makefile lists ark_crypto.c.
 * Limits: no side-channel (timing, cache) testing; the tag compare is written in constant
 * time but not measured. */
#ifndef ARK_CRYPTO_H
#define ARK_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

/* RFC 8439 2.4: out = in XOR keystream(key, counter, nonce); in == out is allowed */
void arkc_chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                   const uint8_t *in, uint8_t *out, size_t len);
/* RFC 8439 2.5: a one-time authenticator (the 32-byte key is r || s) */
void arkc_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);
/* RFC 8439 2.6: the one-time Poly1305 key, the first 32 bytes of block 0 */
void arkc_poly1305_keygen(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32]);
/* RFC 8439 2.8: seal (ct may be pt) */
void arkc_aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]);
/* RFC 7748 (stage 2): out = X25519(scalar, u), the ladder of ed25519.c. -1 (out all
 * zero) when the shared value is all zero (6.1: a small-order u), else 0. */
int arkc_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);
/* the public key: X25519(scalar, 9) */
void arkc_x25519_base(uint8_t pub[32], const uint8_t scalar[32]);
/* stage 3: HMAC-SHA-512 (RFC 2104 on ed25519.c's SHA-512). 0, or -1 when out of memory */
int arkc_hmac_sha512(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[64]);
/* stage 3: HKDF-SHA-512 (RFC 5869, extract then expand). okm_len up to 255 * 64; 0, or -1
 * with every byte of okm zero (longer than that, or out of memory). An empty salt is
 * 64 zero bytes (RFC 5869 2.2; HMAC pads both to the same block). */
int arkc_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);
/* stage 4: the one-use key box (ark-entry.md 3b). A record key and a "sealed" mark: seal
 * once (nonce 0), open any number of times. A box unwrapped from storage is born sealed. */
typedef struct { uint8_t k[32]; uint8_t live, sealed; } arkc_box;
#define ARKC_WRAP_LEN 80                     /* ephemeral public 32 || wrapped key 32 || tag 16 */
/* a fresh random key from getrandom: 0, or -1 (box empty) when getrandom fails */
int arkc_box_new(arkc_box *b);
/* 0, or -1 with ct and tag zero (no live key, or already sealed) */
int arkc_box_seal(arkc_box *b, const uint8_t *aad, size_t aad_len,
                  const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]);
/* arkc_aead_open with the box's key and nonce 0 (-1 also when the box is empty) */
int arkc_box_open(const arkc_box *b, const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt);
/* wrap the box's key for to_pub with a fresh ephemeral key: 0, or -1 (out zero) */
int arkc_box_wrap(const arkc_box *b, const uint8_t to_pub[32], uint8_t out[ARKC_WRAP_LEN]);
/* the same with the ephemeral private key given (the test's fixed vector) */
int arkc_box_wrap_eph(const arkc_box *b, const uint8_t to_pub[32], const uint8_t eph[32],
                      uint8_t out[ARKC_WRAP_LEN]);
/* 0 and a live, sealed box; or -1 and an empty box (wrong key, tampered, small-order e) */
int arkc_box_unwrap(arkc_box *b, const uint8_t my_priv[32], const uint8_t in[ARKC_WRAP_LEN]);
void arkc_box_wipe(arkc_box *b);
/* open: 0 and pt, or -1 with every byte of pt zero (nothing of the plaintext is returned) */
int arkc_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt);
#endif
