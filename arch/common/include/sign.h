/*
 *  sign.h — the node signing layer: ONE Ed25519 keypair per NODE, the
 *  signer-allowlist, and the falsifiable `sign test` suite. Built on the
 *  ed25519.h primitive (RFC 8032, KAT-gated).
 *
 *  THE BOUNDARY (signing.md §0; owner directive
 *  docs/claude-memory/feedback_ark_no_identity_verification.md): a signature
 *  attests "this ARTIFACT (code unit / weight blob / Self-chain entry) was
 *  produced by the holder of this KEY." It NEVER verifies a human. The key
 *  belongs to the NODE, not a person. No profile/handle/identity is signed
 *  anywhere in this layer. Said twice, on purpose.
 *
 *  Trust model (signing.md §3): TOFU (Self layer pins a chain's origin key on
 *  the first valid entry) + an explicit per-node signer-allowlist (selfc /
 *  genome adopt keys). NOT a PKI / CA / web-of-trust.
 *
 *  arch/common discipline: fixed-width types, static buffers, no host libc,
 *  output via sio_send_frame. The per-arch entropy source is provided by
 *  sign_entropy() (arch/linux: /dev/urandom; bare-metal: RDRAND or a loudly-
 *  flagged weak fallback — a weak keygen RNG silently weakens every signature,
 *  so it is made VISIBLE, never silent).
 */
#pragma once
#include "kernel.h"
#include "ed25519.h"

/* The node keypair, generated once at first boot from sign_entropy(). The
 * secret seed NEVER gossips / never enters a kdds or pfs-replicated path; only
 * the 32-byte public key is a publishable content-addressed identity. */
INT  sign_node_key_ensure(void);                 /* idempotent; 1=ok 0=fail   */
const U1 *sign_node_pubkey(void);                /* 32 bytes, or NULL          */
INT  sign_node_keygen_was_strong(void);          /* 1=strong entropy 0=weak    */

/* Sign an artifact (its content-id, or any bytes) with the node's secret key.
 * Deterministic. sig_out is 64 bytes. Returns 1 on success, 0 if no key. */
INT  sign_artifact(const U1 *msg, UW msg_len,
                   U1 sig_out[ED25519_SIGNATURE_LEN]);

/* Verify a detached signature over msg under an explicit public key.
 * Returns 1 iff valid (fail-closed). Pure wrapper over ed25519_verify. */
INT  sign_verify(const U1 *msg, UW msg_len,
                 const U1 sig[ED25519_SIGNATURE_LEN],
                 const U1 pk[ED25519_PUBLIC_KEY_LEN]);

/* The signer-allowlist (selfc / genome adopt, signing.md §3.2). Adoption is
 * the human act; verification is the machine act. Bounded, per-node, local. */
#define SIGN_ALLOWLIST_MAX 16
INT  sign_allow_add(const U1 pk[ED25519_PUBLIC_KEY_LEN]);  /* 1=added/present */
INT  sign_allow_has(const U1 pk[ED25519_PUBLIC_KEY_LEN]);  /* 1=allowlisted   */
INT  sign_allow_count(void);                               /* adopted signers */
void sign_allow_clear(void);                               /* test reset      */

/* ------------------------------------------------------------------ */
/* the signed ARTIFACT MANIFEST — ONE shape, ONE primitive, serving      */
/* selfc unit germination AND genome weight resolution (anti-fork,       */
/* signing.md §4.2/§4.3/§6.1). A manifest binds: the artifact's          */
/* content-id (sha256 of its EXACT bytes), a version, the signer pubkey, */
/* and a detached Ed25519 signature OVER {artifact-id || version}.       */
/*                                                                       */
/* THE BOUNDARY: artifact_id is a CONTENT-id of code/weight bytes. A     */
/* manifest NEVER carries or signs a human handle/profile/identity — it  */
/* attests "these bytes came from this NODE KEY", nothing more (§0).     */
/* ------------------------------------------------------------------ */

#define SIGN_MANIFEST_MAGIC  0x4E47534FUL   /* "OSGN" LE                   */
#define SIGN_MANIFEST_VER    1

typedef struct {
    U4 magic;                                /* SIGN_MANIFEST_MAGIC          */
    U4 version;                              /* SIGN_MANIFEST_VER            */
    U4 artifact_ver;                         /* unit/weight version (seq)    */
    U4 _pad;                                 /* keep 8-byte alignment, LP64  */
    U1 artifact_id[ED25519_PUBLIC_KEY_LEN];  /* content-id of the artifact   */
    U1 signer_pk  [ED25519_PUBLIC_KEY_LEN];  /* the author NODE's pubkey     */
    U1 sig        [ED25519_SIGNATURE_LEN];   /* Ed25519 over the signed body */
} __attribute__((packed)) SIGN_MANIFEST;     /* 16 + 32 + 32 + 64 = 144 B   */

/* Build a manifest for an artifact (its content-id + a version) signed by
 * THIS node's key. Returns 1 on success, 0 if the node has no key. The
 * signed body is {artifact_id || artifact_ver} (the version binds the seq so
 * an old signed manifest cannot be replayed against a new artifact id). */
INT  sign_manifest_make(const U1 artifact_id[ED25519_PUBLIC_KEY_LEN],
                        U4 artifact_ver, SIGN_MANIFEST *out);

/* Verify a manifest against (a) its own internal signature and (b) the
 * actual artifact bytes the caller resolved: artifact_id MUST equal
 * pfs_id_compute(bytes) — passed in by the caller as actual_id — AND the
 * signature MUST verify under signer_pk AND signer_pk MUST be in this node's
 * allowlist. ANDed, fail-closed. Returns 1 iff ALL hold, else 0.
 * (The caller computes actual_id over the bytes it is about to run, so a
 * poisoned body whose id != the signed id is refused.) */
INT  sign_manifest_verify(const SIGN_MANIFEST *m,
                          const U1 actual_id[ED25519_PUBLIC_KEY_LEN]);

/* entropy provider — implemented per-arch (arch/linux/sign_entropy.c,
 * arch/x86/sign_entropy.c, arch/aarch64/sign_entropy.c). Fills out[0..n);
 * returns 1 = STRONG (OS RNG / RDRAND), 0 = WEAK fallback (flagged loudly). */
IMPORT int sign_entropy(unsigned char *out, int n);

/* The falsifiable acceptance suite. Emits printed evidence then a canonical
 * "[sign-*] PASS/FAIL" line for each of:
 *   [sign-roundtrip]   sign->verify true; one-byte flip false; + RFC 8032 KATs
 *   [sign-selflayer]   a forged-from-genesis chain by an UNPINNED key REJECTED
 *   [sign-unit]        unsigned / wrong-key unit refused; adopted-key runs
 *   [sign-keyrotation] a successor continues a chain under its OWN key, verifiably
 * Wired to CI via the `sign test` shell verb. */
void sign_self_test(void);

/* ------------------------------------------------------------------ */
/* LIVE production-path gates (wave-39): each drives the REAL call      */
/* site, shows the DISEASE (forged/unsigned refused) AND the genuine    */
/* pass. These are additive to the in-process models in sign_self_test. */
/* ------------------------------------------------------------------ */

/* [sign-selflayer-live]: append a GENUINE signed Self entry to the live
 * "self/lin" chain (signed by THIS node's key, TOFU-pinned); the walker
 * verifies it. Then a from-genesis entry forged under an UNPINNED key is
 * REJECTED by the same live verifier. 1=PASS. (lm_self.c production path.) */
INT  lm_self_sign_live_test(void);

/* [sign-genome]: publish a genome manifest whose weights artifact is signed
 * by an adopted key — genome_sprout resolves it; an UNSIGNED / forged-signer
 * weights artifact is REFUSED. 1=PASS. (genome.c production path.) */
INT  genome_sign_live_test(void);
