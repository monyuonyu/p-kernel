/*
 *  demo_kdds.h
 *
 *  Cross-arch K-DDS heartbeat demo. Each node periodically publishes a
 *  short "n{id} {arch} tick={N}" message on topic "demo/heartbeat" and
 *  also subscribes to the same topic, printing every message it receives.
 *  Run on two nodes (same or different ABI) and the two streams interleave
 *  in each node's shell — visual proof that K-DDS pub/sub traverses the
 *  SWIM mesh, including across heterogeneous architectures.
 */

#pragma once

/* Spawn the demo's publisher + subscriber tasks. `arch_label` is the
 * short architecture string printed inside each published message so the
 * receiving side can tell whose data it's seeing (e.g. "aarch64",
 * "x86_64", "raspi3"). Idempotent — second call prints "already running"
 * and returns. */
void cmd_kdemo(const char *arch_label);
