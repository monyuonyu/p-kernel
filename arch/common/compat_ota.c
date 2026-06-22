/*
 *  compat_ota.c — the [signed-ota-gate] (compat-migration-chain-plan.md §4).
 *
 *  THE TRUST GATE of "the ark ships updates without splitting or being
 *  hijacked." A node ACCEPTS a software/weights update artifact ONLY if it is
 *  correctly signed AND is a legal version successor (no downgrade, no
 *  body-swap). It REUSES the shipped Ed25519 signing layer verbatim
 *  (sign_manifest_verify, sign.c) and invents NO new crypto.
 *
 *  THE 4-GATE AND (design §4.2):
 *      ACCEPT(update) iff
 *         sign_manifest_verify(m, actual_id) == 1   // the shipped 3-gate AND:
 *                                                    //   (1) recomputed
 *                                                    //       artifact_id match
 *                                                    //   (2) Ed25519 sig valid
 *                                                    //   (3) signer adopted
 *         AND m->artifact_ver > running_ver          // (4a) strictly forward —
 *                                                    //       NO downgrade
 *         AND legal_successor(running_ver, ...)      // (4b) reachable step
 *
 *  Because the signed body is {artifact_id || artifact_ver} (sign.c:133-143),
 *  the version is INSIDE the Ed25519 signature: a downgrade or a body-swap
 *  cannot be forged without the signer's secret key — it breaks gate (1)/(2).
 *  Gate (4) only adds the version-POLICY decision (no downgrade); the integrity
 *  of the version itself is already cryptographically bound by gates (1)/(2).
 *
 *  THE BOUNDARY (signing.md §0; feedback_ark_no_identity_verification.md): this
 *  gate attests an ARTIFACT and a KEY, NEVER a human. It introduces NO author
 *  handle / signed identity / human attestation — it gates artifact INTEGRITY
 *  + version LEGALITY only. (Said twice across the codebase, on purpose.)
 *
 *  LENS A (byte-identity): this TU is HOSTED/LOADER-only — it is compiled into
 *  the hosted (boot/linux*) builds, NEVER into the bare-metal default link
 *  (boot/aarch64, boot/x86). So the default aarch64 .text and the
 *  [smp-one-mind] crown are byte-IDENTICAL by construction (this object does
 *  not exist there). r_forward is untouched.
 *
 *  SCOPE (honest, design §9): this is the OTA-ACCEPT gate. The actual artifact
 *  DELIVERY/TRANSPORT (mesh small-update / KLOAD deep-update) and key
 *  REVOCATION (no CRL, no fleet revocation broadcast) are SEPARATE and deferred.
 *
 *  arch/common discipline: fixed-width types, static buffers, output via the
 *  sign.c-style sio_send_frame puts; no host libc in the gate itself.
 */

#include "kernel.h"
#include "sign.h"
#include "pfs_block.h"     /* pfs_id_compute — recompute the artifact content-id */
#include "compat_ota.h"

/* ------------------------------------------------------------------ */
/* THE GATE — a 4-gate AND, returning WHICH gate rejected (OTA_*).     */
/* ------------------------------------------------------------------ */

/* legal_successor: the new version must be reachable by the migration chain
 * (design §4.2/§3.2 — every step is +1, so any strictly-greater version is
 * reachable by running the intermediate steps; a fleet may bound this with a
 * max-step policy, but v1 reaches forward without a cap). Kept as a named
 * predicate so a future per-axis step cap drops in here, not in the caller. */
static INT ota_legal_successor(U4 running_ver, U4 artifact_ver)
{
    return (artifact_ver > running_ver);   /* strictly forward; chain composes */
}

INT compat_ota_accept(const SIGN_MANIFEST *m,
                      const U1 actual_id[PFS_ID_LEN],
                      U4 running_ver)
{
#ifdef OTA_SKIP_VERIFY
    /* ===== THE FALSIFIER (load-bearing, design §5.2 master falsifier) =====
     * The gate is stubbed to ACCEPT unconditionally. A tampered / wrong-key /
     * downgrade OTA is then accepted -> the cert goes RED. This proves the gate
     * below is the thing doing the work, not decoration. */
    (void)m; (void)actual_id; (void)running_ver;
    return OTA_ACCEPT;
#else
    if (!m || !actual_id) return OTA_REJECT_BADARG;

    /* Gates (1)+(2)+(3): the shipped 3-gate AND, REUSED VERBATIM. It refuses a
     * tampered body (recomputed artifact_id mismatch, sign.c:176), a bad/forged
     * signature (sign.c:179), and a non-adopted signer key (sign.c:181). We do
     * NOT weaken or reimplement any of these. */
    if (!sign_manifest_verify(m, actual_id))
        return OTA_REJECT_SIG;     /* one of the 3 shipped gates fired */

    /* Gate (4): legal-successor / no-downgrade. The version is inside the
     * signed body, so a correctly-signed-but-OLDER artifact passes gates 1-3
     * yet MUST still be refused here (the subtle downgrade leg). */
    if (!ota_legal_successor(running_ver, m->artifact_ver))
        return OTA_REJECT_DOWNGRADE;

    return OTA_ACCEPT;   /* all four, ANDed, fail-closed */
#endif
}

const char *compat_ota_reason(INT r)
{
    switch (r) {
        case OTA_ACCEPT:            return "ACCEPT";
        case OTA_REJECT_BADARG:     return "REJECT(gate0: null arg)";
        case OTA_REJECT_SIG:        return "REJECT(gate1-3: sign_manifest_verify)";
        case OTA_REJECT_DOWNGRADE:  return "REJECT(gate4: downgrade/non-successor)";
        default:                    return "REJECT(?)";
    }
}

/* ================================================================== *
 *  [signed-ota-gate] CERT (compat-migration-chain-plan.md §5.2)        *
 *                                                                      *
 *  Cure (PASS): a correctly-signed update whose artifact_ver >          *
 *  running_ver, signed by an ADOPTED key, is ACCEPTED.                 *
 *  Falsifiers (each REFUSED, with the gate that fired named):          *
 *    (a) tampered body   -> id mismatch          -> gate1-3            *
 *    (b) wrong/unadopted  -> signer not allowed   -> gate1-3            *
 *    (c) downgrade        -> ver <= running, signed-OK -> gate4        *
 *  Master falsifier -DOTA_SKIP_VERIFY: the gate is vacuous -> a         *
 *  tampered OTA is ACCEPTED -> cert RED.                               *
 *                                                                      *
 *  GATED: compiles ONLY under -DOTA_GATE_CERT (and HOSTED-only via      *
 *  _TK_HOSTED_LIBC_). The TU is not in the bare-metal link at all, so   *
 *  the default kernel + crown are byte-identical regardless.           *
 * ================================================================== */
#if defined(OTA_GATE_CERT) && defined(_TK_HOSTED_LIBC_)

IMPORT void sio_send_frame(const UB *buf, INT size);
static UW   co_strlen(const char *s){ UW n=0; while(s[n]) n++; return n; }
static void co_puts(const char *s){ sio_send_frame((const UB *)s,(INT)co_strlen(s)); }
static void co_putdec(UW v){ char b[12]; INT i=11; b[11]=0;
    if(!v){ co_puts("0"); return; } while(v&&i>0){ b[--i]=(char)('0'+(v%10)); v/=10; } co_puts(&b[i]); }

/* A deterministic "author" keypair so the cert is reproducible across ABIs
 * (mirrors sign.c gate derive_kp — determinism is a TEST property only).
 * ed25519_keypair_from_seed / ed25519_sign come from ed25519.h (via sign.h). */
static void co_kp(U1 seed_byte, U1 pk[32], U1 sk[64])
{
    U1 seed[32]; INT i;
    for (i = 0; i < 32; i++) seed[i] = (U1)(seed_byte + i * 31u);
    ed25519_keypair_from_seed(seed, pk, sk);
}

/* sign_manifest_make signs with THIS NODE's key (sign.c), which the cert does
 * NOT adopt by default. To exercise an "adopted author" deterministically we
 * build the manifest by hand: body = {artifact_id || artifact_ver} signed by
 * author_sk, exactly the sign.c layout. This is NOT new crypto — it is the
 * same {id||ver} body the shipped sign_manifest_body builds. */
static void co_make_manifest(const U1 artifact_id[PFS_ID_LEN], U4 ver,
                             const U1 author_pk[32], const U1 author_sk[64],
                             SIGN_MANIFEST *out)
{
    U1 body[36]; INT i;
    out->magic        = SIGN_MANIFEST_MAGIC;
    out->version      = SIGN_MANIFEST_VER;
    out->artifact_ver = ver;
    out->_pad         = 0;
    for (i = 0; i < PFS_ID_LEN; i++) out->artifact_id[i] = artifact_id[i];
    for (i = 0; i < 32; i++)         out->signer_pk[i]   = author_pk[i];
    /* body = artifact_id(32) || artifact_ver LE(4) — sign.c sign_manifest_body */
    for (i = 0; i < PFS_ID_LEN; i++) body[i] = artifact_id[i];
    body[32] = (U1)(ver        & 0xFF);
    body[33] = (U1)((ver >> 8)  & 0xFF);
    body[34] = (U1)((ver >> 16) & 0xFF);
    body[35] = (U1)((ver >> 24) & 0xFF);
    ed25519_sign(out->sig, body, (UW)sizeof body, author_sk);
}

void compat_ota_gate_test(void)
{
    INT ok = 1, r;
    U1  author_pk[32], author_sk[64];
    U1  evil_pk[32],   evil_sk[64];
    /* an "update artifact" = some bytes; its content-id is what the node
     * recomputes (pfs_id_compute) over the bytes it is about to install. */
    static const U1 artifact[40] = {
        'O','T','A','-','v','2',':',' ','n','e','w',' ','c','o','d','e',
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23 };
    U1  actual_id[PFS_ID_LEN];
    U4  running_ver = 1;            /* the node currently runs v1 */
    SIGN_MANIFEST m;

    co_puts("[signed-ota-gate] ==== accept iff signed AND legal successor ====\r\n");
    co_puts("[signed-ota-gate] reuse sign_manifest_verify (3-gate AND) + version-successor gate.\r\n");

    co_kp(0x21, author_pk, author_sk);   /* the adopted update author */
    co_kp(0x42, evil_pk,   evil_sk);     /* an UNadopted signer        */

    /* the operator ADOPTS the author key (selfc adopt <key>) — the only trust
     * anchor; a fresh node with an empty allowlist accepts NO OTA (fail-closed). */
    sign_allow_clear();
    sign_allow_add(author_pk);

    /* the node recomputes the content-id of the bytes it is about to install. */
    pfs_id_compute(artifact, (UW)sizeof artifact, actual_id);

    /* ---- (cure) good OTA: signed by adopted key, ver 2 > running 1 -------- */
    co_make_manifest(actual_id, 2u, author_pk, author_sk, &m);
    r = compat_ota_accept(&m, actual_id, running_ver);
    co_puts("  good signed successor (v2 over v1): ");
    co_puts(compat_ota_reason(r)); co_puts("\r\n");
    if (r != OTA_ACCEPT) { ok = 0; }

    /* ---- (a) tampered body: flip one artifact byte -> id no longer matches - */
    {
        U1 bad[40]; U1 bad_id[PFS_ID_LEN]; UW i;
        for (i = 0; i < sizeof bad; i++) bad[i] = artifact[i];
        bad[10] ^= 0x01;                 /* tamper the bytes the node installs */
        pfs_id_compute(bad, (UW)sizeof bad, bad_id);
        /* the manifest still names the ORIGINAL id+sig (the attacker can't
         * re-sign without the key); the node recomputes bad_id over the bytes
         * it actually has -> gate (1) artifact_id mismatch. */
        r = compat_ota_accept(&m, bad_id, running_ver);
        co_puts("  tampered body (id mismatch): ");
        co_puts(compat_ota_reason(r)); co_puts("\r\n");
        if (r != OTA_REJECT_SIG) { ok = 0; }   /* MUST be refused */
    }

    /* ---- (b) wrong/unadopted key: valid signature, signer NOT adopted ------ */
    {
        SIGN_MANIFEST me;
        co_make_manifest(actual_id, 2u, evil_pk, evil_sk, &me);  /* evil signs */
        r = compat_ota_accept(&me, actual_id, running_ver);
        co_puts("  valid sig by NON-adopted key: ");
        co_puts(compat_ota_reason(r)); co_puts("\r\n");
        if (r != OTA_REJECT_SIG) { ok = 0; }   /* allowlist gate refuses it */
    }

    /* ---- (c) downgrade: correctly signed by adopted key, but ver <= running - */
    {
        SIGN_MANIFEST mo;
        /* sign artifact as v1 (== running) — perfectly valid signature, adopted
         * key, body-id matches; ONLY the version policy rejects it. This is the
         * subtle leg: a naive gate that only checks the signature ACCEPTS it. */
        co_make_manifest(actual_id, 1u, author_pk, author_sk, &mo);
        r = compat_ota_accept(&mo, actual_id, running_ver);
        co_puts("  downgrade (v1 over v1, correctly signed): ");
        co_puts(compat_ota_reason(r)); co_puts("\r\n");
        if (r != OTA_REJECT_DOWNGRADE) { ok = 0; }

        /* and a strictly-older version (v0 < v1) likewise refused at gate 4 */
        co_make_manifest(actual_id, 0u, author_pk, author_sk, &mo);
        r = compat_ota_accept(&mo, actual_id, running_ver);
        co_puts("  downgrade (v0 < v1, correctly signed): ");
        co_puts(compat_ota_reason(r)); co_puts("\r\n");
        if (r != OTA_REJECT_DOWNGRADE) { ok = 0; }
    }

    /* sanity: the downgrade leg uses the IN-SIGNED-BODY version. Prove an
     * attacker editing the UNSIGNED... there is no unsigned version field — the
     * struct's artifact_ver is the same field the signature covers. Relabel the
     * v1-signed manifest's artifact_ver to 99 WITHOUT re-signing: gate (1)/(2)
     * (sign_manifest_verify) recomputes the body {id||99} and the signature (over
     * {id||1}) no longer verifies -> REFUSED at gate 1-3, not silently accepted. */
    {
        SIGN_MANIFEST mr;
        co_make_manifest(actual_id, 1u, author_pk, author_sk, &mr);
        mr.artifact_ver = 99u;          /* forge a "newer" label, do NOT re-sign */
        r = compat_ota_accept(&mr, actual_id, running_ver);
        co_puts("  relabel v1->v99 without re-signing: ");
        co_puts(compat_ota_reason(r)); co_puts("\r\n");
        if (r != OTA_REJECT_SIG) { ok = 0; }   /* signature binds the version */
    }

    co_puts("  running_ver="); co_putdec((UW)running_ver);
    co_puts(" allowlist_count="); co_putdec((UW)sign_allow_count());
    co_puts("\r\n");

    sign_allow_clear();

#ifdef OTA_SKIP_VERIFY
    co_puts("[signed-ota-gate] (FALSIFIER -DOTA_SKIP_VERIFY active: gate is vacuous)\r\n");
#endif
    co_puts(ok ? "[signed-ota-gate] PASS\r\n" : "[signed-ota-gate] FAIL\r\n");
    co_puts("[signed-ota-gate] DONE — a node accepts an update iff signed AND a legal successor.\r\n");
}

#endif /* OTA_GATE_CERT && _TK_HOSTED_LIBC_ */
