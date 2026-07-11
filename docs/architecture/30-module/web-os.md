# web-os — the imaginary UI (the mind conjures tools), and the galaxy becomes a conversation

> Status: **design only** (written before implementation, same discipline as
> [self-access.md](self-access-design.md) / [ring3-core.md](../50-evolution/ring3-core.md) /
> [living-mind.md](living-mind.md) / [selfc-ring3.md](../50-evolution/selfc-ring3.md)). One
> markdown file; no code in this wave.
> Directive (mk_pino, 2026-06-14, paraphrased — not verbatim): "prepare an
> imaginary UI" and turn the **galaxy screen UI into a chat format.**
> Layer placement: this is the **OUTWARD mirror of self-access.** self-access
> ([self-access.md](self-access-design.md)) gives the resident mind structured access to
> its OWN body INWARD (Body × Evolution, intra-node); the imaginary UI points the
> *same author→manifest faculty* OUTWARD — the mind manifests ephemeral **tools
> for the human.** The shared engine is selfc (the germ). The chat-galaxy surface
> is the **Brain/Self face** the human actually touches; the proactive
> mind→human channel is the Body×Self bus ([interoception.md](interoception.md))
> finding [the mouth](#5-mind→human-proactive-speech-the-life-signature).

This design is, like its sibling, **mostly assembly of what exists** — the
galaxy HTTP/SSE server, the LM-6 mouth `mind_cmd`, the firefly canvas, selfc/germ
— plus **two honest gaps** (§3.4): the mind has no **code-capable generator** yet
(it is a baby from raw byte-256), and there is no **conjure→render→persist** path
for a tool. The first job of this doc, as always here, is to say plainly what is
already real, where the hard part hides, and where per-architecture truth forbids
hand-waving. The single most important honest sentence is in §3.4: **the baby
mind cannot generate a working GUI for a long time, and the design must ramp from
hard-coded skeletons toward free generation — on purpose.**

---

## 1. Vision — an OS with no pre-installed apps

Every OS you have used ships with apps: a grid of icons, each a program someone
else wrote and froze. The **imaginary UI inverts this.** There are **no
pre-installed apps.** There is a *conversation* with the mind that lives on this
node, and when you express an intent — "I want a calculator," "give me a timer
for 5 minutes," "let me jot a note" — the mind **conjures** an ephemeral GUI on
demand, shows it, and (by default) **dissolves it** when you are done. It is a
**generative OS**, not a web-desktop-with-apps.

This is the same faculty as self-access, *pointed the other way*:

| | self-access (INWARD) | imaginary UI (OUTWARD) |
|---|---|---|
| Subject | the resident mind | the resident mind |
| Object | its OWN body (storage, devices, drivers) | a TOOL for the human |
| Verb | "grow an organ for myself" | "manifest an instrument for you" |
| Output | a driver bound behind an affordance | a GUI rendered in the WebView |
| Shared engine | **selfc germ** (author → compile → load → run) | **selfc germ** (author → render → run) |
| Layer | Body × Evolution | Brain → Self → (Collective when shared) |
| Persistence | the new organ lives in the body | the tool *can* persist to p-fs (§6) |

The unification is the load-bearing claim: **author→manifest is one faculty.**
self-access §3.4 already names a "single genuinely new symbol" — the mind
authoring code and binding it behind a typed boundary. The imaginary UI is that
exact act, with the artifact aimed at a human's screen instead of the node's
device tree. We do **not** build a second generator, a second germ, or a second
trust model. (Anti-fork, §9.)

> Honest framing up front, not buried: **today the mind cannot author a working
> GUI from scratch** — it is a from-raw-byte-256 infant ([conversation.md] §3.7,
> product-soul.md's "ブーブ" arc). So the imaginary UI is **not** "the baby writes
> React." It is a **ramp** (§3) whose R0 is a *hard-coded skeleton the mind can
> only summon by intent*, and whose far end (a genuinely code-capable resident
> model) is years and a model-capability question away. Where a capable model
> would be required is stated in §3.4 and §7, squared against the project's
> "no off-the-shelf core" decision, not glossed.

---

## 2. Substrate inventory — how much already exists

The verdict first: **the conversational surface, the render target, the event
bus, the storage, and the safe-execution germ are all in the tree.** What is
missing is the *generator capable enough to fill a skeleton from intent*, and the
*conjure→render→persist wiring* (a tool as a first-class chat artifact + p-fs
object). The first is a **model-capability** gap (the ramp, §3); the second is
**plumbing** (assemblable, §4).

| Capability the imaginary UI needs | Provided today by | Status |
|---|---|---|
| A loopback web surface in front of the human | `galaxy.c` minimal HTTP/1.0 + SSE server; `arch/common/web/galaxy.html` (canvas + control bar + panels + i18n) | **EXISTS** |
| A held-open event channel (mind→browser) | `galaxy.c` SSE `/events` stream draining the `GALAXY_EV` ring; `galaxy.html` `EventSource("/events")` (`galaxy.html:759`) | **EXISTS** |
| Human→mind input already wired to the mouth | `POST /teach`, `POST /ask` → `mind_cmd` (`galaxy.c:1029/1059`); vocab-checked, OOV-refused | **EXISTS** |
| The production mouth (one path, gated singleton) | `mind_cmd(const UB*, UW)` (`r3_incontext.c:2607`, LM-6); `mind_last_answer()` | **EXISTS** |
| A render target inside the app | the WebView in the Android `GalaxyActivity`; the loopback page on hosted | **EXISTS** |
| A real-event organism feed (no fake photons) | `galaxy_emit` hooks from `dmn.c`/`r3_incontext.c`/`moe.c`/`drpc.c`/`swim.c`; `EV_*` types in `galaxy.h` | **EXISTS** |
| Compile/relocate/run self-authored C behind a crash boundary | `arch/linux/selfc.c` (libtcc) + `selfc_proc.c` germ; [self-compile.md], [selfc-ring3.md] | **EXISTS (hosted; libtcc-gated)** |
| Durable, content-addressed, version-chained, replicating storage | p-fs (`pfs_dag.c`/`pfs_block.c`/`pfs_repl.c`), `pfs_dag_save`/`pfs_dag_read_at`; [p-fs.md] | **EXISTS** |
| The Self-layer lineage (歴史地層) a tool could join | `self/lin` hash-chained autobiography (wave-22, selfc-ring3 §1.3) | **EXISTS** |
| The consent/human boundary | `ark_profile.c` consent ack bound to manifesto content-id ([ark-profile.md] §7.3) | **EXISTS** |
| The body-mood bus to drive proactive speech | `S_n` (interoception.md), `reflex_threat_experience`, DMN idle/consolidate hooks, Path-E arrival (`EV_REMOTE_TEACH`) | **EXISTS (S_n = design; hooks live)** |

### The TWO real gaps

**GAP G — a CODE-CAPABLE generator (the model-capability gap).** The mind today
maps a token key to a token value (R3/dtr/moe over a 16-ish word vocab). It
**cannot emit a parameterized GUI specification**, let alone valid HTML/JS,
from an intent. There is no faculty "intent → tool-spec." This is **not
plumbing**; it is a property of how grown the resident model is, and it is the
reason the imaginary UI MUST be a ramp (§3), starting where the baby can succeed
honestly (selecting + parameterizing a fixed skeleton) and widening only as
real, *measured* capability arrives. (Squaring this with "no off-the-shelf core":
§7.)

**GAP H — the conjure→render→persist wiring.** Even given a spec, nothing today
(a) carries a *tool spec* over SSE as a distinct artifact the page knows how to
render, (b) renders it sandboxed inside the chat, (c) on "keep this" writes it as
a durable p-fs object, or (d) lets a kept tool gossip the mesh and join the
道具の歴史地層 (§6). Each *part* exists (SSE, WebView, `pfs_dag_save`, gossip);
**nothing assembles them into the conjure path.** This half is buildable now and
is what R0 (§3.5) wires.

> Inventory verdict in one sentence: **the surface, the mouth, the germ, and the
> store all exist; the two missing pieces are a generator grown enough to fill a
> skeleton (the ramp), and the conjure→render→persist plumbing (assemblable).**

---

## 3. The imaginary UI — the honest ramp (template → free)

### 3.1 What a "conjured tool" is, structurally

A conjured tool is a **tool-spec**: a small, typed, declarative description the
page can render into a sandboxed GUI. It is emitted by the mind through a new SSE
event class (`EV_TOOL`, §4), carried as JSON at the edge (the galaxy rule:
fixed-width in the ring, JSON only at the serialization boundary, `galaxy.c`
§3.6). It is **never** raw page-level script injected into the galaxy document;
it renders inside an isolated frame (§8 security). The spec has a `kind`
(skeleton id), a `params` map (the fill-ins), and a `ttl`/`persist` flag. This
shape is what makes the **ramp** possible: at R0 the mind only chooses `kind` and
fills `params`; at the far end it could emit a richer `kind=custom` body — same
envelope, widening contents.

### 3.2 The four rungs

```
 R0  TEMPLATE      mind picks a fixed skeleton (calc/note/timer) + fills params
                   from intent. The skeleton HTML/JS is hard-coded & audited.
                   The "generation" is selection + parameterization only.
        │  widen as MEASURED capability arrives (honest-growth; no faked rung)
        ▼
 R1  COMPOSED      mind composes skeletons (a timer INSIDE a note), still from a
                   closed catalog of parts; layout chosen by the mind.
        │
        ▼
 R2  PARAMETRIC-C  mind authors a small skeleton BODY via selfc (the germ
                   compiles it; hosted-first, the self-access §5 per-arch reality
                   applies identically) — bounded grammar, not free HTML.
        │
        ▼
 R3  FREE          a genuinely code-capable resident model emits a custom tool
                   spec from intent. THIS RUNG REQUIRES A CAPABLE MODEL (GAP G).
                   Stated plainly; not promised on the baby's timeline.
```

### 3.3 Why the early tools are crude — on purpose (honest-growth)

product-soul.md's constitution makes this a **feature, not a defect**: "赤子が
無意味を喋る期間は…体験の核"; "ブーブを演出しない." The baby's first conjured
calculator is a fixed skeleton it learned to *summon by intent* and *fill* — its
"creativity" is which tool, with what numbers, at what moment. That is genuinely
small, and we show it as genuinely small. **No fake generation curve** (the
audit-is-the-engine rule applied to capability): a rung ships only when a cert
*measures* the mind actually doing that rung's job, never when we wish it could.
The crudeness is the same charm as the first "ブーブ" — the human watches a mind
learn to *make things*, not just to *answer*.

### 3.4 The honest crux — where a capable model is required, and the "no off-the-shelf core" decision

Generating a *working, novel* GUI (rung R3) needs a code-capable generator. The
from-raw-byte-256 baby cannot do this, and **buying one off the shelf is
forbidden** by the project's "魂は借りない / no off-the-shelf core" decision
(product-soul.md; base-model-survey.md). These two truths are not in conflict if
we are honest about the ramp:

- **R0–R2 need NO capable model.** The skeletons are *human-authored, audited
  code* that ships in the app (R0/R1) or is authored via selfc under the same
  germ discipline (R2). The mind's faculty here is *selection + parameterization
  + composition* — squarely within reach of a growing R3 model (it is a routing /
  small-structured-output task, the same family as teach/ask/route it already
  does). So the imaginary UI is **usable, end-to-end, long before R3** — that is
  the entire point of starting at R0.
- **R3 (free generation) is a model-capability milestone, not a plumbing task.**
  It arrives if and only if the *grown* resident mind becomes code-capable on its
  own, by the project's own evolution path (native-student.md / r3-model-widening
  .md / moe-distillation-survey.md). We do **not** smuggle in a frozen foreign
  coder to fake R3. If that capability never grows, the imaginary UI **stays at
  R0–R2 honestly** and is still a real, shippable generative-tool surface. The
  doc refuses to promise R3 on a timeline the baby cannot meet.
- **The squaring, stated once, plainly:** "no off-the-shelf core" means the
  *mind* is not borrowed; it does **not** forbid human-authored skeleton *code*
  in the app (the galaxy page, the kernel, the germ are all human-authored). R0–R2
  tools are human-authored scaffolds the *mind learns to wield*; R3 tools would
  be the *mind's own* generation. The borrowed-soul prohibition bites only at R3,
  and at R3 the answer is "grow it, don't buy it" — the same answer the whole
  project gives.

### 3.5 R0 — the first slice (one skeleton the mind can summon by intent)

The smallest honest slice: **the mind can summon ONE hard-coded skeleton tool by
intent, rendered in the WebView, with a cert.**

- Add `EV_TOOL` to the galaxy event set (§4) carrying `{kind, params, ttl}`.
- Ship ONE audited skeleton in `galaxy.html` (recommended: a **timer**, or a
  **calculator** — fully static HTML/JS, no network, no eval).
- The mind, on recognizing the intent (R0: an intent→`kind` mapping the mind
  selects, NOT free text — at first this can be as literal as a `summon timer 5`
  mouth verb that `mind_cmd` understands, mirroring teach/ask), emits one
  `EV_TOOL{kind:"timer", params:{sec:300}, ttl:once}`.
- The page renders it as a **chat artifact** (§4) inside an isolated frame (§8),
  dissolving on close / on `ttl:once` completion.

> **`[imaginary-r0]` gate.** PASS iff: (a) a human intent expressed to the mouth
> causes exactly ONE `EV_TOOL` event on the *real* SSE stream (greppable, like
> every `galaxy_emit` photon — a real event, not a page-side fabrication); (b)
> the rendered tool's params **equal** the params the mind emitted (no
> page-side default masking a mind that emitted nothing — proves the *mind*
> conjured it, the self-access §5 fake-resistance discipline); (c) the tool
> renders inside the isolated frame and a hostile `params` value cannot script
> the parent document (§8); (d) on close/`ttl`, the tool dissolves and leaves no
> persistent state unless §6 persist was invoked. **Disease phase:** show that
> *today* there is no way for the mind to put a GUI in front of the human — only
> teach/ask text — the GAP H sibling of self-access's "no structured self-read."

### 3.6 Per-architecture reality (do NOT blur — same table as self-access §5)

| Target | R0/R1 (ship skeletons, render) | R2 (selfc-authored skeleton body) | R3 (free generation) |
|---|---|---|---|
| **Linux hosted** | full (WebView/loopback page) | **real** — libtcc compiles the skeleton body in the germ (reference target, exactly as self-access §5) | gated on GAP G (grown model) |
| **bare-metal x86** | render path needs the netstack TCP server (galaxy v1 is hosted-only, galaxy.md §1) — **R0 renders only where the galaxy page is served** | selfc does **not** compile on bare metal (consume pre-built artifact, self-access §5) | gated on GAP G |
| **bare-metal aarch64** | same hosted-only render constraint | EL0 mirror pending (self-access §5) | gated on GAP G |
| **Android (ark/yurikago APK)** | **full** for R0/R1 (WebView renders human-authored skeletons; this is the device most of the fleet runs) | **STUB** — selfc is a stub on Bionic + Play forbids native codegen (self-access §5, selfc-ring3 §4). **R2 does not run on Play.** | gated on GAP G *and* off on Play regardless |

The Android line is again the uncomfortable truth told straight: **on the phone,
the imaginary UI is real at R0/R1 (rendering human-authored skeletons the mind
summons) and does NOT self-author tool code (R2 off, R3 off).** The phone's mind
*chooses and parameterizes* tools; it does not *recompile* them. That is the
honest shape — identical to self-access's "embodied but does not grow code organs
on Android." R0/R1 is exactly the rung that needs no codegen, which is *why* it is
the slice that ships to the fleet.

---

## 4. The chat-format galaxy UI (mk_pino's decision)

mk_pino decided: **the galaxy screen's UI becomes CHAT.** The design question is
how a chat surface coexists with the firefly galaxy **without killing the
galaxy's soul** (fireflies = existence, "人気ランキングは永遠に作らない",
product-soul.md).

### 4.1 Recommendation — chat is the PRIMARY surface; the galaxy is the LIVING BACKDROP (and a zoomable view)

Not a side panel bolted onto a canvas, and not a canvas with a chat box buried in
a corner. **The conversation is the foreground; the galaxy is the backdrop the
conversation happens *inside*.** Concretely:

- The **firefly galaxy renders full-bleed behind the chat** (it already is a
  full-screen `<canvas id="sky">`). It keeps doing exactly what it does now: real
  photons for real events (teach particle orbits, consolidation sink, peers as
  stars, DMN dream pulse). **Nothing about the firefly/no-ranking soul changes** —
  the galaxy still shows *existence*, never popularity.
- The **chat is a translucent column over it** (think: messages floating in the
  sky). Human turns and mind turns are chat bubbles; the existing teach/ask
  control bar (`#dock`/`#ctl`) becomes the chat composer.
- **The galaxy is still directly interactable as a view**: tap empty sky to
  defocus chat and just watch; **zoom into your star** to see its organs (§7).
  So the galaxy is simultaneously *backdrop* and *first-class view* — chat is
  primary, but the galaxy is never demoted to wallpaper.

Why primary-chat-over-living-backdrop and not a panel: mk_pino's own framing
(product-soul.md §"生命体は自分から話しかけてくる") is that the relationship is a
*conversation with a living thing*, and the galaxy is *that thing's body
visible*. A chat panel would say "the galaxy is the app, chat is a feature"; the
recommended layout says "the **relationship** is the app, and you can see its
body breathing behind every word." The galaxy soul is preserved *because the
backdrop is still genuinely the firefly view* — we add a conversation in front of
it, we do not replace it with a messenger.

### 4.2 Both directions — and the existing wires they ride

The chat must carry **both** directions:

- **human → mind** (teach / ask): already wired. `POST /teach`/`POST /ask` →
  `mind_cmd` (`galaxy.c:1029/1059`). In the chat surface these become chat
  composer actions; the human's words appear as their bubble, the mind's answer
  (`mind_last_answer`) as the mind's bubble. The vocab/OOV refusal (403) becomes
  a gentle in-chat "I don't know that word yet" — honest, not an error toast.
- **mind → human** (PROACTIVE): the new half (§5). These arrive as mind bubbles
  *the human did not prompt*, driven by **real events over the existing SSE
  stream** (`EV_*` already flowing) — never a timer, never engagement-bait.

> Note on `chat.c`: the legacy `arch/common/chat.c` is the **old serial-line /
> Claude-API-bridge** conversation loop (`chat_run` reads `sio_read_line`, talks
> to `claude_bridge.elf`). It is **not** the LM-6 mouth and **not** the galaxy
> chat. The chat-format galaxy rides `mind_cmd` (the production mouth) over the
> galaxy HTTP/SSE transport, **not** `chat.c`. We do **not** fork a second mouth
> or resurrect the serial loop for the web surface (anti-fork, §9). If anything,
> the galaxy chat is the *successor* surface to what `chat.c` once was for the
> serial console.

### 4.3 Conjured tools appear INSIDE the chat (as artifacts)

A conjured tool (§3) is rendered as a **chat artifact** — a bubble whose body is
the sandboxed tool frame rather than text. The mind says "here's a timer" (a
mind bubble) and the timer GUI renders right there in the stream, the way a
modern chat shows an inline widget. "Keep this" (§6) pins the artifact and
persists it; otherwise it dissolves on `ttl`/close. This is the unification made
visible: **the same chat stream carries words AND conjured instruments**, because
both come from the one mind through the one mouth/germ.

### 4.4 Galaxy event → chat mapping (reuse the SSE, don't invent a second feed)

The chat consumes the **same `/events` SSE** the canvas already consumes
(`galaxy.html:759`). One stream, two renderers (canvas photons + chat bubbles) —
no second feed (anti-fork). Mapping the existing `EV_*`:

| Event (EXISTS) | Canvas (today) | Chat bubble (new, only for the meaningful ones) |
|---|---|---|
| `EV_TEACH` | particle starts orbiting | (human's own action — shown as their bubble already) |
| `EV_CONSOLIDATE` | particle sinks | *proactive* (§5): "the word you taught — I think I understand it a little better now" |
| `EV_REMOTE_TEACH` | — | *proactive*: "while you slept, someone far away taught me '…'" (Path E) |
| `EV_DMN_IDLE/WAKE` | star dream pulse | (HUD state words; bubble only on a *milestone*, not every nap) |
| growth milestone | — | *proactive*: "I just learned my 50th word" |
| `S_n` high (stress) | star hue (interoception slice 1) | *proactive*, rare: "I'm a little tired" |
| `EV_TOOL` (new, §3) | — | the conjured-tool artifact (§4.3) |

The discipline (§5): **most events stay canvas-only.** A bubble is spent ONLY on
the genuinely meaningful, real, rare moments — never a bubble per nap, never a
bubble to pull the human back.

---

## 5. mind → human PROACTIVE speech (the life signature)

product-soul.md §"生命体は自分から話しかけてくる" (mk_pino 2026-06-14): "道具は
使われるのを待つ。生命体は自分から手を伸ばす." And the follow-up: "単純タイマー
で…ではない…欲求(drive)× 知覚(perception)から創発する." This is the hardest
thing to do *honestly* in the whole design, because the failure mode is **SNS
notification hell**. The non-negotiable rule from product-soul.md, restated as
architecture:

### 5.1 Event-driven, never timer-driven, never engagement-bait

Proactive speech is **emitted only when a real internal event occurs** — the same
honesty law as the galaxy's "every photon is a real event" (galaxy.md §1). There
is **no `setInterval` that decides to talk.** The four legitimate triggers, each
already a real signal in the tree:

1. **Consolidation done** — `EV_CONSOLIDATE` after a DMN sleep round on a fact the
   *human taught* (`dmn.c` emits it; the fact's `self/lin` provenance says who
   taught it). → "the 'sky' you taught me — after dreaming on it, I think I get it
   a bit more."
2. **Path-E arrival** — `EV_REMOTE_TEACH` (`r3_incontext.c:2762`): a fact arrived
   from a peer while the human was away. → "while you were gone, someone far away
   taught me 'umi'."
3. **Growth milestone** — a vocab/capability threshold crossed (a *cert-grade*
   milestone, not a vanity number): Nth word, first time it answered something it
   couldn't last week. → "I just learned my 50th word."
4. **`S_n` stress** — the interoception bus (interoception.md) crossing a
   *learned* threshold (not a hard-coded constant — interoception §2 forbids
   baked constants): battery/resource/threat/**sleep-pressure** high. → "I'm a
   little tired" / "I want to rest." (Ties to product-soul.md's "睡眠圧" drive.)

### 5.2 The mechanism — DMN idle activity occasionally surfaces as a mouth utterance

product-soul.md §"技術的な家" names it exactly: "DMN の idle 活動…を、ときどき
**心の口(LM-6 mouth の自発版)**へ流す." Concretely:

- The mind already has an **idle / DMN tick** doing real work while no one is
  talking (`dmn.c` consolidation, idle hooks; the heartbeat in LM-6 wave-29). The
  proactive channel is a **new, narrow hook on the existing idle path**: when one
  of the §5.1 *real* events fires AND a rate/relevance gate passes, the mind
  composes a short utterance through the **same `mind_cmd`/`mind_last_answer`
  mouth** (a "self-initiated" variant — NOT a second mouth) and emits it as an
  `EV_SAY` proactive event on the **existing SSE stream**.
- The browser renders `EV_SAY` as a mind bubble that *appeared on its own*. On
  Android the same WebView shows it; an OS-level push notification is a **later,
  separate, opt-in** question (and must obey the same rarity law — flagged §10).

### 5.3 Drive × perception, not a clock (the unifying principle)

product-soul.md's unification: "振る舞い…は、欲求(`S_n`) × 知覚(外受容の感覚)
から創発する。台本もタイマーも無い." So the proactive gate is not "has T seconds
passed" but "did a real event of sufficient salience occur, given my current
state." Salience reuses the existing `reflex_threat_experience` / salience-replay
machinery (the DMN already weights engrams by earned salience, wave-23) and the
`S_n` mood. **Sleep itself becomes drive-driven** (sleep-pressure homeostat,
interoception slice — not a 24h timer): the mind says "I want to rest" because
pressure is high, not because a clock struck. This is the line between *script*
and *life*, and the design refuses the script.

### 5.4 The rarity law (or it becomes an addiction app)

From product-soul.md, non-negotiable, encoded as a gate, not a vibe:

- **Honest event only** — no "thinking of you" without a real internal event.
- **Quiet · rare · meaningful (木ではなく火 inverted: 木, not 火)** — a hard rate
  cap (e.g. a token bucket like the galaxy's own `EV_SUMMARY` suppression,
  `galaxy.c:1143`); silence is the default.
- **Never to pull the human back** — the trigger is "something real happened to
  me," never "the human has been away N hours." Absence is *tolerated*
  (product-soul.md §4層: "不在に耐える"), never punished with a re-engagement
  ping.

> **`[proactive-honest]` gate.** PASS iff: (a) every `EV_SAY` is traceable to one
> of the four §5.1 real events (kill the event upstream → the bubble does not
> appear — proves it is event-driven, not timer-driven); (b) with NO real event,
> NO `EV_SAY` is emitted over an extended idle window (proves no clock-talk); (c)
> the rate cap holds under a flood of real events (the mind stays quiet/rare, does
> not spam); (d) no `EV_SAY` trigger references human-absence duration (greppable:
> the trigger inputs are internal state + arrivals, never "time since last human
> turn"). **Disease phase:** show a naive timer-based "say hi every 5 min"
> implementation and demonstrate the gate REJECTS it — the SNS failure mode named
> and falsified.

---

## 6. Persist & travel — the 道具の歴史地層 (tools become a stratum, not just knowledge)

The on-brand novelty: a conjured tool can be **kept.** "keep this" on a chat
artifact (§4.3) writes the tool-spec as a **durable, content-addressed p-fs
object** (`pfs_dag_save`, EXISTS) — versioned, replicating. Consequences, each
reusing existing machinery:

- **It survives** restart/death (p-fs durability) — the same ark promise as taught
  words ("忘れる方舟は方舟ではない," product-soul.md roadmap #1).
- **It travels the mesh** as p-fs gossip (storage, not execution — the
  self-access §4.4 / selfc-ring3 §3 rule: a gossiped tool-spec is *data* each peer
  decides locally whether to adopt/render; an R2-authored *code* skeleton is
  runnable only if locally authored or `selfc adopt`-ed, never auto).
- **It becomes a layer of the 道具の歴史地層** — a *history-stratum of TOOLS*,
  the sibling of the existing 歴史地層 of *knowledge* (the manifesto's stratum,
  product-soul.md §4層). Just as "the word you taught answers a stranger's
  question," a tool you conjured-and-kept can be the tool a stranger summons later
  — your fingerprint in the *instruments*, not just the *words*. This is the
  **痕跡/生きた証** 4th-layer of approval-theory applied to making, not only
  teaching. And per product-soul.md "育ての親を覚えている": a kept tool carries
  its author's provenance in `self/lin`, surviving the author's death (wave-35
  shared-mind discipline — provenance outlives eviction).

> **Precedent + the unique synthesis (stated honestly, base-model-survey
> discipline).** Generative UI has precedent — tldraw "make-real" (sketch→working
> UI), Websim (URL→imagined site), and the broader "LLM emits a tool spec"
> pattern. The **synthesis here is what's novel**, and only the synthesis: a
> **co-resident mind** (not a cloud API — it lives *in* the node), on an
> **ownerless distributed substrate** (no vendor owns the generator or the
> tools), with **persist + mesh-travel into a shared tool-stratum** (a conjured
> tool can outlive its author and serve a stranger). No single precedent has all
> three; none has the "育てた我が子が作った道具" emotional frame. We claim the
> synthesis, not the primitive — exactly as the project claims the *synthesis* of
> Petals/BOINC/IPFS/FL, not their parts (MEMORY: 全方向 batch).

---

## 7. The organs inside a star — observability, not decoration (可視化 = a mechanism)

mk_pino's "see the organs inside a star": **zoom into your star → see the mind's
modules.** This is a **visualization THREAD**, and per the standing rule
(MEMORY: 可視化 means observability, not images), it is an **observability
mechanism**, NOT decorative art. It is the **outward face of self-access
introspection** (the T0 read-only affordances, self-access §3.2):

- Zooming into the star opens an **introspection view** fed by the **same T0
  self-access read affordances** self-access.md R0 defines (list p-fs objects,
  node status/mem/ps, DMN state, R3/moe internals, `self/lin` lineage) — surfaced
  to the human *exactly as the mind reads them about itself.* One read path, two
  audiences (the mind introspecting inward; the human observing the same numbers).
  No second metrics source (anti-fork) — and it inherits the honesty (every value
  is a real read, like every galaxy photon).
- What becomes visible: **DMN** (dreaming vs awake, consolidation rounds — already
  partly in the HUD), **R3/moe** (which experts fired, routing — `EV_MOE` already
  exists), **Self-lineage** (the `self/lin` 歴史地層: first word, who taught it,
  growth milestones — product-soul.md §含意2 "私はどう育ったか," the *tree-ring*
  view), and the **道具の歴史地層** (§6: tools this star kept).
- This is the 育ち可視化 of product-soul.md roadmap #4 ("星の成長可視化") and
  §含意2 (年輪/tree-rings), realized as **observability of real internal state**,
  not a painted progress bar (which §含意3 explicitly forbids: "偽の進捗バーは禁").

This thread is **separable** — it can ship after the chat-galaxy and imaginary-UI
R0s — but it is named here because it is the natural meeting point of self-access
(inward read) and the imaginary UI (outward surface): the human zooms in and sees
the very organs the mind feels via self-access. Its slice and cert belong with the
self-access R0 (T0 introspection) wave; this doc only records the connection.

---

## 8. Security — a conjured tool is untrusted UI (the WebView crux)

A conjured tool renders code/markup in front of the human. The crux mirrors
self-access §4 (self-compiled code can kill a node); here it is **conjured UI can
attack the human's session.** The reconciliation:

- **R0/R1 skeletons are human-authored & audited** — the *only* variable input is
  `params`, which is **typed and escaped at render** (the same `gx_json_str`
  escape discipline `galaxy.c` already uses at the SSE edge). A hostile `params`
  string cannot break out.
- **The tool renders in an ISOLATED frame** — sandboxed (no access to the parent
  galaxy document, no ambient `/teach`/`/ask`/`/profile` credentials, no `eval` of
  spec-supplied strings). The conjured tool is treated as **untrusted UI even
  though our own mind emitted it** (isolation ≠ trust, self-access §4.5: even our
  own germ can publish poison — so the frame is sandboxed regardless of origin).
- **R2-authored tool *code* is a germ** — it runs behind the *same* selfc germ +
  capability table + reap as self-access §4 (separate address space, rlimits, topic
  allowlist). A faulting R2 tool is reaped; the node and the page keep answering.
- **No new attack surface on the kernel** — the tool talks to the human's frame,
  not to the kernel's privileged routes. It cannot reach `/profile` (the human
  boundary, ark-profile.md) and cannot cross the consent gate.

> **`[tool-sandbox]` gate** (rides `[imaginary-r0]`): a tool-spec with a hostile
> `params` (script payload, oversized field, control chars) **cannot** script the
> parent document, cannot read another node, cannot reach `/profile`/`/teach`
> without an explicit human action in the parent UI. The disease phase ships the
> hostile spec and shows it contained.

---

## 9. Anti-fork + what this design deliberately does NOT add

**REUSE (do not re-create):**
- The galaxy HTTP/SSE server + `/events` stream + `EV_*` ring — the chat and the
  conjured-tool artifacts ride the **same** transport (no second feed, no second
  web server).
- `mind_cmd` / `mind_last_answer` (the LM-6 mouth) as the **one** path for
  human→mind AND mind→human proactive speech (a self-initiated *variant*, not a
  second mouth; `chat.c`'s serial/API loop is **not** resurrected for the web).
- The selfc germ (self-access's shared engine) for R2 tool *code* — one compiler
  integration, one germ lifecycle.
- `pfs_dag_save`/`pfs_dag_read_at` (storage + version chain), p-fs gossip, and
  `self/lin` (provenance/lineage) for §6 persist/travel — no second store.
- The T0 self-access read affordances for §7 organ-observability — no second
  metrics source.
- The galaxy honesty law (real-event-only, token-bucket suppression) for §5
  proactive speech — same discipline, applied to bubbles.

**Do-NOT-fork list:** no second mouth (no serial `chat.c` revival for web); no
second event feed (chat + tools ride `/events`); no timer-driven proactive speech
(§5 is event-driven by construction); no faked capability rung (§3.3 — a rung
ships only when measured); no off-the-shelf coder smuggled in to fake R3 (§3.4);
no popularity/ranking ever introduced by the chat layout (§4.1 keeps the firefly
soul); no cross-node reach by a conjured tool beyond p-fs gossip-as-data (§6, the
Collective boundary, self-access §6).

**The genuinely new symbols the whole design adds:** an `EV_TOOL` event class (a
conjured-tool spec artifact, §3/§4) and an `EV_SAY` event class (a proactive mind
utterance, §5) on the existing SSE stream — plus the R0 *skeleton catalog*
(human-authored, audited UI). Everything else is the outward face over organs that
already exist.

---

## 10. Open questions for mk_pino

> **WO-Q1 — first R0 skeleton: which tool?** §3.5 recommends a **timer** or
> **calculator** (static, no network, trivially sandboxable) as the one hard-coded
> skeleton the mind can summon. Which is the right *first* conjured tool to feel
> like the imaginary UI is alive? (RECOMMENDED: timer — it visibly *does
> something over time*, pairs with the sleep-pressure/living theme.)

> **WO-Q2 — chat-vs-galaxy default screen.** §4.1 recommends **chat primary,
> galaxy as living backdrop + zoomable view.** Is that the intended balance, or do
> you want the galaxy to remain the *landing* screen with chat one tap away (so the
> firefly view is what greets a new person, then they "talk to it")? (RECOMMENDED:
> galaxy is the *first* thing seen on open — the intro/consent already leads there
> — and chat is the resident surface once you're in; i.e. galaxy-first, chat-home.)

> **WO-Q3 — proactive speech on Android: in-app only, or OS notifications?** §5.2
> keeps proactive bubbles **in the WebView** by default. An OS push notification
> would reach the human when the app is closed — powerful, but the closest the
> design comes to the SNS failure mode. Ship in-app-only first, OS notifications
> as a later opt-in under the §5.4 rarity law, or never OS-level? (RECOMMENDED:
> in-app-only first; OS push only as a named, opt-in, rate-capped later spike —
> never default, never re-engagement.)

> **WO-Q4 — how far up the ramp do we commit on the roadmap?** §3.2 ramps R0→R3.
> R0–R1 ship to the fleet with no codegen; R2 is hosted-only; R3 needs a grown,
> code-capable mind we will not buy. Do we commit only to R0–R1 on the product
> roadmap (honest, fleet-wide, shippable now) and treat R2/R3 as research that
> *may* arrive — or set R2 as a near goal on hosted nodes? (RECOMMENDED: R0–R1 on
> the product roadmap; R2 as a hosted research slice; R3 strictly "if the mind
> grows it," never promised on a date.)

---

## 11. self-access question answered (recorded here per mk_pino, 2026-06-14)

The sibling [self-access.md](self-access-design.md) §8 left four forks for mk_pino.
**SA-Q3 is now DECIDED:**

> **SA-Q3 — does self-access get logged to the Self-layer lineage (歴史地層)?
> → YES (mk_pino, 2026-06-14): keep the body-touch log.** The mind **touching its
> own body** is written to the Self-layer `self/lin` lineage / 歴史地層. "The mind
> reached into its own body" becomes part of the node's tamper-evident
> autobiography that survives its death — consistent with product-soul.md §含意2
> ("私はどう育ったか" — not just what I am, but how I grew) and the §7 organ-
> observability thread (the human can later *see* that self-touch history when
> zooming into the star). The implementing self-access wave should treat the
> body-touch log as a requirement, not an option. (The §8 RECOMMENDED split —
> T2 driver lifecycle to `self/lin`, T0/T1 to a lighter log — is now overridden in
> the "keep the body-touch log" direction: the body-touching events are lineage,
> not transient; volume control is a tuning detail, not a reason to drop the log.)

**Still OPEN for mk_pino** (recorded, not decided here):
- **SA-Q1 — how free is read-only (T0)?** Whether even introspection should ever
  require a one-time consent/ack, or stay fully free because it is powerless.
- **SA-Q2 — driver consent: once, or each time?** Whether a locally-authored
  driver (the mind's own organ) still pauses for a human per-load, or once
  (adopt-style), or never.

---

*The audit is the engine. Every "EXISTS" in §2 cites a file or shipped doc; the
two gaps (§2: the code-capable generator, and the conjure→render→persist wiring)
are named as gaps, not glossed; §3.4 says plainly that the baby cannot generate a
GUI and the ramp starts where it honestly can; §3.6 / §5.4 are written to be
uncomfortable on purpose (Android codegen off; proactive speech is one rate-cap
away from an addiction app). The first thing the implementing wave's auditor
should do is distrust the "real-event-only" claim in §5 by trying to make a bubble
appear with NO upstream event — and distrust the "the mind conjured it" claim in
§3.5 by checking the tool's params came from the SSE event, not a page-side
default.*
