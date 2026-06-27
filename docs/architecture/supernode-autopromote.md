# Measured-Capability Supernode Auto-Promotion (N-2d)

Status: SHIPPED + AUDITED + integrated to master (23811db7), 2026-06-27.
design->implement->audit by SEPARATE agents; auditor PASS (both crowns
byte-identical, in-proc cert 6/6 both arches with a demonstrated toothful
falsifier, REFL1 trailer parse audited with no hole + strictly post-auth capture,
no relay/slice-3/slice-4 regression). OPERATIONAL FOLLOW-UP: redeploy the public
relay (relay.helloidea.org) with the REFL1 echo so production nodes can actually
measure reachability. Author: design pass for mk_pino's 2026-06-27 request —
"Skype-style dynamic supernodes." Scope: the **supernode** capability
bit only (the teacher bit is explicitly out — §C.6).

THE GOAL (mk_pino): today a node only becomes supernode-CAPABLE by an explicit
opt-in (`PKERNEL_SUPERNODE=1`). He wants good supernodes to be **born from measured
fitness**: a node AUTO-PROMOTES its own capability bit when it MEASURES that it is
actually a good supernode (reachable / public-ish / stable), and AUTO-DEMOTES when
it isn't. The election, succession, capability gossip, and N-2c forwarding already
work and are NOT changed. We add only the AUTO-SETTING of `super_capable[self]`.

---

## A. Background — what already works and is NOT changed

The whole election + succession + gossip + forwarding machinery is finished and
must stay byte-identical (it lives in CROWN/shared TUs). We quote it so the
boundary is unambiguous.

### A.1 The election (pure, NOCENTRAL, survives death by recomputation)

`arch/common/region.c`:

- `supernode_select()` (region.c:143-149) — the pure core: lowest id that is BOTH
  a region member AND `super_capable[]`:
  ```
  static UB supernode_select(const UB *mbr, const UB *cap) {
      for (UB n = 0; n < DNODE_MAX; n++)
          if (mbr[n] && cap[n]) return n;   /* lowest id wins */
      return 0xFF;                          /* no capable member -> relay */
  }
  ```
- `region_supernode()` (region.c:154-159) — recomputes membership, then
  `supernode_select(member, super_capable)`. `0xFF` = no capable member → fall
  back to the central relay (the correct Skype-style graceful degrade).

This is a deterministic pure function of (membership, capability): every node
computes the same supernode with NO vote, and it survives the supernode's death
by recomputation. **We do not touch any of this.** Auto-promotion changes only
*which nodes carry the `super_capable[self]` bit*, which is exactly the input this
selector already consumes.

### A.2 The capability gossip (self-authoritative; the bit spreads epidemically)

`arch/common/swim.c`:

- `cap_self()` (swim.c:113-117) — self-authoritative source of the supernode bit:
  ```
  static UB cap_self(void) {
      return (drpc_my_node != 0xFF && region_is_super_capable(drpc_my_node)) ? 1 : 0;
  }
  ```
  i.e. the gossiped self-bit is read **directly from `region_is_super_capable(self)`**.
- `cap_self_byte()` (swim.c:160-163) packs bit0=`cap_self()`, bit1=teacher.
- `swim_task` (swim.c:634-639) seeds/refreshes the self-beacon every round with
  `gossip_add(drpc_my_node, DNODE_ALIVE, my_incarnation, cap_self_byte())` and
  `SELF_APPLY_OWN_CAPABILITY()`.
- `gossip_apply` (swim.c:280-281, 337, 360-361) sets `super_capable[peer]` from the
  gossiped bit VERBATIM under the (incarnation,state) anti-stale LWW gate.

So the SELF bit, once present in `super_capable[self]`, is gossiped by the existing
beacon and picked up by every peer's existing `gossip_apply` + election. **We do
not touch any of this either.** The N-2c forwarding plane (`supernode.c`
`snf_send`/`snf_route_target` + the runtime DEAD fail-closed at supernode.c:184)
and the N-3 rendezvous (`np_classify`/`np_decide`, supernode.c:757-780) are also
unchanged and are reused read-only.

### A.3 The thing we ARE replacing

`region_super_init()` (region.c:126-138) is today the ONLY producer of
`super_capable[self]`:
```
void region_super_init(void) {
    if (super_init_done) return;
    super_init_done = TRUE;
#ifdef _TK_HOSTED_LIBC_
    const char *e = getenv("PKERNEL_SUPERNODE");
    if (e && e[0] == '1' && drpc_my_node != 0xFF)
        super_capable[drpc_my_node] = 1;
#endif
}
```
It is a one-shot, env-only DECLARE. Auto-promotion adds a hosted, continuous,
MEASURED setter alongside it. Note `region_super_init` (a) is idempotent
(`super_init_done` guard), (b) only ever SETS the bit to 1 and only when the env
is `1`, and (c) never clears it — so for an env-unset (AUTO) node it is a no-op on
the table, and it cannot fight the auto-setter. This is load-bearing for §C.5.

---

## B. The hosted seam — where the evaluator lives and why CROWN is untouched

### B.1 The seam, proven

The auto-promotion DECISION and ALL measurement live in HOSTED-ONLY TUs
(`arch/linux/<arch>/*.c`, never linked into a bare-metal image). The evaluator's
ONLY write into shared state is the EXISTING public setter:

```
void region_set_super_capable(UB node, BOOL yes);   /* region.c:112, declared region.h:66 */
```

called as `region_set_super_capable(drpc_my_node, TRUE/FALSE)`. The full data path,
every hop of which is EXISTING code, with ZERO change to any CROWN `.text`:

```
[HOSTED evaluator]  region_set_super_capable(self, TRUE)
        |                                     (region.c setter — already shipped)
        v
   super_capable[self] = 1
        |
        v
[CROWN, unchanged]  cap_self()  reads region_is_super_capable(self)   (swim.c:113)
        |
        v
   swim_task beacon  gossip_add(self, ALIVE, inc, cap_self_byte())    (swim.c:638)
        |
        v
[peers, CROWN unchanged]  gossip_apply -> super_capable[self]=1       (swim.c:360)
        |
        v
   region_supernode() / supernode_select() elects self                (region.c:154)
```

The setter is read-write from hosted code and is ALREADY called from hosted-reached
paths today (the swim self-apply macro). `region_is_super_capable` / `np_decide`-
class deciders are read-only. We add NO new symbol to, and change NO line of,
`region.c` / `swim.c` / `supernode.c` / `drpc.c`. Therefore the bare-metal `.text`
is byte-identical **by construction** (§D).

### B.2 Seam findings (honest — two places the brief's assumption needs a shim)

1. **The setter seam exists and is sufficient.** `region_set_super_capable` is
   public (region.h:66), the beacon reads `region_is_super_capable(self)` every
   round (swim.c:113,638), and `region_super_init` cannot fight it (B.A.3). The
   hosted evaluator therefore promotes/demotes purely by calling the setter. No
   CROWN edit, no new CROWN symbol. CONFIRMED.

2. **`np_classify`/`np_decide` are `static` in `supernode.c` (CROWN) — NOT
   callable from hosted.** The brief lists them as "existing read-only deciders"
   the evaluator may call; in fact they are file-static (supernode.c:757, 775) and
   not exported in `supernode.h`. Exporting them would add a symbol to a CROWN TU
   and move `.text` — FORBIDDEN. Resolution: the evaluator does NOT call them. The
   CONE-vs-SYMMETRIC rule is one line (`m1.port == m2.port`, supernode.c:768); the
   hosted evaluator RE-IMPLEMENTS that exact predicate over hosted-observed
   reflexive endpoints (§C.1), and the cert pins it byte-equivalent to np_classify's
   rule. This is the minimal-shim path the brief mandates when a seam is absent.

3. **No reflexive ("what the relay sees as my source IP:port") signal exists in
   hosted code today.** `np_self_mappings` (supernode.c:873-878) SYNTHESIZES a cone
   mapping in-proc — it is a stub, not a real STUN echo — and the relay
   (`relay/relay.c`) echoes keepalives VERBATIM (relay.c:792-815) without reporting
   the observed source address back. So a true reachability measurement needs a
   minimal HOSTED shim (relay is a host binary; net_relay.c is hosted — both
   crown-safe): a reflexive-address echo (§C.1.a). Until that ships, the evaluator
   uses only the signals that exist today and stays fail-closed (§C.4): absence of
   positive reachability evidence is NOT promotion.

---

## C. The measured-fitness function

A new HOSTED file `arch/linux/<arch>/supernode_autopromote.c` (lockstep twins)
holds a PURE fitness core + a thin live wrapper. The wrapper is folded into the
existing `net_heartbeat_task` (usermain.c:295-306, 5 s cadence) — the SAME hosted
seam Slice 4's re-eval uses (`net_xport_reeval`, usermain.c:304). The pure core is
what the cert (§E.1) drives.

### C.1 The signals (each from a concrete source)

Gathered once per evaluator tick into a value struct `SAP_SIGNALS`:

| Signal | Meaning | Concrete hosted source |
|--------|---------|------------------------|
| `relay_contacted` | the relay answered us → a working bidirectional path exists | `net_relay_contacted()` (net_relay.c:924; flag set in `ha_mark_rx`, net_relay.c:545) |
| `refl_count` | how many distinct vantage points have reported our external mapping | NEW hosted `net_relay_reflexive_count()` (§C.1.a) |
| `refl_same_port` | the external PORT is identical from ≥2 vantage points → endpoint-INDEPENDENT (CONE) | NEW hosted `net_relay_reflexive_classify()` reimplementing `m1.port==m2.port` (supernode.c:768) |
| `refl_is_public` | external IP == our local bind IP → NO NAT (DIRECT PUBLIC, strongest) | compare reflexive IP to `net_my_ip` (netstack.c:24) |
| `now_s` | hosted monotonic seconds (uptime / dwell) | `HB_NOW()` idiom (net_relay.c:155 = `time(NULL)`, mock-divertible) |
| `metered` | on a metered/cellular link → never promote | `getenv("PKERNEL_METERED")=="1"` (NEW hosted env; Android sets it on cellular) |
| `stress` | interoception pressure 0..255 → high stress = don't volunteer | `intero_scalar()` (interocept.h:46; CROWN read-only) |
| `degrade` | cluster degrade level (FULL=0..SOLO=max) | `degrade_level()` (degrade.h:57; CROWN read-only) |

(a) **Reflexive echo shim (the only new wire, all hosted/host-binary):**
- `relay/relay.c` (host binary — crown-safe): on a non-probe `REL_KEEPALIVE`,
  append the observed source `ip:port` to the echo, behind a new `REFL1` payload
  magic (reusing the EXISTING probe-stamp append mechanism, relay.c:792-815, so the
  default byte-for-byte pong is unchanged when the magic is absent).
- `arch/linux/<arch>/net_relay.c` (hosted): record `reflexive_ip[idx]`,
  `reflexive_port[idx]` per configured relay `idx` on echo receipt; expose
  `net_relay_reflexive_count()`, `net_relay_reflexive_classify()`
  (CONE/SYMMETRIC/UNKNOWN from comparing the ports across the (up to 4)
  `PKERNEL_RELAY` HA-list vantage points), `net_relay_reflexive_public(void)`
  (reflexive IP == `net_my_ip`).
- A node with ONE relay can observe a reflexive mapping (→ `refl_is_public` works,
  and CONE-vs-SYMMETRIC needs TWO vantage points → returns UNKNOWN with one, which
  fails closed per §C.4). A 2+-entry `PKERNEL_RELAY` list yields a real
  CONE/SYMMETRIC verdict, EXACTLY as `np_classify` does from two echoes.

### C.2 The fitness verdict (pure, integer-only, deterministic)

```
reachable_good(s) :=
        s.relay_contacted == 1
   AND  ( s.refl_is_public == 1                     /* DIRECT PUBLIC, strongest   */
          OR s.refl_classify == CONE )              /* endpoint-independent NAT   */
   AND  s.refl_classify != SYMMETRIC                /* HARD BLOCK (§C.3)          */

fitness_good(s) :=
        reachable_good(s)
   AND  s.metered == 0                              /* never a phone on cellular  */
   AND  s.stress  <  SAP_STRESS_MAX                 /* 200/255                    */
   AND  s.degrade <  SAP_DEGRADE_MAX                /* don't volunteer while SOLO */
```

`fitness_good` is a pure function of `SAP_SIGNALS`. UNKNOWN reachability (only one
vantage point, or no reflexive echo yet) makes both clauses of `reachable_good`
false → `fitness_good` false → **no promotion** (fail-closed; this is why the
missing-shim period in §B.2.3 is safe).

### C.3 The symmetric-NAT HARD BLOCK

`s.refl_classify == SYMMETRIC` forces `reachable_good` false REGARDLESS of every
other signal. A symmetric-NAT node CANNOT serve as a supernode (its hole is
per-destination, unpunchable — supernode.c:768 rationale), so it must never carry
the bit. This is an independent gate, not subject to the promote dwell: the dwell
counter (§C.4) can never accumulate while symmetric. The cert's TOOTHFUL FALSIFIER
(§E.1) removes exactly this clause and proves a symmetric node then wrongly
promotes → cert FAILS, proving the block is load-bearing.

### C.4 Anti-flap hysteresis (the Slice-4 lesson)

A node must not oscillate the supernode role — that thrashes the whole region's
routing for every member. Two asymmetric dwell windows, evaluated on the 5 s tick:

- `SAP_PROMOTE_K_S = 60` — promote ONLY after `fitness_good` holds CONTINUOUSLY for
  60 s (earn it over a full minute; 12 consecutive good ticks). Any bad tick resets
  the good-dwell to 0.
- `SAP_DEMOTE_K_S = 30` — demote ONLY after `fitness_good` has been FALSE
  continuously for 30 s (6 consecutive bad ticks). Any good tick resets the
  bad-dwell to 0.

The gap (60 vs 30) is the hysteresis band: a brief good blip (<60 s) never promotes;
a brief bad blip (<30 s) never demotes. Demote is faster than promote so a node that
genuinely loses reachability stops advertising within 30 s, while the runtime
fail-closed (§C.5) covers the interim. State (the two dwell timers + current
adopted bit) is hosted-only static in `supernode_autopromote.c`.

Pure-core signature (what the cert drives):
```
/* returns SAP_PROMOTE / SAP_DEMOTE / SAP_HOLD; mutates *st (dwell timers). */
int sap_step(SAP_STATE *st, const SAP_SIGNALS *s, long now_s);
```

### C.5 Precedence vs the existing opt-in, and fail-closed safety

- `PKERNEL_SUPERNODE=1` (manual FORCE) remains ALWAYS-capable. The evaluator reads
  the env once; if forced, it (a) never calls `region_set_super_capable(self,FALSE)`
  and (b) skips the fitness logic entirely. `region_super_init` sets the bit to 1
  once (region.c:134); the evaluator leaves it. Precedence: **FORCE > measured**.
- Env unset → AUTO is the new DEFAULT for hosted nodes. `region_super_init` is a
  no-op on the table (env unset), so the evaluator is the sole producer; no fight.
- Bare-metal nodes (no env, no hosted net, no evaluator linked) keep TODAY's
  behaviour by construction: `super_capable[self]` stays 0 unless set
  programmatically. Unchanged.
- **A bad auto-promotion degrades, never breaks (fail-closed):** even if a buggy
  node wrongly promotes, (1) `supernode_select` still picks the LOWEST-id capable
  member, bounding the blast radius to one id; (2) the runtime fail-closed in
  `snf_send` (supernode.c:184: an elected-but-DEAD supernode → DIRECT) and the N-3
  broker guard (`region_supernode()==self`, supernode.c:948) mean an elected-but-
  unreachable supernode degrades traffic to DIRECT/relay, never drops it; (3) the
  30 s demote dwell retracts the bit. The worst case is a brief sub-optimal route,
  identical to today's manual-misconfiguration case.

### C.6 Teacher bit is OUT of scope

Auto-promotion applies ONLY to the supernode bit (`super_capable[]`). The teacher
bit (`teacher_capable[]`) is gated on a VERIFIABLE loaded GGUF — `teacher_self()`
(swim.c:148-154) is true iff `teacher_gguf_loaded()`, a content-based truth, NOT a
measurable network property. Keep it exactly as-is. The evaluator NEVER calls
`region_set_teacher_capable`. Stated bound.

---

## D. CROWN verdict — PASS

FILES TOUCHED (all HOSTED-only or the host-binary relay; never linked bare-metal):
- NEW `arch/linux/x86_64/supernode_autopromote.c` + `arch/linux/aarch64/supernode_autopromote.c`
  (the pure `sap_step` fitness core + live wrapper + the `-DSAP_CERT` in-proc cert;
  lockstep twins).
- `arch/linux/x86_64/usermain.c` + `arch/linux/aarch64/usermain.c`
  (call the wrapper from `net_heartbeat_task`; register the `autopromote test` cert
  command; read `PKERNEL_SUPERNODE`/`PKERNEL_METERED`).
- `arch/linux/x86_64/net_relay.c` + `arch/linux/aarch64/net_relay.c`
  (reflexive-echo capture + `net_relay_reflexive_count/classify/public`).
- `relay/relay.c` (host binary): the `REFL1` reflexive-address echo append.
- NEW `tests/run_autopromote.sh` (in-proc cert harness, mirrors `run_autofallback.sh`).
- NEW `samples/11_distributed/run_supernode_autopromote_live.sh` (live harness,
  SKIP-clean where the netns substrate is absent).

NOT TOUCHED: the CROWN/SHARED TUs `arch/common/region.c`, `swim.c`, `supernode.c`,
`drpc.c`, `interocept.c`, `degrade.c`. The evaluator only CALLS
`region_set_super_capable` (read-write), `region_is_super_capable`, `intero_scalar`,
`degrade_level` (read-only) — all pre-existing public symbols. No new symbol is
added to, and no line changes in, any `arch/common/*` TU.

BYTE-IDENTITY GATE (auditor): recompute sha256 over each bare-metal image's `.text`
BEFORE and AFTER the wave; assert equal to the two crowns:
- aarch64 `755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513`
- x86     `4064d8a95e68950eee263a1bd6f131518f655f002bf2eccc1e824b4d87ee0413`

Because every touched node TU lives under `arch/linux/` (hosted-only) and no
`arch/common/*` TU changes, the crown is unaffected by construction; the hash
recompute is the falsifiable proof. (`relay/relay.c` is a standalone host binary
and never contributes to the bare-metal `.text`.)

---

## E. Falsifiable acceptance gate

Two parts. The IN-PROC cert is the hard, always-runnable gate; the LIVE cert is a
`[live]` row that SKIPs cleanly where the netns/NET_ADMIN substrate is absent (this
PRoot host lacks `unshare -rn`), exactly as connect-anywhere Slices 2-4 defer.

### E.1 IN-PROC cert — `autopromote test` (mock signals, NO sockets)

Mirrors `net_relay_heartbeat_self_test` / `net_xport_select_self_test`: a new
`void sap_self_test(void (*pr)(const char*))` under `-DSAP_CERT` drives the SHIPPED
pure core `sap_step()` against a mock clock and a mock `SAP_SIGNALS`, then asserts
the REAL setter effect by reading `region_is_super_capable(self)` after the live
wrapper applies the verdict (drpc_my_node impersonated, saved/restored — the same
scaffold swim.c's self-tests use). Mock seam compiled out in production →
`.text` unchanged: `sap_use_mock`, `sap_now_s`, and a mock `SAP_SIGNALS`.

- **(A) CONE/public + stable → promotes within the bounded window.**
  Feed `relay_contacted=1, refl_classify=CONE (or refl_is_public=1), metered=0,
  stress<200, degrade<max` continuously; advance the mock clock.
  ASSERT: at `t < SAP_PROMOTE_K_S` the verdict is HOLD and `super_capable[self]==0`;
  at `t >= SAP_PROMOTE_K_S` the verdict is PROMOTE and `region_is_super_capable(self)==TRUE`.
- **(B) SYMMETRIC-NAT node NEVER promotes.**
  Feed `relay_contacted=1, refl_classify=SYMMETRIC` (everything else perfect) for
  10x `SAP_PROMOTE_K_S`. ASSERT verdict stays HOLD and `super_capable[self]==0`
  for the entire run.
- **(C) Hysteresis.** (c1) Start un-promoted; feed a `< SAP_PROMOTE_K_S` good blip
  then bad → ASSERT never promoted. (c2) Start promoted; feed a `< SAP_DEMOTE_K_S`
  bad blip then good → ASSERT never demoted (`super_capable[self]` stays 1).
- **(D) Env force.** With `PKERNEL_SUPERNODE=1`, feed all-bad signals
  (`relay_contacted=0, refl_classify=SYMMETRIC, metered=1`) → ASSERT
  `region_is_super_capable(self)==TRUE` throughout and the evaluator issues NO
  `region_set_super_capable(self,FALSE)` call (force beats measured, §C.5).

- **TOOTHFUL FALSIFIER (the symmetric block is load-bearing):** rebuild with
  `-DSAP_CERT -DSAP_NO_SYMBLOCK` (removes the `refl_classify != SYMMETRIC` clause
  from `reachable_good`). Re-run CASE B → the symmetric node now satisfies fitness
  and PROMOTES → the cert's "symmetric never promotes" assert flips → the harness
  prints `[autopromote] RESULT: FAIL`. Proves the hard-block, not anything else,
  is what keeps an unpunchable node out of the supernode role.

EXACT PASS/FAIL (auditor greps; `tests/run_autopromote.sh`, on BOTH arches —
native aarch64 + qemu-x86_64):
- CURE build (`-DSAP_CERT`): `printf 'autopromote test\nexit\n' | ./p-kernel`
  output MUST contain `[autopromote] RESULT: <N>/<N> PASS` (all sub-asserts A-D pass).
- FALSIFIER build (`-DSAP_CERT -DSAP_NO_SYMBLOCK`): output MUST contain
  `[autopromote] RESULT: FAIL`.
- Harness exit 0 IFF (cure prints `[autopromote] RESULT: <N>/<N> PASS`)
  AND (falsifier prints `[autopromote] RESULT: FAIL`).

### E.2 LIVE cert — `samples/11_distributed/run_supernode_autopromote_live.sh`

Mirrors `run_relay_autofallback_live.sh`. CAPABILITY PROBE FIRST: `unshare -rn true`
(or running as root); if it fails (this PRoot host returns EINVAL) print
`[autopromote-live] SKIP (no netns/NET_ADMIN — in-proc cert is the gate)` and exit 0.

Setup (private net namespace, no host privilege): `unshare -rn` → `ip link set lo up`
→ run `./relay -p $PORT -v` (with the `REFL1` echo enabled) → boot nodes with
`PKERNEL_RELAY=127.0.0.1:$PORT`, `PKERNEL_RELAY_KEY=$KEY`, `PKERNEL_AUTONET=1`, and
NO `PKERNEL_SUPERNODE` (AUTO). Loopback presents a public/cone-equivalent mapping
(reflexive IP == bind IP), so the public/cone node is fitness-good.

PASS (a public/cone node auto-promotes and is elected), EXACT predicates:
- per node `[ "$(grep -c 'autopromote: PROMOTE' nodeN.log)" -ge 1 ]`
  (the node self-promoted from measured fitness).
- on the lowest-id auto-capable node
  `[ "$(grep -c 'my_supernode=0' nodeN.log)" -ge 1 ]` via `snf stat` after settle
  (it elected itself — the bit propagated and the election picked it up), AND on a
  higher-id peer the same `snf stat` shows `my_supernode=<that lowest id>` (peers
  agree, NOCENTRAL).

TEETH (a symmetric-emulated node must NOT advertise the bit NOR be elected): emulate
symmetric NAT by giving the would-be-supernode node TWO relays
(`PKERNEL_RELAY=127.0.0.1:$P1,127.0.0.1:$P2`) where a small `socat`/iptables
port-rewrite makes the two vantage points observe DIFFERENT external ports.
ASSERT `[ "$(grep -c 'autopromote: PROMOTE' nodeSYM.log)" -eq 0 ]` AND on every
peer `snf stat` NEVER shows `my_supernode=<the symmetric node's id>` (it is not
elected — the bit never went high). Force-disambiguates "auto-promotion selects on
measured reachability" from "any node with a relay gets promoted."

Harness exit 0 IFF (PASS predicates) AND (TEETH no-promote / no-elect); OR a clean
SKIP when the netns substrate is absent.

---

## F. Honest bound (v1)

What measured auto-promotion does NOT do in this first slice:

- **Still min-id among the auto-capable, not best-RTT/best-bandwidth.** Election is
  unchanged `supernode_select` (lowest capable member). Auto-promotion only changes
  WHO is capable; it does not pick the *best* supernode among several capables.
  "Promote the lowest-latency / highest-bandwidth capable node" is a later slice.
- **Bandwidth/throughput is not measured.** Reachability + stability + NAT class +
  stress only. A reachable-but-slow link can still auto-promote.
- **CONE/SYMMETRIC needs the reflexive shim (§C.1.a) and ≥2 relay vantage points for
  a definitive verdict.** With one relay the node gets `refl_is_public` (direct-public
  detection) but CONE-vs-SYMMETRIC is UNKNOWN → fail-closed no-promote. Until the
  `REFL1` shim ships, the evaluator runs on `relay_contacted` + negatives only and
  stays conservatively un-promoting where reachability is unproven.
- **The teacher bit is unchanged** (§C.6) — content-gated on a loaded GGUF, never
  auto-promoted.
- **Bare-metal nodes are unchanged** — no evaluator is linked; `super_capable[self]`
  behaves exactly as today.
- **No new election dynamics under churn** beyond the existing SWIM convergence; the
  30 s/60 s dwells deliberately damp role changes, so a flapping link's node simply
  stays un-promoted rather than rapidly toggling the region's routing.
