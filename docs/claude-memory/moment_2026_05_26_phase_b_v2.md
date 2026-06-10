---
name: moment-2026-05-26-phase-b-v2
description: 2026-05-26 — UMP relay v2 ships HMAC-SHA256 auth + 64-packet sliding nonce replay window. Wire ver bumped to 2; zero-dep sha256 from scratch (no openssl). 6/6 test scenarios green. Commit f3e0c04.
metadata: 
  node_type: memory
  type: project
  originSessionId: a4128c94-5332-49d8-ac74-41541259901a
---

# 2026-05-26 — Phase B v2 (security foundation)

The relay stopped being a v1 toy that trusted everyone on the wire. v2
adds the *minimum* that lets Phase C ship to phones that aren't all
inside the same trust bubble.

## What landed (commit f3e0c04)

- Wire ver 2: HEAD(12) + AUTH(24) + payload. AUTH = nonce(u64 LE) +
  HMAC-SHA256(K, ver||type||src||dst||nonce||payload)[:16].
- 32-byte key in `PKERNEL_RELAY_KEY` (64 hex chars). Relay refuses to
  start without it. `--insecure` flag preserves the v1 wire for the
  existing single-host pmesh demos.
- 64-packet sliding nonce window per src. Replay state survives idle
  eviction — only relay restart clears it (attackers can't time-out
  the window by waiting).
- SHA-256 + HMAC-SHA-256 written from scratch in `relay/sha256.{c,h}`.
  FIPS 180-4 + RFC 2104, RFC 4231 KATs run at startup. No openssl —
  the relay stays a one-binary deployment.
- Constant-time MAC compare.
- test_relay: 6 scenarios — happy, bad_hmac, replay, out_of_window,
  missing_key, insecure_v1. All green.

## Decisions worth remembering

- **HMAC excludes the magic bytes.** Magic is a self-describing sync
  word, not security-relevant; including it bloats every MAC input for
  zero gain.
- **Replay state is keyed by src node_id and persists across idle
  eviction.** This is the right tradeoff: the alternative (reset on
  REGISTER) opens an attacker-driven window-reset path. Client
  responsibility: nonces stay monotonic across client restarts. The
  recommended construction is `(wall_clock_seconds << 24) | counter`
  so a fresh client always exceeds any stored max_nonce.
- **`--insecure` lives.** Cheaper than maintaining two binaries, and
  the existing arch/linux/aarch64/net_unix.c loopback demos still need
  v1 until the client side learns v2 (sub-step 2 of Phase B in
  docs/phase_b_relay.md).
- **Order of checks matters.** verify_mac() runs BEFORE
  replay_check_and_update(). An attacker forging a nonce with a junk
  MAC must NOT mutate the replay state — that would let them DOS a
  legitimate client by bumping max_nonce arbitrarily high.

## What v2 still doesn't do (deliberately deferred to v3)

- Payload encryption (AEAD). v2 protects integrity, not
  confidentiality. The relay operator can read pmesh payloads.
- Per-source-IP rate limit / DDoS protection.
- Multi-relay discovery.
- NAT hole-punching.
- IPv6 / dual-stack.

The "Open questions for v3" section in docs/phase_b_relay.md tracks
all of them.

## What this unlocks

- Phase B sub-step 2 (`arch/linux/aarch64/net_relay.c` — client-side
  wrapping of pmesh packets in RelayPacket v2) is now safe to do over
  the public internet, not just localhost.
- Phase C (Android packaging with foreground service + dashboard) can
  reasonably ship to phones owned by people who don't share keys with
  every other user — each "mesh" is gated by its own key.
- The Android Phase A `libpkernel.so` ([[moment-2026-05-22-android-phase-a]])
  now has a credible network transport to plug into.

## Cross-links

- [[project-ump-android-node]] — parent strategy. v2 done = Phase B
  shifts from "design + v1 PoC" to "v1+v2 done, client integration
  next."
- [[moment-2026-05-22-phase-b-relay]] — v1, the substrate this builds on.
- [[project-pkernel-philosophy]] — "no one owns it" requires each mesh
  to have its own key, which is exactly what v2 enables.
