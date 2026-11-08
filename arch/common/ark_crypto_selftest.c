/*
 *  arch/common/ark_crypto_selftest.c — `ark crypto`: the ark crypto parts' known answers,
 *  run inside the kernel (hosted only: ark_crypto.c needs getrandom; ark-entry.md 3b).
 *
 *  The values are the generated headers of tests/host (RFC 8439, RFC 7748, the box's K4
 *  from python's cryptography), not copied by hand. The full set (all of RFC 8439's
 *  appendix, the 83 Wycheproof HKDF tests, ASan and UBSan) stays in
 *  tests/host/run_ark_crypto.sh; this is the kernel's own copy of the parts, checked
 *  where it runs. Lines:
 *    [ark-crypto] aead    RFC 8439 2.8.2 seal and open
 *    [ark-crypto] tamper  one bit of the tag: open refuses, output all zero
 *    [ark-crypto] x25519  RFC 7748 5.2, both pairs
 *    [ark-crypto] wrap    the box wrapped with a fixed ephemeral key equals K4; unwrap opens it
 *                         and the box it gives back refuses a seal (born sealed)
 *    [ark-crypto] box     a new box (getrandom) seals once, refuses the second seal, opens
 *  then "[ark-crypto] SELF PASS 5/5" or "[ark-crypto] SELF FAIL k/5".
 *  Negative controls: -DARKC_NC_NO_CLAMP reddens x25519 (and wrap), -DARKC_NC_TAG_NOCHECK tamper.
 */
#include <string.h>
#include "ark_crypto.h"
#include "../../tests/host/ark_crypto_vectors.h"
#include "../../tests/host/ark_box_vectors.h"

static int zero(const uint8_t *p, size_t n)
{
    uint8_t a = 0;
    for (size_t i = 0; i < n; i++) a |= p[i];
    return a == 0;
}

void ark_crypto_self_test(void (*pr)(const char *))
{
    static uint8_t buf[sizeof AE_CT], pt[sizeof AE_PT];
    uint8_t tag[16], nonce[12], xo[32], w[ARKC_WRAP_LEN];
    int pass = 0, n = 0, ok;

#define LINE(name, cond, what) do { ok = (cond); n++; pass += ok; \
        pr("[ark-crypto] " name); pr(ok ? "  PASS  " : "  FAIL  "); pr(what); pr("\r\n"); } while (0)

    memcpy(nonce, AE_FIXED, 4); memcpy(nonce + 4, AE_IV, 8);
    arkc_aead_seal(AE_KEY, nonce, AE_AAD, sizeof AE_AAD, AE_PT, sizeof AE_PT, buf, tag);
    ok = memcmp(buf, AE_CT, sizeof AE_CT) == 0 && memcmp(tag, AE_TAG, 16) == 0;
    ok = ok && arkc_aead_open(AE_KEY, nonce, AE_AAD, sizeof AE_AAD, AE_CT, sizeof AE_CT, AE_TAG, pt) == 0
            && memcmp(pt, AE_PT, sizeof AE_PT) == 0;
    LINE("aead   ", ok, "RFC 8439 2.8.2 seal and open");

    memcpy(tag, AE_TAG, 16); tag[15] ^= 0x01;
    memset(pt, 0xA5, sizeof pt);
    ok = arkc_aead_open(AE_KEY, nonce, AE_AAD, sizeof AE_AAD, AE_CT, sizeof AE_CT, tag, pt) == -1
         && zero(pt, sizeof AE_CT);
    LINE("tamper ", ok, "one bit of the tag: open refuses, output all zero");

    ok = arkc_x25519(xo, X1_K, X1_U) == 0 && memcmp(xo, X1_OUT, 32) == 0;
    ok = ok && arkc_x25519(xo, X2_K, X2_U) == 0 && memcmp(xo, X2_OUT, 32) == 0;
    LINE("x25519 ", ok, "RFC 7748 5.2, both pairs");

    {
        arkc_box b, u;
        memset(&b, 0, sizeof b); memset(&u, 0, sizeof u);
        memcpy(b.k, BOX_RK, 32); b.live = 1; b.sealed = 0;
        ok = arkc_box_wrap_eph(&b, BOX_RPUB, BOX_E, w) == 0 && memcmp(w, BOX_WRAPPED, ARKC_WRAP_LEN) == 0;
        ok = ok && arkc_box_unwrap(&u, BOX_R, BOX_WRAPPED) == 0 && u.live && memcmp(u.k, BOX_RK, 32) == 0;
        {   /* the unwrapped box is born sealed: a seal is refused (audit-164: green without this) */
            static const uint8_t m1[1] = { 'x' };
            uint8_t c1[1], t1[16];
            ok = ok && arkc_box_seal(&u, NULL, 0, m1, 1, c1, t1) == -1;
        }
        arkc_box_wipe(&b); arkc_box_wipe(&u);
        LINE("wrap   ", ok, "the box wrapped with a fixed ephemeral key equals K4; unwrap opens it, refuses a seal");
    }
    {
        static const uint8_t msg[5] = { 'h', 'e', 'l', 'l', 'o' };
        uint8_t ct[5], t[16], back[5], ct2[5], t2[16];
        arkc_box b;
        memset(&b, 0, sizeof b);
        ok = arkc_box_new(&b) == 0 && b.live && !zero(b.k, 32);
        ok = ok && arkc_box_seal(&b, NULL, 0, msg, sizeof msg, ct, t) == 0;
        ok = ok && arkc_box_seal(&b, NULL, 0, msg, sizeof msg, ct2, t2) == -1 && zero(ct2, sizeof ct2);
        ok = ok && arkc_box_open(&b, NULL, 0, ct, sizeof ct, t, back) == 0 && memcmp(back, msg, sizeof msg) == 0;
        arkc_box_wipe(&b);
        LINE("box    ", ok, "a new box (getrandom) seals once, refuses the second seal, opens");
    }
#undef LINE
    {
        char s[48];
        static const char d[] = "0123456789";
        int i = 0;
        const char *h = pass == n ? "[ark-crypto] SELF PASS " : "[ark-crypto] SELF FAIL ";
        while (*h) s[i++] = *h++;
        s[i++] = d[pass % 10]; s[i++] = '/'; s[i++] = d[n % 10];
        s[i++] = '\r'; s[i++] = '\n'; s[i] = 0;
        pr(s);
    }
}
