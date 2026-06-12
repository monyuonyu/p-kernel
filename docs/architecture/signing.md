# signing — a provenance signature for CODE and WEIGHTS, never for humans

> Status: **design only** (written before implementation, same discipline as
> [selfc-ring3.md](selfc-ring3.md) / [living-mind.md](living-mind.md) /
> [genome.md](genome.md)). ZERO production code in this slice.
> Parents — the three deferrals that all point HERE, by file:line:
> - [living-mind.md](living-mind.md) III.6 / `arch/common/include/lm_self.h:26-32`
>   — the Self layer is **tamper-EVIDENT, not tamper-PROOF**: a malicious node
>   that controls its own store can author a fresh, internally-consistent fake
>   lineage FROM GENESIS. "Per-manifest signatures are deferred."
> - [selfc-ring3.md](selfc-ring3.md) §3 — **"networked evolution without
>   signatures is a malware mesh."** selfc is **LOCAL-ONLY** until signing; the
>   named v3 gate is "node keypair + signed unit manifests + signer-allowlist."
> - [genome.md](genome.md) §5 / `arch/common/include/genome.h:28-29` — "there is
>   no signature / verification on the manifest or its entries — same trust model
>   as selfc, stated, not solved."
> selfc-ring3.md §3 already promised **ONE signing slice serves both** (anti-fork).
> This doc is that one slice's design.

---

## 0. THE BOUNDARY — read this first, it is a permanent owner directive

**Signatures in p-kernel attest CODE and WEIGHTS provenance ONLY. They NEVER
verify a human.** (`docs/claude-memory/feedback_ark_no_identity_verification.md`,
mk_pino 2026-06-10, verbatim intent quoted there.)

A p-kernel signature means exactly:

> "this ARTIFACT (a content-addressed code unit, weight blob, or Self-chain
> entry) was produced by the holder of this KEY."

It MUST NOT be read, extended, or re-used to mean:

> ~~"this human is who they say they are"~~ — **forbidden, forever.**

This is structural, not a footnote:

1. **No identity keys for humans.** A key in this system belongs to a NODE (a
   running p-kernel process / cell), not a person. There is no "verified handle",
   no real-name key, no key-to-human binding anywhere in the design. The ark
   (`docs/architecture/ark-profile.md`) records *declared voices* — pen names,
   anonymous names, real names — all equally first-class and **UNVERIFIED
   FOREVER**. The honest history IS the declaration (歴史地層); adding human
   verification would turn a stratum into a curated record and is explicitly
   rejected.
2. **The Self-layer's `human_ref` is NOT signed as a human claim.** When a signed
   Self entry (§4.1) carries an `LM_SELF_ENTRY.human_ref` (`lm_self.h:63`), the
   signature attests "this NODE committed this chain entry," NOT "this human
   exists / is real / consented as claimed." A profile referenced by a signed
   entry stays a declared voice. The signature binds the *node's authorship of
   the chain*, and stops there.
3. **Say it twice (the house rule for this boundary):** *a signature is a
   statement about an artifact and a key — never about a human.* If a future
   wave ever proposes "verified profiles," "identity keypairs for people," or
   "signed handles," that proposal is OUT OF SCOPE and contradicts a standing
   directive; strike it, as the ark-profile "v3 signed identity" item was struck.

Everything below operates strictly inside that boundary.

---

## 1. The threat model — precisely, per unlock

A signature must DEFEAT a specific attack for each of the three parents. State
each as: the attacker capability, the artifact, what the signature must bind,
and the trust question (whose key is believed).

p-fs gossip is the shared danger surface: **anyone who can write a block into a
region can publish ANY content-addressed object into it.** Content addressing
(`pfs_id_compute` = sha256, `pfs_block.h:44`) proves *integrity* ("these bytes
hash to this id") but says NOTHING about *origin* ("who authored these bytes").
The gap a signature closes is exactly origin.

### 1.1 Self-layer — a peer fabricating a plausible autobiography from genesis
- **Attacker capability:** controls its own p-fs store; can author a fresh,
  internally hash-consistent `self/lin` chain from `seq=1` (genesis prev = all
  zero) — `lm_self.h:29-31` admits this passes today.
- **Artifact:** each `LM_SELF_ENTRY` (148 B, `lm_self.h:52-66`), the
  hash-chained autobiography that survives death and reconstructs ownerless.
- **Bind:** a signature over each committed entry's content-id, by the **origin
  node's key** (`self_id`, the node that authored the chain). A forged
  from-genesis chain then fails because the attacker does not hold the genesis
  key. The wave-22 teeth (can't ALTER/SPLICE a committed entry) UPGRADE to
  *can't FABRICATE a chain claiming to be from node K* — tamper-EVIDENT →
  tamper-PROOF.
- **Trust question:** "do I believe key K is the origin of chain K?" → TOFU on
  first valid entry seen (§3); succession across death records a signed key
  rotation (§3.3) so a successor continues verifiably.

### 1.2 selfc — node A accepting a malicious code unit claiming a trusted author
- **Attacker capability:** publishes a `unit/<name>` C-source block into the
  region (gossip replicates it everywhere by design); the unit need not crash —
  it can politely publish poison on its topics forever (`selfc-ring3.md` §3).
- **Artifact:** a selfc unit = the C source object `unit/<name>@seq` (content-id
  via the same `pfs_id_compute`).
- **Bind:** a signed **unit manifest** (§4.2) = {unit content-id, version,
  signer key} signed by the **author node's key**. A node germinates a gossiped
  unit ONLY if the manifest verifies against a key in this node's local
  **signer-allowlist** (adopted by `selfc adopt`, §4.2). This DROPS the
  LOCAL-ONLY restriction: fleet evolution becomes possible because "from a
  trusted author" is now checkable, not assumed.
- **Trust question:** "is this unit signed by a key I have adopted?" → explicit
  per-node allowlist, seeded by the operator (`selfc adopt <key>`), NOT
  automatic. Adoption is the human act; verification is the machine act.

### 1.3 genome — a poisoned weight artifact gossiped into the fleet
- **Attacker capability:** publishes a `dtr/weights` blob (or a `genome/manifest`
  pointing at one) into the region; a sprouting empty plate
  (`genome_sprout`, `genome.h:110`) or a learner folding gossiped contributions
  would adopt poisoned weights.
- **Artifact:** the weight blob `dtr/weights` (content-id), and the
  `GENOME_MANIFEST` (124 B, `genome.h:84-91`) that names it; also the
  `gl_merge`'d `rw[]` contributions in gossip_learn (`gossip_learn.c`).
- **Bind:** a signed **weight manifest** (§4.3) = {weight content-id, model
  version, signer key} by the producing node's key. `genome_sprout` resolves a
  weight ref only if its manifest verifies against an adopted key; a future
  "no poisoned weights" cert can require gl_merge to fold ONLY signed
  contributions.
- **Trust question:** same allowlist as selfc — one signer set per node serves
  code AND weights (anti-fork).

> The three share ONE primitive, ONE key per node, ONE allowlist. That is the
> whole point of doing this as a single slice.

---

## 2. THE PRIMITIVE — the central fork (symmetric vs asymmetric)

> **COMMANDER DECISION NEEDED — D1 (the primitive).** This is THE fork. The
> recommendation is below with the honest risk stated; the commander reads it.

### 2.1 Why HMAC (symmetric) cannot be the open-fleet answer
We already HAVE symmetric crypto: from-scratch SHA-256 + HMAC-SHA-256
(`relay/sha256.h`, reused kernel-side via `arch/common/pfs_block.c:32`). HMAC
proves "whoever holds the shared secret produced this MAC." For an **open,
ownerless fleet** that is the wrong shape:

- HMAC requires every *verifier* to hold the *signing* secret. "Anyone can
  verify, only the author can sign" is impossible with a symmetric key — anyone
  who can verify can also forge. Sharing the relay's `KEY_LEN=32` PSK
  (`relay.c:42,68`) fleet-wide would let any node forge any other node's units.
- It works ONLY inside a closed trust domain where a secret is genuinely shared
  by mutually-trusting parties. That is the relay's wire-auth case (a deployment
  shares a key) — appropriate there, inappropriate for per-node authorship.

So HMAC is usable as a **staged v0 inside a trust domain** (see §2.4) but is NOT
the destination for selfc/genome fleet evolution.

### 2.2 Why asymmetric (Ed25519) is what the unlocks REQUIRE
"only the holder can sign, anyone can verify" is the definition of a digital
signature and is exactly what an open fleet needs: a node signs with its private
key; every peer verifies with the public key (which is the node's published
identity, content-addressable). Ed25519 (RFC 8032) is the right modern choice:
fixed 32-byte public key, 64-byte signature, 32-byte seed; deterministic (no
per-signature RNG needed — critical on a freestanding kernel with no good
entropy source at sign time); fast verify; well-specified KAT vectors.

### 2.3 Freestanding feasibility — verdict
- **No libsodium, no libcrypto, anywhere.** Bare-metal targets are freestanding;
  hosted (`arch/linux`, Android) deliberately does not assume system crypto —
  the relay rolled its OWN sha256 for exactly this reason
  (`relay/sha256.h:1-13`). A from-scratch primitive is the established pattern.
- **Ed25519 from scratch is ~a few hundred lines** of C (the SHA-512 it needs +
  the curve25519 field arithmetic + the edwards point ops + the
  sign/verify/keygen wrappers). It is FEASIBLE freestanding: fixed-width integer
  math only, no allocation, no syscalls. SHA-512 follows the same shape as the
  existing SHA-256.
- **The honest risk (this is the one place "honest > green" means EXTRA
  caution):** hand-rolled field arithmetic is **correctness- and
  security-critical and error-prone** — a subtle reduction bug or a
  non-constant-time path can silently void the security property while every
  functional test still passes. This is categorically different from the rest of
  the tree: a wrong context-switch crashes loudly; a wrong field mul *verifies
  the wrong things quietly*. Therefore:
  - **Use a single, vetted, public-domain minimal implementation as the
    reference** (e.g. the ref10-derived / TweetNaCl-class compact Ed25519 that is
    widely audited), transcribed into the tree's fixed-width discipline — do NOT
    invent the field math. Provenance of the chosen reference is itself recorded.
  - **Gate it on RFC 8032 known-answer vectors** as a hard CI self-test
    (`[sign-roundtrip]` with KATs, §5) — exactly as `sha256_self_test()`
    (`relay/sha256.h:42`) gates the existing primitive. No KAT pass, no merge.
  - **Constant-time is a stated NON-GOAL of v1 for the verify path** (verify
    handles only public data) but a stated REQUIREMENT to audit for the SIGN
    path; name it honestly in the bounds (§6) rather than claim side-channel
    resistance we have not proven.

### 2.4 RECOMMENDATION — staged, asymmetric destination
- **v1 = Ed25519, asymmetric, from a vetted reference, KAT-gated.** This is the
  destination and it unblocks all three unlocks honestly. Do it as the slice.
- **Optional v0 (only if the commander wants to ship value before the Ed25519
  audit lands): HMAC-within-a-trust-domain** for selfc/genome inside a single
  operator's own nodes (they share a secret = they trust each other). It buys
  "no accidental cross-region poison" but NOT open-fleet safety, and its API must
  be designed so v1 Ed25519 is a drop-in replacement of the verify function, not
  a wire-format do-over. The risk of v0: it can MASQUERADE as the real thing in
  READMEs — so if v0 ships, the LOCAL-ONLY/"trust-domain-only" honesty from
  selfc-ring3 §3 stays loudly attached to it.
- **Recommended path:** go straight to v1 Ed25519. The staged v0 exists only as
  the commander's escape hatch; the slice's *primary* deliverable is the
  asymmetric primitive, because anything less leaves the malware-mesh bound open.

---

## 3. The trust model — the SIMPLEST honest v1

> **COMMANDER DECISION NEEDED — D2 (the trust model).** Recommendation: **TOFU +
> explicit per-node allowlist.** NOT a PKI, NOT a CA, NOT a web-of-trust (those
> are named-not-designed futures, §6).

### 3.1 The node keypair
- **One Ed25519 keypair per NODE, generated at first boot** if absent. The
  public key is the node's *cryptographic identity* (distinct from the 8-bit
  `drpc_my_node` wire id, `drpc.h:124`, which is far too small to be an identity
  — keys are content-addressed by their 32-byte public value, the node-id stays
  a routing tag).
- **Stored in p-fs** as a node-private object (the secret seed never gossips;
  only the public key is published). On hosted nodes the seed lives in the
  durable p-fs store like other persistent state; the ark
  (`docs/architecture/ark-profile.md`) is NOT where secrets live — the ark holds
  declared voices, not credentials (§0). The PUBLIC key MAY be published as an
  ordinary content-addressed object so peers can fetch it by id.
- **The ownerless answer:** a mind with no human owner still has a NODE. The
  NODE's key signs the node's own outputs — "this NODE produced this artifact."
  No human key is ever required or created (§0). Self-evolution is attested by
  the evolving node itself, which is exactly the ownerless property the Self
  layer already has (it reconstructs with no owner).

### 3.2 TOFU + allowlist (the v1 trust decision)
- **Self-layer:** trust-on-first-use — the first valid signed entry seen for a
  chain pins that chain's origin key. Subsequent entries must verify against the
  pinned key; a from-genesis forgery by a *different* key is rejected (§1.1).
- **selfc / genome:** an explicit **signer-allowlist** per node. The operator
  act `selfc adopt <key>` (extending today's `selfc adopt <name>`,
  `selfc.h:44`) ADDS a public key to the local allowlist. A gossiped unit/weight
  manifest germinates/sprouts only if signed by an allowlisted key. Adoption is
  the human decision; verification is mechanical. This is the minimum that turns
  LOCAL-ONLY into FLEET-with-consent.
- **Region scope (named, optional):** an allowlist entry MAY be scoped to a
  region (`docs/architecture/regions.md`) so "I trust author K's units within my
  region" is expressible. v1 may ship global-per-node and add region scope
  later; flagged in §6.

### 3.3 Succession across death (the "never dies" tension)
The Self chain survives node death and is continued by a SUCCESSOR. Signing must
survive succession WITHOUT a human handing over a secret:

- **The successor signs its continuation with ITS OWN key**, and the chain
  records a **signed key-rotation entry**: the successor appends an entry whose
  body declares "continuing chain originally rooted at key K_old, now under
  K_new," signed by **both** K_old (if the dying node could pre-author a
  succession token while alive) and K_new — OR, in the unplanned-death case where
  K_old's secret is gone with the node, by K_new alone, with the rotation entry
  recording that the predecessor key is no longer available (an honest,
  verifiable discontinuity, not a forged continuity).
- **Verification across rotation:** a verifier walking the chain accepts the
  hand-off when the rotation entry is well-formed and (planned case)
  countersigned by K_old, or (unplanned case) treats the chain as
  *continued-under-new-key-from-seq-N* — the lineage stays auditable and the
  break is visible. This is the honest analogue of the Self layer's "tamper-
  EVIDENT": succession is *transition-EVIDENT*. The key-rotation record rides the
  SAME `self/lin` chain (no second chain — anti-fork with `lm_self.h:82-93`).
- This is the `[sign-keyrotation]` gate (§5).

---

## 4. What each unlock gets (the deltas)

### 4.1 Self-layer → tamper-PROOF
- A new signed Self entry (or a signature side-object content-addressed to the
  entry id — to keep `LM_SELF_ENTRY`'s 148 B wire image and its `_Static_assert`
  untouched, the signature is a SEPARATE content-addressed companion object
  keyed by the entry's content-id, NOT a struct field). The walker verifies each
  entry's signature against the chain's TOFU-pinned origin key.
- The wave-22 honest bound (`lm_self.h:26-32`) is RETIRED for signed chains: a
  from-genesis forgery now needs the origin key, which the attacker does not
  hold. Tamper-EVIDENT → tamper-PROOF.

### 4.2 selfc → FLEET evolution (drop LOCAL-ONLY)
- A **signed unit manifest** companion object: {unit content-id, version, signer
  pubkey, signature}. `selfc run <name>` of a gossiped unit succeeds iff the
  manifest verifies against an allowlisted key (§3.2). The §3 LOCAL-ONLY refusal
  becomes "refuse only if unsigned OR signer not adopted." The honest claim
  upgrades from "each node can evolve itself, locally" to "the fleet evolves
  itself among nodes that have adopted each other's keys."
- Play-tier (`SELFC_TIER_PLAY`, selfc-ring3 §4) is UNCHANGED — native codegen
  stays OFF there; signing does not re-enable APK self-update. Signed
  *weights*-evolution (4.3) is the policy-clean path for Play nodes.

### 4.3 genome → signed weight artifacts
- A **signed weight manifest** companion: {weight content-id, model version,
  signer pubkey, signature}. `genome_sprout` (`genome.h:110`) resolves a weight
  ref only if its manifest verifies against an adopted key; `genome_publish`
  emits the signature. Hooks a future "no poisoned weights" cert: `gl_merge`
  (`gossip_learn.c`) folds ONLY signed contributions when that cert is turned on.

---

## 5. The falsifiable cert (disease → cure) + CI plan

Four tags, each a printed-evidence + canonical `[sign-*] PASS/FAIL` line, wired
to CI via a `sign test` shell verb (mirroring `self test` / `selfc test`):

- **`[sign-roundtrip]`** — sign an artifact, verify TRUE; flip ONE byte of the
  artifact OR the signature, verify FALSE. **For the asymmetric primitive this
  MUST also run RFC 8032 known-answer vectors** (the published Ed25519 test
  vectors) and fail closed if any KAT mismatches — the hand-rolled-crypto
  guardrail (§2.3). This is the gate that proves the primitive is the real
  Ed25519, not a look-alike.
- **`[sign-selflayer]`** — build a from-genesis chain signed by a key the
  verifier did NOT pin (the LM-2 disease: `lm_self.h:26-32` accepts this today)
  → it is now REJECTED; a chain signed by the pinned origin key is ACCEPTED. The
  tamper-evident→tamper-proof upgrade made falsifiable.
- **`[sign-unit]`** — a gossiped selfc unit that is UNSIGNED or signed by a
  non-allowlisted key REFUSES germination (names §3); the SAME unit signed by an
  adopted key adopts and runs. (genome variant folds into the same gate or a
  sibling `[sign-weight]` if the commander wants weight coverage explicit.)
- **`[sign-keyrotation]`** — a successor continues a dead node's chain under its
  own key; the rotation entry is well-formed and the post-rotation chain still
  VERIFIES end-to-end (planned: countersigned by K_old; unplanned: visibly
  continued-under-K_new). Succession preserves verifiability.

**CI plan:** add `sign test` to the same harness that runs `self test` /
`selfc test` (the existing CI gate count, e.g. the 18/55-style suites in recent
waves). The KAT block runs unconditionally on every build (cheap, like
`sha256_self_test`). Bare-metal and both hosted ABIs (aarch64 + x86_64) run the
roundtrip + KAT; the selflayer/unit/keyrotation gates run on hosted (where the
fleet lives). No bar lowered: the disease for each gate is a REAL accept-today
case (the three honest bounds in the parents).

---

## 6. Anti-fork, honest bounds, FLAGGED names, decisions

### 6.1 Anti-fork
- **ONE primitive, ONE keypair per node, ONE allowlist** serves Self + selfc +
  genome — exactly as selfc-ring3 §3 promised. No second crypto module, no
  per-unlock keystore.
- **Reuse the existing SHA family discipline:** the Ed25519 reference brings
  SHA-512; it lives beside `relay/sha256.c` as the same zero-dep, KAT-gated,
  fixed-width-typed pattern. `pfs_id_compute` (sha256) is UNCHANGED — signatures
  are companion objects keyed by content-id, never a rewrite of content
  addressing.
- **Self chain is NOT forked:** signatures and key-rotation records ride the
  existing `self/lin` chain as companion/encoded entries (`lm_self.h:82-93`
  precedent), not a parallel chain.

### 6.2 Honest bounds (state loudly — do NOT overclaim)
- **NOT human identity — said twice (§0).** A signature attests an artifact came
  from a key; it never attests a human. No profile signing, no identity keys for
  people, no verified handles. The ark stays declared-not-verified forever.
- **NOT a PKI / CA / web-of-trust.** v1 is TOFU + explicit per-node allowlist.
  There is no central authority, no certificate chain, no automatic trust
  transitivity. Those are named-not-designed futures.
- **Key distribution is manual in v1.** Adoption (`selfc adopt <key>`) is an
  explicit operator act; we do not solve "how does a node discover which keys to
  trust" automatically — that is the web-of-trust future, deliberately deferred.
- **Revocation is weak in v1.** Removing a key from the allowlist stops future
  acceptance; there is no fleet-wide revocation broadcast, no CRL. Already-run
  units are not retroactively un-run (the germ-reap boundary, not signing, is the
  runtime containment). State this; do not imply revocation is solved.
- **Hand-rolled crypto caveat (the big one).** The security rests on a
  from-scratch field-arithmetic implementation. Mitigation: vetted reference +
  RFC KATs as a hard gate (§2.3). v1 does NOT claim constant-time / side-channel
  resistance for the SIGN path beyond what the reference provides and the audit
  confirms; this is named, not claimed-away.
- **No good sign-time entropy on bare metal** — Ed25519 is deterministic
  (signature = f(key, message)), which is WHY it was chosen: keygen needs entropy
  once (first boot), signing needs none. Keygen entropy source is a flagged
  decision (D3).

### 6.3 FLAGGED names (the immune system reviews these)
- `sign-domain-only` (the v0 HMAC tier, IF the commander takes the staged path)
  must read as "trust-domain-only, NOT open-fleet safe" everywhere it appears —
  the masquerade risk from §2.4.
- `fleet evolution` / `the fleet evolves itself` — FORBIDDEN in READMEs/moments
  until `[sign-unit]` is green (inherits selfc-ring3 §3's rule). The honest
  pre-signing claim stays "each node evolves itself locally."
- `tamper-proof` — usable for the Self layer ONLY after `[sign-selflayer]` is
  green; until then the parent's `tamper-EVIDENT` wording stands.
- `verified` near anything human — a tripwire word; any occurrence next to
  profiles/handles/identity is a §0 violation by construction.

### 6.4 COMMANDER DECISIONS NEEDED
- **D1 — the primitive.** Recommend **Ed25519 (asymmetric), vetted reference,
  KAT-gated** as the slice's primary deliverable; staged HMAC v0 only as an
  escape hatch with the masquerade honesty attached. (Honest risk: hand-rolled
  field math — §2.3.)
- **D2 — the trust model.** Recommend **TOFU (Self) + explicit per-node
  signer-allowlist (selfc/genome)**, NOT PKI/CA/web-of-trust. Region-scoped
  allowlist entries optional, deferrable.
- **D3 — staged vs full, and keygen entropy.** (a) Straight-to-v1 Ed25519
  (recommended) vs HMAC-v0-first. (b) The first-boot keygen entropy source
  (hosted: OS RNG; bare-metal: which source — a flagged sub-decision, since a
  weak keygen RNG silently weakens every signature).
- **D4 — `human_ref` interaction.** Confirm the §0 reading: a signed Self entry
  carrying `human_ref` signs the NODE's authorship of the chain entry, NEVER the
  human's identity. (Recommended: yes — this is the directive.)

---

## 7. Sequencing (for the implementing wave — design only here)
1. **Primitive + KAT** (`[sign-roundtrip]`): the Ed25519 reference transcribed,
   SHA-512 beside sha256, RFC 8032 vectors as a hard gate. Nothing else trusts it
   until this is green. (Separate implementer + separate auditor — the audit IS
   the engine; for crypto the auditor independently re-runs the KATs and reviews
   the field-math transcription against the reference.)
2. **Node keypair + p-fs storage + `sign test` harness** (first-boot keygen,
   public-key publish, secret never gossips).
3. **Self-layer signed entries + TOFU** (`[sign-selflayer]`): retire the LM-2
   bound.
4. **selfc signed unit manifests + allowlist** (`[sign-unit]`): drop LOCAL-ONLY.
5. **genome signed weight manifests** (`[sign-weight]`/folded into `[sign-unit]`).
6. **Key rotation across succession** (`[sign-keyrotation]`).

Each step is its own wave-slice with its own falsifiable gate and its own
implementer≠auditor separation. The primitive (step 1) is the long pole and the
one where "honest > green" buys the most caution.
