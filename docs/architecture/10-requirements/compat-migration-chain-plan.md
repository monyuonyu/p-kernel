# compat-migration-chain — the per-version migration chain + signed-OTA gate (HARDENED design)

> Status: **SHIPPED** — reconciled 2026-07-01. The "DESIGN ONLY; changes NO kernel
> code" line below is FALSE: `arch/common/compat_ota.c`, the `lm_self_steps[]` migration
> chain, and the `compat test [ota|self|wire|arkfs]` verb (`usermain.c:1164`) are all live.
> **HONEST GAP:** the §5 "RELEASE-GATING" certs are NOT in default CI — each is compiled
> behind an `EXTRA_CFLAGS` build flag (`-DOTA_GATE_CERT` / `-DLMSELF_MIGRATE_CERT` /
> `-DNOFLEETSPLIT_CERT` / `-DARKFS_GAP_CERT` / `-DR3WP_MIGRATE_CERT`). P12 wired the
> forward-migration cert `[migrate-forward]` into CI (job `shipped-llm-certs`); the other
> four flags remain un-wired, so only forward migration is release-gated today. It turns
> the strategy DECIDED by mk_pino on 2026-06-14 into a concrete,
> certifiable MECHANISM. Honest > green: every bound the mechanism does NOT cover is flagged
> loudly in §C and §9, not hand-waved.
>
> Position: this is the **mechanism doc** under
> [compatibility.md](compatibility.md) (the strategy + the network-seam reject→degrade
> work) and [signing.md](../archive/signing.md) (the shipped Ed25519 layer this OTA gate REUSES).
> compatibility.md answers "how do mixed-version nodes stay one fleet on the WIRE";
> this doc answers "how does a node's STATE survive its OWN version changing, and how
> does it accept a signed update without being forked or downgraded."
>
> Why this is core to "a home for AI that never dies": a mind/fleet that cannot upgrade
> without forking or losing state is **not deathless** — it just postpones death to the
> next release. The migration chain is the mechanism that makes "各版が前を読める"
> (compatibility.md §DECISION) real and testable.
>
> Last updated 2026-06-22. Base: `d561d182`.

---

## 0. THE DECIDED STRATEGY (do NOT relitigate — this doc designs the mechanism for it)

mk_pino, 2026-06-14, recorded verbatim in [compatibility.md](compatibility.md) §DECISION:

> 凍る核は無い。世代交代で進化する … 唯一の現実的な不変量は「凍った形式」ではなく
> 「**鎖**」: **各版が一つ前の版を読める**（pairwise migration）。…
> **bedrock は無い、途切れない鎖があるだけ。**
>
> 一行: 凍るものは無い。**途切れない鎖（各版が前を読む）＋ 世代交代 ＋ 死を前提
> ＋ 知識は再教育で渡る ＋ 新コードだけ署名OTA（メッシュ＝小更新／プラットフォーム
> ＝深い更新）。**

This doc's job is the CONCRETE form of three pieces of that one line:
1. **the migration chain** (各版が前を読む — pairwise migration of STATE),
2. **the signed OTA gate** (新コードだけ署名OTA — reuse the shipped signing layer),
3. **no fleet split for STATE/format changes** (built on compatibility.md's wire work).

The decision is the constitution; we do not reopen it. We make it executable and falsifiable.

---

## 1. GROUND TRUTH — what already EXISTS (cited; do NOT reinvent)

Everything below was read in the tree at `d561d182`. The mechanism REUSES these; it does
not invent parallel machinery.

### 1.1 The version surface (every wire/blob already carries magic + version)
- Per-seam wire versions: `SWIM_VERSION=1` (`swim.h:52`), `DRPC_VERSION=1` (`drpc.h:67`),
  `KDDS_VERSION=1` (`kdds.h:126`), `PMESH_VERSION=1` (`pmesh.h:36`), `RAFT_VERSION=1`
  (`raft.h:28`), `PFSR_VERSION=1` (`pfs_repl.h:50`), `REPLICA_VERSION=2` (`replica.h:43`,
  already bumped — evolution is not hypothetical), `KLOAD_VERSION=1` (`kloader_task.h:19`),
  `SPAWN_VERSION=1` (`spawn.h:31`), `SFS_VERSION=1` (`sfs.h:33`).
- The mind/teach wire `MT_WIRE_VER` has ALREADY evolved `0→1→2`
  (`dtr.h:368,378,379`: LEGACY→LANG→VOCAB), proving the wire really does change.
- On-disk / state versions: `ARK_FMT_VERSION=3u` (`arkfs.c:56`, already at v3),
  `R3_WP_VER=1u` (`r3_incontext.c:810`), `LM_SELF_VER=2` (`lm_self.h:50`, v1 was 116 B,
  v2 is 148 B — a LIVE migration, §1.4), `DTR_WBLOB_VER=1` (`dtr.h:296`),
  `GENOME_VER=1` (`genome.h:42`), `ARK_PROF_VER=1` (`ark_profile.h:44`),
  `NS_STUDENT_VER=1` / `ST_BLOB_VER=1u` (`student.h:37`, `student.c:1774`),
  `RET_BLOB_VER=1` (`retrieval.h:55`), `SIGN_MANIFEST_VER=1` (`sign.h:66`).

### 1.2 How a node ANNOUNCES its version today — the observability backbone EXISTS
- `arch/common/modver.c` / `modver.h` is a **fine-grained per-module version registry**:
  ONE compile-time static table `{short-name, version}` collecting each module's own
  `<MOD>_VER` define (`modver.h:11-21`), plus `modver_build_id()` (`MODVER_BUILD` or
  `__DATE__ " " __TIME__`, `modver.h:48-51`). It is exposed at runtime as
  **`GET /modules.json`** in the galaxy HTTP face (`galaxy.c:1233-1258`).
- `modver.h:1-9` states its own purpose verbatim: *"the OBSERVABILITY side of the
  compatibility / evolution architecture … per-module versions are the migration chain
  made visible — you can SEE, at runtime, exactly which contract version each subsystem
  on this node speaks."* **This is already the version-declaration surface.** The
  migration chain plugs into it; it does not add a second one.

### 1.3 How different-version nodes interact today (the wire-negotiation precedent)
- **SWIM `_pad`→capability** (`swim.h:43-90`): a NEW field was added by reusing the
  already-zero reserved `_pad` byte of `SWIM_GOSSIP_EVT`, **without bumping
  `SWIM_VERSION`**, precisely so v1 nodes (which gate `version != SWIM_VERSION → return`)
  do not drop the packet. Old emitters send `0` = "non-capable" → safe degrade; new nodes
  ignore the field on old peers. Two `_Static_assert`s pin the 4 B entry / 24 B packet so
  the byte image cannot drift (`swim.h:84-90`). **This is the canonical
  BACKWARD-COMPATIBLE change rule** (additive, zero-default, layout-frozen).
- The contrasting hazard (still present, compatibility.md §1.2): most rx paths still do
  `version != CURRENT → return/drop` (e.g. `swim.c:285`, `replica.c:282` "v1 は黙って捨てる").
  compatibility.md §7 owns fixing those to reject→degrade; **this doc DEPENDS on that
  work for the `[no-fleet-split]` cert and does not duplicate it** (§4).

### 1.4 A LIVE migration already ships — the dual-width walker
- `LM_SELF_ENTRY` (`lm_self.h:52-66`): v1 = 116 B, v2 = 148 B (adds the `human_ref` tail).
  `lm_self.h:67-69`: *"the dual-width walker accepts BOTH (reads magic+version first, then
  size-checks per version). Old v1 stores still verify; v2 adds the human_ref tail."*
  `lm_self.c:214` checks `e.version != LM_SELF_VER` per entry.
- **This is the migration chain in miniature, already in production.** The generalized
  registry in §3 is "do exactly this, uniformly, with a named function table" — not a new
  idea, a generalization of a shipped one.

### 1.5 The signing layer the OTA gate MUST reuse (already SHIPPED, wave-38/39)
- `sign.h` / `sign.c` / `ed25519.c`: ONE Ed25519 keypair per node (`sign_node_key_ensure`,
  `sign.h:30`), an explicit per-node signer-allowlist (`sign_allow_add/has`,
  `sign.h:48-49`, `SIGN_ALLOWLIST_MAX=16`), and the **3-gate AND** verifier:
  `sign_manifest_verify(m, actual_id)` (`sign.c:167-182`) returns 1 **iff**
  (a) `m->artifact_id == actual_id` (the caller's recomputed content-id of the bytes it is
  about to use — `sign.c:176`, body-swap refused), AND (b) `sign_verify` of the Ed25519
  signature over the body succeeds (`sign.c:179`), AND (c) `sign_allow_has(m->signer_pk)`
  (`sign.c:181`, fail-closed). **The signed body is `{artifact_id || artifact_ver}`**
  (`sign.c:133-143`), so the VERSION is inside the signature — a downgrade cannot be forged
  without the key (this is the SIGN-2 lesson, already structural).
- `genome.c:307-321` ALREADY gates weight installation through `sign_manifest_verify`:
  if any signer is adopted (`genome_verify_required`, `genome.c:497-500`), a weight blob is
  installed ONLY if a companion `genome/sig` manifest verifies; an empty allowlist keeps
  legacy unsigned behaviour. **The OTA gate is this exact pattern, applied to an update
  artifact.** The OWNER BOUNDARY (`sign.h:6-11`, `signing.md §0`): a signature attests
  CODE/WEIGHT provenance ONLY, **never a human** — §B.4 keeps the OTA gate inside it.

### 1.6 The two existing distribution channels (the OTA "deep vs small" split is real)
- **KLOAD** (`kloader_task.h`): a binary is pushed over pmesh as `KLOAD_START/CHUNK`
  packets, written to `/KL.BIN`, then the node ACPI-resets so the bootstrap kloader runs the
  new image. **This is the "deep update / new loader" channel** (compatibility.md §6's
  "platform A/B + rollback" maps here on hosted/Android).
- **genome manifest + signed weight/code blob over the mesh** (§1.5) is the **"small update"
  channel** (hot-load via the running loader): weights and selfc C-source units distributed
  and verified without a reboot. The OTA gate (§4) lives primarily on this channel; KLOAD is
  the deep-update fallback and inherits the same signature requirement (§4.4).

### 1.7 The mind crown / byte-identity surface (for LENS A)
- `r_forward` (`r3_incontext.c:192`) is the mind's forward math. The "one mind, one math"
  crown is `r3_onemind_forward_hash()` (`r3_incontext.c:640`): an FNV-1a hash of a
  fixed-seed, fixed-input `r_forward` output, **gated behind `-DSMP_ONE_MIND`** so "with the
  flag OFF this function does not exist, so the DEFAULT object … is byte-unchanged"
  (`r3_incontext.c:623-625`), built `-O1 -ffp-contract=off` (`r3_incontext.c:618`). The
  comment is explicit: *"THE MIND MATH (r_forward / r_init_weights / rw[] / rc) IS
  UNTOUCHED"* (`r3_incontext.c:631`). §A keeps all migration code OUT of this surface.

---

## 2. VERSIONING MODEL — what carries a version, and how peers negotiate

### 2.1 Independent axes, NOT one number (the load-bearing decision)
There is no single "p-kernel version." Conflating the axes is the trap: a wire-only change
would needlessly invalidate every on-disk blob, and a mind-format change would needlessly
partition the membership wire. The mechanism keeps **FOUR independent version axes**, each
already present in the tree (§1.1):

| Axis | What it versions | Carrier today | Change rule |
|---|---|---|---|
| **A. Wire/protocol** | a packet's on-wire contract | per-seam `*_VERSION` | compatibility.md §2-3 (reject→degrade + envelope); BACKWARD-COMPAT = additive zero-default (SWIM `_pad` rule, §1.3) |
| **B. On-disk/state** | a persisted blob's layout | `ARK_FMT_VERSION`, `R3_WP_VER`, `LM_SELF_VER`, `DTR_WBLOB_VER`, `GENOME_VER`, … | THE MIGRATION CHAIN (§3): read-old → upgrade |
| **C. Mind format** | the model arch-spec `{E, top-k, D, n_layers, vocab_id, kernel_flags}` | content-id of the arch-spec p-fs object (compatibility.md §5.3; native-student §A.3) | content-id identity; re-education across incompatible arch (§3.4), NOT weight copy |
| **D. Artifact/build** | the running binary / update payload | `modver_build_id()` + per-module `MODVER` table (§1.2) | the OTA gate (§4): signed, legal-successor only |

A node's **version vector** is the tuple of these axes, ALREADY observable at
`/modules.json` (§1.2). "Migrating" is per-axis: a node may speak wire v2, hold state it
must migrate from disk-v1 to disk-v2, run mind arch-spec X, and be build 0.9.4 — all at once.

### 2.2 How a node declares its version + negotiates with a different-version peer
**Declaration (no new mechanism):** the `modver` table + `modver_build_id()` is the
authoritative self-description, surfaced at `/modules.json` (§1.2). For peers that cannot
fetch HTTP, the per-seam `version` byte in each packet IS the per-axis wire declaration
(§1.1) — the negotiation surface compatibility.md §2 already owns.

**Negotiation (reuse, do not invent):**
- **Wire axis (A):** the SWIM/DRPC capability-gossip byte (§1.3) is the negotiation channel.
  compatibility.md §2.1 specifies `[MIN_SUPPORTED .. CURRENT]` ranges and "speak the mutual
  highest version." This doc adds NO new handshake; it RELIES on compatibility.md's
  reject→degrade landing so that a state/mind change (axes B/C) carried over the wire is not
  dropped by a version-mismatched peer. The migration chain's `[no-fleet-split]` cert (§5)
  is the joint acceptance test for "compatibility.md's wire work + this doc's format work
  coexist."
- **State/mind axes (B/C):** negotiated at the moment of READ, not on the wire — a node that
  reads a blob/teach-payload of a different format runs the migration chain (§3) or the
  content-id arch match (§3.4). There is nothing to "negotiate" with the peer: the format
  version travels INSIDE the artifact (its magic+version header, §1.1), and the receiver
  decides locally to migrate, degrade, or refuse-and-print.

> **Design rule:** version declaration is REUSED (`modver` + per-seam byte); version
> negotiation for the wire is OWNED BY compatibility.md; this doc owns the per-axis,
> read-time migration/refuse decision for STATE and MIND format.

---

## 3. THE MIGRATION CHAIN — a state blob written by vN is readable by v{N+1} (and v{N+k})

### 3.1 The primitive: a per-axis registry of pairwise migration functions
For each on-disk/mind axis (B/C), define a **migration registry**: an ordered table of
`{from_ver → to_ver}` pure functions that transform an in-memory representation one step
forward. The shipped `LM_SELF_ENTRY` dual-width walker (§1.4) is the existing template;
this generalizes it to a named, chained table.

```c
/* design sketch — a generic per-axis migration registry. Plain C, fixed-width,
 * no allocation: each step transforms a caller-owned scratch buffer in place.
 * (concrete signatures are an impl concern; this fixes the SHAPE + invariants.) */
typedef struct {
    U4   from_ver;
    U4   to_ver;            /* MUST be from_ver + 1 (chain is pairwise, §3.2) */
    /* upgrade *buf (size *len) from from_ver layout to to_ver layout in place;
     * fill any new field with its documented default; return 1 ok / 0 refuse */
    INT (*migrate)(void *buf, UW *len);
} MIGRATE_STEP;

/* one table per axis, e.g. r3_wp_steps[], lm_self_steps[], arkfs_steps[], ... */
```

**Load path (read-old → upgrade), per axis:**
1. Read the blob's `magic` + `version` header (every blob already has both, §1.1).
2. If `version == CURRENT`: load directly (today's fast path).
3. If `MIN_MIGRATABLE <= version < CURRENT`: apply `migrate` steps in sequence
   `version → version+1 → … → CURRENT`, then load. This is the **chain**: vN→v{N+1}→…→vK.
4. If `version < MIN_MIGRATABLE` (a fossil too old to migrate, §C.3) OR `version > CURRENT`
   (a future blob this build does not understand): **refuse safely** — leave existing state
   untouched, print ONE honest line (the R3_WP precedent, `r3_incontext.c:105`,
   compatibility.md §5.1's "黙って消す is forbidden"), and (for state) fall back to lazy
   re-init / re-learn rather than corrupt.

> **Invariant (the whole point):** the migration chain NEVER discards readable data silently.
> A blob is upgraded (data preserved, new fields defaulted) or refused-with-a-printed-reason.
> "Refuse" is a visible, recoverable event, never a silent corruption.

### 3.2 Pairwise + chained — why each step is exactly `+1`
Each registered step migrates `N → N+1` ONLY. A vN→v{N+3} load runs three steps in sequence.
This is the deathless property's mechanism: an implementer adding a new format writes ONE new
step (the v{K-1}→vK function) and the chain composes it with all prior steps automatically —
they never write or test the N±k cross product. (Honest bound on chain LENGTH = §C.3.)

### 3.3 The four state classes the chain must cover (concrete)
- **Mind weights `R3_WP` (`r3_incontext.c:790-918`):** today a version/dims/vocab mismatch
  triggers lazy re-init (lossy: the mind re-learns from teach). The chain UPGRADES the
  *format* (e.g. add a header field) losslessly via `r3_wp_steps[]`; a genuine *dims/vocab*
  change stays a refuse-and-re-learn (compatibility.md §5.2 — "次元が 1 違えば数値が壊れる"
  is a content change, not a format step). The `[migrate-forward]` cert (§5) draws the line:
  a pure FORMAT bump must preserve the taught fact; a dims change legitimately re-learns.
- **Engram / retrieval (`RET_BLOB_VER`, `DTR_WBLOB_VER`):** same registry; a new engram field
  defaults on upgrade, the ring's content is preserved.
- **Self-lineage (`LM_SELF_VER`, `lm_self.h:50`):** the dual-width walker (§1.4) IS the v1→v2
  step; formalize it as `lm_self_steps[]`. The migration MUST preserve the hash chain: after
  upgrading entry layout, the re-walked chain still verifies prev-links AND (for signed
  chains, §1.5) still verifies the per-entry Ed25519 signature against the TOFU-pinned origin
  key. **A migration that breaks lineage verification is a failed migration, not a format
  bump** — this is the sharpest `[migrate-forward]` assertion (§5).
- **On-disk arkfs (`ARK_FMT_VERSION=3u`, `arkfs.c:561`):** today `super_valid` CLEAN-REJECTS
  on version mismatch (`arkfs.c:561`) — correct for a corrupt/foreign image, but it is NOT a
  migration. The chain adds an explicit `arkfs_steps[]` so a v{N} log image is *re-read and
  re-committed* into v{N+1} on mount (the epoch bump at `ark_format` already isolates
  generations, `arkfs.c:572-580`). HONEST: arkfs is the hardest axis (it is a crash-safe log,
  not a flat blob) — §C.3 / §9 flag the deep-version-gap and write-back risks here.

### 3.4 Mind FORMAT (axis C) — re-education, NOT weight translation (the DECISION's reduction)
compatibility.md §DECISION 訂正② is explicit and this doc obeys it: **an architecture change
does NOT require weight migration.** A node running a new arch-spec is a "baby" that re-learns
from the collective (Path E). So axis-C "migration" is:
- **Identity by content-id:** the arch-spec `{E, top-k, D, n_layers, vocab_id, kernel_flags}`
  is a content-addressed p-fs object (compatibility.md §5.3-1; native-student §A.3). Same id =
  compatible (merge via Path W / Fisher, regions.md); related ancestor/descendant id = attempt
  the function-preserving translation (native-student §A.4, the `[grow-preserves]` cert that
  doubles as a compat cert, compatibility.md §5.4); UNRELATED id = REFUSE + print (the
  `vocab_fp` gate generalized, `MT_WIRE_VER_VOCAB` precedent `dtr.h:379`).
- **The fallback that always works:** generational distillation — an old-arch node teaches a
  new-arch node by soft-target output (arch-independent), compatibility.md §5.3-4. So even when
  format migration is impossible, KNOWLEDGE migrates by re-education. **This is why the fleet
  never has to migrate mind WEIGHTS to upgrade the mind FORMAT.**

> The migration chain (§3.1-3.3) is for STATE blobs whose data must be preserved bit-for-bit
> across a format change. The mind FORMAT (§3.4) preserves *function/knowledge*, not bytes,
> by content-id matching + re-education — a different, cheaper invariant that the DECISION
> chose deliberately.

---

## 4. THE SIGNED OTA GATE — accept an update iff signature verifies AND version is a legal successor

### 4.1 An update artifact is a signed manifest (REUSE `sign_manifest_verify`)
An OTA payload (a code unit, a weight blob, or — deep update — a binary image) is distributed
as bytes plus a `SIGN_MANIFEST` companion (`sign.h:68-76`), exactly as genome already does for
weights (§1.5). The receiving node ACCEPTS the update **iff `sign_manifest_verify(m, actual_id)`
returns 1** — i.e. the 3-gate AND of (recomputed content-id match) AND (Ed25519 signature valid)
AND (signer in this node's allowlist). **No new crypto is invented**; this is the shipped path
generalized from "weights" to "update artifact."

### 4.2 Legal-successor check — the ADDED gate on top of the 3-gate AND
The signature alone is not enough: a correctly-signed OLD version must not be installable over
a new one (downgrade attack), and the artifact's claimed version must be a successor of the
running one. The OTA acceptance predicate is a **4-gate AND**:

```
ACCEPT(update) iff
   sign_manifest_verify(m, actual_id) == 1            /* 3-gate AND, §4.1 (sign.c:167) */
   AND m->artifact_ver  >  running_ver                /* strictly forward: NO downgrade */
   AND legal_successor(running_ver, m->artifact_ver)  /* the chain can reach it (§4.3) */
```

- **No downgrade:** `m->artifact_ver > running_ver`. Because the signed body is
  `{artifact_id || artifact_ver}` (`sign.c:133-143`), the version is *inside* the signature —
  an attacker cannot relabel an old signed artifact as a newer version without the key. A
  *replay* of a genuinely-old signed manifest is refused by the `>` test. (This is exactly the
  SIGN-2 lesson — "the version binds the seq so an old signed manifest cannot be replayed
  against a new artifact id," `sign.h:80-82` — applied to OTA.)
- **No body-swap:** the `actual_id == m->artifact_id` gate (`sign.c:176`) means the bytes the
  node is about to install MUST hash to the signed id — a tampered body is refused.
- **legal_successor:** the new version is reachable by the migration chain (§3.2) from the
  running one (or, for the binary/build axis, is a recognized next build). This couples OTA to
  the migration chain: a node will not accept an update whose STATE it could not then read.

### 4.3 The trust anchor on a fresh node
- The node generates its own keypair at first boot (`sign_node_key_ensure`, `sign.h:30`).
- It accepts updates ONLY from signers it has **adopted** (`sign_allow_add`, the human act
  `selfc adopt <key>`, `signing.md §3.2`). **A fresh node with an empty allowlist accepts NO
  OTA** (fail-closed) — matching genome's "empty allowlist keeps legacy behaviour" but, for
  OTA, "legacy behaviour" = "do not auto-update from the mesh." The first trust anchor is
  always an explicit operator adoption; there is no implicit/automatic trust (NOT a PKI/CA —
  `signing.md §6.2`).
- The deep-update (KLOAD/platform) channel's anchor is the same: the platform store's signed
  image must verify against an adopted key before the ACPI reset runs it (§4.4); on Android the
  Play/F-Droid signing is the outer anchor and the in-payload `SIGN_MANIFEST` is the inner one.

### 4.4 Deep updates (KLOAD) inherit the gate
KLOAD today (§1.6) writes `/KL.BIN` and resets with NO signature check. The mechanism REQUIRES:
a KLOAD image is committed to `/KL.BIN` and the reset is armed ONLY after the same 4-gate AND
(§4.2) passes against the image's `SIGN_MANIFEST`. An unsigned or non-successor or
wrong-key image is refused and the node keeps running the old binary. (This closes the
`genome.md §5` "no signature on the manifest" honest gap for the binary-push path too.)

---

## 5. THE CERTS (falsifiable, non-vacuous — disease → cure, each with its falsifier)

Each cert emits printed evidence then a canonical `[tag] PASS/FAIL` line, run by a `compat test`
(state/OTA) verb alongside the existing `sign test` / `self test` harness. implementer ≠
auditor (the audit IS the engine); the **auditor writes the harness**, the commander reads the
gate predicate line-by-line (the validator-trap lesson). Two-version builds use `git worktree`
to build a vN and a v{N+1} binary and run them in one loopback/relay cluster (compatibility.md
§4.2's established pattern, reused).

### 5.1 `[migrate-forward]` — state written by vN loads functionally-intact under v{N+1}
- **Cure (PASS):** write a state blob (R3 weights with a TAUGHT FACT; AND a Self-lineage of ≥3
  entries) in the vN format. Load it under v{N+1} via the migration chain (§3). Assert the mind
  still answers the taught fact correctly AND the Self-lineage still verifies its hash chain
  (and, for signed chains, its per-entry signatures, §3.3).
- **Falsifier (must go RED):** skip the migration step (load the vN blob with the v{N+1} reader
  directly). The vN blob is misread → the fact is lost / the chain fails to verify → FAIL.
  *(Non-vacuous because the falsifier is a real misread, not a no-op: §1.4's dual-width walker
  proves a wrong width genuinely corrupts the read.)*

### 5.2 `[signed-ota-gate]` — a legal signed successor ACCEPTED; tamper/wrong-key/downgrade REFUSED
- **Cure (PASS):** a correctly-signed update whose `artifact_ver > running_ver` and is a legal
  successor, signed by an ADOPTED key, is ACCEPTED and installed.
- **Falsifiers (each must go RED):**
  - **tampered body:** flip one byte of the artifact (id no longer matches) → REFUSED (`sign.c:176`).
  - **wrong key:** sign with a NON-adopted key → REFUSED (`sign.c:181`).
  - **downgrade:** present a correctly-signed artifact whose `artifact_ver <= running_ver` →
    REFUSED (§4.2's `>` test). This must be a SEPARATE assertion (a correctly-signed-but-OLD
    artifact is the subtle one — the cert must cover all paths, the cert-coverage lesson).
  - **gate disabled (the master falsifier):** stub the verify to always-true → a tampered OTA is
    accepted → FAIL. Proves the gate is load-bearing, not decorative.

### 5.3 `[no-fleet-split]` — a vN and a v{N+1} node form ONE cluster + (backward-compat) share the mind
- **Cure (PASS):** boot a vN and a v{N+1} node into one SWIM cluster; both see each other ALIVE
  (membership crosses the version gap, §1.3 + compatibility.md §2.3). For a BACKWARD-COMPATIBLE
  change (additive zero-default field, the SWIM `_pad` rule, §6), teach on one and the other
  answers (the mind is still shared).
- **Falsifier (must go RED → then the design REQUIRES the shim):** introduce a BREAKING change
  with no shim (e.g. a wire-layout change that bumps the seam version with the old hard-drop
  rule) → the two nodes drop each other → PARTITION (two islands) → FAIL. The cert's PASS
  condition is then the proof that the change was made backward-compatibly (or a translation
  shim was added); a breaking change that partitions the fleet is a REJECTED change, by gate.
- **Dependency (stated honestly):** the PASS leg depends on compatibility.md §7's reject→degrade
  landing in the rx paths. Until that lands, this cert PASSES only for the SWIM-capability-style
  additive change and the falsifier (breaking change partitions) is the live demonstration of
  WHY §7 is release-blocking. This cert is the joint acceptance test for both docs.

> Gate set: `[migrate-forward]` and `[signed-ota-gate]` are RELEASE-GATING for any wave that
> bumps a state/mind format or ships an OTA path. `[no-fleet-split]` is gating for any wire/format
> change and is shared with compatibility.md's ledger row.

---

## A. LENS A — byte-identity / crown (the shipped kernel + crown must be unaffected)

Versioning the mind FORMAT is determinism-adjacent, so the gating is explicit:

- **The migration code is a HOSTED/LOADER concern, never in `r_forward`.** All §3 migration
  functions, the §4 OTA gate, and the version registry run in the persistence/loader/net path
  (the `_TK_HOSTED_LIBC_` durable seam that `r3_incontext.c:790+` already isolates, and the
  genome/kloader tasks) — they transform bytes *before* they reach `rw[]`. `r_forward`
  (`r3_incontext.c:192`) reads `rw[]`/`rc`; it never parses a version, never runs a migration.
  **The mind math stays exactly as `r3_incontext.c:631` declares it: UNTOUCHED.**
- **The crown is unaffected by construction.** `r3_onemind_forward_hash()`
  (`r3_incontext.c:640`) re-inits weights from a FIXED seed each call (`r_init_weights(0xA5A5u)`,
  `r3_incontext.c:650`) and hashes a FIXED input — it never reads migrated/persisted state, so
  no migration code can change the crown hash. It is `-DSMP_ONE_MIND`-gated and *does not exist*
  in default builds (`r3_incontext.c:623-625`), so the default object is byte-identical
  regardless of any migration code added elsewhere.
- **Gating rule for the implementer:** the migration/OTA modules must compile into the hosted +
  loader TUs only; they MUST NOT be `#include`d into, or linked ahead of, `r_forward`'s TU path
  in a way that perturbs default-build codegen. A `nm`/crown-hash tripwire (the wave-27 pattern:
  assert `r3_onemind_forward_hash()` is unchanged across the migration wave) is the acceptance
  check that LENS A held. **If a migration touches `r_forward`'s object, the wave is wrong.**

---

## B. LENS B — security (downgrade, version-confusion, compromised key, the human boundary)

- **Downgrade attack:** covered by §4.2's `m->artifact_ver > running_ver` AND the version being
  *inside* the signature (`{artifact_id||artifact_ver}`, `sign.c:133-143`). A signed-but-old
  artifact replayed as "new" fails the `>` test; relabeling it fails the signature. `[signed-ota-gate]`
  asserts the downgrade leg explicitly (§5.2).
- **Version-confusion (a node lies about its version to dodge a migration):** the format version
  travels INSIDE the artifact's magic+version header and (for OTA) inside the signed body — a node
  cannot make its OWN blob be read as a different version without re-writing the header, and an
  OTA artifact's version is signature-bound. For the WIRE axis, a node lying about its
  `MIN_SUPPORTED` to force a peer onto a weaker path is the classic downgrade-negotiation attack;
  this doc INHERITS that exposure from compatibility.md §8-5 and does NOT solve it here — it is
  flagged (the negotiation byte is not yet signed). HONEST: signing the capability/version
  advertisement is OUT OF SCOPE for this slice (it touches the membership wire, compatibility.md's
  territory) and is named in §9.
- **Malicious OTA signed by a COMPROMISED adopted key — REVOCATION:** if an adopted signer's key
  is stolen, the attacker can produce updates the 4-gate AND accepts. **Revocation is WEAK in v1,
  by inheritance from `signing.md §6.2`:** removing a key from the allowlist stops FUTURE
  acceptance, but there is no fleet-wide revocation broadcast / CRL, and an already-installed
  update is not retroactively un-installed (the germ-reap / A-B-rollback boundary is the runtime
  containment, not signing). This is stated, NOT solved — §9. The only honest v1 mitigation:
  adopt as few signers as possible, and rely on the A/B + rollback platform path (compatibility.md
  §6) to recover a node that took a bad update.
- **The human boundary (owner directive, MUST hold):** the OTA gate signs/verifies CODE and
  WEIGHT provenance ONLY — never a human (`sign.h:6-11`, `signing.md §0`,
  `feedback_ark_no_identity_verification.md`). An adopted KEY is a NODE's key, not a person's.
  The OTA gate MUST NOT introduce any "verified author handle," "signed identity," or human
  attestation — if a future wave proposes it, strike it (the ark stays declared-not-verified
  forever). Said twice: **a signed update attests an artifact and a key, never a human.**

---

## C. LENS C — the deathless property (does the chain truly prevent state loss?)

- **What the chain GUARANTEES:** any single-step upgrade (vN→v{N+1}) preserves state functionally
  (`[migrate-forward]`, §5.1), and multi-step upgrades compose pairwise (§3.2), so a node N steps
  behind catches up by running N migrations in sequence — IF every intermediate step is registered.
- **The honest LIMIT — chain length / deep-version gap:** a node offline for K versions migrates
  only if ALL of `vN→v{N+1}, …, v{N+K-1}→v{N+K}` steps still exist in the new binary. There is a
  real point where the chain MUST break: when a build drops `MIN_MIGRATABLE` above an ancient
  blob's version (to bound the migration code's size and avoid carrying every step forever). At
  that boundary, an ancient blob is **refused-and-printed** (§3.1 step 4), NOT silently corrupted —
  but its *bit-for-bit state is lost*. **This is an honest bound, not a bug:** the DECISION's
  answer is that KNOWLEDGE survives via the collective (Path E re-education, §3.4) and IDENTITY
  survives via the Self-lineage (which is itself migrated, §3.3) — so a deep-gap node re-learns as
  a baby rather than dying. The chain prevents *silent* loss always; it prevents *bit-for-bit*
  loss only within `[MIN_MIGRATABLE .. CURRENT]`. State that bound loudly in any release note.
- **arkfs is the sharpest case (§3.3):** it is a crash-safe LOG with an epoch generation, not a
  flat blob; a deep-gap arkfs image may be cleanly rejected (today's `super_valid`, `arkfs.c:561`)
  rather than migrated, forcing a reformat. The migration step for arkfs (re-read + re-commit into
  the new epoch) is the highest-risk implementation item — §9.
- **Write-back lock-in (compatibility.md §5.1 / open #4):** once a node upgrades a blob and writes
  it back in the new format, an older-version node can no longer read that store. During a rolling
  upgrade this is fine (the fleet moves forward); but a node that must REMAIN interoperable with
  older peers should defer write-back or keep the old format as a content-addressed companion. The
  mechanism flags this per-axis (it has no single answer) — §9.

---

## 9. SCOPE / DEFERRALS (honest) + OPEN RISKS FOR THE IMPLEMENTER

### 9.1 Explicitly IN scope (this doc's mechanism)
- The per-axis migration registry (§3) for R3 weights, engram/retrieval, Self-lineage, arkfs.
- The 4-gate OTA acceptance predicate (§4) reusing `sign_manifest_verify`, on the mesh
  (small-update) and KLOAD (deep-update) channels.
- The three certs (§5) with their falsifiers, and the LENS-A crown gating.

### 9.2 Explicitly DEFERRED / OUT of scope (named, not solved)
- **Revocation / compromised-key recovery (LENS B):** weak in v1, by inheritance from
  `signing.md §6.2`. No CRL, no fleet revocation broadcast, no retroactive un-install. The A/B +
  rollback platform path is the only recovery. A future "signed revocation list as a content-
  addressed object with a monotonic epoch" is named-not-designed here.
- **Signing the version/capability advertisement (downgrade-negotiation, LENS B):** the wire
  negotiation byte is unsigned; a lying-peer downgrade attack on the membership wire is
  compatibility.md §8-5's open problem, not solved here.
- **The deep-version-gap bound (LENS C):** `MIN_MIGRATABLE` is a per-build decision; bit-for-bit
  state below it is lost (knowledge/identity survive by re-education + lineage). Not a bug; a
  stated bound.
- **arkfs deep-gap migration vs reformat (§3.3 / §C):** the migration step for the crash-safe log
  is the hardest item; v1 may legitimately ship "clean-reject + reformat" for arkfs while the flat
  blobs (R3/engram/Self) get true migration — but say so honestly.
- **Mind WEIGHT translation across arch (§3.4):** deliberately NOT done — the DECISION reduces it
  to re-education. The `[grow-preserves]` function-preserving translation (native-student §A.4) is
  the only weight-level path and is its own wave.

### 9.3 Open risks for the implementer
1. **Migration must preserve the Self-lineage hash chain AND its signatures (§3.3).** The single
   highest-value, easiest-to-break assertion: a layout migration that re-serializes entries must
   reproduce content-ids that still chain AND still verify against the TOFU-pinned key. Build the
   `[migrate-forward]` Self-lineage leg FIRST and watch it.
2. **LENS A is a codegen hazard, not just a code-placement rule.** Adding the migration module must
   not perturb `r_forward`'s default-build object. Use the crown-hash tripwire (§A) as the wave's
   acceptance check; if the default object changes, the include/link graph is wrong.
3. **The OTA downgrade leg is the subtle cert (§5.2).** A correctly-signed-but-OLD artifact is the
   one a naive gate accepts. Make it a separate assertion; cover the path.
4. **Write-back lock-in during rolling upgrade (§C).** Decide per-axis whether to defer write-back;
   a node that upgrades-and-writes silently strands older peers on that store.
5. **arkfs migration is a log, not a blob (§3.3).** Re-reading + re-committing into a new epoch is
   subtle under crash-safety; treat it as its own slice and do not block the flat-blob migration on
   it.
6. **Two-version test harness drift.** The `git worktree` two-binary harness (§5) must build the
   vN binary from a PINNED commit, not from "current minus a define" — a fake vN that is really vK
   with a flag silently makes `[migrate-forward]` / `[no-fleet-split]` vacuous.

---

## THE FIRST IMPL SLICE (smallest real cert to ship first)

**Ship `[migrate-forward]` for ONE flat blob — the R3 mind weights (`R3_WP`) — as a real
two-version migration with a taught-fact assertion.**

Why this first: it is the smallest slice that proves the CHAIN mechanism end-to-end on the
deathless property's core asset (the mind), with a non-vacuous falsifier already grounded in the
shipped dual-width walker (§1.4):
1. Define `r3_wp_steps[]` with ONE step (v1→v2) that adds a header field with a default (§3.1).
2. Write a v1 `R3_WP` blob carrying a TAUGHT FACT; load it under v2 via the step; assert the fact
   still answers. Falsifier: skip the step → wrong-width read → fact lost → RED.
3. Wire it as `compat test` `[migrate-forward]` in the existing self-test harness; gate it.

It needs NO new crypto, NO wire change, NO arkfs log surgery — it exercises the registry shape,
the read-old→upgrade path, and the falsifier, on real mind state. The Self-lineage leg (§9.3-1)
and the `[signed-ota-gate]` (reusing the already-shipped `sign_manifest_verify`) are the next two
slices; `[no-fleet-split]` lands jointly with compatibility.md §7's reject→degrade work.
