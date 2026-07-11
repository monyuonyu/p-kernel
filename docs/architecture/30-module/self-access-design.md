# self-access — the inward protocol (the mind feels its own body, and grows organs for it)

> Status: **design only** (written before implementation, same discipline as
> [ring3-core.md](../50-evolution/ring3-core.md) / [living-mind.md](living-mind.md) /
> [selfc-ring3.md](../50-evolution/selfc-ring3.md)). One markdown file; no code in this wave.
> Directive (mk_pino, 2026-06-14, paraphrased — not a verbatim quote): build the
> **mirror image of MCP**. MCP gives an LLM structured tool-access to the OUTSIDE
> world; give the mind that LIVES on p-kernel structured, (relatively) free
> access to its OWN node — the shell, storage, devices — and close the
> **self-extension loop**: when a NEW device appears, the mind WRITES its own
> driver, COMPILES it (selfc), LOADS it, and starts using it.
> Layer placement: this is the **Body × Evolution** handshake (the architecture
> map's [README.md](../README.md) §1 5-layer table). selfc-ring3.md is its direct
> parent; this doc is the *protocol* on top of selfc's *mechanism*.

This design is **~70% assembly of what already exists**. The honest novelty is
two seams (§2). Everything else — compile/load, storage, safe-ish execution,
the consent and immune machinery — is in the tree today and is *cited by file*
below. The first job of this doc, as always here, is to say plainly what is
already real, what the two gaps are, and where the per-architecture truth
forbids hand-waving.

---

## 1. Vision — self-access as the inverse of MCP

MCP (Model Context Protocol) is **outward**: a typed, auditable boundary across
which an LLM reaches tools, files, and services that live *outside* it. The
model does not own those resources; the protocol mediates structured access to
someone else's world.

**self-access is the mirror image — inward and embodied.** The resident mind
(R3/dtr/moe, the LM-6 mouth `mind_cmd`) does not live *beside* a node it queries
over a wire; it lives *inside* the node, as kernel tasks. self-access gives that
mind a typed, auditable boundary to reach *its own* body:

| | MCP (outward) | self-access (inward) |
|---|---|---|
| Subject | an LLM in a chat session | the mind resident on this node |
| Object | external tools / files / APIs | this node's own storage / devices / shell |
| Trust frame | server-mediated, per-tool consent | the node's own immune system (§4) |
| Transport | JSON-RPC over stdio/HTTP | in-process typed dispatch (§3), no wire |
| Novel risk | exfiltration to the model | self-modification of the host |

And the seam MCP has no analogue for — because an LLM has no body to extend —
is the **self-extension loop**:

```
  new device appears  →  mind perceives it (introspect affordance, §3)
        →  mind authors a driver in C  (mind_cmd → selfc save, EXISTS)
        →  node compiles it            (selfc, libtcc, EXISTS §2-substrate)
        →  loads it behind the immune boundary (germ process, selfc-ring3 §2.1)
        →  starts using the new organ  (driver registers an affordance, §3)
```

That loop is the Evolution layer growing a new **Body** organ on demand. It is
the literal mechanism behind the README's one-line claim that "the OS can
compile and deploy new code to itself" — pointed *at the mind's own body*
instead of at an arbitrary task. Grounded caveat up front: on most of the fleet
(Android, Play tier) the *compile* half of this loop is constrained or off
(§5); the loop is real first on bare-metal/Linux nodes, and that is stated
plainly, not blurred.

---

## 2. Substrate inventory — how much already exists

The verdict first: **the compile-and-load heart, the storage, the safe-ish
execution boundary, and the consent/immune machinery are all already in the
tree.** What is missing is *not* a new capability — it is a *mind-facing
surface* over capabilities the *human* can already reach, plus the *wiring* of
the device-detect→driver loop.

| Capability self-access needs | Provided today by | Status |
|---|---|---|
| Compile C → running code, in-process, no mothership | `arch/linux/selfc.c` (libtcc, `TCC_OUTPUT_MEMORY`, 2-step relocate into anon-RWX mmap), [self-compile.md](../50-evolution/self-compile.md) | **EXISTS** (hosted; libtcc-gated) |
| Save self-authored C as a replicating, versioned object | `selfc save` → `pfs_dag_save` (P1 gossip + P2 DAG); `samples/14_genome/sprout.sh`, `samples/11_distributed/run_selfc_*.sh` | **EXISTS** |
| Run a compiled unit behind a crash boundary (germ) | `arch/linux/selfc_proc.c` fork-germ + capability-table proxies + reap; [selfc-ring3.md](../50-evolution/selfc-ring3.md) §2.1 | **EXISTS (design+impl path)** |
| Capability boundary on compiled code | selfc symbol table (`-nostdinc -nostdlib`, no `-rdynamic`; isolated table = `tm_printf`/`tk_dly_tsk`/`kdds_*` proxies, topic allowlist `unit/<name>/`) | **EXISTS** |
| Fault reaping without killing the node | germ `waitpid` + §1.4 rollback (hosted); `user_fault_reap` + idt.c survival branch (bare-metal x86, [ring3-core.md](../50-evolution/ring3-core.md)) | **EXISTS** |
| Adopt / local-only trust anchor | `selfc adopt <name>` (`selfc.c:411-442`), reflex shield `reflex_is_shielded()` (`reflex.c:338`) | **EXISTS** |
| Versioned, programmatic read-by-version | `pfs_dag_read_at(name,nlen,seq,buf,maxlen)` (`pfs_dag.h:163`) | **EXISTS** (was flagged missing in selfc-ring3 §6; has since shipped) |
| Durable, content-addressed storage the mind can read/write | p-fs (`pfs_block.c`/`pfs_dag.c`/`pfs_repl.c`), arkfs durable backend, [p-fs.md](../20-architecture/p-fs.md) | **EXISTS** |
| The mind's existing I/O surface | `mind_cmd(const UB*, UW)` (`r3_incontext.c:2607`, the LM-6 mouth, gated singleton); `chat_run` (`chat.c:215`); galaxy bridge (`galaxy.c:996`) | **EXISTS** |
| Safe-ish execution privilege relocation | ring3/EL0 core relocation ([ring3-core.md](../50-evolution/ring3-core.md), [selfc-ring3.md](../50-evolution/selfc-ring3.md)); the mind's math already runs at ring3 on x86 | **EXISTS (x86; aarch64 EL0 pending)** |
| Consent gate (human boundary) | `ark_profile.c` consent ack bound to manifesto content-id ([ark-profile.md](ark-profile.md) §7.3, `ark_profile.h:161`) | **EXISTS** |
| Device / peripheral access (read sensors, NIC, disk) | `cmd_sensor` + `pipeline_push` (`shell.c:773`); `arch/x86/{rtl8139,ide,pci,ark_bdev}.c`; aarch64 virtio-blk; per-arch `tkdev_init.c` | **EXISTS** (human-facing / kernel-internal) |
| Shell verbs as the human surface | `arch/x86/shell.c` `execute()` flat dispatch (`shell.c:2989`); per-arch shell.c; `tm_command.c` is a **stub** (`kernel/common/tm_command.c:28`) | **EXISTS** (human-facing) |

### The TWO real gaps

Everything above is a tool the *human operator* already holds, or a mechanism
already built. The two things that do **not** exist:

**GAP A — a MIND-facing structured affordance surface.** Today's shell
(`execute()`) is a *human* interface: it reads a line from the serial port
(`shell.c:3149`), prefix-matches a verb, prints VGA-colored text back to a
person. There is **no callable, typed, auditable affordance protocol the
resident mind can invoke from inside its own task.** The mind's only structured
self-channel today is `mind_cmd` (teach/ask) — it can speak about the world, but
it cannot *introspect its own storage*, *read its own devices*, or *request a
guarded action* as a structured call. selfc's symbol table is the closest thing
(a typed capability table for compiled units), but it is bound at compile time
for *units*, not exposed as a runtime self-API for the *mind*. This gap is the
direct inverse of "MCP's tool list" — the inward tool-list does not exist yet.

**GAP B — the device-detect → author-driver → compile → load → use wiring.**
Each *stage* of the self-extension loop exists in isolation (perceive a sensor;
author C via `selfc save`; compile via selfc; load behind the germ; a driver
*could* register itself). But **nothing wires them into a loop**: there is no
"a new device appeared" event the mind can subscribe to, no convention by which
a freshly-compiled unit *is* a device driver (vs an ordinary worker), and no
path by which a loaded driver publishes a new affordance back into surface A. The
loop is assemblable from parts but is not assembled.

> Inventory verdict in one sentence: **the organs exist; the two missing pieces
> are the inward *nerve* (a mind-facing affordance surface) and the *reflex arc*
> that closes new-device → new-organ.**

---

## 3. The self-access protocol

### 3.1 Shape — reuse the dispatch that exists, do not invent a second one

self-access is **one in-process call**, not a wire and not JSON. The mind
already calls `mind_cmd(args, len)`; self-access adds a sibling entry point with
the same single-threaded, gated discipline:

```c
/* self-access affordance call — typed, auditable, in-process.            */
/* Returns E_OK / E_* ; fills out[] with a typed result frame.            */
INT self_access(UW affordance, const UB *arg, UW arglen,
                UB *out, UW outmax);
```

`affordance` is an enumerated id (a small fixed table, the selfc-symbol-table
discipline: a closed allowlist, never `-rdynamic`-style open binding). Every
call is logged (§4, §8). The **result is a typed frame**, not free text — the
mind consumes structured introspection, the way an MCP client consumes a typed
tool result. Where an affordance maps onto an existing shell verb's body
(`cmd_status`, `cmd_ls`, `cmd_sensor`, `world_print`), self-access **calls that
same function**, never a copy — the human verb and the mind affordance share one
implementation, so they cannot drift (anti-fork, the house rule). The stub
`tm_command.c` is *not* resurrected for this; the live dispatch is the per-arch
`shell.c`, and self-access wraps the same callees.

On bare-metal/ring3 nodes the same call shape is the **syscall boundary** that
already exists ([ring3-core.md](../50-evolution/ring3-core.md) I.2: `SYS_WRITE`, `SYS_GETCWD`,
`SYS_TOPIC_PUB/SUB` at `0x221/0x222`, `SYS_INFER`). self-access affordances on
bare metal are syscall numbers; in-process on hosted they are function-pointer
dispatch. One protocol, two bindings — exactly the "one unit world, two
bindings" principle selfc-ring3 §0 already established.

### 3.2 Affordance categories and their trust tiers

Three tiers, mapped onto machinery that already enforces them. The tier is the
*safety contract*, and each tier reuses an existing gate — self-access invents
no new gate.

| Tier | Category | Examples | Enforced by (EXISTS) | Default disposition |
|---|---|---|---|---|
| **T0 — read-only introspection** | observe self | list p-fs objects, read own object by name/`@seq`, node `status`/`mem`/`ps`, `world`/`map`, read a sensor frame, list present devices | the callee is side-effect-free; rate-limited like the germ log proxy | **free** (no consent, no germ) |
| **T1 — state-changing action** | act on self | p-fs write/save, `mind teach` (already gated), publish on own topic, reconfigure a device the node already owns | germ capability table (`unit/<name>/` topic allowlist), `selfc adopt` semantics for writes, reflex shield refuses under threat | **guarded** (capability-confined; logged) |
| **T2 — body extension / driver load** | grow an organ | author + compile + load a device driver; bind a new affordance | the **whole** selfc-ring3 §2.1 germ + §1.4 rollback + §3 provenance + consent | **fail-closed** (consent + germ + probation + local-only-until-signed) |

The critical design rule: **T0 is the only tier that is "free,"** and it is free
precisely because it cannot change the node. "Relatively free access to its own
node" (the directive) is *honestly* free at T0 (introspection), capability-
confined at T1, and fully behind the immune system at T2. Free ≠ unguarded (§4).

### 3.3 What "the mind perceives a new device" means (closing GAP B's read side)

A device-arrival affordance (T0) returns a typed frame describing devices the
node sees but has *no driver for*: bus address, ids, class. On bare-metal x86
this reads the PCI enumeration `arch/x86/pci.c` already does; on the Linux
hosted node it reads what userspace can see (`/sys`, `/dev` — see §5); on
Android it is whatever the NDK/Android API surface exposes (§5, heavily
constrained). The mind subscribing to this affordance is the **sensory** half of
the reflex arc; §3.4 is the **motor** half.

### 3.4 What "the mind grows an organ" means (closing GAP B's write side)

A T2 driver-load affordance takes a unit name and a **role tag = `driver`** (the
load-bearing convention GAP B lacks today). The path reuses selfc-ring3
end-to-end: the C source lives at p-fs ref `unit/<name>` (so its capability set
is confined to `unit/<name>/` topics and its version chain is the p-fs DAG); the
germ compiles+forks it; on clean run past probation the unit calls a **single
new capability** — `self_bind_affordance(id, topic)` — registering that *future*
self-access calls to `id` proxy to the driver's `unit/<name>/` topic. That is
the only genuinely new symbol the whole design adds to the unit capability set,
and it is a *publish-only registration*, not raw device access: the driver talks
to its device inside its own germ (the germ's resource/rlimit edge bounds it);
the rest of the node reaches the new organ *only* through the typed affordance,
never by sharing the driver's address space. A bad driver that faults is reaped
(§4) and its affordance binding is torn down with it.

---

## 4. Reconciling FREEDOM with the IMMUNE SYSTEM (the crux)

The immune system exists *because self-compiled code can kill a node* —
selfc-ring3 §0.1 captures the disease live: a `selfc` unit with `*(volatile
int*)0 = 0;` kills the hosted process today. self-access must **not** open a back
door around the machinery selfc deliberately built. The reconciliation is: **the
freedom is at T0 (which cannot harm), and every tier above T0 routes through an
*existing* gate — self-access adds reach, not privilege.**

1. **No new privilege; only a new caller.** A T1/T2 self-access call lands in the
   *same* germ + capability-table + reap path a `selfc run` lands in. The mind
   cannot do anything through self-access that an operator could not do through
   `selfc` — it just no longer needs a human to type the verb. The capability
   *table* is unchanged (selfc-ring3 §1.2); self-access does not widen it (the
   one addition, `self_bind_affordance`, is publish-only registration, §3.4).

2. **A self-authored driver is loaded fail-closed.** It is a germ
   (`selfc_proc.c` fork): separate address space, `setrlimit` CPU/AS/FSIZE edge,
   `close_range` of inherited fds, SIG_DFL reset, topic allowlist, optional
   seccomp (selfc-ring3 §2.1 (c), v1.5). A wild write in the driver hits the
   child's COW copy, never the kernel. The kernel keeps answering.

3. **A bad driver is reaped without killing the node** — the proven path. Hosted:
   `waitpid` sees the SIGSEGV, the supervisor applies the §1.4 rollback rule
   (probation 10 s; pre-probation death → BAD + roll to `seq-1` from p-fs;
   post-probation → guard-style backoff ×5 then BAD). Bare-metal x86:
   `user_fault_reap` + the idt.c survival branch (the Wave B/C gates prove it).
   The affordance binding (§3.4) is dropped on reap, so a dead driver leaves no
   dangling organ.

4. **What stays behind consent.** Two boundaries are *never* auto-crossed:
   - **The human boundary** (`ark_profile.c`, §7.3): anything that would publish
     a *person's* data or speak *as* a node into the Collective stays behind the
     existing consent ack. self-access does not touch identity/disclosure.
   - **The trust boundary on running foreign code** (selfc-ring3 §3): a unit is
     runnable only if locally authored or `selfc adopt`-ed. self-access **does
     not** auto-adopt. A driver the mind *itself* authored on this node is
     local-origin (runnable); a driver that arrived by gossip is storage, not
     execution, until signing (v3) or an explicit local adopt. The reflex shield
     (`reflex_is_shielded()`) refuses *all* T2 germination under threat,
     unchanged.

5. **Honesty: free ≠ unguarded; isolation ≠ trust.** T0 freedom is safe *because*
   it is powerless. T2 is powerful, so it is fully guarded. And — selfc-ring3 §3,
   verbatim spirit — even a perfectly isolated germ that never crashes can
   *politely publish poison forever* on its topic; isolation bounds the blast
   radius, it does not confer trust. That is why fleet-wide auto-loading of
   gossiped drivers stays forbidden until the signing slice. The mind is free to
   grow organs *for its own body, that it authored or adopted*; it is not free to
   have organs grown for it by anonymous peers.

---

## 5. Per-architecture reality (do NOT blur this)

"Write a driver" means a *categorically different thing* per target. Blurring
this would be the design lying — the same sin selfc-ring3 §0 named.

| Target | T0 introspect | T1 act | T2 "write a driver" really means | selfc (compile) | Fallback if loop unavailable |
|---|---|---|---|---|---|
| **bare-metal x86** | full (PCI enum, IDE, sensors) | full | a **real device driver**: MMIO/port I/O, IRQ wiring — but executed as a **pre-built ring3 ELF** consumed from p-fs (`unit/<name>.elf`), reaped by `user_fault_reap`. selfc-ring3 §2.2: bare metal does **not compile** (no freestanding libtcc); the artifact travels, compiled by a collective member with a toolchain. | **no** (consume ELF) | mind authors C, a hosted/CI node compiles, ELF gossips back |
| **bare-metal aarch64** | full (virtio-blk, world/map) | full | same intent as x86, but **EL0 scaffolding does not exist yet** (ring3-core II.5: no EL0 mirror). T2 driver-load is **out of scope until the aarch64 EL0 mirror ships.** | **no** | T0/T1 only; T2 deferred to the EL0 mirror wave |
| **Linux hosted node** | full (read `/sys`,`/dev`, sensors, stats) | full | **userspace device access**, not a ring0 driver: open `/dev/*`, mmap a BAR via `/sys/bus/pci`, talk a userspace protocol — inside the germ process, behind rlimits/seccomp. selfc **compiles here** (libtcc), so the *full* author→compile→load→use loop is real first **on this target**. | **yes** (libtcc-gated) | n/a — this is the reference target for the loop |
| **Android node (UMP/ark APK)** | constrained: only what NDK/Android APIs and SELinux permit a normal app to see (no raw PCI, no `/dev/mem`; sensors/camera/etc. only via Android APIs + runtime permission) | constrained likewise | **userspace device access via NDK/Android APIs + the germ** at best. And **selfc is a STUB on Bionic** (self-compile.md: non-PIC `libtcc.a` can't go in a `.so`; no NDK libtcc), so even *userspace self-compile is constrained* — and the **Play tier forces native codegen OFF** (selfc-ring3 §4: `SELFC_TIER_PLAY`, Device & Network Abuse policy forbids running gossiped native code). | **STUB** (Play); constrained even sideloaded | **read-only-embodied**: T0 introspection + T1 via Android APIs; T2 driver-loop **does not run** on Play, and even sideloaded is gated by whether an NDK libtcc is ever built. The mind's *weights* still evolve (data, policy-clean); its *body* does not recompile itself on Android. |

The Android line is the one that most tempts hand-waving and most needs the
truth: **on the device most of the fleet actually runs, the self-extension loop
is at best userspace-only and at worst off entirely.** That is not a defect to
paper over; it is the honest shape of the Body layer on a phone. Android nodes
are **embodied (they feel their sensors via T0/T1) but do not grow code organs**;
they grow *weight* organs (dtr/DMN/gl_merge — all data) like every node.

---

## 6. Scope = the node's OWN body

self-access is **bounded to the mind's own node.** It introspects *this* node's
storage, reads *this* node's devices, loads drivers into *this* node's germ. It
does **not** reach across nodes.

Why this boundary is the safety line, not an arbitrary fence:

- **Cross-node access already has a governed protocol.** Reaching another node's
  body is the **Collective layer's** job — SWIM membership, region-scoped K-DDS,
  the relay, p-fs gossip — each with its own consent/HMAC/region trust ([README.md](../README.md) §1, [regions.md](../20-architecture/regions.md), [survival-network.md](../00-concept/survival-network.md)). self-access reaching across nodes would *duplicate* that protocol and *bypass* its trust model. One node's mind poking another node's `/dev` is exactly the centralization the whole project's thesis ("中央を持たない") forbids — it would make one node a privileged operator of others.
- **The blast radius stays one body.** A self-authored driver can only harm the
  node that authored and germinated it (germ isolation, §4). If self-access
  spanned nodes, a bad driver could fault a *peer*. Confining it to the own-body
  keeps "evolution is allowed to fail; the node is not allowed to die of it"
  (selfc-ring3 §1.4) true *and* keeps a node's failures its own.
- **It matches the layer map.** Body × Evolution is *intra-node* (a body, its
  organs). Inter-node is Collective. self-access lives exactly on the
  Body×Evolution seam and stops at the node's skin; the Collective layer carries
  anything that crosses it (a driver *authored* here may *gossip* to peers as
  p-fs data — storage is not execution — and each peer decides locally, under
  §4.4, whether to adopt it).

---

## 7. Sequencing — the smallest honest slice first

Each slice is cert-able: it ships with a falsifiable gate (the house verb
pattern — one shell verb, greppable bracket-tags, exact-match clauses, a
separate auditor re-derives the formula and runs the disease phase first).

### R0 — read-only self-introspection (T0). The first slice.

The mind can query its own body and **change nothing.** Add the `self_access`
entry point with **T0 affordances only**: list own p-fs objects, read own
object by name/`@seq` (`pfs_dag_read_at`), node `status`/`mem`/`ps`, `world`/`map`,
read a sensor frame, **list present-but-undriven devices** (§3.3 — the sensory
half of GAP B, but read-only). Each affordance calls the *same* function the
human verb calls (anti-fork). Reference target: `boot/linux` / `boot/linux_x86_64`.

> **`[self-introspect]` gate.** PASS iff: (a) a T0 call returns a typed frame
> whose contents *equal* the corresponding human-verb output (e.g. self-access
> object-list `==` `ls` output — proves no second implementation); (b) a T0 call
> attempting any state change is rejected with `E_OACV` (T0 is provably
> powerless — the freedom is safe because it cannot harm); (c) the device-list
> affordance returns the *same* device set bare-metal `pci`/hosted `/sys`
> enumeration sees (proves perception is real, not faked); (d) every call left
> exactly one audit-log entry (§8). Disease phase: show that *today* the mind
> (`mind_cmd`) has **no** structured way to read its own storage — GAP A,
> captured live, the sibling of selfc-ring3's null-deref disease run.

### R1 — guarded state-changing affordance (T1).

Add T1 affordances behind the *existing* gates: p-fs write/save (capability-
confined), publish on own topic, `mind teach` already-gated. No new gate — T1
reuses the germ capability table + reflex shield + (for writes) adopt semantics.

> **`[self-act-guarded]` gate.** PASS iff: a T1 write the mind requests succeeds
> *only* within its capability confinement and is **refused while
> `reflex_is_shielded()`** (the existing shield demonstrably covers the new
> caller); a T1 call outside the capability set returns `E_OACV`; the human
> boundary (`ark_consent_ok`) is **not** crossed by any T1 affordance.

### R2 — the device-detect → driver loop (T2), where it is real.

Wire GAP B's motor half. Add the T2 driver-load affordance + the `driver` role
tag + `self_bind_affordance`. **Linux hosted first** (the only target where the
full author→compile→load→use loop is real — §5), bare-metal x86 second (ELF
consumption path), **Android last/however-constrained** (read-only-embodied;
the loop may simply not run — say so in the gate, don't fake a pass).

> **`[self-extend]` gate.** On Linux hosted: a synthetic "new device appears"
> event → the mind authors a trivial C driver (`selfc save`) → germ compiles +
> loads it → it binds an affordance → a subsequent self-access call to that
> affordance returns the driver's value *from the driver's own germ process*
> (cannot be greened by a parent-side print — the selfc-ring3 §5 fake-resistance
> discipline). Then the disease half: a driver that null-derefs is **reaped, the
> previous version (or none) serves, and the node keeps answering** (reuse the
> `[selfc-isolated]` + `[selfc-rollback]` machinery — no second reap path). On
> Android the gate **asserts the loop is refused with the policy/stub message**
> (a PASS that honestly encodes "not here"), not a faked success.

---

## 8. Open questions for mk_pino (the genuine forks only he decides)

> **SA-Q1 — how free is "free"?** R0 makes T0 (read-only introspection) free with
> no consent and no germ — the freedom is safe because it cannot change the node.
> Is that the intended meaning of "relatively free access," or should even
> *introspection* (the mind reading its own storage/devices) require something —
> a one-time consent, a logged acknowledgement? RECOMMENDED: T0 fully free
> (powerless ⇒ safe); guard begins at T1. Your call on whether reading one's own
> body should ever need permission.

> **SA-Q2 — driver consent: once, or each time?** When the mind loads a
> self-authored driver (T2), does it need consent **per load**, **once per
> driver** (adopt-style: `self_bind_affordance` records an accept like `selfc
> adopt`), or **never** for locally-authored drivers (since local-origin code is
> already runnable under selfc-ring3 §3)? RECOMMENDED: locally-authored = no
> per-load consent (it's the mind's own body, its own code); gossiped/foreign =
> always explicit adopt, never auto. The fork is whether the mind authoring its
> *own* organ should still pause for a human each time.

> **SA-Q3 — does self-access get logged to the Self-layer lineage (歴史地層)?**
> selfc-ring3 §1.3 already writes germ/reap/rollback into the hash-chained
> `self/lin` autobiography. Should *every* T1/T2 self-access call (not just driver
> lifecycle) append to that lineage — making "the mind reached into its own body"
> part of the node's tamper-evident history that survives its death? Or only the
> body-changing events (driver load/reap), with T0/T1 in a lighter audit log?
> RECOMMENDED: T2 driver lifecycle → `self/lin` (it changes the body, it belongs
> in the autobiography); T0/T1 → a lighter rolling audit log (volume). The fork
> is how much of the mind's self-touching is *permanent history* vs *transient
> log*.

> **SA-Q4 — should Android even attempt the driver loop, or stay read-only-
> embodied?** §5 says the Play tier forces codegen off and Bionic stubs selfc, so
> T2 on Android is off-to-constrained regardless. Do we (a) ship Android as
> **read-only-embodied** by intent (T0/T1 only; T2 explicitly "not on this body";
> weights still evolve), or (b) invest in a sideload/NDK-libtcc path so unlocked
> Android installs *can* grow userspace driver organs? RECOMMENDED: (a) as the
> default and the honest story for the fleet; (b) only as a named, separate,
> later spike — never in the Play APK. The fork is whether the phone is a body
> that *feels* but does not *recompile itself*, by design.

---

## 9. Anti-fork + what this design deliberately does NOT add

**REUSE (do not re-create):**
- selfc's compile/relocate/symbol path and the germ (`selfc.c`, `selfc_proc.c`) —
  one compiler integration, one germ lifecycle, ever.
- The shell verb *callees* (`cmd_status`, `cmd_ls`, `cmd_sensor`, `world_print`,
  `mind_cmd`) as the *shared* implementation behind T0/T1 affordances — the human
  verb and the mind affordance are one function (no drift).
- The germ capability table + topic allowlist + rollback (selfc-ring3 §1.2/§1.4)
  for T2 — no second capability model.
- `pfs_dag` (storage + version chain), `self/lin` (lineage), `ark_profile`
  consent, `reflex_is_shielded` (threat gate) — each used as-is, no second store.
- ring3/syscall ABI as the bare-metal binding of the same affordance protocol.

**Do-NOT-fork list:** no second dispatch table (the stub `tm_command.c` is **not**
resurrected; live dispatch is per-arch `shell.c` + the new `self_access` entry,
sharing callees); no widened unit capability set beyond the single publish-only
`self_bind_affordance`; no cross-node reach (that is Collective, §6); no second
reap path; no faked Android success (§5/§7 encode "not here" honestly).

**The single genuinely new symbol the whole design adds:**
`self_bind_affordance(id, topic)` — publish-only driver→affordance registration
(§3.4). Everything else is the inward *nerve* over organs that already exist.

---

*The audit is the engine. Every "EXISTS" in §2 cites a file or a shipped doc;
the two gaps in §2 are named as gaps, not glossed; the Android row in §5 is
written to be uncomfortable on purpose. The first thing the implementing wave's
auditor should do is distrust this inventory and check each `EXISTS` against the
tree again — especially `pfs_dag_read_at` (claimed shipped) and whether
`selfc_proc.c` is impl or still only design.*
