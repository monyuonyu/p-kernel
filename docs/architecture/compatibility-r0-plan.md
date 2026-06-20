# Compatibility R0 — interop cert + forward-compatible framing: implementation plan (cert-first)

> Status: **DESIGN PLAN** by an automated design-harden on trunk `95b20899` (`degrade: honest label on cap_experts_of …`). Read-only on code; no edits made. Awaiting commander review + a separate impl→audit cycle (implementer ≠ auditor ≠ commander — the project's immune system).
>
> Parent design DRAFT: `docs/architecture/compatibility.md` (DECISION 2026-06-14: 凍る核は無い — generational succession + 途切れない鎖 + 死を前提 + 知識は再教育で渡る + 新コードだけ署名OTA). Memory: `project_compat_evolution`.
>
> Discipline note (2026-06-20 review lesson): this plan LEADS with a falsifiable cert, names `[in-proc]` vs `[live]` for every arm, ships the SMALLEST real slice, and does NOT defer any load-bearing half behind grand prose. Where the cert starts RED on today's code, it says so — that red IS the finding.

---

## 0. The one-paragraph truth

Today every network seam does `if (version != CURRENT) return;` — a **hard reject** on mismatch (verified below, file:line). Ship a v2 onto a never-dying fleet and v1↔v2 **both silently drop each other** → the one mesh hard-forks into a v1 island and a v2 island, and with no central authority they **never re-join**. The DECISION says "no frozen core; evolve by an unbroken chain where each version reads the previous one." The cheapest falsifiable proof of that decision is a cert that **builds two versions and makes them actually talk**. On today's reject-on-mismatch code **that cert starts RED** — and that red is the whole point: it pins the exact gap R1 must close. R0 is: (a) write the failing interop cert (the falsifiable gate), and (b) ship the smallest forward-compat retrofit that is *physically impossible to add later* — the future-tolerant receive path.

---

## 1. Ground truth (verified on `95b20899`, file:line)

### 1.1 The reject pattern is uniform across all UDP seams

Every receive handler is the same two lines: a minimum-length gate, then a version equality reject. (The DRAFT cited older line numbers from a different base; these are the **real** lines on `95b20899`.)

| seam | version constant (def) | rx reject site (verified) |
|---|---|---|
| SWIM membership | `SWIM_VERSION 1` — `arch/common/include/swim.h:52` | `arch/common/swim.c:415` `if (len < sizeof(SWIM_PKT)) return;` → `:418` `if (magic!=.. \|\| version!=SWIM_VERSION) return;` |
| DRPC heartbeat | `DRPC_VERSION 1` — `arch/common/include/drpc.h:67` | `arch/common/drpc.c:349` len gate → `:352` version reject |
| K-DDS pub/sub | `KDDS_VERSION 1` — `arch/common/include/kdds.h:126` | `arch/common/kdds.c:419` len gate → `:422` version reject |
| raft | `RAFT_VERSION 1` — `arch/common/include/raft.h:28` | `arch/common/raft.c:174` len gate → `:177` version reject |
| pfs replication | `PFSR_VERSION 1` — `arch/common/include/pfs_repl.h:50` | `arch/common/pfs_repl.c:407` len gate → `:410` magic → `:411` version reject |
| replica | `REPLICA_VERSION 2` — `arch/common/include/replica.h:43` | `arch/common/replica.c:278` len gate → `:282` `version!=REPLICA_VERSION` `/* v1 は黙って捨てる */` |
| pmesh | `PMESH_VERSION 1` — `arch/common/include/pmesh.h:36` | (no dedicated rx grep hit; carried by drpc/swim) |
| kloader | `KLOAD_VERSION 1` — `arch/common/include/kloader_task.h:19` | `arch/common/kloader_task.c` |
| **mind/teach wire** | `MT_WIRE_VER_VOCAB 2` — `arch/common/include/dtr.h:379` | `arch/common/r3_incontext.c:2707` `if (wire_ver != MT_WIRE_VER_VOCAB)` → **drop + print** (the good precedent) |
| storage: weights | `R3_WP_VER 1` — `arch/common/r3_incontext.c:706` | `r3_incontext.c:799` magic+version+dims gate → `:804` "stale … (dims/vocab mismatch)" reinit |

**Two facts this table proves.** (1) The version byte already exists on every seam — "change the format later" was *anticipated*. (2) The *policy* on every seam except the mind-wire is **silent hard reject** — that is the hard-fork landmine.

### 1.2 The good precedents the design copies (do not reinvent)

- **MT_WIRE_VER drop+print** (`r3_incontext.c:2701-2720`): the ONE seam that already makes a version partition **observable** rather than silent — it prints `[mind] … wire_ver mismatch … packet DROPPED (version-partitioned region)` and emits `[lang-wire-verdrop] PASS`. R0/R1 keep this print culture and flip "drop" → "degrade".
- **N-2b reserved-byte trick** (`arch/common/include/swim.h:84-104`): `SWIM_PKT` is 24 bytes packed; a capability bit was added by **reusing the reserved `_pad`**, keeping the on-wire layout byte-identical, enforced by `_Static_assert(sizeof(SWIM_PKT)==24,…)` and `_Static_assert(sizeof(SWIM_GOSSIP_EVT)==4,…)`. This is the worked example of "additive, no version bump, old nodes safe-degrade" the whole forward-compat story generalizes. The static-assert tripwire pattern is the model for §3.
- **R3_WP dims/vocab gate** (`r3_incontext.c:799-804`): a *correct* refuse — weights of a different shape are not silently loaded; they are reinitialized with a printed reason. The contract is **not** "accept everything"; it is "degrade what is safe, **refuse with a print** what would corrupt." Keep this gate; it is the model for content-id refusal.

### 1.3 What is missing (the gaps R0–R3 close)

- No `[MIN_SUPPORTED..CURRENT]` accept range — only a single `CURRENT` equality.
- No forward-compatible framing. Packets are fixed packed structs (`SWIM_PKT` 24B). The receive gate is `if (len < sizeof(PKT)) return;` — it does **not** reject `len > sizeof(PKT)`, but nothing *uses* a tail either, and any struct-size change forces a version bump that the reject path then drops.
- No interop cert: nothing builds two versions and asserts they interoperate.

### 1.4 The two-version build substrate already exists

`samples/41_shared_mind/run.sh` is the template for the `[live]` arm: it builds `boot/linux` (and/or `boot/linux_x86_64`), starts the mesh through `relay/relay`, gives each node a **distinct `PKERNEL_PFS_DIR`**, and already honors a **`PKERNEL_BOOT_DIR`** override so node A and node B can run *different build directories* (used today for the aarch64+x86_64 cross-arch mesh). That override is exactly the seam an old↔new two-version harness rides. The version constants are plain `#define` (`swim.h:52` etc.), **not** `#ifndef`-guarded — so the second ("future") build cannot be made with a bare `-DSWIM_VERSION=2`; see §2.3 for the mechanism choice.

---

## 2. R0 — the smallest FALSIFIABLE interop slice

### 2.1 What R0 ships (two things, both small)

1. **The interop cert** `samples/46_compat_interop/compat_cert.sh` — builds an OLD binary and a "future" NEW binary (one version constant bumped) and asserts they interoperate. On today's reject code, the PASS arms **START RED**. That is honest and intended.
2. **The forward-compatible receive path** (§3) — the one retrofit that cannot be added after release. Small (single-digit lines per seam at the receive gate + one frozen envelope-header definition). This is the genuinely time-sensitive piece.

R0 deliberately does **not** ship the negotiate logic, the TLV ext area, capability handshake, or persisted-blob upgrade — those are R1/R2/R3 and are non-destructive once the §3 retrofit is in. Smallest real slice.

### 2.2 The `[compat-interop]` cert — exact tags and falsifiers

The cert builds **two binaries from the same tree** (see §2.3) and runs them as a 2-node mesh. PASS = they interoperate; the falsifier arm = a *deliberately incompatible* bump is correctly **refused with an honest print, never a crash/hang**.

| tag | asserts (PASS) | falsifier (FAIL) | arm |
|---|---|---|---|
| `[compat-mesh]` | old + new in one cluster each see the other ALIVE | either side DROPs the other → two islands | `[live]` |
| `[compat-teach]` | old teaches a fact (`sun→yellow`, same vocab/`vocab_fp`); new answers it | version skew drops the teach → no answer | `[live]` |
| `[compat-persist]` | new boots, reads a blob the OLD binary wrote, answers across the gap | new reject/discards the old blob | `[in-proc]` (single boot, two PFS dirs) |
| `[compat-degrade]` | new feeds old a packet with `proto_ver = CURRENT+1`; old reads the legacy core, does not crash/drop | hard-drop or panic | `[in-proc]` self-test preferred; `[live]` if framed as a 2-proc arm |
| `[compat-refuse-honest]` (the FALSIFIER) | a *truly* incompatible artifact (e.g. `R3_WP` with mismatched dims, or `MT_WIRE_VER` skew) is **refused with a printed reason**, process stays alive | silent discard, OR a crash/hang instead of a clean refuse-print | `[live]` |

`[compat-refuse-honest]` is load-bearing: the contract is "don't silently fork," **not** "merge anything." It certifies that the *correct* refusals (dims mismatch at `r3_incontext.c:804`, wire skew at `r3_incontext.c:2707`) remain loud and non-fatal.

### 2.3 How to build "two versions" (the mechanism decision)

Three options; the plan recommends **(B)** for R0 and flags it for commander sign-off.

- **(A) second worktree, edit one `#define`.** `git worktree add` a copy, change `SWIM_VERSION 1`→`2` in `swim.h:52`, build its `boot/linux`, point `PKERNEL_BOOT_DIR` at it. Zero source change to trunk. Fragile: the harness must keep the two trees in sync and rebuild both; awkward in CI.
- **(B) make the version constants `-D`-overridable, then bump via a CFLAG.** Wrap each constant: `#ifndef SWIM_VERSION` / `#define SWIM_VERSION 1` / `#endif`. The harness builds the NEW binary into a throwaway dir with `make CC='cc -DSWIM_VERSION=2'`. This is a tiny, safe, *additive* trunk change (default behavior identical) and makes the cert self-contained in one tree. **Recommended.** It also documents, in code, *which* constants are the version seams.
- **(C) a single binary with a `PKERNEL_COMPAT_FAKE_VER` env knob** that, when set, makes the rx path *pretend* to be a future version on TX. Cheapest to run but it certifies a fake, not a real second build — weakest evidence. Reject for the gate; acceptable only as a fast `[in-proc]` smoke during development.

> **Q for commander (Q-R0):** approve (B) — adding `#ifndef` guards around the ~8 version `#define`s purely so the cert can fork one constant. It changes no default behavior (still defaults to 1/2) but it does touch the headers. The alternative (A) keeps trunk untouched at the cost of a two-tree harness.

### 2.4 Honest expectation: the cert starts RED

On `95b20899`, with `swim.c:418` doing `version != SWIM_VERSION → return`, the `[compat-mesh]` and `[compat-teach]` arms **FAIL** — old and new drop each other. That is correct and desired: R0 lands the cert as a **new OPEN row in `gap-ledger.md`** (a known-red gate), exactly as the DRAFT §4.3/§10 prescribes. R1 turns it green. A gate that is green the day it lands proves nothing; this one earns its green.

`[compat-degrade]` may *partially* pass today only by luck: the len gate is `len < sizeof(PKT)` (not `!=`), so a longer future packet whose first bytes are a valid current packet would currently parse — but a *bumped version byte* still trips `:418` and drops. So even degrade is red until R1. Document the measured starting color in the cert header.

---

## 3. The one thing you can't add later — forward-compatible framing

This is the time-sensitive piece. If the **initial released binary** hard-drops a future version or a longer packet, then no future version can ever reach it — the gap is *physically* unclosable post-release (DRAFT §0, §7.1, §8-1). R0 must retrofit the receive side so old nodes *tolerate the future*. Two layers, cheapest first.

### 3.1 R0 minimum — accept-longer + read-future-core + print-fossil (the few-line change)

At each rx site in §1.1, replace the two-line reject with three honest cases. Concretely, for SWIM (`swim.c:415-418`), the shape becomes:

- keep `if (len < sizeof(SWIM_PKT)) return;` (minimum-length floor — still required);
- **do not** reject `len > sizeof(SWIM_PKT)` — read the legacy core, ignore the tail (reserved-tail forward-compat; DRAFT §3.3);
- replace `version != SWIM_VERSION → return` with:
  - `version == CURRENT` → parse as today;
  - `MIN_SUPPORTED <= version < CURRENT` → parse with the old layout (R1 fills this in; for R0 with a single live version it is a no-op placeholder);
  - `version > CURRENT` → **read the legacy core anyway** (membership/heartbeat must survive) — do NOT hard-drop;
  - `version < MIN_SUPPORTED` → `print` a fossil notice and drop (honest, loud).

Membership is the lifeline (DRAFT §2.3/§6): even across a version gap, SWIM/DRPC must still mark the peer ALIVE. That single property is what prevents the island split.

This is **per-seam single-digit lines** at one site each (the rx functions enumerated in §1.1) — the DRAFT's §7.3 "cheapest decisive first step."

### 3.2 R0 also freezes the envelope header bytes (define-only, no wiring yet)

Define — but do not yet *emit* — the frozen envelope header from DRAFT §3.1, so its byte layout is nailed before release:

```
u32 magic       (reuse each seam's existing leading magic, e.g. SWIM_MAGIC)
u8  env_ver     = 1   (frozen forever)
u8  proto_ver   (= the existing per-seam VERSION)
u16 core_len    (sizeof legacy struct)
u16 ext_len     (0 = no extension)
u16 _rsv        (0)
```

Plus a `_Static_assert` on the header size (mirroring the N-2b `SWIM_PKT==24` tripwire at `swim.h:101`). R0 ships only the **definition + static-assert**; the TLV ext area and actually wrapping packets is **R2** (§5). Freezing the *byte order now* is what keeps a future env-ver negotiable; it costs ~one struct + one assert.

### 3.3 Which structs need what (R0 scope vs deferred)

- **Lifeline seams (R0 receive-path retrofit required):** `SWIM_PKT` (membership) and `DRPC_PKT` (heartbeat). If these two tolerate the future, the fleet cannot split on liveness — the irreversible property is secured.
- **Region/data seams (R0 receive-path retrofit *recommended*, same few lines):** `KDDS`, `MT_TEACH_PKT` (mind-wire — already prints; flip drop→degrade where vocab matches), `PFSR`, `REPLICA`, `RAFT`.
- **Storage blobs (NOT R0 — that is R3):** `R3_WP`, `DTR_WBLOB`, `LM_SELF`, `ARK_PROF`, `SIGN_MANIFEST`, `GENOME`. These need read-old→upgrade, not a wire retrofit.
- **Correct-refuse seams (leave as-is, certify they stay loud):** the `R3_WP` dims/vocab gate (`r3_incontext.c:804`) and `MT_WIRE_VER` skew (`:2707`). The cert's `[compat-refuse-honest]` arm guards these.

---

## 4. NOCENTRAL / signed-OTA boundary

How a new version propagates with **no central authority** (consistent with `45_nocentral_boot`: no boot-time raft leader; the decentralized swim/world stack is the substrate):

- **Two distribution tiers** (DRAFT §0 訂正②):
  - *Loader-compatible updates* (weights + small code modules): signed payloads gossiped over the **mesh** and hot-loaded by the current loader. Signing is **mandatory** — Ed25519 / `SIGN_MANIFEST` in `arch/common/sign.c` (verified present; the signed-OTA half has a real substrate). New code runs in the `selfc` germ sandbox (wave-31).
  - *Loader/wire-changing deep updates*: the **platform** ships them (APK A/B + rollback). No mesh path can replace a running loader on a phone; that is honest.
- **Signing scope, hard boundary:** `sign.c` gates **code/weights provenance only** — the content-id / artifact authenticity of an update. It **never** verifies human identity (the ark's inviolable rule). A node trusts an update because the *artifact* verifies against an adopted key (`selfc adopt key` is the only local trust anchor), not because of who a person is.
- **Propagation has no gatekeeper:** there is no "current version" authority. A signed module spreads node-to-node; nodes that can load it do, nodes that cannot **degrade** (§3) and stay in the mesh. Convergence on "which arch/version is prevalent" is *eventual* via gossip, not a raft decision (DRAFT §5.3-5) — flagged as an open bet in §7.
- **New downgrade-attack surface (carry forward):** version/capability advertisement is *not* signed today (DRAFT §8-5). A malicious node could advertise a fake low version to force a peer onto a weaker path. The defense must close on **artifact authenticity** (signed content-id / arch-spec id), never on node identity. OPEN RISK, R-later.

---

## 5. Sequencing

| stage | deliverable | new? destructive? | cert / gate |
|---|---|---|---|
| **R0** | (a) `[compat-interop]` cert (starts RED); (b) §3.1 receive-path tolerate-future on the two lifeline seams (SWIM/DRPC) + reserved-tail; (c) §3.2 frozen envelope-header **definition + static-assert** | additive; safe | land cert as new OPEN gap-ledger row; `[compat-mesh]`+`[compat-degrade]` are the gate |
| **R1** | negotiate-instead-of-reject: `[MIN_SUPPORTED..CURRENT]` accept range + parse-by-version across **all** §1.1 seams; flip mind-wire drop→degrade where `vocab_fp` matches | behavior change at rx; safe | **turns R0's RED arms GREEN** — this is the proof of "no frozen core" |
| **R2** | envelope TLV ext area wired (emit + skip-unknown) on lifeline seams first; capability handshake optional on top | additive once §3.2 frozen | `[compat-skip-unknown]`: old reads known core, skips unknown TLV |
| **R3** | persisted-blob READ-OLD→UPGRADE (`switch(blob->version)`), default-fill missing fields, **never silently discard**; choose write-back policy per seam | careful — write-back can lock out old readers (§7) | `[compat-persist-read-old]` per blob type |
| **R-later** | model-format generational migration (§5.2 本丸) | hardest; couples with growth | `[grow-preserves]` = the interop cert |

R0→R1 is the spine: R0 writes the failing test and secures the irreversible retrofit; R1 makes it pass. Everything after is non-destructive because §3.2 froze the envelope bytes.

---

## 6. DEFERRED / OUT-OF-SCOPE (explicitly not in R0)

- Negotiate range + parse-by-old-version (R1).
- TLV ext area emission + capability handshake (R2) — only the header *bytes* are frozen in R0; nothing is wrapped yet.
- Persisted-blob read-old→upgrade for any of `R3_WP`/`LM_SELF`/`ARK_PROF`/… (R3).
- **Model-format generational migration (DRAFT §5.2, the 本丸)** — the sharpest face: "membership tolerates a version gap" is cheap, but a model whose dims differ by one is numerically broken. This couples with the live-growth work (SS-4/SS-7: experts/width/vocab growth) and with `native-student.md §A.3-A.5` (versioned arch-spec as p-fs object, function-preserving Net2Net translation). Deliberately deferred; do NOT let R0 imply it is solved.
- Signed version/capability advertisement (downgrade-attack defense).
- All-seam interop-cert expansion (R0 certifies the two lifeline seams + teach/persist; full-seam coverage is incremental).

---

## 7. OPEN RISKS (carry every unknown)

1. **Hard-fork is irreversible (highest).** If the §3.1 tolerate-future retrofit is NOT in the first released binary, a future v2 can never reach v1 and the mesh splits forever. This is the one class where "fix it later" is physically void — the initial binary is effectively the constitution (DRAFT §8-1). R0 exists to close exactly this.
2. **Cert starts RED — make sure that's read as a finding, not a failure.** The gate must land as a known-OPEN gap-ledger row; an auditor reviewing R0 must confirm the red is the *expected* red (reject-on-mismatch), not a harness bug. (Lesson: `feedback_validator_and_learner_traps` — have the audit own the acceptance test; commander reads the gate formula line-by-line.)
3. **Mechanism (B) touches headers.** Adding `#ifndef` guards is additive but it is a trunk change before any behavior change — Q-R0 needs sign-off, or fall back to (A).
4. **Eventual schema convergence is unproven (DRAFT §8-3).** "No one declares the active arch; gossip converges" may oscillate (same risk shape as the survival-network two-layer oscillation). Out of R0 scope but it shadows R-later; certify with a minimal experiment before widening, not by assertion.
5. **Persist write-back lock-in (DRAFT §8-4, R3).** Reading an old blob and rewriting it in the new format can make old-version nodes unable to read it again — one-way upgrade lock-in. Per-seam policy ("write oldest-supported" vs "keep both as content-addressed objects"); no general solution. Not R0.
6. **Downgrade attack surface (DRAFT §8-5).** Negotiation advertises versions that are not signed today; defense must stay on artifact authenticity, never node identity. Not R0.
7. **Cert cost honesty.** `[live]` arms spin two `boot/linux` builds through `relay` — not free in CI time. The `[in-proc]` arms (`[compat-degrade]`, `[compat-persist]`) are the cheap fast-feedback core; `[compat-mesh]`/`[compat-teach]` are the slower `[live]` truth. Name which arm is which in the cert header so a green `[in-proc]` is never mistaken for a green `[live]`.

---

## 8. The smallest shippable R0, restated for the implementer

1. **Cert first** — `samples/46_compat_interop/compat_cert.sh`, modeled on `samples/41_shared_mind/run.sh` (relay + distinct `PKERNEL_PFS_DIR` + `PKERNEL_BOOT_DIR`). Tags and falsifiers per §2.2. Header must state: each arm's `[in-proc]`/`[live]` class, and the **measured starting color** (expected: `[compat-mesh]`/`[compat-teach]` RED on `95b20899`). Build-two-versions via mechanism (B) pending Q-R0 (§2.3).
2. **Forward-compat retrofit** — §3.1 tolerate-future + reserved-tail at the **two lifeline rx sites only** (`swim.c:415-418`, `drpc.c:349-352`); recommended on the region seams too if cheap. Membership must mark ALIVE across a version gap. Keep the loud refuses (`r3_incontext.c:804`, `:2707`) intact — the `[compat-refuse-honest]` arm guards them.
3. **Freeze the envelope header** — §3.2: the struct definition + a `sizeof` static-assert (mirror `swim.h:101`). No emission/wrapping in R0.

Definition of done for R0: the cert exists and is wired into the gap-ledger as an OPEN row (RED is acceptable and expected); the lifeline rx path no longer hard-drops a future version or a longer packet; the envelope header byte layout is frozen and static-asserted. R1 (negotiate) then flips the RED arms green — that flip is the falsifiable proof that the core is not frozen.

---

## 9. mk_pino への確認 (二読みできる意図)

- **Q-R0 (mechanism):** approve `#ifndef`-guarding the ~8 version `#define`s so the cert can fork one constant (option B, recommended), or keep trunk untouched with a two-worktree harness (option A)?
- **Q1 (envelope weight, from DRAFT §9):** R0 freezes only the header *bytes* and defers TLV emission to R2 — confirm the reserved-tail-first path (cheap forward-compat now, typed TLV later) over a full typed envelope at first release.
- **Q4 (the long tail as design, not bug):** "old versions live forever" is a design premise (death-aware, knowledge in the collective, lineage in Self). Frame R0's honest RED + the never-dying-fleet story as 地層/多様性, not as a defect.

---
