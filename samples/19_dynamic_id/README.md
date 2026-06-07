# 19_dynamic_id — relay-side dynamic node-id lease

第11波 (G6 audit, first step). Verifies that the relay can **lease a
dynamic node id** to a client that does not have a human-assigned
`PKERNEL_NODE_ID`, without breaking the existing fixed-id path or the
v2 wire protocol.

Design rationale and the road to fully self-organising topology:
[`docs/architecture/dynamic-id.md`](../../docs/architecture/dynamic-id.md).

## What this tests

A Python pseudo-client speaks the v2 wire (HMAC-SHA256 + nonce) directly
to `./relay`. The kernel side (`net_relay.c`) is **not** wired up here —
that is a follow-up wave. This sample exercises only the relay's lease
behaviour:

- **Phase 1 (port 28400)** — unique lease + no collision with fixed ids:
  - a `REGISTER` with `src=0` ("auto") is answered with a grant carrying
    the smallest free id (`1`, then `2`);
  - a re-request from the same address is idempotent (same id back, no
    second id consumed);
  - a human-pinned `src=5` REGISTER is honoured, and subsequent autos
    skip it (`3`, `4`, then `6` — never `5`).
- **Phase 2 (port 28401, `PKERNEL_RELAY_IDLE=1`)** — reclaim + reuse:
  a leased id is freed by idle eviction and the next auto-request reuses
  that very id.

## Run

```sh
samples/19_dynamic_id/run_dynamic_id.sh
```

Prints `[dynamic-id] PASS` on success; any failed assertion exits
non-zero. The relay child is always killed by pid on exit.

## Wire compatibility

The lease request rides on the existing `REL_REGISTER` type using the
reserved-and-previously-dropped value `src=0`; the grant reply is a
`REGISTER` with `src=0, dst=<leased id>` (v2-signed so the client can
trust it). No new packet type, no header change — `make -C relay test`
stays 6/6 green. `PKERNEL_RELAY_IDLE` only overrides the idle window for
this test; the production default (300 s) is unchanged.

## Scope / not yet done

- Kernel client wiring (`net_relay.c` requesting/adopting a leased id) —
  next wave.
- Multi-relay lease arbitration — design only (see the doc); the relay
  implementation here assumes a single relay.
