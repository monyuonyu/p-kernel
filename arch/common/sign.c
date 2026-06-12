/*
 *  sign.c — the node signing layer (signing.md). One Ed25519 keypair per
 *  NODE, a per-node signer-allowlist, and the four falsifiable `sign test`
 *  gates. Built on ed25519.c (RFC 8032, KAT-gated).
 *
 *  THE BOUNDARY (signing.md §0): a signature attests an ARTIFACT came from a
 *  KEY, NEVER that a human is who they claim. Keys belong to NODES. Nothing in
 *  this file signs a profile / handle / human-identity field. (Twice.)
 *
 *  PROVENANCE: the crypto is ed25519.c (verbatim TweetNaCl, see that file).
 *  This TU only orchestrates keygen, the allowlist, and the gates.
 *
 *  arch/common discipline: fixed-width types (U1/U4 from typedef.h; never the
 *  long-derived UW/W in wire images), static (not task-stack) buffers, output
 *  via sio_send_frame, no host libc.
 */

#include "sign.h"
#include "kernel.h"
#include "pfs_block.h"    /* pfs_id_compute — artifact content-id */

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* tiny output helpers (genome.c / lm_self.c pattern)                  */
/* ------------------------------------------------------------------ */
static UW sg_strlen(const char *s) { UW n = 0; while (s[n]) n++; return n; }
static void sg_puts(const char *s) { sio_send_frame((const UB *)s, (INT)sg_strlen(s)); }
static void sg_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[11] = 0;
    if (v == 0) { sg_puts("0"); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sg_puts(&buf[i]);
}

static void sg_memcpy(void *d, const void *s, UW n)
{
    U1 *dd = (U1 *)d; const U1 *ss = (const U1 *)s; UW i;
    for (i = 0; i < n; i++) dd[i] = ss[i];
}
static INT sg_eq(const U1 *a, const U1 *b, UW n)
{
    UW i, d = 0; for (i = 0; i < n; i++) d |= (U1)(a[i] ^ b[i]); return d == 0;
}

/* ------------------------------------------------------------------ */
/* the node keypair (first-boot keygen via per-arch sign_entropy)      */
/* ------------------------------------------------------------------ */
static U1  node_pk[ED25519_PUBLIC_KEY_LEN];
static U1  node_sk[ED25519_SECRET_KEY_LEN];   /* seed||pk — NEVER gossiped   */
static INT node_have_key   = 0;
static INT node_keygen_strong = 0;

INT sign_node_key_ensure(void)
{
    U1 seed[ED25519_SEED_LEN];
    int strong;
    if (node_have_key) return 1;

    strong = sign_entropy(seed, (int)sizeof seed);
    node_keygen_strong = strong;
    ed25519_keypair_from_seed(seed, node_pk, node_sk);
    /* scrub the local seed copy; node_sk already embeds it (TweetNaCl layout) */
    { UW i; for (i = 0; i < sizeof seed; i++) seed[i] = 0; }
    node_have_key = 1;

    if (!strong) {
        sg_puts("[sign] *** WEAK KEYGEN — bare-metal entropy is a follow-up; "
                "this node's key is NOT cryptographically strong ***\r\n");
    }
    return 1;
}

const U1 *sign_node_pubkey(void)
{
    if (!node_have_key) return (const U1 *)0;
    return node_pk;
}

INT sign_node_keygen_was_strong(void) { return node_keygen_strong; }

INT sign_artifact(const U1 *msg, UW msg_len, U1 sig_out[ED25519_SIGNATURE_LEN])
{
    if (!node_have_key) { if (!sign_node_key_ensure()) return 0; }
    ed25519_sign(sig_out, msg, (size_t)msg_len, node_sk);
    return 1;
}

INT sign_verify(const U1 *msg, UW msg_len,
                const U1 sig[ED25519_SIGNATURE_LEN],
                const U1 pk[ED25519_PUBLIC_KEY_LEN])
{
    return ed25519_verify(sig, msg, (size_t)msg_len, pk) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* the signer-allowlist (selfc / genome adopt)                         */
/* ------------------------------------------------------------------ */
static U1  allow_pk[SIGN_ALLOWLIST_MAX][ED25519_PUBLIC_KEY_LEN];
static INT allow_n = 0;

INT sign_allow_add(const U1 pk[ED25519_PUBLIC_KEY_LEN])
{
    INT i;
    for (i = 0; i < allow_n; i++)
        if (sg_eq(allow_pk[i], pk, ED25519_PUBLIC_KEY_LEN)) return 1;  /* already */
    if (allow_n >= SIGN_ALLOWLIST_MAX) return 0;
    sg_memcpy(allow_pk[allow_n], pk, ED25519_PUBLIC_KEY_LEN);
    allow_n++;
    return 1;
}

INT sign_allow_has(const U1 pk[ED25519_PUBLIC_KEY_LEN])
{
    INT i;
    for (i = 0; i < allow_n; i++)
        if (sg_eq(allow_pk[i], pk, ED25519_PUBLIC_KEY_LEN)) return 1;
    return 0;
}

void sign_allow_clear(void) { allow_n = 0; }

/* ================================================================== */
/* THE FALSIFIABLE SUITE — `sign test`                                 */
/* ================================================================== */

/* A tiny self-contained second keypair (the "attacker" / "successor" node)
 * derived deterministically from a fixed seed, so the gates are reproducible
 * across ABIs. (Determinism here is a TEST property, not a security one — the
 * production node key comes from sign_entropy.) */
static void derive_kp(U1 seed_byte, U1 pk[32], U1 sk[64])
{
    U1 seed[32]; INT i;
    for (i = 0; i < 32; i++) seed[i] = (U1)(seed_byte + i * 31u);
    ed25519_keypair_from_seed(seed, pk, sk);
}

/* ---- [sign-roundtrip] — primitive truth (incl. the RFC 8032 KATs) ---- */
static INT gate_roundtrip(void)
{
    INT ok = 1, sub;
    U1 pk[32], sk[64], sig[64];
    static const U1 art[40] = {
        0xde,0xad,0xbe,0xef,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35 };

    /* The non-negotiable ground truth: RFC 8032 §7.1 vectors, byte-exact. */
    sub = ed25519_self_test();
    sg_puts("  KAT (RFC8032 §7.1 + SHA-512): ");
    if (sub == 0) sg_puts("PASS\r\n");
    else { sg_puts("FAIL vec="); sg_putdec((UW)sub); sg_puts("\r\n"); ok = 0; }

    derive_kp(7, pk, sk);
    ed25519_sign(sig, art, sizeof art, sk);
    if (ed25519_verify(sig, art, sizeof art, pk) != 1) {
        sg_puts("  sign->verify: FAIL\r\n"); ok = 0;
    } else sg_puts("  sign->verify: TRUE\r\n");

    /* one-byte flip of the ARTIFACT -> reject */
    { U1 bad[40]; sg_memcpy(bad, art, sizeof art); bad[20] ^= 0x01;
      if (ed25519_verify(sig, bad, sizeof art, pk) != 0) {
          sg_puts("  flip-artifact: FAIL (accepted!)\r\n"); ok = 0;
      } else sg_puts("  flip-artifact: FALSE (rejected)\r\n"); }

    /* one-byte flip of the SIGNATURE -> reject */
    { U1 bs[64]; sg_memcpy(bs, sig, 64); bs[10] ^= 0x01;
      if (ed25519_verify(bs, art, sizeof art, pk) != 0) {
          sg_puts("  flip-signature: FAIL (accepted!)\r\n"); ok = 0;
      } else sg_puts("  flip-signature: FALSE (rejected)\r\n"); }

    sg_puts(ok ? "[sign-roundtrip] PASS\r\n" : "[sign-roundtrip] FAIL\r\n");
    return ok;
}

/* ---- a minimal signed-self-chain model (mirrors lm_self content-id walk) --
 * Each entry is {seq, prev_id, payload}; its content-id = sha256(entry bytes)
 * via pfs_id_compute; the ORIGIN node signs that content-id. The walker
 * verifies every entry's signature against the chain's TOFU-pinned origin key.
 * A from-genesis forgery by a different key fails because the forger does not
 * hold the pinned key. (Structurally identical to signing the LM_SELF_ENTRY
 * content-id as a companion object — §4.1.) */
typedef struct { U4 seq; U1 prev[PFS_ID_LEN]; U4 payload; } SCHAIN_ENTRY;

/* build a 3-entry chain signed by sk; out_ids[i] = content-id of entry i;
 * out_sigs[i] = signature over that id. Returns the origin pubkey via caller. */
static void build_chain(const U1 sk[64], U1 ids[3][PFS_ID_LEN],
                        U1 sigs[3][64])
{
    SCHAIN_ENTRY e; INT i;
    U1 prev[PFS_ID_LEN]; UW k;
    for (k = 0; k < PFS_ID_LEN; k++) prev[k] = 0;   /* genesis prev = all-zero */
    for (i = 0; i < 3; i++) {
        e.seq = (U4)(i + 1);
        sg_memcpy(e.prev, prev, PFS_ID_LEN);
        e.payload = (U4)(0x1000 + i);
        pfs_id_compute(&e, (UW)sizeof e, ids[i]);     /* THE content address */
        ed25519_sign(sigs[i], ids[i], PFS_ID_LEN, sk);/* origin signs the id  */
        sg_memcpy(prev, ids[i], PFS_ID_LEN);
    }
}

/* walk: every entry's signature must verify under the pinned origin key. */
static INT walk_verify(U1 ids[3][PFS_ID_LEN], U1 sigs[3][64],
                       const U1 pinned_pk[32])
{
    INT i;
    for (i = 0; i < 3; i++)
        if (ed25519_verify(sigs[i], ids[i], PFS_ID_LEN, pinned_pk) != 1)
            return 0;        /* fail-closed */
    return 1;
}

/* ---- [sign-selflayer] — the wave-22 disease cured ---- */
static INT gate_selflayer(void)
{
    INT ok = 1;
    U1 origin_pk[32], origin_sk[64];
    U1 forger_pk[32], forger_sk[64];
    U1 ids[3][PFS_ID_LEN], sigs[3][64];

    derive_kp(11, origin_pk, origin_sk);   /* the genuine origin node */
    derive_kp(99, forger_pk, forger_sk);   /* an UNPINNED attacker    */

    /* TOFU: the verifier pins origin_pk on the chain's first valid entry. */
    build_chain(origin_sk, ids, sigs);
    if (walk_verify(ids, sigs, origin_pk) != 1) {
        sg_puts("  genuine chain under pinned key: REJECTED (bug)\r\n"); ok = 0;
    } else sg_puts("  genuine chain under pinned key: ACCEPTED\r\n");

    /* The disease: a fresh, internally hash-consistent chain FROM GENESIS,
     * signed by the forger. Under the pre-signing (wave-22) model this was
     * ACCEPTED — the hash chain is self-consistent. Now the walker checks the
     * signature against the PINNED origin key, which the forger does not hold. */
    {
        U1 fids[3][PFS_ID_LEN], fsigs[3][64];
        build_chain(forger_sk, fids, fsigs);
        /* the forged chain DOES verify under the FORGER's own key... */
        if (walk_verify(fids, fsigs, forger_pk) != 1) {
            sg_puts("  forged chain under forger key: unexpectedly invalid\r\n"); ok = 0;
        }
        /* ...but is REJECTED against the pinned origin key (the cure). */
        if (walk_verify(fids, fsigs, origin_pk) != 0) {
            sg_puts("  forged-from-genesis under PINNED key: ACCEPTED (DISEASE!)\r\n");
            ok = 0;
        } else {
            sg_puts("  forged-from-genesis under PINNED key: REJECTED (cured)\r\n");
        }
    }

    sg_puts(ok ? "[sign-selflayer] PASS\r\n" : "[sign-selflayer] FAIL\r\n");
    return ok;
}

/* ---- [sign-unit] — selfc signed unit manifest + allowlist ---- */
/* A unit manifest = {unit content-id, version, signer pubkey}; the author node
 * signs it. germination is permitted iff the manifest verifies AND the signer
 * is in this node's allowlist (selfc adopt). (§4.2) */
static INT gate_unit(void)
{
    INT ok = 1;
    U1 author_pk[32], author_sk[64];
    U1 evil_pk[32],   evil_sk[64];
    U1 unit_id[PFS_ID_LEN], sig[64];
    static const U1 unit_src[24] = { 'i','n','t',' ','m','a','i','n','(',')','{',
        'r','e','t','u','r','n',' ','0',';','}',0,0,0 };

    derive_kp(21, author_pk, author_sk);
    derive_kp(42, evil_pk,   evil_sk);
    pfs_id_compute(unit_src, (UW)sizeof unit_src, unit_id);

    sign_allow_clear();   /* start from a clean allowlist */

    /* (1) UNSIGNED / not-adopted: must REFUSE. We model "unsigned" as: no
     * matching allowlisted signer. */
    {
        INT germinate = sign_allow_has(author_pk);   /* false: empty list */
        if (germinate) { sg_puts("  empty allowlist germinated (bug)\r\n"); ok = 0; }
        else sg_puts("  unsigned/un-adopted unit: REFUSED\r\n");
    }

    /* (2) signed by a WRONG (non-adopted) key: must REFUSE even though the
     * signature itself is valid for evil_pk. */
    ed25519_sign(sig, unit_id, PFS_ID_LEN, evil_sk);
    {
        INT sig_ok  = ed25519_verify(sig, unit_id, PFS_ID_LEN, evil_pk);
        INT adopted = sign_allow_has(evil_pk);
        INT germinate = sig_ok && adopted;
        if (germinate) { sg_puts("  wrong-key unit germinated (bug)\r\n"); ok = 0; }
        else sg_puts("  valid sig by NON-adopted key: REFUSED\r\n");
    }

    /* (3) operator adopts the author key (`selfc adopt <key>`), then the SAME
     * unit signed by the author germinates. */
    sign_allow_add(author_pk);
    ed25519_sign(sig, unit_id, PFS_ID_LEN, author_sk);
    {
        INT sig_ok  = ed25519_verify(sig, unit_id, PFS_ID_LEN, author_pk);
        INT adopted = sign_allow_has(author_pk);
        INT germinate = sig_ok && adopted;
        if (!germinate) { sg_puts("  adopted-key unit refused (bug)\r\n"); ok = 0; }
        else sg_puts("  adopted-key signed unit: GERMINATES + runs\r\n");

        /* tamper: same author sig but the unit bytes changed -> id mismatch
         * -> verify FALSE even though signer is adopted. */
        { U1 bad_id[PFS_ID_LEN]; U1 bad_src[24];
          sg_memcpy(bad_src, unit_src, sizeof bad_src); bad_src[0] ^= 0x01;
          pfs_id_compute(bad_src, (UW)sizeof bad_src, bad_id);
          if (ed25519_verify(sig, bad_id, PFS_ID_LEN, author_pk) != 0) {
              sg_puts("  poisoned unit body under adopted sig: ACCEPTED (bug)\r\n"); ok = 0;
          } else sg_puts("  poisoned unit body: REFUSED (content-id binds)\r\n"); }
    }

    sign_allow_clear();
    sg_puts(ok ? "[sign-unit] PASS\r\n" : "[sign-unit] FAIL\r\n");
    return ok;
}

/* ---- [sign-keyrotation] — succession under the successor's OWN key ---- */
/* The dead node's chain is continued by a successor. The successor appends a
 * signed ROTATION entry: body = {"rotate", K_old_pubkey, K_new_pubkey, seq},
 * signed by K_new (and, planned case, countersigned by K_old). Post-rotation
 * entries verify under K_new. The lineage stays auditable; the transition is
 * EVIDENT. (§3.3) */
typedef struct {
    U4 kind;                                /* 1 = rotation                  */
    U4 at_seq;
    U1 k_old[ED25519_PUBLIC_KEY_LEN];
    U1 k_new[ED25519_PUBLIC_KEY_LEN];
} ROT_ENTRY;

static INT gate_keyrotation(void)
{
    INT ok = 1;
    U1 k_old_pk[32], k_old_sk[64];
    U1 k_new_pk[32], k_new_sk[64];
    ROT_ENTRY r;
    U1 rot_id[PFS_ID_LEN];
    U1 sig_new[64], sig_old[64];
    U1 post_id[PFS_ID_LEN], post_sig[64];

    derive_kp(31, k_old_pk, k_old_sk);     /* the dying node's key  */
    derive_kp(57, k_new_pk, k_new_sk);     /* the successor's key   */

    /* the rotation entry */
    r.kind = 1; r.at_seq = 5;
    sg_memcpy(r.k_old, k_old_pk, ED25519_PUBLIC_KEY_LEN);
    sg_memcpy(r.k_new, k_new_pk, ED25519_PUBLIC_KEY_LEN);
    pfs_id_compute(&r, (UW)sizeof r, rot_id);

    /* PLANNED succession: the dying node pre-authored a token => countersigned
     * by BOTH keys. A verifier accepts the hand-off as continuous. */
    ed25519_sign(sig_new, rot_id, PFS_ID_LEN, k_new_sk);
    ed25519_sign(sig_old, rot_id, PFS_ID_LEN, k_old_sk);
    {
        INT by_new = ed25519_verify(sig_new, rot_id, PFS_ID_LEN, k_new_pk);
        INT by_old = ed25519_verify(sig_old, rot_id, PFS_ID_LEN, k_old_pk);
        if (by_new && by_old)
            sg_puts("  planned rotation: countersigned by K_old AND K_new (continuous)\r\n");
        else { sg_puts("  planned rotation countersign FAILED (bug)\r\n"); ok = 0; }
    }

    /* UNPLANNED death: K_old's secret is gone; the rotation is signed by K_new
     * ALONE — the discontinuity is HONEST and verifiable, not forged. The
     * K_old countersignature is (correctly) absent: an attacker cannot mint it. */
    {
        INT by_new = ed25519_verify(sig_new, rot_id, PFS_ID_LEN, k_new_pk);
        /* a forger trying to fake K_old's countersignature with K_new's key
         * must FAIL to verify under K_old. */
        INT forged_old = ed25519_verify(sig_new, rot_id, PFS_ID_LEN, k_old_pk);
        if (by_new && !forged_old)
            sg_puts("  unplanned rotation: continued-under-K_new (transition EVIDENT)\r\n");
        else { sg_puts("  unplanned rotation evidence broke (bug)\r\n"); ok = 0; }
    }

    /* post-rotation entries verify under K_new end-to-end. */
    {
        SCHAIN_ENTRY e; e.seq = 6; sg_memcpy(e.prev, rot_id, PFS_ID_LEN);
        e.payload = 0x2000;
        pfs_id_compute(&e, (UW)sizeof e, post_id);
        ed25519_sign(post_sig, post_id, PFS_ID_LEN, k_new_sk);
        if (ed25519_verify(post_sig, post_id, PFS_ID_LEN, k_new_pk) != 1) {
            sg_puts("  post-rotation entry under K_new: REJECTED (bug)\r\n"); ok = 0;
        } else sg_puts("  post-rotation chain under K_new: VERIFIES end-to-end\r\n");
        /* and an old-key signature is NOT silently accepted on new-key entries */
        if (ed25519_verify(post_sig, post_id, PFS_ID_LEN, k_old_pk) != 0) {
            sg_puts("  post-rotation accepted under STALE K_old (bug)\r\n"); ok = 0;
        }
    }

    sg_puts(ok ? "[sign-keyrotation] PASS\r\n" : "[sign-keyrotation] FAIL\r\n");
    return ok;
}

void sign_self_test(void)
{
    INT all = 1;
    sg_puts("== sign test (Ed25519 / RFC 8032) ==\r\n");

    /* make sure the node has a key (prints the WEAK warning on bare metal) */
    sign_node_key_ensure();
    sg_puts("  node keygen entropy: ");
    sg_puts(sign_node_keygen_was_strong() ? "STRONG\r\n"
                                          : "WEAK (flagged — see warning above)\r\n");

    all &= gate_roundtrip();
    all &= gate_selflayer();
    all &= gate_unit();
    all &= gate_keyrotation();

    sg_puts(all ? "== sign test: ALL PASS ==\r\n"
                : "== sign test: FAIL ==\r\n");
}
