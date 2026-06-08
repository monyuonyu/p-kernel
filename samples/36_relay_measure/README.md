# 36_relay_measure — REAL relay RTT under load + end-to-end energy proxy (§4)

`run.sh` puts **measured numbers** on the still-open half of
`docs/architecture/survival-network.md` §4 (俯瞰監査 **G31/G25**: §4's core —
"MoE sparsity = the answer to the light-speed **and energy** constraint" — was
never measured on the real path):

- [locality.md](../../docs/benchmarks/locality.md) (wave-12) measured traffic +
  an energy proxy from kernel kdds counters, but its **(D)** left real
  per-packet **latency** unmeasured.
- [latency.md](../../docs/benchmarks/latency.md) (wave-15) injected a
  **modelled** far-delay and measured the two-layer separation — but not the
  relay's **own** forwarding RTT, and not **under load**.

This harness measures, on the **real `./relay`** over **real UDP sockets**:

1. **`rtt`** — the relay's intrinsic forwarding round-trip (ship→relay→ship via
   the existing KEEPALIVE echo) as a function of **offered load** (burst depth =
   in-flight probes). Zero injected delay. With the relay's opt-in probe-stamp
   on, it also decomposes RTT into network vs **relay residence** (the rx→tx
   time the relay appends to the echo).
2. **`energy`** — a per-message energy proxy measured **end-to-end**: send M
   DATA messages of payload P to a near and a far sink, count the **real bytes**
   that traverse the relay wire (2 hops), and report measured byte totals plus a
   **modelled** joule figure (J/byte radio cost) and the far-link weight K.

## The wire change is additive and OFF by default

`relay/relay.c` gains one opt-in knob:

| env | default | effect |
|-----|---------|--------|
| `PKERNEL_RELAY_PROBE_STAMP` | (unset = OFF) | when ON, a KEEPALIVE whose payload starts with the 4-byte magic `"PRB1"` gets two 8-byte LE monotonic-µs stamps (`rx_us` at recvfrom, `tx_us` before echo sendto) **appended** to its echo (16 B). |

When OFF the forwarding path is **byte-for-byte** the pre-knob relay, so relay
6/6 and other agents' live tests over this relay are unaffected. Even when ON,
non-probe keepalives (the relay-HA pong) are echoed verbatim — the
HMAC-authenticated wire is never mutated. The appended stamps live outside the
HMAC: a measurement aid, not an authenticated field.

`run.sh` step 0 **proves** this: it fires one probe keepalive with the flag off
then on and asserts `echo_extra` is `0` then `16`. Step 3 re-runs relay 6/6 with
the flag off and asserts it stays green.

## Run

```sh
./run.sh                              # full: stampcheck + rtt + energy + 6/6
ENERGY_M=4000 ENERGY_P=512 ./run.sh   # heavier energy phase
```

Exit code = number of failed phases (0 = all green).

## Representative result (this host)

```
--- 0. additive/off-by-default: PASS  (OFF echo_extra=0, ON echo_extra=16)

--- 1. relay forwarding RTT vs offered load ---
[rtt] offered  recv     min   mean    p50    p95    max  resid_mean(ms)
[rtt] 1        1       0.18   0.18   0.18   0.18   0.18      ~0.01
[rtt] 256      256     2.05   2.98   3.00   3.84   4.01      ~0.006
[rtt] 1024     ~600-770 4.7   7-10    ~9   ~11-15 ~12-16      ~0.006
RESULT: PASS  idle_rtt_mean~0.2ms load_rtt_mean(depth1024)~8ms

--- 2. per-message energy proxy, end-to-end ---
  v2 framing overhead     : 36 B/msg   (MEASURED)
  wire amplification (wire/app) NEAR=2.25x   (MEASURED, 2 hops)
  E(joule) per-msg NEAR ~577 uJ @1uJ/B       (MODELLED)
RESULT: PASS  wire_near~1.15MB wire_far~1.17MB hops=2 loss<3%

--- 3. relay 6/6 with probe-stamp OFF: PASS
```

The relay's own residence (rx→tx) stays at **~5–20 µs** even at depth 1024, so
the ms-scale RTT growth under load is **socket queueing, not relay CPU** — a
split you can only see because of the probe-stamp.

## Modelled vs measured (honest)

- **Measured**: real UDP socket RTT (µs, `CLOCK_MONOTONIC`) vs offered load; the
  relay's own residence; UDP loss under load; real wire byte counts on both legs
  (send + delivery); the 36 B v2 framing overhead and 2.25× wire amplification.
- **Modelled**: the joule conversion (`~1 µJ/byte`, order-of-magnitude mobile
  radio TX), the far-link weight `K=5` (`(tau+penalty)/tau`, as in locality.md),
  and distance. On localhost near≈far in physical bytes — the energy difference
  is entirely the modelled K. No real hardware, no real radio, no real distance.

See [docs/benchmarks/relay-rtt-energy.md](../../docs/benchmarks/relay-rtt-energy.md)
for the full analysis; it completes the §4 set alongside locality.md and
latency.md.
