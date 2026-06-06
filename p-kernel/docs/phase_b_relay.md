# Phase B — the UMP relay server

This is the design (and now a working prototype) of the tiny public relay
that lets two phones running the UMP Android app find each other and
talk, despite both being behind cellular NAT.

It is **not** STUN, **not** TURN, **not** a SIP signalling server, **not**
a coturn deployment. It is ~150 lines of C that listens on one UDP port,
maps `node_id → last-seen UDP address`, and forwards datagrams between
clients. It's the smallest thing that can make Phase B happen.

> Long-term, this is bootstrap-only. F-Droid + direct APK install
> ([[project-ump-android-node]]) means the relay is one node in an
> open federation, not a single point of control. The intent is the
> *relay protocol* survives even if no specific server does — anyone
> can run one.

## What the relay does, in one sentence

> Maintain a table of `{node_id → (peer_address, last_seen)}`; when a
> packet arrives, look up its `dst_node` and forward the payload there.

That's the entire feature set v1. Auth, encryption, multi-relay
federation, NAT hole-punching — all later.

## Why this is enough for Phase B

p-kernel already has `pmesh` packets carrying `src_node` + `dst_node` in
the header. On the current `arch/linux/aarch64/net_unix.c` two `./p-kernel`
processes find each other via fixed `127.0.0.1:29001/29002` UDP — that
works because they're on the same host. On real phones over cellular
networks the same packet format works, but neither phone can sendto the
other (NAT eats the outbound→inbound mapping for connections it didn't
see leave). What both phones CAN do is sendto a known public IP. So a
trivial forwarder on that public IP is all that's missing — Phase B
sub-step 1.

## Wire format

```
RelayPacket header (12 bytes, packed):

   0     1     2     3     4     5     6     7     8     9    10    11
+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+
|         magic = 0x52454C59 ("RELY")            | ver |type | src | dst |
+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+

  ver  = 1
  type = 0x01 REGISTER     // client announces its node_id
         0x02 DATA         // payload follows; relay forwards by dst
         0x03 KEEPALIVE    // refresh NAT mapping
         0x04 BROADCAST    // payload, forward to every known node
  src  = client's node_id (1..255; 0 reserved)
  dst  = REGISTER/KEEPALIVE: ignored
         DATA:              recipient node_id, 1..255
         BROADCAST:         0

Payload (DATA / BROADCAST only) follows the 12-byte header. Max
payload = 1380 bytes (one UDP datagram minus headers minus our wrapper),
matches PMESH_DATA_MAX so a pmesh packet can be encapsulated as-is.
```

The 12-byte header is the only thing the relay parses. Whatever's in
the payload is opaque — for Phase B it's a pmesh DATA packet, but it
could be anything.

## Wire format v2 (auth + replay protection)

v1 above is what the prototype actually shipped. v2 keeps the 12-byte
header layout but bumps `ver` to 2, and inserts a 24-byte AUTH block
between the header and the payload:

```
v2 packet:
  [ HEAD 12 ][ AUTH 24 ][ payload <=1380 ]

AUTH block (24 bytes):
   0     1     2     3     4     5     6     7
+-----+-----+-----+-----+-----+-----+-----+-----+
|              nonce (u64 LE, per-src monotonic)        |
+-----+-----+-----+-----+-----+-----+-----+-----+
|                hmac16 (HMAC-SHA256 truncated to 16 B) |
|                          (16 bytes total)             |
+-----+-----+-----+-----+-----+-----+-----+-----+
```

The MAC is computed as:

```
hmac16 = HMAC-SHA256(K, ver || type || src || dst ||
                        nonce(8 LE) || payload)[:16]
```

`magic` is intentionally NOT covered by the MAC: it's a self-describing
sync word, not a security-relevant field. Including it would just bloat
every MAC input by 4 bytes for zero benefit.

`payload` may be zero-length (REGISTER / KEEPALIVE), in which case the
MAC covers only the 12-byte preamble (ver..nonce).

### Replay protection

Per src node_id the relay keeps:

```
struct {
    uint64_t max_nonce;     // highest nonce ever accepted
    uint64_t window_bits;   // 64-bit bitmap; bit i = "nonce (max - i) seen"
} replay[NODE_MAX];
```

Rule:

- `nonce > max_nonce`:        accept, shift bitmap left by (nonce - max),
                              set bit 0, update max.
- `nonce == max_nonce`:       drop (already seen).
- `max - 64 < nonce < max`:   look at bit; drop if set, else set + accept.
- `nonce <= max - 64`:        drop (outside window; can't tell if replay).

Window size 64 is chosen so the replay state per node is one word, and
the window comfortably exceeds the realistic out-of-order budget over a
single UDP path (a few packets, never tens).

Replay state survives idle eviction from the routing table: only a
relay-process restart clears it. Clients therefore MUST keep nonces
monotonic across their own restarts — the recommended construction is
`nonce = (wall_clock_seconds << 24) | counter` so a freshly-started
client always exceeds the relay's stored `max_nonce`.

### Key distribution

The relay reads `PKERNEL_RELAY_KEY` from the environment — 64 hex chars
(= 32 bytes raw). Without it, the relay refuses to start:

```sh
$ ./relay
[relay] PKERNEL_RELAY_KEY not set — refusing to start (use --insecure
        for v1-compatible no-auth mode)
```

Pass `--insecure` to fall back to v1 behaviour (no MAC checked, no
replay state, ver=1 packets accepted). This is intended for local
loopback testing and the existing single-host pmesh demos. Production
deployments and any phone fleet MUST run with a key.

Key distribution to clients is out of band for v2 — QR code, manual
paste, F-Droid metadata pin, take your pick. v3 may add an in-band
"trust ratchet" but for now the threat model is "the user holds the
key for their own mesh."

### Client-side inbound verification (wave 10, gap G4)

The relay forwards DATA / BROADCAST frames **verbatim** — it does not
re-MAC them. That means an inbound frame at a client still carries the
*originator's* HMAC over the shared PSK, so the client can (and now does)
verify it. Previously `net_relay.c` trusted the relay blindly and only
checked the 12-byte structure; anyone able to send to a client's UDP
tuple from the relay's address could inject arbitrary frames straight
into the netstack. The client now recomputes the HMAC over
`(ver,type,src,dst,nonce,payload)` and drops mismatches *before* the
frame is used for either liveness (relay-HA failback) or data, logging a
rate-limited `[net_relay] mac drop n=<count>`.

Policy (client side), keyed off whether `PKERNEL_RELAY_KEY` is set:

- **No key (v1 wire):** nothing to verify; frames pass as before.
- **Key set (v2 wire), default permissive:** v2 frames are always
  verified and mismatches dropped; an unauthenticated *v1* frame is
  accepted once with a one-shot warning, easing a mixed new/old fleet
  during migration.
- **Key set + `PKERNEL_RELAY_STRICT=1`:** a v1 inbound frame is a hard
  drop too — no unauthenticated traffic enters the stack.

Regression test: `samples/11_distributed/run_relay_forgery.sh` stands up a
malicious relay that injects bad-MAC frames and asserts the node drops them.

### Client-side replay window (wave 11)

Wave 10's HMAC check closes forgery, but a *replay* — a legitimately
captured v2 frame resent verbatim — still carries a **valid** HMAC, so
authenticity verification alone re-admits it. The relay has its own
64-packet nonce window, but it lives in another process and protects a
different hop: an injector that reaches the client's UDP tuple from the
relay's address (or a buggy/compromised relay) bypasses it entirely.

So the client now keeps its **own** per-source 64-packet sliding nonce
window (`rx_nonce_max/win/armed[256]`, keyed by src node id, same logic as
the relay's `replay_check_and_update`). It is consulted *after* the HMAC is
verified and *after* control packets (keepalive echoes) are filtered out, so
control traffic never consumes a data nonce slot. A fresh nonce is **never**
dropped — legitimate traffic always passes — while a repeated or too-old
nonce is dropped and counted in a rate-limited `[net_relay] replay drop
n=<count>`. Counters are exposed via `net_relay_stats(ok,badmac,replay)` and
surfaced by the shell `rx` command's `[rx-relay] ok=.. badmac=.. replay=..`
line.

End to end the client now defends against **both** threats: forgery (bad
MAC → `mac drop`) *and* replay (valid MAC, repeated nonce → `replay drop`).
Regression test: `samples/11_distributed/run_replay_reject.sh` injects three
distinct valid frames (asserts all accepted, zero replay drops) then resends
two of them verbatim (asserts each dropped as a replay), so a false drop of
fresh traffic fails the test as loudly as a missed replay.

## State

Server-side, two maps:

```
node_table[node_id] = { ip, port, last_seen_seconds }
```

256 entries (one per possible node_id). Updated on every incoming
packet from a registered node. Entries older than `IDLE_TIMEOUT`
(default 300 s) are evicted on next access — the relay never holds
stale UDP mappings.

That's the entire state. No DB, no files. Restart = clean.

## Lifecycle

```
phone-A boots → relay := getenv("PKERNEL_RELAY_HOST")
              → if set: socket() + sendto(REGISTER, src=N, dst=0)
              → every 30 s: sendto(KEEPALIVE, src=N, dst=0)
phone-A pubs  → net_relay.c wraps the pmesh packet in
                RelayPacket{type=DATA, src=N, dst=M}
              → relay forwards to phone-B's last-seen addr
phone-B recv  → strips 12-byte header, hands payload to existing
                pmesh_rx (so the rest of the stack is identical to
                the local-loopback case)
```

## What the relay does NOT do (yet)

- **Authentication.** ~~v1 has none.~~ v2 adds HMAC-SHA256 keyed by a
  per-network secret in `PKERNEL_RELAY_KEY`. Distribution channel is
  still out of band (QR / paste / pin).
- **Replay protection.** ~~v1 packets can be replayed.~~ v2 enforces a
  64-packet sliding nonce window per src — **on both hops**: the relay on
  ingress, and (wave 11) the client on inbound, so a frame replayed straight
  at a client's UDP tuple is dropped even when the relay never saw it.
- **Encryption.** v2 still has none. The payload is integrity-protected
  but visible to anyone on the wire (and to the relay operator). v3
  layers AEAD on top — pmesh payloads then become opaque even to the
  relay.
- **DDoS protection.** Rate-limit per src IP, or per claimed node_id.
  v3.
- **Multi-relay.** If the single relay dies, the mesh dies. v3 adds
  a discovery protocol so phones learn alternate relays.
- **NAT hole-punching.** A proper STUN-style attempt would let two
  cooperating phones eventually talk directly, with the relay only
  needed for the introduction. The current design just forwards every
  packet forever; works, but uses the relay's bandwidth proportional
  to mesh traffic. v3+.

The non-features above are not security theater — they're the right
sequence to ship a v1 the user can hold and trust before adding
complexity.

## Deployment

The relay is a single static C binary. To run:

```sh
$ make -C relay
$ ./relay/relay [-p 7400] [-v]
[relay] listening on 0.0.0.0:7400
```

Suitable for any cheap VPS (Hetzner CX11, Lightsail nano, Oracle Always
Free ARM). One TCP/UDP port (default 7400). No filesystem dependencies.
No outbound network access required. ~5 MB residency, idle CPU.

Suggested systemd unit (omit if running ad-hoc):

```
[Unit]
Description=p-kernel UMP relay
After=network.target

[Service]
ExecStart=/opt/pkernel-relay/relay -p 7400
Restart=always
DynamicUser=true
ProtectSystem=strict
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

## Client integration (Phase B sub-step 2)

Currently `arch/linux/aarch64/net_unix.c` implements:

```
arch_linux_net_init()  → bind 127.0.0.1:(29000 + node_id)
arch_linux_net_send()  → sendto each other peer's port
arch_linux_net_recv()  → recv from own socket
```

The Phase B work-in-progress is `arch/linux/aarch64/net_relay.c`
(planned, not in this commit):

```
arch_linux_net_init()  → resolve PKERNEL_RELAY_HOST
                       → socket() to relay:RELAY_PORT
                       → sendto(REGISTER, src=node_id)
arch_linux_net_send()  → wrap pmesh packet in RelayPacket{
                            type=DATA, src=node_id, dst=N }
                       → sendto relay
arch_linux_net_recv()  → recv from relay, strip header, return payload
plus a periodic timer task sending KEEPALIVE every 30 s.
```

The `cmd_net()` path stays unchanged — same `rtl8139_init / drpc_init /
netstack_start / pmesh_init / kdds_init` sequence; only the underlying
transport switches to relay. Pickable via env var:

```
PKERNEL_RELAY_HOST=relay.example.com  # if set, use relay
                                       # else fall back to loopback
```

## Test plan for the relay alone

`relay/test_relay.c` simulates two clients:

1. Forks two children A (src=1) and B (src=2).
2. Each child sendto's REGISTER to localhost relay.
3. A sends DATA{src=1, dst=2, payload="hello from A"}.
4. B sends DATA{src=2, dst=1, payload="hello from B"}.
5. Each child reads from its socket; expects exactly the OTHER child's
   payload.
6. Parent waits, reports pass/fail.

The relay itself is the unit under test; p-kernel is not involved at
this stage. Run via:

```sh
$ make -C relay test
[relay-test] node 1 registered
[relay-test] node 2 registered
[relay-test] node 1 → 2: "hello from A"
[relay-test] node 2 → 1: "hello from B"
[relay-test] PASS — both payloads round-tripped through relay
```

Phase B sub-step 2 (`net_relay.c` on the p-kernel client side) lights
up only after this passes.

## Why C and not Go / Python

- Matches the rest of the repo. One toolchain, one calling convention.
- ~150 lines is small enough that the readability win of a higher-
  level language doesn't pay for itself.
- Static binary is trivial to deploy to ARM VPS instances ("Oracle
  Always Free Ampere") that match the long-term aarch64 phone fleet.
- No dependencies, no version churn, no Cargo.lock drift over months.

A Go relay would be lighter to write (~30 lines with the standard
library); we'd ship that too if someone prefers. The wire protocol is
the load-bearing piece, and that's plain bytes — any implementation
that gets the 12-byte header right interops.

## Open questions for v3

- IPv6 only? Dual-stack? (Lean: dual-stack, no opinion yet.)
- Multicast group support — could simulate K-DDS multicast across the
  relay so we don't have to broadcast to every node.
- Heartbeat → presence: the relay already knows last-seen-time, so we
  could expose a `who's online` query (now MAC-verified for free since
  v2's key already gates trust). Probably useful for UI ("3 other
  phones reachable").
- Payload encryption (AEAD) on top of v2's integrity — relay operator
  stops being inside the trust boundary.
- Per-src-IP rate limit for DDoS resistance.

---

Cross-links: [[project-ump-android-node]] (parent strategy),
[[moment-2026-05-22-cross-arch-kdds]] (the K-DDS work that this relay
extends from one-host to many-host),
[[project-pkernel-philosophy]] ("a home for AI no-one owns" — the
single-relay v1 violates this in spirit; F-Droid + bring-your-own-relay
restore it).
