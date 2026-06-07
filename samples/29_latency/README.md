# 29_latency — two-layer latency against the REAL relay (§8 / §10-B)

`run.sh` puts a **measured number** on the central physical claim of
`docs/architecture/survival-network.md` §8 (and fills 俯瞰監査 **G31**: §4's
core — latency / the light-speed wall — had never been measured):

> 第1層(反射層 / ローカル): 即時の反射は光速で間に合う範囲の近傍ノードが担う。今を守る。
> 第2層(熟慮層 / グローバル): 時間をかけてよい深い判断は宇宙全体が担う。未来を強くする。
> **遠方ノードの遅延が、反射層の即応性を損なってはならない。**

§10 ステップB asks exactly this: *introduce distance-proportional delay; confirm
the far layer's latency does not degrade the near (reflex) layer's immediacy.*

## What it does

Starts the **real `./relay`** with its new distance→delay knob on:

| env | value | meaning |
|-----|-------|---------|
| `RELAY_FAR_NODES`    | `3`   | only forwards **to node 3** (= far / deliberation) are delayed |
| `RELAY_FAR_DELAY_MS` | `300` | how long node-3-bound packets are held (light-speed proxy) |
| (near nodes)         | `0`   | node 2 (reflex peer) is forwarded immediately |

`latency_client` holds three sockets in one process — ship (node 1), near peer
(node 2), far peer (node 3) — and speaks the same v2 wire as the kernel's
`net_relay.c`. The peers echo every probe straight back, so each probe is a real
UDP round-trip **ship → peer → ship through the real relay**.

The decisive test is **non-interference**: fire one far probe, then — while it is
still sitting in the relay's delay queue — fire a burst of near probes every
20 ms. If the two layers are truly separated, every near probe round-trips in
~1 ms and returns *before* the single far probe. If the relay head-of-line
blocked the near layer behind the far one, the near probes would stall for the
full far delay. The harness asserts they do not.

## Run

```sh
./run.sh                       # 3 runs, default far_delay=300ms
FAR_DELAY_MS=600 REPEAT=5 ./run.sh
```

Each run prints a `RESULT:` line and the script ends with a `summary`. Exit code
= number of failed runs (0 = all green).

## Representative result (3/3 PASS, this host)

```
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=301.0ms noninterference=yes separation=301x
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=300.0ms noninterference=yes separation=300x
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=301.0ms noninterference=yes separation=301x
```

The near (reflex) round-trip stays at **~1 ms** — far under both the 50 ms reflex
budget and the 1000 ms debris deadline — while the far (deliberation) probe
arrives at **~300 ms**, exactly the injected delay. The near layer's immediacy is
**unaffected** by the far latency (last near echo at ~+220 ms, far echo at
~+301 ms): the two time constants stay ~300× apart. That is §8's whole point,
measured.

## Modelled vs measured (honest)

- **Measured**: real UDP socket round-trip latency through the real `./relay`,
  and that injecting far-node delay does **not** raise the near round-trip.
- **Modelled**: the *value* `far_delay=300ms` as a stand-in for the light-speed
  wall. Real Earth–Mars one-way light delay is ~3–22 min; 300 ms is that wall
  *compressed* to a localhost-observable scale. No real hardware, no real radio,
  no real distance. The light-speed → delay mapping itself is explored in
  `tools/sim/latency_twolayer_sim.py` and written up in
  `docs/benchmarks/latency.md`.

See [docs/benchmarks/latency.md](../../docs/benchmarks/latency.md) for the full
analysis and the companion host simulation.
