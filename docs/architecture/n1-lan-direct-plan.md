# N-1 — LAN-direct transport (same-WiFi auto-mesh, no relay): design plan (cert-first)

**Status: DESIGN PLAN on trunk `2fadc380`, awaiting commander review + a separate
impl→audit cycle.** Read-only investigation; no code changed.

> **Provenance note:** all findings below were ground-truthed against the **shared checkout
> `/root/p-kernel` at base `2fadc380`** (verified `git rev-parse` ✓). This isolated worktree
> sits on an OLDER divergent commit (`5833b1a4`, the picture-book intro) that PREDATES the N-1
> work and contains no `net_lan.c`/`p2p-overlay.md`; the file:line citations are to the
> `2fadc380` tree, which is the base the brief specifies and where the N-1 code actually lives.

mk_pino's passion (`docs/architecture/p2p-overlay.md:2`): *"Skypeのようなネットワークを
築きたい"* — two same-WiFi phones auto-mesh **directly over the LAN, with NO central
`./relay`**. This plan is **cert-first** and **honest about what is already done**: the
N-1 *transport backend* already exists in the tree at this base; what is NOT yet defended
behaviour is the **`[lan-direct]` cert + the load-bearing falsifier + the host-first 2-node
`[live]` proof**. The plan's deliverable is to **close the cert gap** (and document the
real-2-phone payoff as the deferred `[live]` row), not to re-write what ships.

---

## 0. Base verification (DONE)

```
git rev-parse --short HEAD     → 2fadc380          ✓   (shared checkout)
ls arch/common/swim.c          → present (59 KB)    ✓   (STOP-gate file)
ls arch/common/net_relay.c     → ABSENT             ⚠   (see note)
```

**Correction to the brief's STOP-gate:** `net_relay.c` is **not** under `arch/common/`.
The relay backend lives **per-arch** at `arch/linux/aarch64/net_relay.c` (779 lines) and
`arch/linux/x86_64/net_relay.c`, exactly like `net_unix.c` and `net_lan.c`. The transport
backends are an `arch/linux/<arch>/` family, NOT `arch/common/`. `arch/common/swim.c` (the
named alternative) exists, so the gate passes; I flag the path discrepancy so the implementer
does not chase a non-existent `arch/common/net_relay.c`.

**The decisive finding:** the N-1 transport is **already implemented and committed**:

```
git log --oneline -- arch/linux/x86_64/net_lan.c
  85017bb4 N-0: distinct, stable per-install node id (no more collide-on-1)
  b68b845f net: N-1 LAN-DIRECT — relay-free same-WiFi mesh transport (PKERNEL_LAN=1)
```

So this is **not a green-field design**. The honest scope below separates **DONE (shipped
in `b68b845f`)** from **the OPEN cert gap** — the plan's actual job.

---

## 1. What already ships at `2fadc380` (DONE — verify, don't rebuild)

### 1.1 The `net_lan.c` transport — DONE
`arch/linux/{aarch64,x86_64}/net_lan.c` (472 lines; the two are byte-identical except the
header-comment path — `diff` shows only line 2). It implements the 4-symbol backend ABI:

- **`net_lan_init()`** (`net_lan.c:292`): `socket(AF_INET, SOCK_DGRAM)` (`:334`),
  `SO_REUSEADDR` + `SO_BROADCAST` (`:344`, `:345`), `O_NONBLOCK` (`:351`), `bind 0.0.0.0:PORT`
  (`:359`) with `PKERNEL_LAN_PORT` default **7351** (`:83`, `:313`). Builds the broadcast
  sockaddr `255.255.255.255:PORT` once (`:365`, `INADDR_BROADCAST`). Node id from
  `PKERNEL_NODE_ID` else the N-0 stable id `pkernel_default_node_id()` (`:301`). Returns the
  node id (the dispatcher treats `>0` as success).
- **`net_lan_send(frame,len)`** (`net_lan.c:381`): builds one v1/v2 packet (`build_packet`,
  `:224`), then **(1)** `sendto` to the broadcast addr (`:390`, first-contact rendezvous)
  **(2)** `sendto` unicast to **every learned peer** (`:395`–`:398`). This is the exact
  p2p-overlay.md design (`p2p-overlay.md:23`–`:24`).
- **`net_lan_recv(out,maxlen)`** (`net_lan.c:402`): non-blocking `recvfrom` loop; validates
  magic (`:415`), **drops its own echoed broadcast** by `src == my_node_id` (`:426`), v2
  HMAC-verifies (`:434`–`:446`), then **`learn_peer(&from)`** (`:455`) — every accepted inbound
  datagram populates the learned-peer table; broadcast is only the bootstrap, thereafter
  unicast-direct (`p2p-overlay.md:24`).
- **`net_lan_node_id()`** (`net_lan.c:472`).

**The learned-peer table** (`net_lan.c:89`–`:95`): `struct sockaddr_in lan_peers[LAN_PEER_MAX]`
(64), `lan_peer_count`, **all file-static** (never a task-stack local — obeys
`feedback_hosted_relay_stack_overflow`; `net_lan_send`/`recv` scratch `buf[MAX_PKT]` is also
`static`, `:385`, `:405`). `learn_peer` (`:257`) dedups by `(sin_addr, sin_port)`. **Map note:**
the table is **real-LAN-IP keyed** (`sockaddr_in`), NOT node-id keyed — the synthetic
`10.1.0.(n+1)` overlay IP rides **opaque inside the Ethernet frame** and is never parsed by
`net_lan.c` (`:30`–`:32`, `:454`). So node-id → real LAN IP:port is resolved *implicitly*:
a frame from node-id N (in the synthetic stack) arrives from N's real `sockaddr_in`, which is
learned and re-used for all subsequent unicast. This is **simpler and more correct** than an
explicit node-id→IP map (no parsing of the inner overlay), and it is what the brief's "learned-
peer table (node-id → real LAN IP:port)" reduces to in practice.

- **PSK boundary** (`net_lan.c:25`–`:32`, `:316`–`:327`): reuses the **relay v2 wire**
  (HMAC-SHA256 + 64-packet sliding nonce window, `compute_mac` at `:189` is byte-identical to
  `net_relay.c::compute_mac`) whenever `PKERNEL_RELAY_KEY` is set; falls back to v1 plaintext
  (with a warning, `:325`) otherwise. Per-source replay window `rx_replay_ok` (`:162`). This
  exactly matches the critique's PSK requirement (`p2p-overlay.md:152`): a LAN mesh is for
  *nodes sharing a PSK* (mk_pino's own phones), not arbitrary strangers.

### 1.2 The backend selection — DONE
`arch/linux/{aarch64,x86_64}/net_dispatch.c` owns the 4 public `arch_linux_net_*` symbols and
picks a backend at `arch_linux_net_init()`:

```
PKERNEL_LAN set & != "0"   → net_lan_*   (checked FIRST so explicit opt-in wins)   net_dispatch.c:48
PKERNEL_RELAY / _HOST set  → net_relay_*                                            net_dispatch.c:62
else                       → net_unix_*  (loopback default)                         net_dispatch.c:76
```

The LAN branch is gated, falls back on init failure (`:59`), and **leaves the relay + loopback
paths byte-unchanged** (`:45`–`:47` comment + the code). The two arch copies differ **only** in
the header-comment path (`diff` confirmed).

### 1.3 The build wiring — DONE (the wave-36 parity lesson is already honoured)
- `boot/linux/Makefile:120` and `boot/linux_x86_64/Makefile:119` both list `net_lan.c`.
- `android/app/src/main/cpp/CMakeLists.txt:91` lists `${ARCH_LX}/aarch64/net_lan.c`.
- The JNI bridge sets the env: `pkernel_jni.c:185` `setenv("PKERNEL_LAN","1")` +
  `:188` `PKERNEL_LAN_PORT`, cleared when off (`:191`).

So N-1's TU is in **all three** build lists already. **Parity caveat (real):**
`tools/android/check_parity.sh` compares `COMMON`/`ARCH_SHARED`/`KERNEL`/`LIBSTR`/`RELAY` and
the two host Makefiles — but `net_lan.c` is a **per-arch `ARCH` file**, and the script
**deliberately does NOT parity-check `ARCH_C_SRCS`** ("DELIBERATELY per-arch … NOT parity-
checked here", `check_parity.sh:182`–`:184`). I confirmed `net_lan.c` is present in the CMake
`ARCH_SRC` **by hand** (`CMakeLists.txt:91`). **Plan item N-1b-parity:** because the parity
guard structurally can't catch a future drop of a per-arch TU, the cert's build step (below)
must `grep net_lan.c` in **both** host Makefiles AND the CMake, so a regression fails the cert.

### 1.4 The SWIM broadcast beacon — ALREADY a broadcast (no swim.c change needed)
The brief asks to "make swim.c's beacon a real LAN broadcast (SO_BROADCAST)". **This is
already true at the layer where swim.c lives, and the SO_BROADCAST is in net_lan.c, not
swim.c.** Grounding:

- `swim_task` emits **one beacon per round to `IP4(255,255,255,255)`** (`swim.c:589`–`:596`),
  typed `SWIM_ACK` seq=0 so no ACK storm (`:592`), carrying the node's own id + capability
  byte (`:586`, `cap_self_byte()`). This is the NET-DISCOVERY-STAR beacon (`:553`–`:574`).
- `udp_send` maps `IP4(255,255,255,255)` to the **Ethernet broadcast MAC ff:ff:ff:ff:ff:ff**
  with no ARP/gateway hop (`netstack.c:340`–`:348`). The whole Ethernet frame is then handed to
  `arch_linux_net_send` → the selected backend.
- Under `net_unix` the frame goes to loopback peers; under `net_relay` it goes to the relay's
  fan-out bus; **under `net_lan` it leaves the host as a real `255.255.255.255:7351` UDP
  broadcast** (`net_lan_send` `:390`) — and `SO_BROADCAST` (the socket option the kernel
  requires to send to the limited-broadcast addr) is set in **`net_lan_init`** (`net_lan.c:345`),
  which is the correct place (the transport owns the socket). swim.c is transport-agnostic and
  **must stay so** (one beacon, three transports). So: **NO swim.c edit is in scope.** The
  beacon-→-real-LAN-broadcast transformation is `net_lan.c`'s job and is done. The plan's
  deliverable #2 is therefore **"verify the beacon path end-to-end in the cert"**, not a code
  change to swim.c.

### 1.5 Membership convergence over the LAN — the mechanism is present
On receiving any SWIM packet, `swim_rx` marks the **sender ALIVE** and applies the piggybacked
gossip (`swim.c:566`–`:568`), and the NET-DISCOVERY-STAR **transitive adoption** path adopts a
first-seen UNKNOWN peer (`swim.c:254`–`:274`), seeding `dnode_table[nid].ip` from the
deterministic `10.1.0.(nid+1)` map (`swim_node_ip`, `:396`). So once node A's broadcast reaches
B over real LAN UDP, B learns A directly AND learns A's knowledge of C transitively — exactly
the convergence the brief asks for, with **NO wire change** and **NO central node**
(NOCENTRAL — the broadcast is peer-symmetric, every node both emits and adopts).

---

## 2. What is OPEN — the cert gap (the plan's real work)

A repo-wide grep for a net_lan **cert / self-test / shell verb / sample script** finds
**none**: no `net_lan_self_test`, no `lan` shell verb, no `samples/**/run_*lan*.sh`. The
transport is **shipped but undefended** — there is no falsifiable proof that two p-kernel nodes
auto-mesh directly over LAN UDP with no relay, and no executable falsifier proving the LAN path
is load-bearing. **This is the gap N-1 must close**, consistent with the BACKLOG's standing
HONEST framing (`BACKLOG.md:22`): *"the next win must be ONE thing driven to a real `[live]`
PASS, not another pure function … most of this session shipped the SAFE half and DEFERRED the
load-bearing distributed half to a `[live]` row."* N-1's transport is the SAFE half (done);
the cert + the host-2-node `[live]` is the load-bearing half (open).

---

## 3. THE CERT `[lan-direct]` (cert-first, falsifiable)

A **host-first** cert, modelled on `samples/11_distributed/run_ss6_live.sh` (the live-proof
template). New file: **`samples/11_distributed/run_lan_direct.sh`**.

### 3.1 The positive arm — auto-mesh + deliver, with NO relay
Two `./p-kernel` processes, **distinct `PKERNEL_NODE_ID`** (N-0 prerequisite — two phones both
defaulting to id 1 self-echo-filter at `net_lan.c:426` and never mesh; the brief's HARD
PREREQUISITE `p2p-overlay.md:150`), **same `PKERNEL_RELAY_KEY`** (PSK, so v2 wire authenticates
both ways), **`PKERNEL_LAN=1`**, **no `PKERNEL_RELAY*`**, and crucially **NO `./relay` process
started at all**.

PROVE (all four, or the row stays OPEN):
1. **No relay in the process list** — assert `pgrep -f relay/relay` is empty for the whole run
   (the cert's defining NOCENTRAL claim; print it).
2. **Auto-discovery** — each node's log shows `[net] transport = lan-direct` (`net_dispatch.c:56`)
   and the peer becomes a learned peer (`net_lan_peer_count() >= 1`, exposed at `net_lan.c:288`;
   surface it via a tiny `lan` shell verb — see §3.4).
3. **Membership converges** — each node sees the other ALIVE in its `dnode_table` (the existing
   `nodes` shell verb / `region` size ≥ 2), driven by the real `swim_task` beacon →
   `net_lan_send` broadcast → peer `swim_rx`.
4. **A message is delivered peer-to-peer** — node A teaches/publishes one datum (reuse an
   existing K-DDS or `mind teach`/`ask` round, or a drpc echo); node B receives it
   **byte-identical**, with the relay still absent. This is the "Skype-like" payoff in miniature.

### 3.2 The FALSIFIER — PKERNEL_LAN OFF → they DON'T auto-mesh
Re-run the **identical** two processes with **`PKERNEL_LAN` unset** and **still no relay**.
The dispatcher then falls to `net_unix` loopback (`net_dispatch.c:76`); loopback unicasts only
to `127.0.0.1:(29000+id)` (`net_unix.c:62`,`:83`) and there is **no LAN broadcast and no
learned-peer table** — so on **two separate hosts / two namespaces they CANNOT discover each
other** (loopback never leaves the box). The cert MUST observe: membership does **NOT** converge
(each stays region-size 1) and the message is **NOT** delivered. PASS only if positive-arm
converges+delivers AND falsifier-arm does neither — proving **the LAN path is load-bearing**, not
incidental. (Mirror the `[remote-falsifiable]`/`[*-falsifiable]` discipline the repo already
uses, e.g. `run_ss6.sh`, BACKLOG `:42`.)

> Sabotage self-test (the repo's `RED` discipline): rebuild with `learn_peer` neutered (or
> the broadcast `sendto` removed) and confirm the positive arm FAILS — so a green is meaningful.

### 3.3 The SO_REUSEPORT caveat (REAL — drives the namespace design)
Two processes on **one host** sharing a bound UDP port is exactly where this breaks. The
critique calls it out (`p2p-overlay.md:155`–`:156`): *"SO_REUSEPORT load-balances datagrams
across 2 processes on ONE host — use two network namespaces + veth (or two machines)."* The
implementation already chose the safe side: **`net_lan.c` sets `SO_REUSEADDR` but deliberately
NOT `SO_REUSEPORT`** (`net_lan.c:341`–`:344`, with the comment naming the exact failure: "it
would load-balance two processes on one host and silently break a same-host two-node test").
**Consequence for the cert:** the host-first `[live]` MUST use **two network namespaces joined
by a veth pair** (each namespace has its own loopback + its own broadcast domain on the veth),
NOT two bare processes on the host's default namespace (the second `bind` on `0.0.0.0:7351`
without REUSEPORT will `EADDRINUSE`, and even with it the datagrams load-balance). Concretely:

```
ip netns add nsA ; ip netns add nsB
ip link add vethA type veth peer name vethB
ip link set vethA netns nsA ; ip link set vethB netns nsB
ip -n nsA addr add 10.9.0.1/24 dev vethA ; ip -n nsA link set vethA up ; ip -n nsA link set lo up
ip -n nsB addr add 10.9.0.2/24 dev vethB ; ip -n nsB link set vethB up ; ip -n nsB link set lo up
# broadcast on /24 → 10.9.0.255 reaches the peer; net_lan sends 255.255.255.255 (limited bcast)
ip netns exec nsA env PKERNEL_LAN=1 PKERNEL_NODE_ID=1 PKERNEL_RELAY_KEY=$KEY ./p-kernel ...
ip netns exec nsB env PKERNEL_LAN=1 PKERNEL_NODE_ID=2 PKERNEL_RELAY_KEY=$KEY ./p-kernel ...
```

`ip netns add` needs `CAP_NET_ADMIN` / root. **Honest sandbox bound:** the PRoot/Termux dev
sandbox cannot create netns (same wall `run_ss6_live.sh`/`run_supernode_fwd.sh` hit with
backgrounded children — `p2p-overlay.md:128`–`:131`). So the `[live]` 2-namespace run is
**cashed on a real Linux host (the ThinkPad) or the 2-machine SSH env**, exactly like the
SS-6 → SS-6-live pattern. The script must `set -e`-guard the netns setup and print a clear
"OPEN: needs CAP_NET_ADMIN — run on a real host" instead of a fake green.

### 3.4 An in-process / two-process scratch arm (so SOMETHING runs in the sandbox)
Because the full netns `[live]` can't run in the dev sandbox, add a **cheaper arm that DOES**:
a tiny `net_lan_self_test()` (shell verb `lan`) that drives the **real** `build_packet` /
`learn_peer` / `rx_replay_ok` / `compute_mac` in-process — proving (a) a v2 packet round-trips
and authenticates, (b) `learn_peer` dedups and the table caps at `LAN_PEER_MAX`, (c) the
own-broadcast `src==my_node_id` echo is dropped (`net_lan.c:426`), (d) a wrong-PSK frame is
`mac_drop`'d. This is the `[in-proc]` floor (drives shipped code, like `run_ss6.sh`'s stub-peer
arm); the **2-namespace `[live]`** stays the load-bearing row. Be explicit in the BACKLOG which
is which — do not let the in-proc arm masquerade as the `[live]` payoff (the standing
`BACKLOG.md:22` finding).

---

## 4. The honest host-first-vs-real-2-phone split

- **What the host 2-namespace `[live]` proves:** the **mechanism** — two real p-kernel
  processes, no relay anywhere, auto-discover over real LAN UDP broadcast and converge+deliver;
  and the falsifier proves the LAN path is load-bearing. This is a true, reproducible,
  CI-gateable proof of the relay-free LAN mesh.
- **What it does NOT prove (the real payoff):** **two actual phones on the same home WiFi
  auto-meshing with no relay and no internet.** That needs real devices (or the 2-machine SSH
  host) and has two extra real-world dependencies the namespace test cannot exercise:
  1. **AP broadcast forwarding** — most home routers forward UDP broadcast, but "client
     isolation"/"AP isolation" APs drop station-to-station broadcast. Documented fallback
     (`p2p-overlay.md:32`): 224.0.0.x **link-local multicast** via the existing
     `udp_join_group` (`netstack.c:693`), or the supernode (N-2). Deferred, but flag it.
  2. **Android multicast/broadcast RX permission** — Android drops inbound multicast/broadcast
     by default; the app needs a **`WifiManager.MulticastLock`** held while meshing
     (`p2p-overlay.md:148`) plus the right manifest permission. This is N-1b on-phone work,
     NOT covered by the host cert.

So: **the host-first cert is the deliverable that lands now; the real 2-phone same-WiFi
auto-mesh is the `[live]` payoff that needs mk_pino's phones (or the ThinkPad/2-machine env).**
Say so in the BACKLOG row — do not claim the phone win from a green namespace test.

---

## 5. Sequencing

### N-1a — host-first cert (the wave to ship)
The transport, dispatcher, build-wiring, and JNI are **already in `b68b845f`**; N-1a's wave is
**purely the cert + falsifier + the parity-grep guard**:
1. Add `net_lan_self_test()` + a `lan` shell verb (surfaces `net_lan_peer_count`,
   `net_lan_stats`, and the in-proc arm §3.4) in **both** `arch/linux/{aarch64,x86_64}/usermain.c`
   (mirror how `region test` / `nodes cap` are wired, `p2p-overlay.md:52`,`:83`).
2. Add `samples/11_distributed/run_lan_direct.sh` (the 2-namespace `[live]` §3.1 + falsifier
   §3.2 + the build-grep §1.3), `set -e`-guarded to print OPEN (not green) when netns is
   unavailable.
3. Cross-arch: run the in-proc `lan` cert on **aarch64-linux AND x86_64-linux** (the cert must
   PASS on both, like every prior wave); confirm no regression in `nodes`/`region` certs.
4. **Cash the 2-namespace `[live]` on a real host** (ThinkPad / 2-machine SSH) and record the
   PASS in the BACKLOG `[lan-direct]` row.

### N-1b — Android (after N-1a is green on a real host)
1. `net_lan.c` is **already** in the CMake `ARCH_SRC` (`CMakeLists.txt:91`) and the JNI sets the
   env (`pkernel_jni.c:185`) — so the TU/parity work is mostly done. Add the explicit
   **build-grep** to the cert (§1.3) since `check_parity.sh` structurally skips per-arch TUs.
2. On-phone: hold a **`WifiManager.MulticastLock`** while the LAN mesh is on; request the
   broadcast/multicast RX path; add the manifest bits. (`p2p-overlay.md:148`)
3. The real-2-phone-same-WiFi auto-mesh is the **`[live]` payoff row** — cash on mk_pino's two
   phones; document the AP-isolation fallback (multicast/supernode) honestly.

### DEFERRED (explicitly out of scope, per the brief)
- **N-2** emergent supernodes — selector + capability gossip already DONE (`BACKLOG.md:37`–`:38`);
  **N-2c packet forwarding** present as host-only `arch/common/supernode.c` (allowlisted in
  `check_parity.sh:57`–`:63`).
- **N-3** NAT hole-punch (supernode-assisted STUN; cone-NAT only, symmetric stays relayed —
  `p2p-overlay.md:136`–`:139`).
- **N-4** bootstrap/seed (`PKERNEL_SEED` list; relay = one optional well-known seed —
  `p2p-overlay.md:140`–`:143`).
- **AP-isolation multicast fallback** (224.0.0.x via `udp_join_group`) — design exists
  (`p2p-overlay.md:32`); wire only if a real AP needs it.

---

## 6. Determinism / parity boundary (invariants the wave MUST hold)

- **Default behaviour UNCHANGED, byte-identical.** With `PKERNEL_LAN` unset the dispatcher never
  enters the LAN branch (`net_dispatch.c:48`); relay + loopback paths are textually untouched
  (`:62`–`:82`). A default node (no env) is byte-identical to pre-N-1. The cert's falsifier arm
  is itself the proof that LAN-off ≠ LAN-on.
- **NOCENTRAL / peer-symmetric.** No central authority: the broadcast is symmetric (every node
  emits `swim.c:596` and adopts `swim.c:254`); the learned-peer table is local; no relay, no
  registrar, no vote. The cert's "no relay in `pgrep`" assertion is the literal NOCENTRAL proof.
- **One mind / byte-identical payload.** `net_lan.c` moves **whole Ethernet frames only**, never
  parses the inner synthetic IP (`:30`–`:32`); the delivered message is byte-identical to the
  relay/loopback path (same v1/v2 wire, same `compute_mac`). The delivered-datum arm asserts
  byte-identity.
- **No new wire.** SWIM stays `SWIM_VERSION` unchanged (no beacon-format change); the relay v2
  wire is reused verbatim. Membership converges with NO wire change.
- **Build-list lock-step.** `net_lan.c` is in both host Makefiles AND the Android CMake; the cert
  adds a `grep`-guard because `check_parity.sh` does not cover per-arch TUs (`:182`–`:184`).
- **Do NOT touch:** the SMP / aarch64 production scheduler files (Thread ② is mid-flight,
  `BACKLOG.md:18`), `swim.c`'s beacon (transport-agnostic by design), `region.c` selectors, the
  mind math (`moe.c`/`dtr.c`/`student.c`), or the `pfs`/signing organs. N-1a edits are confined to
  **two `usermain.c` (the `lan` verb) + one new sample script** (the transport itself already
  ships). N-1b adds **Android JNI/manifest/MulticastLock only**.

---

## 7. Caveats, flagged honestly (for the auditor)

1. **SO_REUSEPORT** — intentionally NOT set (`net_lan.c:341`); two procs on one host won't work,
   the cert MUST use 2 netns/veth or 2 machines (§3.3). The auditor should confirm the cert does
   not silently degrade to "one host, one process answering both" (a false green).
2. **Broadcast on loopback** — `255.255.255.255` on a single host's `lo` does not reach a sibling
   process the way a veth broadcast domain does; this is *why* the netns/veth shape is mandatory,
   not optional.
3. **Android multicast permission** — inbound broadcast/multicast is dropped without a
   `MulticastLock` (`p2p-overlay.md:148`); the host cert cannot exercise this — it is a real N-1b
   gap, not covered by a green namespace run.
4. **AP client-isolation** — a real-WiFi failure mode the namespace test can't see; honest
   fallback is multicast/supernode (`p2p-overlay.md:32`), deferred.
5. **`net_relay.c` path** — lives at `arch/linux/<arch>/`, NOT `arch/common/` (the brief's
   STOP-gate path was off; `swim.c` carried the gate).
6. **The transport already ships** — the wave is the **cert**, not the backend. The auditor must
   verify the cert drives the **real shipped** `net_lan_send`/`recv`/`learn_peer` (not a re-impl),
   and that the **falsifier actually fails** with LAN off (the load-bearing proof).

---

## 8. Acceptance gate (what "DONE" means for N-1a)

- `[lan-direct]` host 2-namespace `[live]` (on a real host): **no relay in `pgrep`**, both nodes
  log `transport = lan-direct`, membership converges (region ≥ 2), a datum is delivered
  byte-identical — **AND** the `PKERNEL_LAN`-off falsifier converges/delivers **nothing**.
- The in-proc `lan` cert PASSES on **both** aarch64-linux and x86_64-linux; `nodes`/`region`
  certs regress clean; a sabotage rebuild goes **RED**.
- The build-grep finds `net_lan.c` in both host Makefiles AND the CMake.
- BACKLOG `[lan-direct]` row records the host `[live]` PASS **and** names the **real-2-phone
  same-WiFi auto-mesh as the still-deferred `[live]` payoff** (no claiming the phone win from a
  namespace green).

---

*Grounded in (base `2fadc380`, shared checkout): `docs/architecture/p2p-overlay.md`,
`docs/architecture/BACKLOG.md` (Thread N), `arch/linux/{aarch64,x86_64}/net_lan.c`,
`…/net_dispatch.c`, `…/net_unix.c`, `…/net_relay.c`, `arch/common/swim.c`,
`arch/common/netstack.c`, `boot/linux{,_x86_64}/Makefile`,
`android/app/src/main/cpp/{CMakeLists.txt,pkernel_jni.c}`, `tools/android/check_parity.sh`,
`samples/11_distributed/run_ss6_live.sh`, `arch/linux/node_id.c`.*
