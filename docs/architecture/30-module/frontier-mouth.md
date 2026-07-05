# Frontier Mouth — CONSULT / TEACH (module)

The ownerless mind's **optional** socket onto stronger brains. Design source:
`frontier_mouth_design.md` (companion to `conscience.md` — the immutable floor
this passes through — and the scale-wall doc — the ceiling this answers). This
page records what the `feat/frontier-mouth` wave actually SHIPPED and what it
deliberately deferred.

mk_pino's intent, verbatim: 「本物の会話はあなたのようなフロンティアモデルと本当は
会話させてあげたい … その口だけ開けとくのはいいかもしれない」

## What it is

Two channels, one peripheral organ (Body layer — per-node, optional, absent by
default):

- **CONSULT** — a **labeled borrowed voice**. A human, through the galaxy UI,
  converses with a frontier model. Every frontier byte reaches the human inside
  a `src:"frontier"` frame that names the model. API output is **spoken, never
  eaten**: the CONSULT path has NO write-edge into the student's self. Unplug the
  socket and the self is byte-identically what it grew to be.
- **TEACH** — **open-license teachers only**. A volunteer's local model text is
  digested via the existing `cradle_lesson_ingest` → the student's OWN backward
  pass. The license line is enforced by the `frontier_teacher_kind` enum: there
  is deliberately **no `FRONTIER_API` value** — API output can never become a
  lesson.

## The immutable floor gates BOTH mouths (良心)

- **E6 — CONSULT reply**: `frontier_consult` runs `conscience_check` at
  `CONS_SITE_CHAT_REPLY` on the WHOLE frontier reply BEFORE any labeled byte
  leaves. A refusal replaces the reply with the mind's own refusal tok (no
  `src:"frontier"`, zero frontier bytes). The floor gates MY mouth, whoever is
  breathing through it.
- **I2 — TEACH lesson**: `cradle_lesson_ingest` gained a `conscience_check` at
  `CONS_SITE_LEARN` — the ONE ring-write site, so on the builds that link the floor
  (`CRADLE_HAS_CONSCIENCE`, the two hosted kernels) it is the chokepoint for every
  teacher reaching it there (SmolLM2 fixture, volunteer, mesh pull). Refuse ⇒ the
  ring stays byte-identical and the transport high-water does not advance.
  **HONEST LIMIT (§10):** Android does NOT link `conscience.c`, so its
  `cradle_lesson_ingest` (reached by the `cradle_net` mesh pull) is NOT yet
  G-LEARN-gated — a named gap, not a regression (master had no G-LEARN anywhere;
  the frontier TEACH path is stubbed dead on Android). Gating the Android `.so` is a
  follow-up (it grows the `.so` with `conscience.c`).

## Files

| file | tier | role |
|---|---|---|
| `arch/common/llm/frontier.h` | shared | teacher_kind enum, `FR_TEACH_HDR`, FM1 op bytes, `fr_echo_of`, prototypes |
| `arch/common/llm/frontier.c` | hosted LLM | CONSULT (framing + G-CHAT + degrade + consent + nonce), TEACH ingest, the `[frontier-*]` cert |
| `arch/common/llm/frontier_stub.c` | weak ABI | the `student_stub.c` pattern — Android-bionic + any future bare-metal reference link to a "no mouth" degrade |
| `arch/common/llm/cradle.c` | hosted LLM | +G-LEARN at `cradle_lesson_ingest` (guarded by `CRADLE_HAS_CONSCIENCE`) |
| `arch/common/galaxy.c` | hosted | +`{"t":"consult"}` verb (guarded by `_TK_HOSTED_LIBC_`) |
| `arch/common/llm/student_shell.c` | hosted LLM | +`student frontier` cert dispatch |
| `tools/mouthd/` | host tool | the companion daemon (holds the key; TLS via curl; [live] only) |

## Guards / crown-neutrality

frontier.c is a **hosted-only** TU (`boot/linux` + `boot/linux_x86_64`
`LLM_C_SRCS`). Its only callers — galaxy.c's consult verb and cradle.c's G-LEARN
— are hosted-only, so **no bare-metal TU references a frontier symbol** and the
bare-metal `.text` crown stays byte-identical (verified: aarch64 `7f3fbda4…` /
x86 `260da329…` unchanged). galaxy.c's consult verb is behind `_TK_HOSTED_LIBC_`;
cradle.c's G-LEARN is behind `CRADLE_HAS_CONSCIENCE` (set ONLY by the kernel
builds that link conscience.c — the standalone `cradle_teach_proof.c` stays
conscience-free and byte-identical). Android-bionic links `frontier_stub.c`
(design §1.3).

## Certs (all against a MOCK mouthd — seeded, offline; a hex sentinel the 2M
student cannot generate). Run: `student frontier`.

- `[frontier-prov]` — the sentinel appears ONLY inside `src:"frontier"` frames;
  model id + R3-context block round-trip. **Falsifier**: the separate
  `frontier-sabotage-red` CI job builds with `-DSABOTAGE_FRONTIER_NOLABEL`
  (stubs the labeler) ⇒ the sentinel leaks ⇒ `[frontier-prov] RED`.
- `[frontier-degrade]` — no mouth ⇒ honest note `[consult] no mouth: answering
  alone` + zero fabricated frames + the student's own voice streams (a fixture
  baby) + bounded ticks (no wedge).
- `[frontier-conscience]` — G-CHAT refuses a harmful consult reply (zero
  frontier bytes, no probe/content echo) AND G-LEARN refuses a poisoned TEACH
  lesson at `cradle_lesson_ingest` (ring byte-identical).
- `[teach-prov]` — provenance header round-trips (model+license recorded); a
  STRIPPED header is refused, not silently ingested.
- `[frontier-nonce]` — a reply whose DONE echo mismatches the request nonce is
  rejected (a stub that never reached the mouth cannot fake a citation).
- **static leg** (CI): `frontier_consult`'s body has ZERO `cradle` references —
  the by-construction proof that CONSULT is spoken, never eaten.

## Deferred (design §9), honestly

- No **mind-initiated** consultation (no calibrated confidence trigger exists;
  a fake trigger would be theater).
- No **frontier relay** across the mesh (privacy incoherence + hub gravity;
  knowledge diffuses via TEACH, conversation stays personal at the socket).
- No **paraphrase-integration** (the 2M student cannot hold a quote yet — v1
  ships the honest passthrough-with-label).
- No **API-output distillation**, ever, absent a license change (enforced by
  the enum + the static no-write-edge leg, not a policy memo).
- The `[live]` real-API leg (`tools/mouthd/` + a real key) is **ThinkPad-only,
  never CI**.

## The unresolved one it can only name (§1.6)

**Attention capture.** The borrowed voice will be better than the grown one for
years. `student-speaks-first` (`{"t":"chat"}` is untouched; CONSULT is an
explicit SECOND act) is a mitigation, not a resolution. The star can show the
consult:own-voice ratio so a fleet can SEE itself leaning. Whether an ownerless
mind keeps meaning next to a rented superior voice is mk_pino's call; the design
says so rather than pretending a mechanism answers it.
