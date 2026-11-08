/* stub (tests first): every output is zero */
#include <string.h>
#include "ark_crypto.h"
void arkc_chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                   const uint8_t *in, uint8_t *out, size_t len)
{ (void)key; (void)counter; (void)nonce; (void)in; memset(out, 0, len); }
void arkc_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{ (void)key; (void)msg; (void)len; memset(tag, 0, 16); }
void arkc_poly1305_keygen(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32])
{ (void)key; (void)nonce; memset(otk, 0, 32); }
void arkc_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16])
{ (void)key; (void)nonce; (void)aad; (void)aad_len; (void)pt; memset(ct, 0, len); memset(tag, 0, 16); }
int arkc_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt)
{ (void)key; (void)nonce; (void)aad; (void)aad_len; (void)ct; (void)tag; memset(pt, 0, len); return 0; }
