# DMOE-A — distributed-MoE expert bank (fleet capacity that GROWS with N)

> **Status: SHIPPED 2026-07-05 (`8f754c3b`) — CI green** (job "distributed-MoE capacity
> ([dmoe-*] bank; hosted-only, crown-neutral)", confirmed green on run `33812013193`,
> 2026-09-03). **This doc did not exist until 2026-09-05**, when the unattended routine
> backfilled it per mk_pino's instruction ("無人 run に書かせる", 2026-09-05 03:05).

> **A note on how this doc was written (read before trusting anything below).**
> The instruction for this backfill pass was explicit: *record what is already
> implemented, do not add new design judgment, and if something is ambiguous, flag
> it rather than fill it in.* The only material used was (1) the full body of commit
> `8f754c3b`, (2) the `distributed-moe` job's comments in `.github/workflows/ci.yml`
> (as they read on 2026-09-05, including a later-added "FOLLOW-UP" correction), and
> (3) the fact that CI run `33812013193` is green. **`arch/common/llm/dmoe_bank.c`/`.h`
> and `student.c` were deliberately NOT re-read for additional detail** — everything
> below is a transcription/organization of what mk_pino already wrote in the commit
> and the CI comments, not an independent re-derivation from source. Anywhere this
> doc quotes ci.yml verbatim, that is marked.

Related: [[special-structure-mind.md]] (SS-5 HRW placement + SS-6 remote firing — the
two mechanisms DMOE-A supersedes), [[scaling-law.md]], [[BACKLOG.md]] (2026-09-05
resync block, where this gap was first found), [[gap-ledger.md]].

Implementation: `arch/common/llm/dmoe_bank.c` / `.h` (new), `arch/common/llm/student.c`
/ `.h`, `arch/common/include/ss6_live.h`.
Cert: `tests/llm/dmoe_test.c` + `tests/llm/run_dmoe.sh`.
CI: job `distributed-moe` ("distributed-MoE capacity ([dmoe-*] bank; hosted-only,
crown-neutral)") in `.github/workflows/ci.yml`.

---

## 1. Why — the one clause SS-6 left unfixed

Quoting the commit message directly, because this is the design's own framing and
paraphrasing it risks losing precision:

> The successor of SS-5 (HRW placement) + SS-6 (remote firing) that turns
> `degrade.c:155`'s honest confession — capacity(N) is the display number, NOT the
> growth mechanism — into a real mechanism. SS-6 shares WORK (every node holds every
> expert; a remote timeout recomputes LOCALLY), so adding a node adds FLOPs, never
> capacity. DMOE inverts exactly that one clause.

So SS-6 (see [[special-structure-mind.md]] §6) already let a node fire an expert it
does not hold, but only by falling back to recomputing it locally on timeout — the
fleet gets more parallel compute, not more distinct expert capacity. DMOE-A's stated
goal is to make a node score and route to an expert it neither holds nor can
recompute, so that `dmoe_experts_reachable()` (the real capacity number, per the
commit) actually rises with fleet size and falls when owners die.

## 2. The bank — mechanism, as described in the commit

- A bank expert's FFN blocks are **SHARDED** onto its HRW owners. This **REUSES**
  `placement.c`'s `st_expert_owners_in` **read-only, UNMODIFIED** — DMOE-A does not
  change how ownership is computed, only what a node does once it knows an expert's
  owners.
- The expert's **router row is REPLICATED**, so every node's gate can score an
  expert it does not hold.
- **Version pin**: an FNV hash over the expert's blocks + the core-epoch. A version
  mismatch across nodes causes **REFUSAL, never a deterministic-but-wrong answer**
  (skew turns into a fail-closed refuse, not silent divergence).
- **ZERO new K-DDS topics** — the mechanism is built on top of existing gossip/lookup
  plumbing.

## 3. Student integration

- `student.c` gains a **joint `router_pick`** over `[floor | bank]` candidates. The
  fired width still respects `ST_KMAX` — only the candidate space the router chooses
  from grows, not the number of experts fired per token.
- **Degrade ladder**: if a bank expert is unreachable, the router **DROPs** it and
  **re-softmaxes over the survivors in ascending order** (deterministic given the
  failure set) and reports `degraded(k/n)`. The commit is explicit that the node
  **NEVER recomputes what it does not hold** — that is precisely the SS-6 behavior
  DMOE-A replaces with refusal/degrade instead of local recompute.
- **Bank-inactive forward is BYTE-IDENTICAL to the pre-DMOE student** — with the bank
  off, single-node S/M/L logit hashes are unchanged (stated in the commit as a
  regression guarantee, not something this doc independently re-verified).

## 4. SS6L v2 wire (`ss6_live.c`/`.h`)

- The fire packet gains `ver_lo`/`ver_hi`, `core_epoch`, `flags`, `refuse_reason`.
- The packet magic is bumped so that v1 nodes cleanly ignore v2 packets (no silent
  misparse across a mixed-version fleet).
- Floor-serve behaviour (the pre-DMOE serving path) is unchanged.

## 5. Crown neutrality

Per the commit: **all of `arch/common/llm/*` + `ss6_live.c` is hosted-tier** (bare
metal links `student_stub.o` instead); `placement.c`, `kdds`, and `degrade.c` are
untouched. The claimed bare-metal `.text` is byte-identical to the crown:
`aarch64 7f3fbda4…` / `x86 260da329…` (same hashes as the pre-DMOE crown — i.e. no
re-bless, because nothing in the bare-metal link changed). All 4 targets are stated
to build clean.

**This doc did not independently reproduce those hashes** — see the constitution's
"implementer ≠ auditor" rule; that reproduction is a separate audit step, not
something this backfill pass performed.

## 6. Cert battery (`[dmoe-*]`, commit claims 15/15 PASS in-process)

From the commit body, the `run_dmoe.sh` battery covers: bank-empty-identity,
nonresident+NaN-canary, bit-ref (oracle == distributed, with an anti-theater
stub-the-transport arm that must go RED), version-skew (refuse + a force-accept arm
that must diverge), kill-degrade (mid-token drop), solo-floor, genericity (from ≥2
member sets), and capacity-number.

CI additionally greps `llm_dmoe.log` for (verbatim from `ci.yml`):
- `[result] PASS`
- `ANTI-THEATER: stub the remote transport -> RED`
- `[dmoe-bit-ref] oracle == distributed`
- `[dmoe-version-skew] force-accept the stale blob -> hash DIVERGES`
- no line matching `^  FAIL  `

**`[dmoe-capacity-grows]`** is called out separately in both the commit and the
ci.yml comment as the honest/non-tuned result: it measures ROUTED utility through the
REAL gate (never hosted bytes); the routed GAIN is a **pre-registered NULL at toy
scale under a drifted core** (the commit cites its own design doc §§10.1/10.5 for
this — see §8 below for why this doc could not resolve that cross-reference) —
**reported, not tuned**. A theater arm (resident-only beating solo) is a **HARD RED**.

**Not run in hosted CI** (per the ci.yml FOLLOW-UP comment, §7 below): the
multi-process SS6L v2 relay `[live]` rows, and the aarch64-vs-x86_64 cross-arch
determinism diff. Both are stated to be ThinkPad-self-hosted-runner-only.

## 7. Honest gaps — NOT-YET-WIRED (quoted verbatim from `ci.yml`)

The `distributed-moe` job comment was edited after the original commit to add an
honest correction, presumably during an earlier audit pass. Quoting it in full since
paraphrasing an already-carefully-worded honesty disclosure risks weakening it:

> NOT-YET-WIRED DMOE-A FOLLOW-UP (honest, per the audit): the multi-process SS6L v2
> relay [live] rows + the aarch64 cross-arch determinism diff (identical [dmoe-*]
> machine hashes x86_64 AND aarch64) are NOT wired here yet — there is no self-hosted
> DMOE [live] job; the shipped cert is the in-process mechanism + determinism half
> only. Also DEFERRED in DMOE-A: SS6L v2 bank-SERVE (ss6_live.c serves the floor
> only) and §7.5 [dmoe-rehome-repair] + §2.2 repair blob-pull.

In plain terms: the shipped, green cert proves the bank mechanism works and is
deterministic **in a single process**; it does not yet prove the same thing across
real, separate processes talking over the relay, nor across the two CPU
architectures. It also does not implement expert re-homing after an owner dies, or
serving the bank (as opposed to just the floor) over SS6L v2.

## 8. What this doc could not reconstruct — left open, not guessed

The ci.yml comment cites section numbers of "this design" — `§7` (implicitly, by
being the comment attached to the job that gates it), `§7.5`
(`[dmoe-rehome-repair]`), `§2.2` (repair blob-pull), and `§10.1`/`§10.5` (the
drifted-core toy-scale NULL result) — as if a fuller design document with that exact
section numbering already existed or was planned. **This backfill pass could not
determine what those sections were meant to contain**, because the only material
available (commit body + ci.yml comments) describes the mechanism that shipped, not
the fuller design those numbers imply (e.g. what `[dmoe-rehome-repair]` was meant to
do beyond its name, or the exact geometry of the "drifted core" in §10.1/§10.5).
Reconstructing that would mean guessing at mk_pino's design intent, which this pass
was explicitly told not to do. **This is a judgment-call item, not filled in here —
see the 判断待ち list in `pkernel-baton.md`.**
