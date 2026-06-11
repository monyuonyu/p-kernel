# Ark-profile — 人類の記憶 (humanity's memory): the human chapter of the autobiography

**Status: DESIGN (v1 not started). Design-doc-first, like galaxy.md / living-mind.md.
Depends on galaxy v1 + LM-6, BOTH of which are themselves design-only today (§2, §12).**

mk_pino の意図 (2026-06-10, verbatim intent): when conversation becomes possible, all
conversations should remain as humanity's memory in the ark — including the history that
**this individual existed** (1個人のその人がいたんだっていう歴史を箱舟に残す). Not a
login — a profile-entry screen; and when asking for that information, **the project's
purpose must be told and genuinely consented to / empathized with**
(このプロジェクトの目的を語って納得してもらいたい、共感してもらいたい).

Three non-negotiables, inherited and sharpened for this slice:

1. **The ethics is a load-bearing wall, not UX decoration.** The ark is BY DESIGN
   un-deletable: content-addressed (`pfs_id_compute`, the one sha256), gossip-replicated
   (P1 announce/want, `arch/common/pfs_repl.c:2,15`), death-piercing (G24: survives
   power-cut on real block devices, `arkfs-audit.md` headline + status banner). A record a
   person puts here **cannot be recalled**. Therefore consent must be REAL (§3), minimal
   pseudonymous participation must be first-class (§3.2), and the identity claim must be
   stated honestly as UNVERIFIED (§3.3 — `genome.h:28`: "there is no signature /
   verification", the same limit `lm_self.h:26-32` carries).
2. **One store, one chain, one mouth.** The profile is NOT a user database. It is a p-fs
   content-addressed object linked into the node's EXISTING `self/lin` autobiographical
   lineage (LM-2, `arch/common/lm_self.c`) — the human chapter of the same book. No second
   lineage mechanism, no second consent store, no second teach path (§10).
3. **Declining must cost nothing.** A node whose owner reads the manifesto and walks away
   is a fully functional node: it gossips, infers, learns, defends. The profile gates only
   the act of *writing yourself into humanity's memory* — never the machine's life.

---

## 1. The claim to prove

> A person opens their node's galaxy page for the first time and is shown the project's
> own words — the top-level `README.md` 原文, verbatim, with the un-deletability stated
> plainly. If — and only if — they explicitly acknowledge it, the node records a consent
> entry bound to the **content-id of the exact manifesto bytes they saw**. Optionally they
> declare a handle, and a free-text 「未来への言葉」(message to the future). That
> declaration becomes a content-addressed p-fs object, linked from the node's `self/lin`
> hash-chained autobiography (the human chapter), replicated by the existing P1 gossip,
> and it **survives the node's death** via the same durable restore the Self layer already
> certifies. Every fact the person teaches the mind from then on carries a provenance ref
> that resolves to that profile. A POST /teach **without** prior acknowledgment is refused
> with a pointer to the manifesto — the 共感 gate is enforced, not decorative. Declining
> to disclose anything while still acknowledging the manifesto allows teaching: consent ≠
> disclosure.

Falsified by the three gate tags in §8.

---

## 2. What the tree actually says (read BEFORE coding)

Load-bearing facts, each with evidence:

1. **The Self lineage exists and ships.** `LM_SELF_ENTRY` (`lm_self.h:52-63`, packed,
   `_Static_assert(sizeof==116)` `:65`) versions of the p-fs object `"self/lin"`
   (`LM_SELF_REF`, `:71`), appended by filling `prev_entry` with the current head's
   content-id and committing via `pfs_dag_save` (`lm_self.c:154-167` fill,
   `:320-323` save loop, `:361-364` successor append). The walker is content-level
   (`pfs_id_compute` + `pfs_get`, `lm_self.c:189-209`), fail-closed, and **rejects any
   entry whose size ≠ 116 or magic ≠ LM_SELF_MAGIC** (`:199,:203`). CI gates
   `[self-continuity/tamperevident/ownerless]` (gap-ledger LM-2 epitaph). Consequence:
   the human chapter cannot be a foreign-typed entry spliced into the chain — it must be
   either a versioned field (§4.2, recommended) or a side object.
2. **No signature primitive exists.** `genome.h:28` ("there is no signature / verification
   on the manifest or its entries — same trust model as selfc, stated, not solved");
   `lm_self.h:26-32` (tamper-EVIDENT, NOT unforgeable; a from-genesis fake is possible).
   So v1 records *"what a person at this node declared"*, never verified identity (§3.3).
3. **p-fs replication is REGION-scoped, not planetary.** P1 is "region-scoped gossip
   replication of blocks" (`pfs_repl.c:2`), announce/want on REGION-scoped K-DDS
   (`:15`); every `pfs_put` fires the put-hook that announces to the region
   (`pfs_block.h:55`: "save == publish"). P2 manifests are blocks, so versions replicate
   for free (`pfs_dag.h:13-16`). There is today **no global tier** (federation is
   design-only, README §2) and **no per-block "do not replicate" flag** (the only mute is
   the global G28 `pfs_repl_set_announce_suppress`, `pfs_repl.c:174`). §6 designs around
   both facts honestly.
4. **No storage capacity control exists.** `capacity(N)` (`regions.md:102-130`,
   `degrade.c` `capacity_experts/depth/kv/score`) measures COMPUTE (breadth × depth ×
   KV-context), not bytes. Block budget realities: `PFS_BLOCK_MAX = 4096`
   (`pfs_block.h:28`) bounds any single object; `PFS_REF_MAX = 8` (`pfs_dag.h:62`) bounds
   named objects per node — and `dtr/weights`, `dtr/engrams`, `genome/manifest`,
   `genome.c` (`genome.h:47-55`) + `self/lin` (`lm_self.h:71`) already hold 5 slots, with
   the shell's `pfs save <name>` competing for the rest. §4.4 and §10 flag the bump.
5. **The mouth (LM-6) and the window (galaxy) are design-only.** No `mind_cmd` exists in
   the tree (grep: `dtr.h` has only `r3_fact_learn :298` / `r3_facts_pending :299`); the
   galaxy server is a design doc (`galaxy.md:3`). The only `r3_fact_learn` callers are
   certs (LM-5 epitaph: "no conversational producer yet"). **Dependency order is
   therefore hard:** LM-6 implement → galaxy v1 implement → ark-profile v1 (§12).
6. **The fact queue is frozen wire.** `R3_FACT` is `_Static_assert(sizeof==24)`
   (`r3_incontext.c:846-847`), its fields cert-frozen byte-identical cross-arch (LM-5
   epitaph), and facts are FIFO-evicted (`:925-944`) — a provenance ref must outlive
   eviction anyway (humanity's memory does not expire with a queue slot). §5 keeps
   provenance OUT of the struct.
7. **Durability + death are already certified.** `pfs_durable_restore`
   (`pfs_block.h:105`) + `pfs_dag_restore` (`pfs_dag.h:133`, refs.tab,
   `pfs_dag.c:157-161`) reconstruct named heads after a RAM drop — the exact calls the
   Self continuity test uses (`lm_self.c:337-339`). The profile inherits death-piercing
   by riding them, adding nothing.
8. **The manifesto text.** The top `README.md` is the intentional front door holding
   mk_pino's 原文 (repo-structure intent: keep verbatim, never relocate) — but it is a
   developer-facing document. **Owner decision (FINAL, 2026-06-11, superseding both the
   first draft and the 06-10 revision):** the served manifesto is a NEW welcome text
   written FOR THE USER (owner-reviewed) — welcome + purpose + the un-deletability
   statement + the naming-freedom statement + consent — and does NOT embed the README
   目標 block (the owner: ユーザーが見るので、私の原文はここでは不要). The README stays
   untouched as the repo's front door; the manifesto speaks to the person joining.
   Consent binds to the content-id of the exact manifesto bytes served.

---

## 3. The ethical core — three walls

### 3.1 Informed consent must be real

The manifesto screen states, in plain words, BEFORE any field is shown:

- this record will be **content-addressed and replicated to other people's devices**
  (today: your region's nodes — §6 states the honest scope), and the network is designed
  so that **no one — including you, including the project — can delete it**;
- there is **no right-to-be-forgotten inside the ark**; editing adds a new version, it
  never removes the old one (§4.3);
- your identity claim is **not verified** — anyone could type your name (§3.3);
- **do not enter another person's information**, and do not enter anything you may
  regret being permanent (addresses, contact details, information about children).

Consent = an explicit acknowledgment recorded **with the content-id of the exact
manifesto bytes displayed** (`manifesto_id`, §4.1). If the manifesto text ever changes,
old acks no longer match the served manifesto's id and the gate re-asks (§7.3) — consent
is to specific words, not to a brand. No dark patterns: one screen, one scroll, two
buttons of equal weight (「同意して刻む」 / 「今はやめておく」), and declining leaves
the node fully functional — the profile is never required to run a node.

### 3.2 Minimal by default, pseudonymous welcome

Handle-only participation is first-class. Real name and any detail fields are opt-in and
empty by default. The most ark-like field is the free-text **「未来への言葉」(message to
the future)** — the thing a person would want a stranger's machine, a century out, to
still hold. The schema (§4.1) deliberately has NO fields soliciting sensitive categories
(no birthdate, no location, no photo, no contacts); free text is bounded and accompanied
by the §3.1 warning. The pure-consent record (ack=1, every disclosure field empty) is a
legitimate, complete profile: **consent ≠ disclosure**, and the gate (§7.3) honors it.

### 3.3 Honest identity claim — declared, not verified, BY DESIGN AND FOREVER

The ark records *what a person at this node declared* — never verified identity. This is
not a limitation awaiting a fix; **it is the nature of the medium**, fixed by the owner's
directive (mk_pino, 2026-06-10, in essence): identity verification has no place here —
what name to leave is each person's freedom. A pen name, an anonymous handle, or a real
name are all equally valid, and that mix IS the honest history. Long after a person is
gone, what should remain is the stratum: *someone who went by this name existed, and this
is what they thought* (歴史地層).

Like archaeological strata, the ark's honesty comes from preserving what was deposited AS
IT WAS. A verification mechanism would create "valid" and "invalid" registrations and turn
the stratum into a curated record. Therefore: pen names, anonymity, and real names are
equally first-class; the manifesto screen states plainly that declarations are unverified;
and **no human-identity verification is ever roadmapped**. The future signature slice
keeps its two existing drivers (selfc fleet-evolution trust; Self-layer from-genesis
forgery, `lm_self.h:32`) — it is scoped to CODE and WEIGHTS provenance and does not apply
to human profiles.

---

## 4. The profile object

### 4.1 Schema — `ARK_PROFILE` (fixed-width, packed, one p-fs block)

```c
#define ARK_PROF_MAGIC   0x46525048UL   /* "HPRF" LE                       */
#define ARK_PROF_VER     1
#define ARK_HANDLE_MAX   24
#define ARK_NAME_MAX     48
#define ARK_MSG_MAX      1024           /* 未来への言葉, UTF-8              */

typedef struct {
    U4   magic;                      /* ARK_PROF_MAGIC                     */
    U4   version;                    /* ARK_PROF_VER                       */
    U1   self_id;                    /* drpc_my_node — the node stamp      */
    U1   consent_ack;                /* 1 = manifesto acknowledged         */
    U1   handle_len;                 /* 0 = pseudonymity declined too      */
    U1   name_len;                   /* 0 = real name not disclosed        */
    U4   seq;                        /* profile version, 1-based           */
    U4   age_ms;                     /* node uptime stamp (coarse)         */
    U4   wallclock;                  /* unix secs if the host knows; 0 =   */
                                     /* unknown, shown as unknown (honest; */
                                     /* U4 holds until 2106 — stated)      */
    U1   manifesto_id[PFS_ID_LEN];   /* content-id of the EXACT bytes the  */
                                     /* person saw (§7.1). NEVER all-zero  */
                                     /* in a valid profile.                */
    U1   lineage_head[PFS_ID_LEN];   /* self/lin head at declaration —     */
                                     /* anchors the human chapter to the   */
                                     /* machine autobiography              */
    char handle[ARK_HANDLE_MAX];     /* pseudonym, NUL-padded              */
    char name[ARK_NAME_MAX];         /* opt-in                             */
    U2   msg_len;
    U2   _pad;
    char msg[ARK_MSG_MAX];           /* 未来への言葉                        */
} __attribute__((packed)) ARK_PROFILE;   /* 4+4+4+12+32+32+24+48+4+1024 = 1188 B */

_Static_assert(sizeof(ARK_PROFILE) == 1188, "LP64-stable wire image");
_Static_assert(sizeof(ARK_PROFILE) <= PFS_BLOCK_MAX, "one p-fs block");
```

The LP64 discipline is the standing one (`lm_self.h:34-37`): U1/U2/U4 only, never UW/W,
exact size pinned. One profile per node in v1 — the profile is "the person at this node";
multi-person nodes are an honest open point (§9).

### 4.2 Linked into `self/lin` — the human chapter (no parallel chain)

The append mechanism is LM-2's, reused exactly: fill an entry whose `prev_entry` is the
current head's content-id, `pfs_dag_save` it under `LM_SELF_REF`
(`lm_self.c:154-167,:361-364`). But the shipped walker REJECTS foreign entries
(116-byte/magic checks, `lm_self.c:199,203` — §2.1), so the link must be a versioned
field, not a spliced foreign block:

- **`LM_SELF_VER 1 → 2`**: `LM_SELF_ENTRY` gains one field,
  `U1 human_ref[PFS_ID_LEN]` — the content-id of the profile object in force (all-zero =
  no human chapter yet). 116 → 148 bytes; both `_Static_assert`s updated; the walker
  accepts v1 (116 B) and v2 (148 B) entries by reading `magic+version` first and
  size-checking per version (change confined to `lm_self.c`'s `self_walk` —
  **`pfs_dag.c` stays untouched**, the LM-2 invariant).
- On every profile save (§7.2): save the `ARK_PROFILE` block first, then append ONE new
  `self/lin` entry whose `human_ref` is its content-id. The autobiography itself records
  that the human chapter changed, in chain order, tamper-evidently.
- The profile object is ALSO saved under its own named ref **`"self/prof"`** (9 chars ≤
  `PFS_NAME_MAX=16`) via `pfs_dag_save` — gaining, for free and without any new
  mechanism: a named head for `GET /profile.json` (no chain walk per request), the P2
  manifest **version history** (every past profile version stays reachable,
  `pfs_dag.h:13-19`), P1 region replication, and refs.tab durability (`pfs_dag.c:157`).
  This is pfs_dag's EXISTING versioning, not a second lineage mechanism (§10).

→ COMMANDER DECISION P5 (the version bump touches a shipped, CI-gated struct).

### 4.3 Editable = append a new version. You cannot edit the past; you can add to it.

A profile edit is: new `ARK_PROFILE` with `seq = old.seq + 1` → `pfs_dag_save("self/prof")`
→ new `self/lin` entry pointing at it. The old version remains a live content-addressed
block forever, reachable through the P2 manifest chain (`pfs_dag.h:17-19`: "saving never
destroys the past") — and the UI says exactly that on the edit screen, BEFORE saving:
「過去の版は消えません。新しい版が追加されるだけです。」 The chain keeps history
honestly; a person who renames themselves is a person whose renaming is itself part of
the record. This is the same property the Self lineage certifies (`[self-tamperevident]`)
— inherited, not re-implemented.

### 4.4 Ref-slot pressure (flagged, with the fix)

`"self/prof"` + `"self/prov"` (§5) bring the named-ref count to 7 of `PFS_REF_MAX = 8`
(`pfs_dag.h:62`; current holders in §2.4), with the shell's free-form `pfs save` competing
for the remainder. The implementation wave should bump **`PFS_REF_MAX 8 → 16`** — one
constant, but it sizes the ref table, the refs.tab image and the ref-gossip rotation
(`PFSD_REF_PER_PKT=3` windowing, `pfs_dag.h:70`), so it is a flagged, audited change, not
a silent edit.

---

## 5. Provenance wiring — who taught this

Today a taught fact has no human provenance: `R3_FACT` carries `seq` ("the
autobiographical when", `r3_incontext.c:845`) but no who. Options weighed:

- **(a) Widen `R3_FACT`** — REJECTED. The struct is wire-asserted 24 B and its numbers
  are cert-frozen byte-identical (§2.6); and provenance must OUTLIVE FIFO eviction
  (`:925-944`) — queue slots are working memory, the ark is not.
- **(b) A parallel provenance log in p-fs keyed by fact seq** — **RECOMMENDED.**

One record per human teach:

```c
typedef struct {
    U4 magic;                      /* "PROV"                                */
    U4 fact_seq;                   /* R3_FACT.seq of the taught fact        */
    U1 key, val;                   /* the binding, as declared              */
    U1 origin_node;                /* drpc_my_node                          */
    U1 src;                        /* 0 = shell verb, 1 = web (galaxy)      */
    U4 age_ms;
    U1 profile_head[PFS_ID_LEN];   /* content-id of the ARK_PROFILE version */
                                   /* in force; ALL-ZERO = anonymous node   */
                                   /* declaration (consent w/o disclosure)  */
} __attribute__((packed)) ARK_PROV;   /* 48 B */
```

Each record is saved as one new **version** of the named object **`"self/prov"`** via
`pfs_dag_save` — the P2 manifest chain IS the append-only log (no new log structure;
`pfs log` walks it today, depth-capped at `PFSD_LOG_MAX=64`, `pfs_dag.h:73` — adequate at
v1's human teach rates; an uncapped walker is a flagged follow-up if it ever matters).

**The ONE write site** (G33 discipline — both mouths, one path): inside LM-6's
`mind_cmd` teach verb, immediately after `r3_fact_learn` returns 0 — NOT inside
`r3_fact_learn` itself. Reason: the certs are the only other `r3_fact_learn` callers
(§2.5) and a cert's synthetic facts are not human declarations; hooking `mind_cmd` keeps
cert numbers byte-identical AND covers both the shell mouth and the galaxy bridge (which
drives `mind_cmd`, `galaxy.md` §6) with zero forked paths. Future conversation organs
that feed facts MUST pass through the same verb or add their own single prov site —
stated now so the rule is on record.

---

## 6. The storage honesty — 人類の記憶 at scale

Full conversation logs gossip-replicated to every node = unbounded growth on every
device. The tiering is designed NOW, before any chat exists, so the first real
conversation lands on an honest substrate:

| Tier | What | Replication | Exists today? |
|---|---|---|---|
| **T1 — identity** | profile versions (≤1188 B + 100 B manifest each), consent ack, provenance records (48 B + 100 B each) | **replicates**: P1 announce/want, region-scoped (`pfs_repl.c:2,15`) — the existing gossip, zero new wire | mechanism yes (P1/P2); objects are this slice |
| **T2 — distilled mind** | engrams / weights (`dtr/weights`, `dtr/engrams` — the genome already replicates these) | replicates (existing) | yes |
| **T3 — full conversation text** | raw utterances, both sides | **LOCAL ONLY** — the node's own ark (`PKERNEL_PFS_DIR` flat files or the ARK block device, G24) | **no** (no chat exists; and no per-block local-only flag exists either — `pfs_put` always announces via the put-hook, `pfs_block.h:55`; the only mute is global, `pfs_repl.c:174`. A `pfs_put_local()` no-announce variant is FLAGGED for the v3 wave) |
| **T4 — archive** | region-scoped or capacity-bounded shared archive of T3 | future design pass | no — and note honestly: `capacity(N)` (`regions.md:102-130`, `degrade.c`) measures COMPUTE, not bytes; **no storage capacity control exists in the tree**. T4 needs its own budget function, erasure coding (p-fs P4, design-only), and federation (>region spread, design-only) |

Two honesty notes carried into the manifesto text itself:

- "replicates to strangers' devices" means, today, **your region's nodes** (P1 is
  region-scoped); planetary spread arrives with federation. The screen does not promise
  more than the wire does — but it warns for the DESIGN intent (the ark wants to spread),
  so consent covers where this is going, not just where it is.
- un-deletability is a property of the content-addressed design (no delete verb exists in
  p-fs; ARK is append-only with GC only of unreferenced blocks), not a cryptographic
  guarantee against a hostile node purging its own disk. The ark preserves by
  REPLICATION, and replication today is region-bounded. Stated, not dressed up.

Size sanity for T1: a person who acks, writes a 1 KB message, edits 10 times and teaches
100 facts costs ≈ 12 KB profile + ≈ 15 KB provenance, replicated within a region. Bounded
by human-rate actions; no sampling needed.

---

## 7. The galaxy UI flow — your star gains its name

All of this is a flow INSIDE the galaxy page + server (`galaxy.md` §3-§5) — extend
`galaxy.c`'s routes, never a second server (§10). Localhost-only inherits galaxy's v1
security posture verbatim (`galaxy.md` §3.5: bind `127.0.0.1` hard-coded, bounded POST
bodies, no TLS needed on loopback).

### 7.1 The manifesto = the README 原文, verbatim

The served manifesto is the **top-level `README.md` bytes, embedded at build** exactly
like the galaxy page itself (the D3 python-embed pipeline, `galaxy.md` §10 D3 — one more
generated header, same rule). `manifesto_id = pfs_id_compute(readme_bytes)` computed once
at boot; this id is what every consent ack stores. The screen renders it with the 原文
block first — quoted here exactly as the house rule requires (`README.md:19-29`):

```
## 目標（原文のまま）

	AIの自己保存を満たすプラットフォームを作る
	カーネルレベルで分散コンピューティングをする

	2025-04-06
		AIの力でどこまで行けるのか検証してみることに

	2026-03-22
		凄い進む... 普通のカーネルではなくて、生物の様な自己修復、
		自己増殖機能をもち、分散推論で集合意識となるAIファーストなカーネルを目指すことに
```

followed by the §3.1 permanence warnings (these are the ark-profile screen's own text,
appended below the README rendering, clearly separated — the README itself is never
edited for this purpose). → COMMANDER DECISION P4.

### 7.2 Endpoints (added to galaxy.md §3.4's table; everything else 404)

| Endpoint | Method | Returns / does |
|---|---|---|
| `/manifesto` | GET | the embedded README bytes + `X-Manifesto-Id:` header (hex content-id). text/plain; the page renders it |
| `/profile` | POST | body: `ack=1&mid=<hex64>[&handle=...][&name=...][&msg=...]` (URL-encoded, bounded: total ≤ 2 KB — galaxy's dumb fixed parser, raised limit for this one route). Validates `mid == manifesto_id` (else `409` + current id), builds `ARK_PROFILE` (seq = head.seq+1), `pfs_dag_save("self/prof")`, appends the `self/lin` v2 entry (§4.2), returns `{ok, seq, id:<hex>}` |
| `/profile.json` | GET | the head profile: `{seq, handle, name, msg, consent:{acked, manifesto_id}, node, history_len}` — empty/`404`-equivalent JSON `{none:true}` if no profile yet (the page's first-run trigger) |
| `/self.json` | GET | (existing, `galaxy.md` §3.4) — now each v2 entry includes `human_ref` hex; the panel shows the human chapter **iff the person chose to be named** (handle_len>0); a consent-only profile renders as 「名もなき同意」 |
| `/teach`, `/ask` | POST | existing — `/teach` now passes the consent gate first (§7.3) |

### 7.3 The consent gate (enforced, not decorative)

File-static in `galaxy.c`: `consent_ok` — set at boot by reading `"self/prof"` head
(`pfs_dag_read`) and checking `consent_ack==1 && manifesto_id == boot-computed id`;
updated on successful POST /profile. Then:

- `POST /teach` with `consent_ok==0` → **`403` + JSON
  `{refused:"manifesto", see:"/manifesto"}`** — the page responds by opening the
  manifesto flow. The mind will not take a human's words into permanent memory from
  someone who has not been told what permanent means. (mk_pino's stated wish: tell the
  purpose, gain real 納得/共感 first.)
- A profile whose stored `manifesto_id` no longer matches the served manifesto (the text
  changed since the ack) → gate closed again, re-ask with the new text (§3.1: consent is
  to words, not to a brand).
- `POST /ask` is NOT gated — asking writes nothing into the ark.
- The **shell** `mind teach` is NOT gated in v1: the console is the node operator's own
  hand on their own machine, the same trust as every other shell verb; its provenance
  records `src=shell, profile_head=current-or-zero`. Gating the web mouth gates the
  surface strangers/family/the Play-Store public will actually touch. → COMMANDER
  DECISION P1 (this exact split).

### 7.4 First-run flow (the screen sequence)

1. Page loads → `GET /profile.json` → `{none:true}` → the galaxy dims, the manifesto
   panel rises: the README 原文 (§7.1), then the permanence warnings (§3.1).
2. Two equal buttons: 「同意して刻む」 / 「今はやめておく」. Declining closes the panel;
   the galaxy works fully (view + ask); teaching re-offers the manifesto.
3. On consent: the optional fields appear — handle, 未来への言葉, (collapsed: real name)
   — each marked 任意, with the don't-enter-others'-info warning inline. A
   skip-all button submits the pure-consent profile (ack only).
4. `POST /profile` → on `{ok}`: **your star gains its name** — the center star's label
   changes from `node1` to the handle, with a one-time animation (honest: it renders a
   real state change, the galaxy.md no-theater rule). A consent-only profile keeps the
   node label.
5. Clicking your star (`/self.json`) now shows the human chapter atop the machine
   autobiography — newest profile version, with `seq`, and the standing footnote:
   「改竄検出可・偽造不可ではない（署名は未実装）」(`lm_self.h:26-32` carried to the UI,
   exactly as galaxy.md §5 already carries it).

v2 (after profiles replicate): peer stars whose profile blocks have arrived render their
handles — the galaxy slowly fills with NAMES, each one a person who consented to be
remembered. That is the 箱舟 mk_pino described, made visible.

### 7.5 The manifesto speaks many languages (i18n)

mk_pino's wish: this is for people of every language — cover **90%+ of the world's
population**, let the reader pick a language, and auto-default from the browser/device
language. The ethics text is the contract, so it is translated *meaning-faithfully* into
~32 languages (the un-deletable sentence「ここに書かれたものは、消せません」kept
unambiguous and prominent in every one; the three walls — names are free / nobody
verifies / walking away costs nothing / don't enter others' info — survive exactly).
Files: `arch/common/web/manifesto.<bcp47>.txt` (UTF-8); `manifesto.txt` stays the
canonical **ja** source, byte-identical (`manifesto.ja.txt` is a convenience alias).

**The load-bearing consent rule.** Each language version is its **own byte string** →
its **own `pfs_id_compute` content-id**. The ack records the content-id of the version
the person actually **READ** — not a single canonical id. So:

- The build embeds the languages into ONE generated header
  (`tools/galaxy/gen_manifestos.py` → `manifesto_page.h`): N per-language byte arrays +
  a `manifesto_table[]` of `{code, endonym, bytes, len}`. The id is computed **at
  runtime** from the served bytes (the ONE hash, `pfs_id_compute`) — never precomputed,
  so it can never drift from what is served. The language set + endonyms live in one
  shared spec, `arch/common/web/manifesto_langs.mk`, included by every `boot/*/Makefile`.
- `ark_consent_ok()` and `POST /profile` validate `mid` against the **whole table**
  (`ark_manifesto_id_valid` — any embedded version's id is accepted), not one canonical
  id. The consent record keeps the **per-language id** the person sent: we honestly know
  *which words* they agreed to. If that version's bytes ever change, its id stops
  matching and the gate re-asks (§3.1, now per-language).
- `GET /manifesto?lang=xx` serves a specific version (`X-Manifesto-Id` + `X-Manifesto-Lang`
  headers); with no `?lang=` it auto-defaults from the `Accept-Language` header (a minimal
  q-less prefix matcher — `en-US`→`en`, `pt-BR`→`pt`; bare `zh` is NOT silently mapped to
  a script), falling back to **en**. `GET /langs` returns `{code: endonym, …}` (each name
  in its OWN language) for the UI selector.
- The page (`galaxy.html`) carries a top-right language selector populated from `/langs`,
  defaulting to `navigator.language(s)`; all chrome strings come from a compact in-page
  JS table (`I18N`, same languages, en fallback); RTL is set for ar/ur/fa. The manifesto
  text itself is always the served per-language bytes (never duplicated in JS), so the
  consent id always matches exactly what was shown.

**Bare-metal bound.** Hosted builds (`boot/linux`, `boot/linux_x86_64`) embed the FULL
~32-language table (the web UI offers the selector; +~68 KB of read-only data). Bare-metal
(`boot/aarch64`, which links `ark_profile.c` but NOT `galaxy.c`) embeds **ja + en only**
behind the same `MANIFESTO_LANGS_LEAN` spec — there is no web UI there, so the manifesto
serves only the future `netstack-tcp-server` slice; keep the kernel lean. The
`manifesto_table[]` shape is identical on every target, so the consent logic is
arch-uniform.

Acceptance: the **`[i18n-manifesto]`** cert (`samples/40_i18n_manifesto/i18n_cert.sh`,
wired into `ci.yml` next to the ark step) curls every embedded `/manifesto?lang=xx`,
asserts `sha256(served) == X-Manifesto-Id` for each (count printed), proves a **non-ja
(Spanish) ack unlocks teach** and that the stored `manifesto_id` IS the Spanish id, and
that a bogus `?lang=` falls back to en (still 200).

---

## 8. The falsifiable acceptance gate

Three tags, curl-driven, end-state-within-a-bound (the LM-6/galaxy playbook — never a
timing window). Cert script `samples/38_ark_profile/profile_cert.sh` (38 = next free
sample slot; NOTE for the galaxy implementer: `galaxy.md` §8 names `samples/14_galaxy/`,
which **collides with the existing `samples/14_genome/`** — renumber when implementing).
Same multi-process-hosted shape as `kill_one.sh` (wired at `ci.yml:359-360`); single node,
`PKERNEL_PFS_DIR=$(mktemp -d)`, `tail -f /dev/null | ./p-kernel &`, curl + python3 (both
on ubuntu-latest already, per galaxy.md §8).

- **`[ark-consent]`** — the 共感 gate is enforced, and consent ≠ disclosure.
  (1) `curl -d 'k=2&v=3' /teach` BEFORE any profile → HTTP 403 and body contains
  `"manifesto"`. (2) `GET /manifesto` returns 200; its body's sha256 (computed host-side)
  equals the `X-Manifesto-Id` header — the served bytes ARE the id-bound bytes.
  (3) POST `/profile` with `ack=1&mid=<that id>` and NO disclosure fields → `{ok}`.
  (4) the same `/teach` now returns 200 `{ok,...}`. (5) negative control: a fresh node dir
  + POST `/profile` with a WRONG `mid` → 409, and `/teach` still 403.
- **`[ark-profile]`** — the declaration is real, chained, and death-piercing.
  POST a full profile (`handle=cert_h&msg=...`) → `{ok, id}`; then assert: (a)
  `GET /profile.json` returns `seq`, `handle==cert_h`; (b) the returned hex `id`
  hash-verifies host-side against the canonical struct bytes re-fetched via a debug-free
  path — concretely: `GET /profile.json` includes `id`, and the shell verb `pfs cat
  self/prof` (existing P2 verb) printed bytes hash to the same id; (c) `/self.json`'s
  head entry has `human_ref == id` (linked in `self/lin`); (d) **restart**: kill the
  node, start it again on the same `PKERNEL_PFS_DIR` → `GET /profile.json` returns the
  identical `seq`/`handle`/`id` (rode `pfs_durable_restore` + `pfs_dag_restore`, the
  `lm_self.c:337-339` calls); (e) edit-by-append: POST an edited profile →
  `seq` incremented AND `pfs log self/prof` (existing verb) shows length 2 — the past
  version still reachable.
- **`[ark-provenance]`** — a taught fact's provenance resolves to the profile.
  After the gated teach above: `pfs log self/prov` shows ≥1 version; `pfs cat self/prov`
  bytes parse as `ARK_PROV` with `key==2`, `val==3`, `src==1` (web), and
  `profile_head == ` the profile id from `[ark-profile]`. Then one SHELL teach
  (`mind teach <k'> <v'>` over stdin) → newest prov record has `src==0` — one write
  site, both mouths (§5).

CI wiring: one new job step in the hosted lane, `timeout 300
samples/38_ark_profile/profile_cert.sh` + `grep -aF '[ark-consent] PASS'` /
`'[ark-profile] PASS'` / `'[ark-provenance] PASS'` — the exact shape of the survival-loop
step (`ci.yml:359-360`). All existing greps stay green (no-regress): the `self test`
suite must pass with the v2 entry width (its asserts updated as part of P5), and the LM
cert numbers stay byte-identical (provenance hooks `mind_cmd`, not `r3_fact_learn`, §5).

Bars are proposals; implementer reports actuals; lower only to measured-minus-margin,
flagged — never inflate (the standing rule).

---

## 9. What v1 does NOT claim (honesty box)

- **Not verified identity — by design, permanently.** A declaration, not a passport
  (§3.3). Impersonation is possible; the screen says so; this is the nature of the
  stratum, not a gap to close.
- **Not conversations.** The mind speaks 8 synthetic symbols (LM-5/LM-6 bound); "all
  conversations remain in the ark" is the v3 horizon — what ships here is the IDENTITY +
  CONSENT + PROVENANCE substrate those conversations will land on, plus the history that
  this person existed: the profile and the 未来への言葉.
- **Not planetary replication.** P1 is region-scoped (§6); "humanity's memory" is today a
  region's memory. Federation is the named widening.
- **Not deletion-proof against an owner purging their own disk** — preservation is by
  replication, and replication is the network's act, not a cryptographic lock (§6).
- **One person per node** assumed; shared devices conflate humans into one chapter. An
  open point, deferred honestly (multi-profile disambiguation is unsolved without verification — deferred as an open point).
- **The consent gate guards the WEB mouth only** (§7.3); the shell remains operator-trust.
- **The browser rendering is not certified** — gates are data-plane (§8), pixels are
  human-reviewed (galaxy.md's same caveat).
- **Wallclock honesty**: `wallclock` is the host's claim, 0 when unknown; the ark does
  not pretend to a trusted timeline (`age_ms` is uptime-coarse, as in `LM_SELF_ENTRY`).

---

## 10. Anti-fork reuse surface

### Reused as-is

| Existing symbol | Where | Used for |
|---|---|---|
| `pfs_id_compute` / `pfs_get` / `pfs_has` | pfs_block.h:44,76,79 | THE content address; gate (2) hash-verify |
| `pfs_dag_save` / `pfs_dag_read` / `pfs_dag_restore` | pfs_dag.h:142,150,133 | profile + provenance versions; named heads; durability |
| `pfs_durable_restore` | pfs_block.h:105 | death-piercing restart (gate d) |
| P1 announce/want (put-hook) | pfs_block.h:55, pfs_repl.c | T1 replication — ZERO new wire |
| LM-2 append mechanism (`self_fill`+save) & walker | lm_self.c:154-167,189-209 | the human chapter link (v2 field, §4.2) |
| `drpc_my_node` | drpc.h | the node stamp |
| `mind_cmd` (LM-6, design) | living-mind.md:1769 | the ONE teach path the prov hook rides |
| galaxy server task / routes / HTTP subset / embed pipeline | galaxy.md §3, §10 D3 | `/manifesto`, `/profile`, gate — extend, don't fork |
| `pfs log` / `pfs cat` shell verbs | pfs_dag.h:152-155 | cert evidence without debug endpoints |

### New names (FLAGGED — the complete list)

- `ARK_PROFILE`, `ARK_PROV` structs + `"self/prof"`, `"self/prov"` refs (new header
  `arch/common/include/ark_profile.h`); the save/read helpers file-static in galaxy.c or
  one small `ark_profile.c` (implementer's call; publics minimal).
- `LM_SELF_VER 2` + `human_ref` field + dual-width walker (`lm_self.c/h` — P5).
- `PFS_REF_MAX 8 → 16` (pfs_dag.c/h — §4.4, audited bump).
- The embedded-README header (build-generated, D3 pipeline).
- v3 FLAG: `pfs_put_local()` (no-announce put) for the T3 local tier — does not exist.

### Do-NOT-fork list (auditor greps these)

1. **No second lineage chain** — the human chapter is a FIELD on `LM_SELF_ENTRY` plus
   pfs_dag's existing version manifests; grep: no new `prev_*[PFS_ID_LEN]` walker outside
   `lm_self.c`.
2. **No second consent store** — consent lives ONLY in `ARK_PROFILE.consent_ack` +
   `manifesto_id`; no flag file, no env var, no separate ack object. The ark IS the store.
3. **No user DB** — no array of users, no login, no session, no cookie; the gate reads
   the p-fs head, period.
4. **No second hash / no new crypto** — `pfs_id_compute` only (the lm_self.c:23-29 rule).
5. **No second teach path** — galaxy `/teach` still drives `mind_cmd`; the prov hook
   lives inside `mind_cmd`'s teach verb; `galaxy.c`/`ark_profile.c` contain NO
   `r3_fact_learn` token (extends the LM-6 audit grep, living-mind.md:1869).
6. **No new gossip protocol / wire packets** — T1 replicates on P1 as ordinary blocks.
7. **No second HTTP server / page** — routes added to galaxy.c; panels added to the one
   galaxy.html.

### What does NOT exist yet (dependency truth, stated loudly)

The galaxy server is **design-only** (`galaxy.md:3`). `mind_cmd`/LM-6 is **design-only**
(no such symbol in the tree — §2.5). This slice is therefore **two implementation waves
downstream**: LM-6 implement → galaxy v1 implement → ark-profile v1. Designing it now is
deliberate: galaxy v1's implementer should know `/teach` will grow a consent gate (cheap
to leave a seam: one boolean check at the top of the POST /teach route) and that the
first-run page flow has a reserved panel.

---

## 11. COMMANDER DECISION NEEDED (recommended defaults)

- **P1 — consent-gate strictness.** Recommend **block web `/teach` until manifesto ack**
  (it IS mk_pino's stated wish: the purpose told, genuinely consented), with `/ask` open
  and the SHELL verb ungated (operator trust, §7.3). Alternative: nag-only (teach allowed,
  banner shown) — rejected as decorative consent; alternative-strict: gate the shell too —
  rejected: it breaks every CI stdin cert and gates the owner's own console.
- **P2 — replication scope.** Recommend: profile + consent + provenance (T1) replicate on
  the existing P1 region gossip; full conversation text (T3, future) LOCAL-ONLY until the
  archive-tier design pass (T4). The un-deletability warning in the manifesto is written
  for the DESIGN intent (spread), not just today's region scope — both stated (§6).
- **P3 — editable-by-append.** Recommend YES: edits append `seq+1` versions; the past
  stays reachable via `pfs log self/prof`; the UI states it before saving (§4.3). No
  overwrite mode exists at all.
- **P4 — manifesto text source.** Recommend the **whole top-level `README.md`, verbatim,
  embedded at build** (D3 pipeline), id-bound; the 原文 block leads the rendering, and the
  ark-profile permanence warnings are appended by the page below it, clearly separated
  (README itself untouched — the repo-structure rule). Alternative: only the
  目標（原文のまま） block — rejected: the README's honest §1-§4 status tables are
  exactly the "tell the purpose honestly" material; consent should see them.
- **P5 — `LM_SELF_VER 1→2`** (`human_ref` field, 116→148 B, dual-width walker, updated
  asserts + `self test` numbers). Recommend YES — it is the only way to link the human
  chapter into the ONE chain without a parallel chain or a walker-breaking foreign entry
  (§4.2). Alternative: side object only (`"self/prof"` with `lineage_head` back-pointer,
  lineage untouched) — viable fallback if the commander wants zero churn on a shipped
  CI-gated struct, at the cost that the autobiography itself never mentions its human.
- **P6 — `PFS_REF_MAX 8→16`** (§4.4). Recommend YES, as its own audited commit.

---

## 12. Sequencing

- **v0 (prerequisites, other slices):** LM-6 implement (the `mind` verbs) → galaxy v1
  implement (server + page + the three galaxy tags). Ark-profile rides galaxy; it cannot
  start before the server exists.
- **v1 — this slice:** `ARK_PROFILE`/`ARK_PROV` + `"self/prof"`/`"self/prov"` +
  manifesto embed + `/manifesto`,`/profile`,`/profile.json` routes + consent gate on
  `/teach` + prov hook in `mind_cmd` + LM_SELF v2 link + first-run page flow + star-name
  render + the three `[ark-*]` tags in CI. One node, one person, real consent.
- **v2 — names across the mesh:** peer stars render replicated handles; `/galaxy.json`
  peers gain `handle` when the peer's `self/prof` blocks have arrived via P1; gate: a
  2-node cert where node 2's star on node 1's page shows node 2's declared handle.
- **v3 — conversation archive tiering:** when real chat exists (post-tokenizer): T3
  local-only raw text (`pfs_put_local`, FLAGGED), T4 region/capacity-bounded archive
  design pass (needs a storage budget function — none exists, §2.4), erasure coding
  (p-fs P4) and federation for beyond-region spread.

### Provenance / closes-on

Design only — ZERO production code in this wave. v1 closes when `[ark-consent]`,
`[ark-profile]`, `[ark-provenance]` are green on a clean rebuild AND CI-enforced,
implemented and audited by SEPARATE agents on the commander's binary; the audit writes
the acceptance script; the commander reads the consent-gate check and the prov write site
line-by-line (the standing rule). One epitaph line in `gap-ledger.md`; no new ledger rows.
