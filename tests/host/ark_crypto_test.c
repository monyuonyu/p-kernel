/* tests/host/ark_crypto_test.c: C2 and C4 of ark-entry.md 3b (stage 1, RFC 8439).
 * The vectors (ark_crypto_vectors.h) are generated from the RFC text, not typed.
 * Falsifiers (run_ark_crypto.sh): -DARKC_NC_NO_PAD reddens [arkc-aead-seal],
 * [arkc-aead-open] and [arkc-aead-a5]; -DARKC_NC_TAG_NOCHECK reddens [arkc-open-tamper]. */
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
    {   /* appendix A.3 #1-#11; #5-#11 are the edges of the final reduction and of s's carry */
        int bad = 0;
        for (unsigned n = 0; n < sizeof A3 / sizeof A3[0]; n++) {
            arkc_poly1305(A3[n].key, A3[n].msg, A3[n].len, tag);
            if (memcmp(tag, A3[n].tag, 16) != 0) { printf("    A.3 #%u differs\n", n + 1); bad = 1; }
        }
        check(!bad, "[arkc-poly1305-a3] RFC 8439 appendix A.3, all 11 tags");
    }

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

    {   /* appendix A.5: decryption of a message we did not seal */
        static uint8_t p5[sizeof A5_CT];
        rc = arkc_aead_open(A5_KEY, A5_NONCE, A5_AAD, sizeof A5_AAD, A5_CT, sizeof A5_CT, A5_TAG, p5);
        check(rc == 0 && memcmp(p5, A5_PT, sizeof A5_PT) == 0, "[arkc-aead-a5] RFC 8439 appendix A.5 opens to the plaintext");
    }

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

    /* ---- stage 2: X25519 (C1 of ark-entry.md 3b, RFC 7748) ---- */
    uint8_t xo[32], k[32], u[32], t[32];
    int x1 = arkc_x25519(xo, X1_K, X1_U) == 0 && memcmp(xo, X1_OUT, 32) == 0;
    int x2 = arkc_x25519(xo, X2_K, X2_U) == 0 && memcmp(xo, X2_OUT, 32) == 0;
    check(x1 && x2, "[arkc-x25519] RFC 7748 5.2, both pairs (the second u has its top bit set)");
    memset(k, 0, 32); k[0] = 9; memcpy(u, k, 32);
    int it1 = 0;
    for (int n = 1; n <= 1000; n++) {          /* 5.2: k = X25519(k, u), u = the old k */
        arkc_x25519(t, k, u);
        memcpy(u, k, 32); memcpy(k, t, 32);
        if (n == 1) it1 = memcmp(k, XI_1, 32) == 0;
    }
    check(it1 && memcmp(k, XI_1000, 32) == 0, "[arkc-x25519-iter] RFC 7748 5.2, after 1 and 1,000 iterations");
    uint8_t pa[32], pb[32], ka[32], kb[32];
    arkc_x25519_base(pa, XA_PRIV);
    arkc_x25519_base(pb, XB_PRIV);
    int ra = arkc_x25519(ka, XA_PRIV, XB_PUB), rb = arkc_x25519(kb, XB_PRIV, XA_PUB);
    check(memcmp(pa, XA_PUB, 32) == 0 && memcmp(pb, XB_PUB, 32) == 0 && ra == 0 && rb == 0
          && memcmp(ka, XK, 32) == 0 && memcmp(kb, XK, 32) == 0,
          "[arkc-x25519-dh] RFC 7748 6.1, both public keys and the shared K on both sides");
    memset(u, 0, 32);
    memset(xo, 0xA5, 32);
    int r0 = arkc_x25519(xo, XA_PRIV, u);
    memset(u, 0, 32); u[0] = 1;                 /* u = 1: order 4, K is zero too */
    int r1 = arkc_x25519(t, XA_PRIV, u);
    check(r0 == -1 && r1 == -1 && all_zero(xo, 32), "[arkc-x25519-zero] u = 0 and u = 1 give an all-zero K: refused");

    /* ---- stage 3: HMAC-SHA-512 (C3, RFC 4231 4.2-4.8; case 5 is the first 128 bits) ---- */
    {
        const struct { const uint8_t *k, *d, *m; size_t kl, dl, ml; } hm[7] = {
            { HM_KEY_1, HM_DATA_1, HM_MAC_1, sizeof HM_KEY_1, sizeof HM_DATA_1, sizeof HM_MAC_1 },
            { HM_KEY_2, HM_DATA_2, HM_MAC_2, sizeof HM_KEY_2, sizeof HM_DATA_2, sizeof HM_MAC_2 },
            { HM_KEY_3, HM_DATA_3, HM_MAC_3, sizeof HM_KEY_3, sizeof HM_DATA_3, sizeof HM_MAC_3 },
            { HM_KEY_4, HM_DATA_4, HM_MAC_4, sizeof HM_KEY_4, sizeof HM_DATA_4, sizeof HM_MAC_4 },
            { HM_KEY_5, HM_DATA_5, HM_MAC_5, sizeof HM_KEY_5, sizeof HM_DATA_5, sizeof HM_MAC_5 },
            { HM_KEY_6, HM_DATA_6, HM_MAC_6, sizeof HM_KEY_6, sizeof HM_DATA_6, sizeof HM_MAC_6 },
            { HM_KEY_7, HM_DATA_7, HM_MAC_7, sizeof HM_KEY_7, sizeof HM_DATA_7, sizeof HM_MAC_7 },
        };
        int bad = 0;
        uint8_t mac[64];
        for (int n = 0; n < 7; n++) {
            if (arkc_hmac_sha512(hm[n].k, hm[n].kl, hm[n].d, hm[n].dl, mac) != 0
                || memcmp(mac, hm[n].m, hm[n].ml) != 0) { printf("    RFC 4231 case %d differs\n", n + 1); bad = 1; }
        }
        check(!bad, "[arkc-hmac512] RFC 4231 4.2-4.8, all 7 cases (case 5 truncated to 128 bits)");
    }

    printf("SUMMARY: %d FAIL\n", fails);
    return fails ? 1 : 0;
}
