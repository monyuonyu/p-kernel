/* tests/host/ark_crypto_test.c: C2 and C4 of ark-entry.md 3b (stage 1, RFC 8439).
 * The vectors (ark_crypto_vectors.h) are generated from the RFC text, not typed.
 * Falsifiers (run_ark_crypto.sh): -DARKC_NC_NO_PAD reddens [arkc-aead-seal] and
 * [arkc-aead-open]; -DARKC_NC_TAG_NOCHECK reddens [arkc-open-tamper]. */
#include <stdio.h>
#include <string.h>
#include "ark_crypto.h"
#include "ark_crypto_vectors.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}
static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

int main(void)
{
    uint8_t buf[256], tag[16], otk[32], nonce[12];

    arkc_chacha20(CC_KEY, 1, CC_NONCE, CC_PT, buf, sizeof CC_PT);
    check(memcmp(buf, CC_CT, sizeof CC_CT) == 0, "[arkc-chacha20] RFC 8439 2.4.2 ciphertext, all bytes");
    arkc_chacha20(CC_KEY, 1, CC_NONCE, buf, buf, sizeof CC_CT);      /* in place, back */
    check(memcmp(buf, CC_PT, sizeof CC_PT) == 0, "[arkc-chacha20] in place, decrypts back");

    arkc_poly1305(P1305_KEY, P1305_MSG, sizeof P1305_MSG, tag);
    check(memcmp(tag, P1305_TAG, 16) == 0, "[arkc-poly1305] RFC 8439 2.5.2 tag");

    arkc_poly1305_keygen(KG_KEY, KG_NONCE, otk);
    check(memcmp(otk, KG_OUT, 32) == 0, "[arkc-keygen] RFC 8439 2.6.2 one-time key");

    memcpy(nonce, AE_FIXED, 4); memcpy(nonce + 4, AE_IV, 8);   /* 2.8.2: fixed part || IV */
    arkc_poly1305_keygen(AE_KEY, nonce, otk);
    check(memcmp(otk, AE_OTK, 32) == 0, "[arkc-keygen] RFC 8439 2.8.2 Poly1305 key");
    arkc_aead_seal(AE_KEY, nonce, AE_AAD, sizeof AE_AAD, AE_PT, sizeof AE_PT, buf, tag);
    check(memcmp(buf, AE_CT, sizeof AE_CT) == 0 && memcmp(tag, AE_TAG, 16) == 0,
          "[arkc-aead-seal] RFC 8439 2.8.2 ciphertext and tag");
    uint8_t pt[256];
    int rc = arkc_aead_open(AE_KEY, nonce, AE_AAD, sizeof AE_AAD, AE_CT, sizeof AE_CT, AE_TAG, pt);
    check(rc == 0 && memcmp(pt, AE_PT, sizeof AE_PT) == 0, "[arkc-aead-open] RFC 8439 2.8.2 opens to the plaintext");

    /* C4: one bit of the tag, the AAD or the ciphertext: no open, all of pt zero */
    int ok = 1;
    for (int which = 0; which < 3; which++) {
        uint8_t t2[16], a2[sizeof AE_AAD], c2[sizeof AE_CT];
        memcpy(t2, AE_TAG, 16); memcpy(a2, AE_AAD, sizeof a2); memcpy(c2, AE_CT, sizeof c2);
        if (which == 0) t2[15] ^= 0x80;
        if (which == 1) a2[0] ^= 0x01;
        if (which == 2) c2[57] ^= 0x10;
        memset(pt, 0xA5, sizeof pt);
        rc = arkc_aead_open(AE_KEY, nonce, a2, sizeof a2, c2, sizeof c2, t2, pt);
        ok &= rc == -1 && all_zero(pt, sizeof AE_CT);
    }
    check(ok, "[arkc-open-tamper] a bit of the tag, the AAD or the ciphertext: -1 and every byte zero");

    printf("SUMMARY: %d FAIL\n", fails);
    return fails ? 1 : 0;
}
