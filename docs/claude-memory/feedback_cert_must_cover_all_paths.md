---
name: feedback-cert-must-cover-all-paths
description: A cert that drives only ONE code path silently misses bugs in sibling paths doing the same job; enumerate every site and cover each.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

When a behaviour is implemented at SEVERAL call sites (e.g. SWIM gossip re-propagation happens at ~5 distinct relay sites: self-beacon, self-refutation, transitive-discovery, the superseding LWW gate, AND `swim_rx` rediscovery), a cert that exercises only ONE path (e.g. only `gossip_apply`) will pass while a sibling path is silently broken.

**Concrete case (T-fix-a, 2026-06-20):** the teacher-capability bit-pack was converted at 4 of 5 relay sites; the 5th (`swim_rx` rediscovery, swim.c:444) still emitted the N-2b supernode-only byte, dropping teacher bit 1 for `GOSSIP_TTL` rounds. Local correctness was fine (local table set by `gossip_apply` which ran first), so the bug was invisible to the `gossip_apply`-only cert — a third node first-learning the peer via that relay would latch teacher=FALSE until an incarnation bump. The implementer's own internal review caught it and added a `[teacher-rx-relay]` arm that drives the REAL `swim_rx` and goes RED without the fix.

**Why:** "passes the cert" only means "the path the cert drives works." Sibling paths that do the same job are dark.

**How to apply:** when reviewing/auditing a change that touches a repeated pattern, ENUMERATE every site (grep the call) and confirm the cert exercises EACH — or at least that the untested ones are byte-identical to a tested one. In the audit brief, explicitly ask the auditor to "find every site that does X and confirm each is covered." Relatedly: the cert that found this only existed because the auditor demanded a path-specific arm — see [[feedback_audit_is_the_engine]] and the same "drive the production path, not a reconstruction" lesson in [[feedback_validator_and_learner_traps]].
