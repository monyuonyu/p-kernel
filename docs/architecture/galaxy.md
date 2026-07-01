# Galaxy — 銀河の観測窓 (the per-node observation window)

**Status: SHIPPED (v1 live; on the phones). 2026-06-19 doc-status fix.**
What actually shipped: the per-node web galaxy is live — `arch/common/galaxy.c` runs the
minimal HTTP/1.0 + SSE server on port 7800+(node_id-1), serves the UI compiled in from
`arch/common/web/galaxy.html` (→ generated `galaxy_page.h`), and the ark app opens it as a
WebView (`GalaxyActivity`, wave-30 / wave-36). Teach/ask, real organism events, and the
living-body vitals snapshot all flow over it. (Original design-doc-first intent below.)

mk_pino の意図 (2026-06-10, verbatim intent): every node runs a tiny web server; the owner
opens localhost in a browser and sees **a galaxy** — their own device floating in it, visibly
sleeping or pulsing; they can teach/ask the mind from that screen and watch the activation
ripple across other devices in the galaxy; incoming activation requests from other nodes
visibly arrive at their star.

Three non-negotiables, inherited from the project's soul:

1. **Honesty.** Every photon in the galaxy is a REAL event from the running organism. No
   animation theater: if nothing is happening, the galaxy is still. If the star is "dreaming",
   `dmn_state == DMN_IDLE` is literally true at that moment (`arch/common/dmn.c:54`).
2. **Decentralized.** Each node serves its OWN view from its OWN gossip-bounded knowledge
   (the world.c NO-CENTRAL invariant, `arch/common/include/world.h:11-23`). There is no
   galaxy server. The mind is ownerless; so is its window.
3. **Observation must never destabilize the organism.** A dead/stuck browser must not kill
   the node. All hooks are O(1); all client I/O is non-blocking; overflow drops events and
   says so.

This is the missing ワクワク/experience layer for the UMP distribution strategy: the reason a
person keeps the app installed is that they can *see* their device thinking with others.

---

## 1. Transport reality — the honest verdict

**The in-kernel netstack cannot serve HTTP today, and on hosted builds it couldn't reach the
browser even if it could.** Two independent facts:

- `arch/common/netstack.c` has TCP, but **client-only**: `tcp_connect` (`netstack.c:565`),
  `tcp_write` (`:600`), `tcp_read` (`:609`), `tcp_close` (`:630`) — there is **no
  `tcp_listen` / `tcp_accept`**, no LISTEN state in `tcp_input`'s switch (`:451-560`; an
  unmatched SYN is silently dropped at `:481` "if (!c) return"). `TCP_MAX_CONN` is **2**
  (`:377`). Serving a browser (which opens 2-6 parallel connections) needs a listener and a
  bigger pool — a real netstack slice, not a tweak.
- On the hosted builds (the fleet's main targets: `boot/linux_x86_64`, `boot/linux`,
  Android `libpkernel.so`), the netstack's "Ethernet" is the rtl8139 shim over UDP loopback
  (`arch/linux/x86_64/net_unix.c:53` — `socket(AF_INET, SOCK_DGRAM, 0)`, frames to
  `127.0.0.1:29000+node_id`) or the relay wire (`net_relay.c:637`, same `SOCK_DGRAM`),
  selected by `net_dispatch.c:40-61`. **No TAP/TUN** (explicitly rejected, `net_unix.c:8`
  — "TUN/TAP requires CAP_NET_ADMIN; not available in Termux proot"). A host browser
  speaks host TCP; the in-kernel netstack's IP world is encapsulated frames between
  p-kernel processes only. They cannot meet.

**Verdict, stated loudly: v1 is hosted-nodes-only**, using a **host TCP listen socket in the
port layer**, following the exact pattern that already carries all hosted networking: a
POSIX-only TU (`T-Kernel headers are NOT included`, the `net_unix.c:19-21` rule, errno shim
`net_unix.c:34-37`) polled by a kernel task, like `rtl8139.c:52` polls
`arch_linux_net_recv`. Bare-metal x86/aarch64 get the galaxy only after a named future slice
**`netstack-tcp-server`** (add LISTEN/accept + pool≥4 to netstack.c); the kernel-side galaxy
code is written against a 5-function transport ABI so that slice slots in without rework
(§3.2). This is the same "hosted first, bare-metal when the substrate exists" sequencing the
relay took.

---

## 2. Signal survey — what can really light the galaxy

For each signal: where it lives, and the ONE cheap hook point that emits into the event ring
(§4). Hooks are one `galaxy_emit(type, src, dst, a, b)` call — O(1), no allocation, first
instruction `if (!galaxy_on) return;`.

| # | Signal | Source of truth (file:line) | The ONE hook point | Galaxy meaning |
|---|--------|------------------------------|--------------------|----------------|
| S1 | Node membership + alive/suspect/dead | `dnode_table[]` shared by drpc/swim (`arch/common/include/drpc.h:107-110`, `DNODE_MAX=64` `:35`) | **No event hook needed for the table** — `GET /galaxy.json` reads `dnode_table[]` directly. State-CHANGE events: swim has multiple mutation sites (`swim.c:118-119` gossip, `:225-228` beacon rx, `:420-421` suspect, `:427-428` dead) — add one file-static `sw_note(nid, st)` called at each existing `sw_puts` print site, which calls `galaxy_emit(EV_SWIM, nid, 0, old, st)` | a star appears / dims / goes dark |
| S2 | DMN state (awake/dreaming) | `dmn_state` static in `dmn.c:54`; transitions `dmn.c:83-85` (IDLE→ACTIVE) and `:166-168` (ACTIVE→IDLE); `dmn_stats` public (`dmn.h:62`) | the two transition sites (already unique, already print) emit `EV_DMN_WAKE` / `EV_DMN_IDLE`. Snapshot getter for /galaxy.json: new public `dmn_current_state()` (FLAGGED §9) | my star dims to a slow breathing pulse: **the star is dreaming — literally true** |
| S3 | Sleep-consolidation round (a fact sinks in) | `dmn.c:106-121` — the two `[dmn] sleep:` print sites; LM-6 adds `dmn_r3_round_count++` as the ONLY ++ site beside the r3 print (living-mind.md VII.5, `:1846-1852`) | emit `EV_CONSOLIDATE` at the same two success branches, beside the existing prints (same one-site discipline) | the orbiting fact-particle SINKS INTO the star |
| S4 | Teach/ask (the mouth) | LM-6 `mind_cmd` in `r3_incontext.c` (living-mind.md `:1769`); queue feed = `r3_fact_learn` (`dtr.h:298`), pending = `r3_facts_pending()` (`dtr.h:299`) | emit `EV_TEACH` inside `mind_cmd`'s teach verb beside the `[teach-arrival]` print, `EV_ASK` beside the `ask` result print — **production sites, so a shell-taught fact ripples in the galaxy too** (G33 spirit) | a fact-particle starts orbiting my star |
| S5 | Incoming inference (another node asks ME) | `drpc_dispatch` (`drpc.c:293`), `case DRPC_CALL_INFER:` (`:303`) — already prints `[drpc/infer] from node N` (`:316`) | emit `EV_DRPC_IN(src, my, call_id, cls)` beside that print; `EV_DRPC_OUT` at the single `drpc_call` entry (`drpc.c:417`) | **an arriving ripple hits my star** / an outgoing ray leaves it |
| S6 | MoE expert pick / firing | `select_expert` (`moe.c:278`); remote answer print `moe.c:468`; firing already feeds world via `world_note_firing` (`moe.c:446`) | emit `EV_MOE(my, chosen_node, gate_class, cls)` at the single point after `select_expert` returns in `moe_infer` | an inference flash: my star fires toward the chosen expert star |
| S7 | Gossip threads (K-DDS publish) | `kdds_pub` (`kdds.c:216`) — carries world beacons (`world.c:229`, every `WORLD_BEACON_MS=3000`), vital, moe scores, G22 weights... | emit `EV_KDDS(my, 0, topic_hash16, len)` at the top of `kdds_pub` — **sampled** (§4.2): this is the chattiest hook | faint shimmering threads between stars (the gossip web) |
| S8 | pmesh data send/recv | `pmesh_send` (`pmesh.c:295`), delivery `pmesh_stats.data_rx` site (`:260`) | emit `EV_PMESH_TX/RX` at those two sites — sampled | brighter directed threads |
| S9 | DKVA distributed attention fan-in | `cagg_start` (`dkva.c:282`) / `cagg_step` finalize (`:321`) | emit `EV_DKVA(origin, my, phase, n)` at cagg_start and at finalize | several stars momentarily converge their light (the mind attending across devices) |
| S10 | Self lineage (click my star) | `lm_self.c` — hash-chained `"self/lin"` versions (`lm_self.h:71`) via `pfs_dag_save/read` (`pfs_dag.h:142,150`) | no hook — `GET /self.json` walks the lineage on demand (lazy; it's a p-fs read) | my star's autobiography panel |
| S11 | Regions (constellations) | `region_id()` (`region.h:32`) for me; peers via `world_peer_region_fresh(node)` (`world.h:148`) — gossip-bounded, honestly stale-aware | no hook — /galaxy.json reads accessors | constellation grouping; stale peers drawn faded (古さの尊重) |
| S12 | Peer load/threat/at-risk (star color/size) | world-table accessors: `world_peer_pressure` (`world.h:121`), `world_peer_threat` (`:128`), `world_peer_atrisk` (`:135`), known-ness `world_peer_known` (`:116`) | no hook — /galaxy.json reads accessors. **world.c is the existing decentralized observability organ — the galaxy is its face, NOT its replacement** | star hue = threat, brightness = spare capacity |

The deliberate split: **slow state lives in tables that /galaxy.json snapshots** (S1, S11,
S12, S2-snapshot) — zero new gossip, zero hooks; **fast transients live in the event ring**
(S3-S9) — one emit call each, at sites that already print.

---

## 3. Architecture

### 3.1 Files (the net_unix/net_relay pattern, exactly)

```
arch/common/galaxy.c              kernel-side: event ring, JSON assembly, server task
arch/common/include/galaxy.h      publics + event types + wire-free (host-local) schema
arch/linux/x86_64/galaxy_posix.c  POSIX TU: TCP listen socket, accept, non-blocking I/O
arch/linux/aarch64/galaxy_posix.c (same source; promoted to arch/common/linux/ when
                                   net_unix.c's existing duplication is cleaned up —
                                   the net_dispatch.c:13-14 note applies verbatim)
arch/common/web/galaxy.html       ONE self-contained page (canvas + inline JS, zero
                                   external deps — nodes can be offline)
```

Build wiring: add `galaxy.c` to `COMMON_C_SRCS` and `galaxy_posix.c` to the arch list in
`boot/linux_x86_64/Makefile` (the `net_unix.c`/`net_relay.c` slots, `:101-102`) and the
aarch64 twin. Bare-metal Makefiles do NOT list them in v1.

### 3.2 The transport ABI (5 functions, so bare-metal can slot in later)

`galaxy_posix.c` exports exactly (C ABI, no T-Kernel types — the `net_unix.c:19-21` rule):

```c
int  galaxy_io_init(int port);                 /* listen on 127.0.0.1:port; nonblock; ret fd count or -1 */
int  galaxy_io_accept(void);                   /* ret client slot 0..GALAXY_MAX_CLIENTS-1 or -1 */
int  galaxy_io_read(int slot, void *buf, int max);   /* nonblock; 0 = nothing, -1 = closed */
int  galaxy_io_write(int slot, const void *buf, int len); /* nonblock; short writes OK; -1 = closed */
void galaxy_io_close(int slot);
```

`GALAXY_MAX_CLIENTS = 4` (one page + one SSE + slack). All fds `O_NONBLOCK`. **A client that
stops reading gets dropped on `EWOULDBLOCK` backlog overflow** (per-client small out-buffer,
~4KB; when full, close the slot) — a dead observer cannot wedge the server task, let alone
the organism. The future `netstack-tcp-server` slice implements the same 5 functions over
netstack TCP and bare-metal joins without touching `galaxy.c`.

### 3.3 The server task

One task `galaxy_task`, created in both hosted usermains beside the existing fleet
(`arch/linux/x86_64/usermain.c:261-354` block), **priority 8** — below net/swim (3/6), above
the dmn (13, `usermain.c:593`). Loop: accept → read request lines → route → write → poll the
event ring → fan out SSE → `tk_dly_tsk(50)`. 50ms polling = at most 20 ring drains/s; human
eyes need no better, and the organism's hot paths never wait on it.

### 3.4 Endpoints

| Endpoint | Method | Returns |
|---|---|---|
| `/` | GET | the embedded page (`Content-Type: text/html`) |
| `/galaxy.json` | GET | snapshot: `{me:{id,device,region,dmn,pending,rounds,pressure,threat}, peers:[{id,state,region,fresh,pressure,threat,atrisk,device}], dropped:N}` — `id`=`drpc_my_node`, peers from `dnode_table[]` + `world_peer_*` accessors, `dmn`=`dmn_current_state()`, `pending`=`r3_facts_pending()`, `rounds`=`dmn_r3_rounds()` |
| `/events` | GET | SSE stream (`Content-Type: text/event-stream`), one `data:{json}\n\n` per ring event |
| `/self.json` | GET | the Self lineage (S10): entries of `"self/lin"` walked via `pfs_dag_read` — id, seq, hash-prefix, summary per version |
| `/teach` | POST | body `k=<0..7>&v=<0..3>` → the LM-6 bridge (§6); returns `{ok,pending,rounds}` |
| `/ask` | POST | body `k=<0..7>` → the LM-6 bridge; returns `{pred,share}` |

Anything else: `404`. That is the WHOLE surface. **Read-only + teach/ask, NOTHING else** — no
verb that creates tasks, writes p-fs, changes weights directly, or reaches any other shell
command. Rationale: the galaxy must not become a remote control plane installed on every node
on Earth; the blast radius of the window is "see + the two mouth verbs the owner already has
at the console", nothing more.

### 3.5 Security posture (v1)

- **Bind `127.0.0.1` ONLY** (hard-coded `htonl(0x7F000001)`, same constant as
  `net_unix.c:61`), by default and in v1 unconditionally — no config to bind `0.0.0.0`.
  The owner's own browser on the owner's own device. Remote view is v3, via the
  authenticated relay (HMAC wire already exists), NOT by opening this port.
- No TLS in v1 — loopback only, nothing leaves the host.
- POST bodies are bounded (≤256 bytes) and parsed by a dumb fixed parser; oversize = `413`,
  close.
- On Android the same loopback bind works inside the app sandbox (the Phase-C WebView
  decision, §10 D4).

### 3.6 The minimal HTTP subset (spec, exact)

HTTP/1.0-style, hand-rolled (~150 lines), the same "zero-dep from scratch" discipline as the
relay's sha256:

- Parse: first line only (`GET /path` / `POST /path`) + scan headers solely for
  `Content-Length:` on POST. Ignore everything else. Request line > 512 bytes → `400`, close.
- Respond: `HTTP/1.0 200 OK\r\nContent-Type: ...\r\nConnection: close\r\n\r\n` + body, then
  close — **except** `/events`, which omits Content-Length, sends
  `Content-Type: text/event-stream` + `Cache-Control: no-cache`, and holds the socket open,
  writing `data:{...}\n\n` frames as the ring drains, plus a `: ping\n\n` comment every 15 s
  so dead clients are detected by write failure.
- No keep-alive, no chunked encoding, no compression, no HEAD, no query strings except the
  POST body form. Browsers accept all of this happily; SSE over a held-open HTTP/1.0-style
  response is exactly how EventSource degrades.

---

## 4. The event ring

### 4.1 Structure

```c
typedef struct {            /* 12 bytes, fixed-width only (the world.h LP64 rule) */
    U4 ms;                  /* uptime ms (wraps at ~49 days; the page handles wrap) */
    U1 type;                /* EV_* */
    U1 src, dst;            /* node ids; 0xFF = none */
    U1 _pad;
    U2 a, b;                /* type-specific detail (class, topic hash16, len...) */
} GALAXY_EV;
#define GALAXY_RING  256    /* 3 KB static; power of two */
```

Single static ring + one `UW g_head`. Producers (hooks, running on many tasks) wrap the
3-store append in `tk_dis_dsp()/tk_ena_dsp()` — hosted p-kernel is a uniprocessor T-Kernel,
so a dispatch-disabled window of ~6 instructions is the cheapest correct exclusion (no
semaphore object, no syscall on the hot path beyond the two dispatcher toggles). The consumer
(galaxy_task) snapshots `g_head` and reads behind it; a lapped consumer skips ahead and
increments a public `g_dropped` counter that `/galaxy.json` reports — **overflow is shown,
never hidden**.

Cost bounds: hook = 1 predictable branch when off (`galaxy_on==0`); when on, ~20
instructions. No hook ever blocks, allocates, or takes a semaphore. The ring is written even
with zero SSE clients (256 events of history make the page's first seconds honest), but
sampling (below) keeps that write rate trivial.

### 4.2 Sampling / coalescing rule (stated, enforced, reported)

Two classes:

- **Precious events — NEVER sampled:** EV_TEACH, EV_ASK, EV_CONSOLIDATE, EV_DMN_IDLE,
  EV_DMN_WAKE, EV_DRPC_IN, EV_DRPC_OUT, EV_MOE, EV_DKVA, EV_SWIM. These are rare (human-rate
  or state-change-rate) and are the galaxy's meaning.
- **Chatty events — token-bucket per type:** EV_KDDS, EV_PMESH_TX, EV_PMESH_RX at most
  **4/s each** (one `U1` budget per type, refilled by the galaxy_task's 1 s tick).
  Suppressed counts accumulate in per-type `U2` counters; every 5 s the task itself emits one
  `EV_SUMMARY(type, suppressed_count)` event — the page renders the gossip web's *intensity*
  from summaries, individual threads from the sampled events. The screen shows real traffic
  density without the ring drowning in 3-second beacons × 64 nodes.

### 4.3 JSON at the edge only

The ring stores 12-byte structs; `galaxy.c` formats JSON only when writing to a live SSE
client (snprintf-free: the same hand-rolled putdec style as `wo_putdec`, `world.c:39`). No
JSON cost when no one is watching.

---

## 5. Galaxy semantics — the honest mapping

The page draws ONLY from `/galaxy.json` + `/events`. Every visual rule below names its real
signal; anything without a listed signal does not move.

| Visual | Real signal | Honesty note |
|---|---|---|
| My star, center | `me.id` from /galaxy.json | the view is MY situational awareness, not a god-view |
| Other stars | `dnode_table` states: ALIVE=lit, SUSPECT=flickering, DEAD=dark husk (fades out after a while), UNKNOWN=not drawn | exactly SWIM's belief, with SWIM's latency |
| Constellations | `world_peer_region_fresh()` grouping; stale beacons (>9 s, `WORLD_STALE_MS world.h:78`) drawn faded with an age label | 古さ・不完全さの尊重 — staleness is rendered, not masked |
| Star brightness / hue | pressure (dim = loaded) / threat (red-shift) from world accessors | the §6/§2 gradients the MoE itself reads — owner sees what the organism feels |
| My star pulsing slowly, "dreaming" | EV_DMN_IDLE → breathing animation **state**; EV_DMN_WAKE ends it | literally `dmn_state` (dmn.c:54); the animation is a state indicator, not theater |
| Fact-particle orbiting my star | EV_TEACH (k,v shown) | a queued engram really is waiting (`r3_facts_pending()`) |
| Particle SINKS INTO the star | EV_CONSOLIDATE | fired beside the `[dmn] sleep: distilled...` print (dmn.c:119-120) — the actual SGD round that buries the fact in `rw[]` |
| Arriving ripple at my star | EV_DRPC_IN(src) | another node really called DRPC_CALL_INFER on me (drpc.c:303) |
| Outgoing ray / inference flash | EV_DRPC_OUT, EV_MOE(chosen expert) | the MoE really routed there (moe.c:278/:446) |
| Stars converging light | EV_DKVA fan-in | distributed attention really aggregated (dkva.c:282/:321) |
| Faint gossip web | EV_KDDS / EV_PMESH (sampled) + EV_SUMMARY intensity | sampled, and the sampling is declared on-screen ("showing 4/s of N/s") |
| Click my star | /self.json — the hash-chained autobiography, newest first | the same lineage `self test` certifies (tamper-EVIDENT, not unforgeable — the III caveat carries to the UI text) |
| Click a peer star | that peer's world-table row (pressure/threat/atrisk/region/age) | gossip-bounded knowledge ONLY — the panel says "as gossiped Xs ago" |

**The sparse-galaxy stance:** 3 nodes = a small constellation of 3, shown truthfully on a
big dark field. No fake background stars, no decorative nebulae implying scale that doesn't
exist. The empty space is the point: it fills as the mind spreads. (A subtle static
backdrop is allowed only if visually distinct from node-stars — recommended: none in v1.)

---

## 6. The teach/ask bridge — driving the production mouth

**Rule (G33, the discipline of every shipped wave): the web layer drives the SAME production
verbs — `mind_cmd` — not a parallel implementation.** `POST /teach k=2&v=3` builds the byte
string `"teach 2 3"` and calls `mind_cmd(args, len)` (the ONE public of `r3_incontext.c`,
living-mind.md `:1769,1986`). `POST /ask k=2` → `mind_cmd("ask 2")`. The galaxy code contains
**no** call to `r3_fact_learn`, `r3_consolidate_idle_round`, or `s_round` — the same grep the
LM-6 auditor already runs (living-mind.md `:1869-1871`) extends to `galaxy.c`.

**The concurrency slice Part VII named — now due.** VII.4 is explicit: the quiesce flag
`r3_round_busy` is "a flag, not a lock: sufficient ONLY because... verbs cannot be preempted
by the round. If R3 queries ever move off the shell task, this becomes a real mutex — named
for that slice, not built" (living-mind.md `:1841-1844`). The galaxy task IS a second caller
task. Therefore this slice builds the named mutex:

- One semaphore `m_gate` (`tk_cre_sem`, maxsem 1), file-static in `r3_incontext.c`,
  acquired at `mind_cmd` entry, released at exit. Inside the gate, the existing
  `r3_round_busy` quiesce spin-sleep is unchanged. Putting the mutex INSIDE `mind_cmd` means
  no caller (shell, web, future) can forget it.
- Priority argument re-checked: galaxy_task pri 8 ≫ dmn pri 13, so a web verb cannot be
  preempted by the round (the VII.0 #3 strict-priority argument holds for the new caller),
  and the verb's `tk_dly_tsk(20)` quiesce sleep is precisely what lets the pri-13 round
  finish — same mechanics as the sleeping shell.
- `mind_cmd` prints to the console; the web response cannot scrape stdout. Two small
  FLAGGED publics in `r3_incontext.c` close the loop without forking the verb: after
  `mind_cmd("ask k")` returns, the bridge reads `mind_last_answer(&k,&v,&share)` — a 3-field
  file-static snapshot written at the single existing site where `ask` computes
  `pred/share`; for `/teach` the response is assembled from `r3_facts_pending()` +
  `dmn_r3_rounds()` (both already public, `dtr.h:299` + LM-6). The console output remains
  the verb's primary record; the JSON is a reading of the same state, not a second path.
- Honesty in the response: `/teach` returns `{ok:true,pending:1,rounds:R}` meaning
  *queued* — consolidation happens later, in sleep, and the OWNER WATCHES IT HAPPEN as the
  particle sinks (EV_CONSOLIDATE). The UI never pretends teach is instant. This delay is
  the product, not a bug: it is the visible difference between hearing and learning.
- A web teach calls `dmn_trigger()` exactly as the console one does (via `r3_fact_learn`,
  unchanged) — a stimulus is a stimulus (`dtr.c:1258` precedent).

---

## 7. What v1 does NOT claim (honesty box)

- **No natural-language chat.** The mind's vocabulary is the synthetic k∈0..7 / v∈0..3
  binding space (LM-5/LM-6). The UI says **teach / ask** with two number pickers — it never
  says "chat", never free-text, until the tokenizer slice exists. The page states this
  plainly: "this mind speaks 8 symbols today; it is an infant, truthfully shown."
- **Hosted nodes only** (linux x86_64/aarch64, Android libpkernel). Bare-metal stars appear
  in OTHER nodes' galaxies (they gossip world beacons like anyone) but serve no window
  themselves until `netstack-tcp-server`.
- **localhost only.** No remote/fleet view in v1; that is the named v3 slice
  **`galaxy-relay`** (§11), which must ride the authenticated relay wire, not an open port.
- **This node's view only.** The galaxy is MY star's situational awareness: peers I haven't
  gossiped with don't exist for me; stale peers are faded, not freshened. Two owners'
  galaxies legitimately differ. The page footer says whose view it is.
- **The browser rendering is not certified.** CI gates the data plane (§8); pixels are
  reviewed by humans. Stated in the gate section, not buried.
- **Tamper-evidence caveat carried through:** /self.json shows the hash-chained lineage with
  the same "tamper-evident, not unforgeable (signatures deferred)" footnote the Self layer
  ships under.

---

## 8. The falsifiable acceptance gate

Data-plane only, curl-driven, NON-flaky by the LM-6 playbook (end-state within a bound,
never timing): three tags printed by a host-side cert script
`samples/38_galaxy/galaxy_cert.sh` (the `kill_one.sh` pattern — multi-process hosted certs
already live in `samples/13_survival_loop/`, wired at `ci.yml:359-360`).

- **`[galaxy-serve]`** — boot a 2-node loopback mesh (`PKERNEL_NODE_ID=1/2`, the net_unix
  transport, ports 7801/7802); after SWIM converges (bounded retry ≤30 s, poll 1 s):
  `curl -s 127.0.0.1:7801/galaxy.json` parses as JSON (python3 -c json.load), contains
  `me.id==1`, and `peers[]` contains node 2 with `state==ALIVE`. Also `GET /` returns 200
  with `<canvas` in the body (the page is really embedded, render not judged).
- **`[galaxy-events]`** — with `curl -sN 127.0.0.1:7801/events` capturing in the
  background, drive ONE real stimulus: node 2 runs a `moe` inference that remotes to node 1
  (or simplest deterministic: POST /teach below also counts — but keep this tag on the
  non-mouth path: use the drpc INFER that `kill_one`-style scripts already exercise). PASS =
  within 30 s the capture contains ≥1 `"type":"drpc_in"` event with `src:2`. End-state
  grep on a capture file; no timing window on WHEN.
- **`[galaxy-teach]`** — single node: `curl -d 'k=2&v=3' /teach` returns `ok` and
  `pending:1`; then poll `GET /galaxy.json` until `pending==0` (≤120 s — the LM-6 8×-margin
  bound, same drain physics as `mind wait`, living-mind.md VII.5); then
  `curl -d 'k=2' /ask` returns `pred:3` with `share` ≥ the LM-6 bar. The SSE capture must
  contain ≥1 `consolidate` event between teach and drain. **This rides the LM-6 cert
  machinery end-to-end through HTTP** — proving the web mouth and the console mouth are one
  mouth. (Use the implementer-measured off-bias (k*,v*) from LM-6, not literal 2/3.)

CI wiring: one new step in the `ump-x86_64` job (after "Kernel self-tests",
`ci.yml:54-57`): `timeout 300 samples/38_galaxy/galaxy_cert.sh` + `grep -aF '[galaxy-*] PASS'`
lines, same shape as the kill_one step (`ci.yml:359-360`). curl + python3 are already on
ubuntu-latest. The kernel binary needs stdin held open while curl drives it:
`tail -f /dev/null | ./p-kernel &` inside the script (the shell task idles; the galaxy task
serves) — the script owns setup/teardown like kill_one.sh does.

Bars are proposals; implementer reports actuals; lower only to measured-minus-margin,
flagged — never inflate (the standing rule).

---

## 9. Anti-fork reuse surface

### Reused as-is (the galaxy is a FACE on existing organs)

| Existing symbol | Where | Used for |
|---|---|---|
| `dnode_table[]`, `drpc_my_node`, `DNODE_*` | drpc.h:35,107-110 | peer stars + my id |
| `world_peer_known/pressure/threat/atrisk/region_fresh` | world.h:116-148 | star color/size/constellation/age |
| `dmn_stats`, `dmn_trigger`, (LM-6) `dmn_r3_rounds()` | dmn.h:62,75 + VII.5 | my star's sleep state + rounds |
| `mind_cmd` | r3_incontext.c (LM-6) | THE teach/ask path — the only one |
| `r3_facts_pending` | dtr.h:299 | pending particle count |
| `region_id()` | region.h:32 | my constellation |
| `pfs_dag_read` + `LM_SELF_REF` | pfs_dag.h:150, lm_self.h:71 | /self.json lineage walk |
| `kdds_pub`, `pmesh_send`, `drpc_dispatch/call`, `select_expert`, `cagg_*`, swim sites | §2 table | hook host sites (one emit each) |
| net_unix/net_relay POSIX-TU pattern + errno shim | net_unix.c:19-37 | galaxy_posix.c shape |
| `create_task` fleet block | usermain.c:261-354 | galaxy_task creation |

### New publics (FLAGGED — the complete list)

- `galaxy.h`: `void galaxy_init(void);` `void galaxy_task(INT,void*);`
  `void galaxy_emit(UB type, UB src, UB dst, UH a, UH b);` (+ `volatile UB galaxy_on`)
- `dmn.h`: `UB dmn_current_state(void);` (one-line getter over the dmn.c:54 static)
- `dtr.h`: `void mind_last_answer(UB *k, UB *v, UW *share);` (snapshot of ask's result)
- `world.h`: `INT world_peer_device(UB node);` (the table already stores device_type from
  the beacon, world.h:53; no accessor exists yet)
- `galaxy_posix.c` C-ABI: the 5 `galaxy_io_*` functions (§3.2)

Everything else stays file-static in `galaxy.c` (ring, HTTP parser, JSON writer, samplers) —
the V.6/VI.8 discipline.

### Do-NOT-fork list (auditor greps these)

1. **No second HTTP/JSON library** — no third-party code, no duplicated parser elsewhere;
   the only HTTP server in the tree lives in galaxy.c (today the tree has only HTTP
   *clients*: shell.c:2417, samples 02/10 — they stay untouched).
2. **No second node table** — galaxy.c holds NO peer array; greps must show /galaxy.json
   assembled from `dnode_table` + `world_peer_*` reads only.
3. **No second mind path** — `galaxy.c` contains no `r3_fact_learn` / `r3_consolidate` /
   `s_round` / `dtr_train` token (extends the LM-6 audit grep, living-mind.md:1869).
4. **No new gossip protocol** — the galaxy adds ZERO wire packets in v1/v2; peers' states
   come from beacons that already flow (world.c). v3's remote view rides the existing relay
   wire, versioned there.
5. **No second event/stat system** — subsystem stats stay where they are (`pmesh_stats`,
   `dmn_stats`, `net_rx_tcp`...); the ring stores transients only and never duplicates an
   aggregate that an existing struct owns. (Survey result: no general event ring exists in
   the tree today — engram rings in lm_consolidate.c are training replay buffers, a
   different organ — so this is a genuinely new, single, shared mechanism.)
6. **No second consolidation counter** — EV_CONSOLIDATE is emitted beside the existing
   single `++`/print sites in dmn.c, never from r3_incontext.c.
7. **No second page** — one galaxy.html for all arches and Android.

---

## 10. Commander decisions D1–D6 — RESOLVED (SHIPPED wave-30)

> Trimmed 2026-07-01: GALAXY-1 shipped (gap-ledger **GALAXY-1 Closed wave-30**). The D1–D6
> decisions this section enumerated are resolved in code — `arch/common/galaxy.c` (loopback
> `127.0.0.1:7800+id`, 6 endpoints, SSE ring, `m_gate` maxsem-1 serialization) + cert
> `samples/38_galaxy/galaxy_cert.sh` (`[galaxy-serve/events/teach]`). D4 (the Android galaxy)
> shipped wave-36. Full pre-impl text: `git show 79518a33:docs/architecture/galaxy.md`.

## 11. Sequencing

- **v1 — one star that breathes (this slice).** galaxy.c + galaxy_posix.c + embedded page;
  endpoints §3.4; hooks S2-S6 + S1-snapshot (swim/world reads); the m_gate mutex slice in
  r3_incontext.c; the three cert tags in CI. Peers already render from
  dnode_table/world (data is free), but peer-event richness is not gated. Falsifiable,
  small: ~1 new C file + 1 POSIX TU + 1 HTML + ~8 one-line emit hooks + 4 tiny getters.
- **v2 — the constellation lives.** S7-S9 sampled hooks + EV_SUMMARY intensity; constellation
  layout by region; click-a-peer panel (gossip-age-honest); staleness fading; the sparse
  galaxy aesthetic pass. Gate: `[galaxy-events]` extended to a kdds/pmesh sample event in a
  3-node mesh.
- **v3 — `galaxy-relay` (the Play-Store polish).** A node may OPT IN to forwarding its
  event stream summary over the authenticated relay wire (HMAC v2) so an owner's phone can
  watch their OTHER devices remotely; still no central server — the phone's node merges
  peers' shared views into its own, labeled by origin and age. Needs its own design pass
  (privacy: events reveal activity patterns; per-link consent; wire versioning). Named now,
  not built.

---

## Appendix A — one screen (ASCII)

```
            ·                                  region 2 (constellation)
                         ✦ node5(stale 12s)        ✦ node7
   ✦ node2 ~~~~~~ gossip ~~~~~~ ╮
        ╲                       │
         ╲ drpc ripple →      ★ ME (node1)  ((breathing — dreaming))
          ╲                   ∘ ← fact k=2,v=3 orbiting... sinks at next sleep
   ✦ node3(SUSPECT, flickering)
                              [teach k▾ v▾] [ask k▾]   "this mind speaks 8 symbols today"
   dropped:0  sampled: kdds 4/s of 21/s     this is node1's view, gossip-bounded
```
