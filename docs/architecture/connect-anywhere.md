# Connect From Anywhere — hardening Thread N connectivity

Status: Slice 1 (heartbeat) + Slice 3 (plain-TCP fallback) + Slice 4 (automatic
UDP<->TCP relay-transport fallback) SHIPPED + AUDITED + integrated to master
(4f1eb07e, 42ac0c54, 0b94f6a0), 2026-06-27. Slice 2 (public relay) DEPLOYED on
relay.helloidea.org:7400 with EXTERNAL reachability proven (mobile -> home router
7400/udp + 7400/tcp -> relay). With Slice 4, a node on a UDP-blocked network
auto-falls-back to TCP with NO manual PKERNEL_RELAY_TCP. Remaining for true
"anywhere": TLS-443 sub-slice (Caddy SNI demux) for egress-443-only/DPI nets — see
§5. Original design below; trunk tip was ~29a85d77.
Author: planning pass for mk_pino's CORE requirement (2026-06-26):
"どんな環境でも繋がるように" — a home no one owns means every device must be
able to join, not "works on some networks." This doc closes the gaps a real
two-machine ゆりかご (cradle_*) debug exposed today (ThinkPad x86_64 relay+teacher
⇄ phone aarch64 PRoot student over real LAN), tcpdump-verified.

## 0. Crown-safety ground rule (applies to every slice)

The bare-metal `.text` (x86 ring0 / aarch64 EL1, crown 755a20fa) MUST stay
byte-identical. The compile split decides safety:

- HOSTED-ONLY (never linked bare-metal → crown-safe by construction):
  `arch/linux/<arch>/net_relay.c`, `net_dispatch.c`, `net_lan.c`, `rtl8139.c`,
  and the standalone host binary `relay/relay.c`.
- CROWN/SHARED (compiled into BOTH images): `arch/common/swim.c`, `region.c`,
  `supernode.c`, `drpc.c`. Any edit here changes bare-metal `.text` unless it is
  `#ifdef`-gated to a hosted-only define.

DESIGN INVARIANT: all three slices land entirely in HOSTED TUs. Where they need
a decision that already lives in a shared TU (e.g. `np_decide`), they REUSE it
read-only — no edit to the shared `.text`.

## 1. The deadlock, from the code

Ground-truth peeled three layers: (1) ThinkPad ufw default-deny blocked UDP;
(2) phone AdGuard VPN dropped return UDP; (3) DEEPEST — after 1+2, the relay sent
713 UDP packets to the student, the student received 0, and the student sent
only its ~4 registration packets then went SILENT. The phone can send UDP,
receive ICMP back, do TCP both ways, and do UDP↔INTERNET both ways (DNS returns)
— it just does NOT receive inbound *LAN* UDP DATA (a 6 GHz AP station-isolation
behaviour).

How p-kernel originates relay traffic (tx drivers), traced:

- `net_task` (rtl8139.c:76-89) polls `rtl8139_recv` → `arch_linux_net_recv`
  (net_dispatch.c:97) → `net_relay_recv` (net_relay.c:778) at ~100 Hz
  (`tk_dly_tsk(10)` when idle).
- `net_relay_recv` calls `ha_tick()` FIRST (net_relay.c:781); so does
  `net_relay_send` (net_relay.c:770).
- `ha_tick` (net_relay.c:470-499) holds a 25 s keepalive
  (`KEEPALIVE_SEC`, net_relay.c:100): `if (time(NULL) - last_send_ts >=
  KEEPALIVE_SEC) send_keepalive_to(cur_relay);` (net_relay.c:474-476).
- `swim_task` (swim.c:594-602) emits one limited-broadcast membership beacon per
  round (swim.c:642-650) via `swim_send` → `udp_send` → … → `net_relay_send`.

Two findings that re-frame the brief:

FINDING A — the keepalive already exists and is already peer-INDEPENDENT.
It rides `net_task`'s unconditional 100 Hz recv poll, not SWIM's peer logic. So
the bug is NOT "there is no heartbeat." The real defects are: its 25 s interval
can EXCEED the AP/NAT return-mapping lifetime (commonly ~30 s, often less on
mobile/6 GHz APs — and a single lost keepalive then opens a 50 s gap); it is
COUPLED to `net_task` actually being scheduled; and on a hostile LAN-AP no
client-side keepalive can help at all (slice 2).

FINDING B — origination of CLUSTER traffic is gated on admission.
`swim_task` does `if (drpc_my_node == 0xFF) continue;` (swim.c:603); `drpc_my_node`
defaults to 0xFF (drpc.c:77) and is only set at `drpc_init` (drpc.c:524). In
dynamic-lease mode (relay grants the id to a `src=0` auto-REGISTER, relay.c:370+),
a node that never receives its grant (inbound blocked) is never admitted, so the
beacon at swim.c:649 never fires. THIS is the "went silent after 4 packets"
signature: the ~4 packets are the boot-time REGISTER/auto-REGISTER attempts; with
no inbound grant the cluster layer never originates again. The deadlock closes:
no inbound → not admitted → no cluster tx → return mapping ages out → inbound
stays dead.

Honest bound on B: with a STATIC `PKERNEL_NODE_ID`, `drpc_my_node` is set before
`swim_task` starts (usermain.c:319 `drpc_init` precedes :347 `swim_task`), so the
swim beacon would keep firing and the `ha_tick` keepalive would too. The fully
silent observation is therefore most consistent with dynamic-lease/cradle mode
and/or the keepalive interval lapsing during the debug window. Either way the
robust fix is the same: a heartbeat that does not depend on admission OR on an
upstream task's cadence.

---

## SLICE 1 — Unconditional relay heartbeat (the deadlock breaker)

GOAL: a steady node→relay heartbeat that fires REGARDLESS of peer discovery AND
regardless of cluster admission, at an interval safely below the NAT/AP UDP
mapping lifetime, to hold the return path open so inbound can ever arrive.

WHERE IT BELONGS: `arch/linux/<arch>/net_relay.c` — HOSTED, crown-safe. This is
already where `ha_tick`/`KEEPALIVE_SEC`/`send_keepalive_to` live. Do NOT touch
`swim.c`/`drpc.c` (crown).

MINIMAL CHANGE (hosted-only):
1. Lower and document the interval: `KEEPALIVE_SEC` 25 → 15 (net_relay.c:100),
   well under a 30 s mapping and tolerant of one lost keepalive. (Relay-side
   `IDLE_TIMEOUT` is 300 s, relay.c:41 — unaffected; this is about the NAT in the
   middle, not the relay's own eviction.)
2. DECOUPLE the heartbeat from the recv poll and from admission. `ha_tick`'s
   keepalive clause already does not look at peers or `drpc_my_node`; the only
   coupling is that `ha_tick` runs solely when `net_relay_send/recv` are called.
   Make a dedicated hosted heartbeat tick so the beat survives even if the
   netstack stops calling send/recv:
   - Preferred: a tiny hosted heartbeat task created in usermain alongside
     `net_task` (usermain.c:340) that calls a new `net_relay_heartbeat()` (which
     just runs the keepalive clause of `ha_tick`) every `KEEPALIVE_SEC`. The task
     creation is in `arch/linux/<arch>/usermain.c` (HOSTED) and the function in
     net_relay.c (HOSTED) — both crown-safe.
   - The heartbeat MUST run before admission: it only needs `sock_fd` and
     `cur_relay`, both set in `net_relay_init` (net_relay.c:730-763) independent
     of `drpc_my_node`. So a node that has registered but not been admitted still
     beats, holding the mapping open until the grant can arrive.
3. Keep the existing solo-degrade guard intact: `udp_send_to` no-ops when
   `idx >= relay_count` (net_relay.c:325), so a solo node beats nothing — correct.

CROWN VERDICT: PASS. All edits in `net_relay.c` + `arch/linux/<arch>/usermain.c`,
both hosted-only. `swim.c`/`region.c`/`drpc.c` untouched → bare-metal `.text`
byte-identical. Verify with the standard `nm`/objdump tripwire on the bare-metal
image before/after.

FALSIFIABLE CERT (in-proc, no sockets — mirrors `seed_bootstrap_self_test`,
net_relay.c:931):
- Build net_relay.c with a mock `sendto` that counts packets and a mock
  monotonic clock. Drive `net_relay_heartbeat()` across T seconds of simulated
  time with ZERO inbound and ZERO `net_relay_send` calls (an isolated,
  un-admitted node). ASSERT: at least `floor(T/KEEPALIVE_SEC)` keepalives were
  emitted, and the max inter-keepalive gap < `NAT_TIMEOUT_FLOOR` (e.g. 30 s).
- FALSIFIER (proves unconditionality is load-bearing): compile a variant that
  gates the heartbeat on `peer_count > 0` (or on `drpc_my_node != 0xFF`); ASSERT
  the harness now emits ZERO keepalives → cert FAILs. This shows the gate, if
  reintroduced, reopens the deadlock.
- DEFERRED [live] row: isolated node pointed at a real relay on another host,
  `tcpdump` shows steady keepalives at the cadence for ≥2 minutes with no peers.

EMPIRICAL GROUNDING: this directly attacks Finding A/B — the beat no longer
depends on `net_task` scheduling or on the `drpc_my_node==0xFF` admission gate
(swim.c:603), so "register, then silence" becomes "register, then beat forever
until admitted."

---

## SLICE 2 — Public / internet-reachable relay (sidestep the LAN-AP wall)

GOAL: because the phone's WAN-UDP is bidirectional (DNS proved it), an internet
relay's return datagram arrives via the router's WAN NAT — never via the
station-isolating LAN AP. With Slice-1's heartbeat holding the WAN mapping open,
inbound now works. This is the slice that makes today's exact failure go away.

WHAT'S ALREADY THERE (reuse, near-zero new code):
- `relay/relay.c` already binds `INADDR_ANY` (relay.c:687) and is filesystem-
  stateless (relay.c header) — it runs unmodified on a public host. Operationally
  it needs an open inbound UDP port (default 7400) and a v2 PSK (`--insecure`
  only for bring-up).
- Node discovery/config already exists: `PKERNEL_RELAY` (ordered list, ≤4) and
  `PKERNEL_SEED` (a relay is just a seed that answers REGISTER) are parsed by the
  same `parse_relay_list` into `relay_list[]` (net_relay.c:584-622, 698-727);
  selection is wired in `net_dispatch.c` (net_dispatch.c:63-82).
- Multi-relay failover (HA) is already implemented and certified: deterministic
  "first live relay on the shared list" via `ha_tick` probing (net_relay.c:
  470-518) and the pure `seed_select_next` core (net_relay.c:632-646), with its
  own in-proc cert (net_relay.c:931, `SEED_BOOTSTRAP_CERT`).

WHAT'S NEEDED (small, all hosted):
1. Numeric-IP contract: `resolve_relay` is numeric-only by design to avoid a
   getaddrinfo crash in the T-Kernel task context (net_relay.c:552-582). A public
   relay is given by IP — fine. If we ever want a DNS name (e.g. relay.pino.net),
   that's a SEPARATE hosted helper resolving the name in `main`/usermain BEFORE
   the kernel task starts and passing the numeric IP via env — do NOT lift the
   in-task guard. Note as a follow-up, not part of this slice.
2. Document the public-relay recipe in `samples/11_distributed/` style: run
   `relay` on a VPS with the PSK, set `PKERNEL_RELAY=<vps-ip>:7400` (or a 2-entry
   HA list `<vps-a>:7400,<vps-b>:7400`) + `PKERNEL_RELAY_KEY=<hex>` on every node.
   Both phone and ThinkPad point at the SAME public relay; the LAN AP is bypassed.
3. Failover/failback already converges fleet-wide because the rule is a pure
   function of (shared list, per-relay liveness) — no coordinator (net_relay.c:
   18-43 header).

CROWN VERDICT: PASS. `relay/relay.c` is a standalone host binary; node-side config
is all in `net_relay.c`/`net_dispatch.c` (hosted). No shared-TU edit.

FALSIFIABLE CERT:
- Pure-core already covered by `seed_bootstrap_self_test` (net_relay.c:931) and
  the relay-HA failover test (greps `[net_relay] failover ->`, net_relay.c:492).
- NEW DEFERRED [live] row (`run_public_relay.sh`): node A behind NAT-1, node B
  behind NAT-2, relay on a public IP. ASSERT A and B exchange data through the
  relay. FALSIFIER: `iptables` drop inbound LAN UDP on A's host (emulate the AP
  wall) while leaving WAN intact; ASSERT A still joins — proving the WAN-return
  path, not the LAN path, is what carries it. This is the on-device repro of
  today's failure with the fix applied.

---

## SLICE 3 — TCP (later TLS-443) relay fallback (the "anywhere" last resort)

GOAL: for networks that block UDP entirely (corporate/cafe/some carriers), join
the SAME relay mesh over a TCP stream. 443/TLS is the ideal egress-friendly port
(looks like HTTPS) but TLS is a later sub-slice; PLAIN-TCP-relay first.

WHY A NAIVE TUNNEL FAILED TODAY: TCP is a byte stream with no datagram
boundaries, so a `socat` UDP↔TCP tunnel concatenates frames and the receiver
cannot re-split them. The fix is explicit length-framing.

DESIGN — a new HOSTED transport backend beside `net_relay.c`/`net_lan.c`:
- New file `arch/linux/<arch>/net_relay_tcp.c` exposing the same 4 symbols
  (`*_init/_send/_recv/_node_id`) that `net_dispatch.c` already dispatches through
  (net_dispatch.c:29-37). Selected by a new `PKERNEL_RELAY_TCP=1` (or by the
  ladder in §4) in `arch_linux_net_init` (net_dispatch.c:44-90).
- Wire: REUSE the v2 HMAC framing already in `net_relay.c` (`build_packet`,
  net_relay.c:290-320; `compute_mac`, :254-286) and in `relay/relay.c`
  VERBATIM — same magic/ver/type/src/dst/nonce/HMAC. The ONLY addition is a
  2-byte big-endian length prefix per packet on the stream:
  `[u16 len][12+24+payload v2 packet]`. Receiver reads 2 bytes, then `len` bytes,
  reassembling across short reads (the part `socat` couldn't do).
- Relay side: `relay/relay.c` gains a second listening socket — a TCP
  `accept()` loop that de-frames length-prefixed packets and feeds them into the
  SAME `handle_packet`/forward path (relay.c:471-494), so a TCP client and a UDP
  client see one shared `{node_id → peer}` table and fan out to each other. The
  relay binary is a host program → hosted/crown-safe.
- Keepalive: the same heartbeat (Slice 1) applies — a TCP keepalive frame holds
  the proxy/NAT TCP state and lets the relay detect a dead stream.

CROWN VERDICT: PASS. New `net_relay_tcp.c` + `net_dispatch.c` selection +
`relay/relay.c` TCP loop are ALL hosted. Bare-metal unaffected — `swim.c` et al.
never see the transport; they keep calling `arch_linux_net_send/recv`.

FALSIFIABLE CERT:
- UNIT (the load-bearing bug): feed the de-framer a buffer holding TWO packets
  concatenated, delivered in arbitrary chunk splits (1 byte at a time, then all
  at once). ASSERT it yields exactly the two original payloads, byte-identical.
  FALSIFIER: drop the length prefix (naive socat behaviour) → ASSERT the
  de-framer mis-splits → cert FAILs. This certifies precisely why today's tunnel
  failed and that framing fixes it.
- DEFERRED [live] row (`run_tcp_fallback.sh`): `iptables` DROP all UDP (in+out)
  on the node host; ASSERT the node still joins the mesh and exchanges data via
  TCP to the relay. FALSIFIER: also block the TCP port → ASSERT it cannot join
  (honest bound — see §5).
- TLS-443 sub-slice (later): wrap the TCP stream in TLS to port 443; cert adds a
  handshake-success assertion and an egress-only-443 network emulation. Deferred.

---

## 4. RUNTIME CONNECTION FALLBACK LADDER (first-class; distinct from §6 sequencing)

This is the per-connection decision a node makes AT RUNTIME, orthogonal to which
slice we BUILD first. Two axes — TRANSPORT (UDP/TCP) × PATH (direct-P2P /
via-relay) — ordered by a failure taxonomy (what each rung recovers from):

| Rung | Transport × Path | Recovers from | Cost |
|------|------------------|---------------|------|
| 1 | Direct UDP P2P (same-LAN, or public-IP candidate, or N-3 cone-NAT punch) | nothing blocked; both reachable | fastest, zero relay load |
| 2 | Direct TCP P2P | local net allows TCP but blocks UDP inbound (TODAY'S CASE: phone dropped direct UDP inbound on the 6 GHz AP, but direct TCP/SSH LAN→phone WORKED). Only same-LAN or one-peer-public; two-NAT direct TCP punch is unreliable → falls through | low |
| 3 | Relay over UDP (public relay, Slice 2) | two-NAT + asymmetric local nets; phone WAN-UDP is bidirectional (DNS), so relay return arrives via router NAT | one relay hop |
| 4 | Relay over TCP/443 (Slice 3) | UDP fully blocked end-to-end | one relay hop, TCP overhead |

Rung 2 is empirically validated by today's measurement: direct-TCP-before-relay
is real, not theoretical. Caveat captured in the table: direct TCP only works
same-LAN or when one peer is publicly reachable.

### 4.1 Detection / selection — DON'T serialize the timeouts

Strictly-serial fallback stacks each rung's timeout (≈4 rungs × ~1 s = slow,
visible "won't connect" stalls). Use an ICE / happy-eyeballs approach: GATHER
candidate paths, PROBE them CONCURRENTLY with bounded per-rung timeouts and a
small staggered head start, and ADOPT the first/best that connects; cancel the
rest. Bounded budget, not summed:

- Rung 1 (direct UDP): probe window 250 ms.
- Rung 2 (direct TCP): start +150 ms after rung 1, window 500 ms.
- Rung 3 (relay UDP): start +400 ms, window 1000 ms.
- Rung 4 (relay TCP/443): start +800 ms, window 2000 ms.
- TOTAL bound ≈ 2.8 s wall (concurrent), vs ≈3.75 s if serialized — and in the
  common case rung 1 or 2 wins in <300 ms. Prefer the lowest-numbered rung that
  answers within its window ("happy eyeballs": first success wins, lower rung
  preferred on a tie).

### 4.2 Mapping onto existing p-kernel code

- The transport choice lives in `net_dispatch.c` (net_dispatch.c:44-90). TODAY it
  picks ONE backend at init and is "stable for the process lifetime"
  (net_dispatch.c:14-16). The ladder turns this into a candidate RACER: a hosted
  selector that initializes multiple backends (lan / relay-udp / relay-tcp),
  probes concurrently, and points `g_send`/`g_recv` (net_dispatch.c:39-41) at the
  winner — with periodic re-evaluation so a better rung is adopted when the
  network changes. ALL hosted → crown-safe.
- The direct-P2P punch/relay decision ALREADY exists and is REUSED read-only:
  `np_decide` (supernode.c:775) returns PUNCH iff both peers are CONE, else RELAY
  (supernode.c:772-783), from per-peer NAT classification (`classify`,
  supernode.c:756-762, N-3 rendezvous on `SNF_PORT`, supernode.c:350-351). Rung 1
  consults this; a SYMMETRIC/relay verdict skips straight to rung 3. NOTE:
  `supernode.c` is a SHARED/crown TU — the ladder must only CALL `np_decide`, not
  edit it, to keep `.text` byte-identical.
- Relay fallback is already the N-2c/N-4 behaviour: a relay is one optional seed
  (net_relay.c:29-43), and the N-2c forwarding plane (supernode.c, "Supernodes")
  already relays when a punch is refused. Rungs 3/4 are the UDP and TCP flavours
  of this existing fallback.
- Direct-LAN candidate gathering reuses `net_lan.c` (broadcast rendezvous +
  learned-unicast, net_lan.c:389-396) for rung 1's same-LAN case.

### 4.3 Crown + cert for the ladder

CROWN VERDICT: PASS provided the racer/selector and all per-peer transport state
live in HOSTED TUs (`net_dispatch.c`, the backends), and shared deciders
(`np_decide`, supernode forwarding) are called but not modified. If the ladder
ever needs to PERSIST a per-peer transport choice, store it in a hosted table
(net_dispatch/net_relay), NEVER in the shared `swim.c`/`region.c` peer tables —
that would change bare-metal `.text`.

FALSIFIABLE CERT (in-proc):
- Drive the selector with a mock "network" that marks each rung
  reachable/blocked. Case A: only rung 2 reachable (today's net) → ASSERT the
  selector adopts rung 2 within its bounded window and emits NO rung-3/4 join.
  Case B: only rung 4 reachable → ASSERT adopts TCP/443. FALSIFIER: force serial
  probing and ASSERT total time exceeds the concurrent bound → proves the
  happy-eyeballs concurrency is load-bearing for latency.
- Re-evaluation: flip rung 1 from blocked→reachable mid-run → ASSERT the selector
  upgrades to the lower rung.

---

## 5. Honest bound — what stays unreachable

Truly hostile networks remain unreachable and we should SAY so, not pretend:
- A network that blocks ALL UDP AND all outbound TCP (no 443 egress) cannot be
  joined — there is no channel left.
- A network with an HTTPS-only allowlist + deep-packet-inspection that drops
  non-TLS or non-whitelisted-SNI traffic defeats plain-TCP rung 4; only the
  later TLS-443-mimicking-HTTPS sub-slice has a chance, and even that loses to a
  TLS-terminating proxy that whitelists destinations.
- Carrier-grade NAT on BOTH peers with no public relay reachable = no path.

The ladder maximizes reach (1→4 covers the overwhelming majority of home/mobile/
office nets, including today's 6 GHz-AP case at rung 2/3), but "anywhere" is
asymptotic, not absolute. A reachable PUBLIC RELAY is the single dependency that
makes rungs 3/4 work — so the ark should always ship with at least one
well-known public relay endpoint.

---

## SLICE 4 — Automatic relay-transport fallback (UDP↔TCP), v1 of the §4 ladder

This is the FIRST CONCRETE IMPLEMENTATION of the §4 runtime ladder, narrowed to
ONE axis: the two RELAY transports to the SAME configured endpoint — rung 3
(relay-UDP, Slice 2/`net_relay.c`) ↔ rung 4 (relay-TCP, Slice 3/
`net_relay_tcp.c`). A node brings up relay-UDP and, if relay contact is not
achieved within a bounded window, brings up relay-TCP to the SAME endpoint and
adopts whichever achieves relay contact first (happy-eyeballs, lower-rung-
preferred-on-tie, §4.1), with periodic re-evaluation that prefers UDP once it
recovers. NO human sets `PKERNEL_RELAY_TCP`; the manual override is preserved.

SCOPE LOCK. IN: automatic UDP↔TCP relay-transport selection to the same
endpoint(s). OUT (deferred, see §4/§5, NOT designed here): direct-P2P rungs 1/2
(LAN broadcast, N-3 cone-NAT punch / `np_decide`), and the TLS-443 flavour of
rung 4. Multi-relay HA *across* the transport choice is also deferred (the race
uses the list head, `cur_relay==0`; per-transport HA is unchanged).

### 4S.a Mechanism

WHERE THE SELECTOR LIVES. `arch/linux/<arch>/net_dispatch.c` (both twins, in
lockstep) — it already owns `g_send`/`g_recv`/`g_node_id` (net_dispatch.c:48-50)
and the env-driven backend choice. A new hosted selector `xport_auto_init()` is
entered from `arch_linux_net_init()` ONLY when a relay endpoint is configured
(`PKERNEL_RELAY`/`PKERNEL_RELAY_HOST`/`PKERNEL_SEED`) AND `PKERNEL_RELAY_TCP` is
UNSET AND `PKERNEL_RELAY_AUTOFALLBACK` != "0". It sits BETWEEN the existing
force-TCP branch (net_dispatch.c:84-97) and the plain-UDP branch
(net_dispatch.c:99-110), so both existing manual paths are untouched.

OVERRIDES PRESERVED:
- `PKERNEL_RELAY_TCP=1` → the existing force-TCP branch (net_dispatch.c:84-97)
  runs FIRST and returns; the selector is never reached. Unchanged.
- `PKERNEL_RELAY_AUTOFALLBACK=0` → selector skipped; control falls through to the
  plain `net_relay_init()` UDP branch (net_dispatch.c:99-110). Pure-UDP escape
  hatch AND the disabling switch the live falsifier's second teeth-check needs.
- Nothing set (relay configured) → AUTO (new default behaviour of this slice).

THE "relay contact" PREDICATE (concrete, code-observable, per backend):

- relay-UDP: a NEW hosted flag `relay_contacted` (0/1), SET inside `ha_mark_rx()`
  (net_relay.c:533) — i.e. on ANY inbound frame from a *configured* relay
  (net_relay.c:872 is the only caller, after the v2-HMAC check). Exposed by a new
  `int net_relay_contacted(void)`. The round trip is FORCED, not waited-for: a new
  `void net_relay_probe(void)` calls `send_keepalive_to(cur_relay)`
  (net_relay.c:373) immediately at probe start; the relay echoes a non-probe
  keepalive VERBATIM (relay.c:780 `REL_KEEPALIVE`, see the relay.c:164 note "Non-
  probe keepalives … are echoed verbatim"); the next `net_relay_recv()`
  (net_relay.c:807) drains the echo → `ha_mark_rx()` → `relay_contacted=1`. So
  "UDP contact" == "the relay answered our keepalive", not merely "socket open".

- relay-TCP: a NEW hosted flag `relay_contacted`, SET when the FIRST COMPLETE
  inbound framed packet is popped in `net_relay_tcp_recv()` (at the successful
  return, net_relay_tcp.c:524-527). Exposed by `int net_relay_tcp_contacted(void)`.
  A successful non-blocking `connect()` in `net_relay_tcp_init()`
  (net_relay_tcp.c:444 returns the node id) is a NECESSARY PRECONDITION but is
  DELIBERATELY NOT sufficient — a dumb TCP port that accepts but never speaks the
  v2 wire must NOT count as contact. Contact requires the relay's keepalive echo
  over the stream (relay.c:792 "a TCP keepalive is echoed length-framed"), forced
  the same way by `net_relay_tcp` sending an immediate keepalive at probe start.

CONCURRENT-PROBE WINDOWS (2-rung; bounded, NOT serialized per §4.1):
- `t=0`: `net_relay_init()` (UDP) then `net_relay_probe()`; begin pumping
  `net_relay_recv()` + polling `net_relay_contacted()` on a tight cadence (~20 ms).
- `UDP_HEADSTART_MS = 300`: relay-TCP is NOT started before this (lower-rung head
  start). If UDP contacts within `[0, 300]` → adopt UDP and **never call
  `net_relay_tcp_init()`** (Case A: emits NO TCP connect).
- `t=300`: if still no UDP contact, start `net_relay_tcp_init()` (bounded connect;
  the race lowers the connect cap to `RACE_CONNECT_TMO_MS = 700`) + its keepalive
  probe; now pump BOTH backends' recv + contacted concurrently.
- `ADOPT_DEADLINE_MS = 2500`: overall wall bound. Total ≈2.5 s worst case, vs
  ≈3.7 s if the two rungs were serialized; common case adopts in <300 ms.

ADOPT RULE: first-contact-wins; lower rung (UDP) preferred on a tie. On adopt,
`g_send`/`g_recv`/`g_node_id` (net_dispatch.c:48-50) are repointed at the winner
and the LOSER is torn down via a new hosted `net_relay_close()` /
`net_relay_tcp_close()` (closes the fd, clears `*_contacted`) so the relay's
single `{node_id→peer}` entry (relay.c:384-389) settles on the surviving
transport; the winner re-sends a REGISTER so the relay's `via_tcp` flag
(relay.c:388) matches. If NEITHER contacts by the deadline → provisional UDP
backend stays pointed (best-effort) but NO mesh is claimed (honest; §5).

POINTER REPOINT / CONCURRENCY: `g_send`/`g_recv`/`g_node_id` are single aligned
function-pointer words; a store is atomic on x86_64 and aarch64 (same as the
init-time assignment). Only the selector / re-eval task writes them; `net_task`
reads them through `arch_linux_net_send/recv` (net_dispatch.c:120-133). Honest
bound: a call already in flight may use the prior transport for one frame — no
torn pointer, no crash. Per-backend state (sockets, nonce windows) is independent
static storage in each TU; nothing shared is mutated.

RE-EVALUATION + HYSTERESIS (no flap): a periodic hosted re-eval, folded into the
existing `net_heartbeat_task` (usermain.c:287) or a sibling task created beside it
(usermain.c:370) — both arch twins. `RE_EVAL_PERIOD_S = 30`. When currently on
TCP, it keeps relay-UDP warm and probes it; it tracks `udp_stable_since` and
switches back to UDP ONLY after UDP has reported contact CONTINUOUSLY for
`UDP_RECOVER_K_S = 20 s` (the hysteresis — prefer-UDP-only-after-stable). A short
UDP blip does NOT flip the transport. On switch-back it repoints to UDP, closes
TCP, re-registers UDP, and logs. If UDP later drops while adopted, the same
bounded race re-runs and falls to TCP again.

OBSERVABLE LOG LINES (the auditor greps these; emitted by the selector via the
low-stack `write(2)` discipline already in net_relay.c, never glibc stdio on a
task stack):
- adopt UDP:  `[net] auto: adopted relay-udp (contact <ms>ms)`
- adopt TCP:  `[net] auto: relay-udp no contact (<win>ms) — adopted relay-tcp`
- switchback: `[net] auto: relay-udp recovered (stable <K>s) — switched from relay-tcp`
- no contact: `[net] auto: no relay contact (udp+tcp) — provisional relay-udp, no mesh`

### 4S.b CROWN VERDICT — PASS

FILES TOUCHED (ALL HOSTED-ONLY; never linked into a bare-metal image):
- `arch/linux/x86_64/net_dispatch.c` + `arch/linux/aarch64/net_dispatch.c`
  (the selector `xport_auto_init` + re-eval entry; lockstep twins).
- `arch/linux/x86_64/net_relay.c` + `arch/linux/aarch64/net_relay.c`
  (`relay_contacted` flag in `ha_mark_rx`, `net_relay_contacted()`,
  `net_relay_probe()`, `net_relay_close()`).
- `arch/linux/x86_64/net_relay_tcp.c` + `arch/linux/aarch64/net_relay_tcp.c`
  (`relay_contacted` flag at the recv pop, `net_relay_tcp_contacted()`,
  `net_relay_tcp_close()`, the `RACE_CONNECT_TMO_MS` race-connect cap).
- `arch/linux/x86_64/usermain.c` + `arch/linux/aarch64/usermain.c`
  (start/extend the re-eval task; register the `autoxport test` cert command).
- NEW `samples/11_distributed/run_relay_autofallback_live.sh` (live harness).
- NEW `tests/run_autofallback.sh` (in-proc cert harness, mirrors run_heartbeat.sh).

NOT TOUCHED: `relay/relay.c` needs NO change — it already serves UDP+TCP on one
port, echoes keepalives on both (relay.c:780-815, relay.c:792), and logs `(tcp)`
registrations (relay.c:393-395). The CROWN/SHARED TUs `arch/common/swim.c`,
`region.c`, `supernode.c`, `drpc.c` are untouched; `np_decide` is NOT needed for
the transport axis (it is a direct-P2P decider, deferred with rungs 1/2). No new
call into any shared TU is added.

BYTE-IDENTITY GATE: the bare-metal `.text` crowns
- aarch64 `755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513`
- x86     `4064d8a95e68950eee263a1bd6f131518f655f002bf2eccc1e824b4d87ee0413`
MUST stay byte-identical. GATE (auditor): recompute the sha256 over each
bare-metal image's `.text` BEFORE and AFTER the wave and assert equal to the two
crowns above. Because every touched TU lives under `arch/linux/` (hosted-only)
and no `arch/common/*` TU changes, the crown is unaffected by construction; the
hash recompute is the falsifiable proof.

### 4S.c FALSIFIABLE ACCEPTANCE GATE

Two parts. The IN-PROC cert is the hard, always-runnable gate; the LIVE cert is a
[live] row that runs where a net-namespace / NET_ADMIN substrate exists and SKIPs
(cleanly, exit 0) elsewhere — exactly as Slice 2/3's live rows defer.

#### (1) IN-PROC cert — `autoxport test` (mock network, NO sockets)

Mirrors `net_relay_heartbeat_self_test` / `seed_bootstrap_self_test`: a new
`void net_xport_select_self_test(void (*pr)(const char*))` under `-DAUTOXPORT_CERT`
drives the SHIPPED selector against a mock monotonic clock and a mock "network".
Mock seam (compiled out in production → `.text` unchanged): `xp_use_mock`,
`xp_now_ms`, `xp_udp_contact_at` (ms; -1 = never), `xp_tcp_contact_at` (ms from
TCP start; -1 = never), counter `xp_tcp_init_count`, outcome `xp_adopted`
(0=none / 3=udp / 4=tcp), `xp_adopt_ms`. The selector's calls to
`net_relay_probe/contacted/close` and `net_relay_tcp_init/contacted/close` divert
to the mock under `-DAUTOXPORT_CERT`.

- CASE A (UDP open): `xp_udp_contact_at=200`. ASSERT `xp_adopted==3` (UDP) AND
  `xp_tcp_init_count==0` (UDP contacted inside the 300 ms head start → relay-TCP
  is NEVER initialised → NO TCP connect emitted).
- CASE B (UDP blocked, TCP open): `xp_udp_contact_at=-1`,
  `xp_tcp_contact_at=400`. ASSERT `xp_tcp_init_count==1` (TCP started after the
  head start) AND `xp_adopted==4` AND `xp_adopt_ms <= 2500` (auto-adopts TCP
  within the bounded window).
- HYSTERESIS: start adopted on TCP (UDP blocked); flip `xp_udp_contact_at` so UDP
  becomes contactable. ASSERT a 5 s UDP blip does NOT switch back, and a switch
  back happens ONLY after ≥`UDP_RECOVER_K_S`(20 s) of continuous UDP contact (no
  flap).
- FALSIFIER (auto-fallback is load-bearing): rebuild with
  `-DAUTOXPORT_CERT -DAUTOXPORT_NOFALLBACK` (selector becomes UDP-only, never
  starts relay-TCP); re-run CASE B → `xp_adopted==0` (no mesh) → the harness emits
  `[autoxport] RESULT: FAIL`. Proves that without the auto-fallback the UDP-blocked
  net cannot join — i.e. the fallback, not anything else, carries it.

EXACT PASS/FAIL (auditor greps; `tests/run_autofallback.sh`, mirroring
`tests/run_heartbeat.sh`, on BOTH arches — native aarch64 + qemu-x86_64):
- CURE build (`-DAUTOXPORT_CERT`): `printf 'autoxport test\nexit\n' | p-kernel`
  output MUST contain `[autoxport] RESULT: <N>/<N> PASS` (all sub-asserts pass).
- FALSIFIER build (`-DAUTOXPORT_CERT -DAUTOXPORT_NOFALLBACK`): output MUST contain
  `[autoxport] RESULT: FAIL`.
- Harness exit 0 IFF (cure prints `RESULT: <N>/<N> PASS`) AND (falsifier prints
  `RESULT: FAIL`).

#### (2) LIVE cert — `samples/11_distributed/run_relay_autofallback_live.sh`

Mirrors `run_relay_tcp_live.sh`. Emulates a UDP-blocked net WITHOUT setting
`PKERNEL_RELAY_TCP`, using a private net namespace so it needs NO host privilege:
`unshare -rn` → `ip link set lo up` → run `./relay -p $PORT -v` (UDP+TCP on one
port) → `iptables -A INPUT -p udp --dport $PORT -j DROP` and the matching
`-A OUTPUT` (UDP blackholed to the relay; TCP intact). CAPABILITY PROBE FIRST:
`unshare -rn true` (or running as root); if it fails (e.g. this PRoot host, where
`unshare -rn` returns EINVAL) print `[autofallback-live] SKIP (no netns/NET_ADMIN
— in-proc cert is the gate)` and exit 0.

Boot TWO nodes: `PKERNEL_RELAY=127.0.0.1:$PORT`, `PKERNEL_RELAY_KEY=$KEY`,
`PKERNEL_AUTONET=1`, and NO `PKERNEL_RELAY_TCP` (auto). Settle, `nodes`, `exit`.

PASS (auto-fallback to TCP), EXACT predicates:
- `[ "$(grep -c 'registered.*(tcp)' relay.log)" -ge 2 ]`  (both registered over TCP)
- per node `[ "$(grep -c 'alive=2' nodeN.log)" -ge 1 ]`     (SWIM membership ≥2)
- per node `[ "$(grep -c 'auto: .* adopted relay-tcp' nodeN.log)" -ge 1 ]` (selector chose TCP)
- `udp_regs=$(( $(grep -c 'registered' relay.log) - $(grep -c 'registered.*(tcp)' relay.log) )); [ "$udp_regs" -eq 0 ]`  (NO node meshed over UDP — the block held)

TEETH 1 (block BOTH transports → no join): also `iptables -A OUTPUT -p tcp
--dport $PORT -j DROP`. ASSERT `[ "$(grep -c 'alive=2' nodeN.log)" -eq 0 ]` on
BOTH nodes AND `[ "$(grep -c 'registered' relay.log)" -eq 0 ]`.

TEETH 2 (auto DISABLED in the UDP-blocked netns → no join): same UDP-blocked
netns, boot with `PKERNEL_RELAY_AUTOFALLBACK=0` (force UDP-only). ASSERT
`[ "$(grep -c 'alive=2' nodeN.log)" -eq 0 ]` on both AND
`[ "$(grep -c '(tcp)' relay.log)" -eq 0 ]` — force-UDP cannot fall back, proving
the AUTO selection (not the relay merely being up) is what carries the join.

Harness exit 0 IFF (auto PASS, all four predicates) AND (TEETH 1 no-join) AND
(TEETH 2 no-join); OR a clean SKIP when the netns substrate is absent.

### 4S.d Honest bound (v1)

This v1 covers ONLY the relay-transport axis (rung 3 relay-UDP ↔ rung 4
relay-TCP) to the SAME configured endpoint. It does NOT implement direct-P2P
rungs 1/2 (LAN broadcast, N-3 cone-NAT punch via `np_decide`) and does NOT
implement the TLS/443 flavour of rung 4 — both remain later slices of the §4
ladder. It assumes ≥1 relay endpoint reachable on at least ONE transport; a net
that blocks both UDP AND outbound TCP egress leaves no channel (§5). Auto-
selecting across BOTH the multi-relay HA list AND the transport simultaneously is
deferred (the race uses the list head `cur_relay==0`; per-transport HA failover,
net_relay.c:499-528, is unchanged). See §5 for what stays unreachable.

---

## 6. Sequencing (what to BUILD first)

1. SLICE 1 — Unconditional heartbeat. FIRST. Smallest change, entirely in
   `net_relay.c` + hosted usermain, in-proc certifiable today, and it breaks the
   broad class of home/mobile deadlocks. Testable on the ThinkPad immediately and
   on the phone once it's on 5 GHz or pointed at a public relay.
2. SLICE 2 — Public relay. SECOND. Almost no new node code (config + a VPS-hosted
   `relay` that already binds `INADDR_ANY`); with Slice 1's heartbeat it directly
   fixes today's exact failure by routing the return path through the WAN NAT.
3. SLICE 3 — TCP fallback. THIRD. New hosted backend + relay TCP loop + length
   framing; needed only for UDP-totally-blocked nets. TLS-443 is a later
   sub-slice.
4. The §4 LADDER is the integration layer that sequences these AT RUNTIME; build
   its selector after Slice 2 (so there are ≥2 real rungs to race) and extend it
   when Slice 3 lands.

Recommended first slice: SLICE 1.
