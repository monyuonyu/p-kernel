/* ark_crypto.h: the ark entry's crypto parts (docs/architecture/30-module/ark-entry.md 3b).
 * Stage 1: ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (RFC 8439 sections 2.4, 2.5, 2.6, 2.8).
 * Stage 2: X25519 (RFC 7748, the ladder in ed25519.c). Stage 3: HMAC-SHA-512 (RFC 4231);
 * HKDF on it and the one-use key box are next.
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
/* open: 0 and pt, or -1 with every byte of pt zero (nothing of the plaintext is returned) */
int arkc_aead_open(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt);
#endif
