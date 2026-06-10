---
name: project-linux-userspace-time-domain
description: "Make the Linux-hosted p-kernel's notion of time match wall-clock — Linux can delay SIGALRM delivery so naïve tick-counting drifts. Two complementary fixes plus a debug-friendly CPU-time mode."
metadata: 
  node_type: memory
  type: project
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

**The problem.** On `arch/linux/aarch64`, T-Kernel's internal time advances when the SIGALRM handler increments a tick counter. Linux can withhold scheduling from the p-kernel process arbitrarily long (other processes get the CPU), and POSIX standard signals **collapse** — N missed expirations of the 10 ms timer arrive as a single SIGALRM when we next run. Result: T-Kernel time drifts behind wall-clock by the amount of CPU we did not get.

**Two complementary fixes; together they make the hosted port indistinguishable from bare metal in user-visible timing:**

### Fix 1 — `timer_getoverrun` in the SIGALRM handler

POSIX exposes the missed-expiration count directly. Replay them on each delivery:

```c
static timer_t tid;   /* exported from preempt.c */

static void sigalrm_handler(int sig) {
    int missed = timer_getoverrun(tid);   /* 0 if none, N otherwise */
    if (arch_irq_disabled_flag) {
        pending_timer_ticks += (1 + missed);
        return;
    }
    for (int i = 0; i <= missed; i++) {
        knl_timer_handler_startup();
    }
}
```

Result: tick *count* matches wall-clock count exactly. Bursts at signal-delivery time, but no accumulating drift.

### Fix 2 — anchor "what time is it" on `clock_gettime(CLOCK_MONOTONIC)`

Even with fix 1, query-the-time APIs (`tk_get_tim` etc.) only get a correct answer on the SIGALRM delivery moment. Between deliveries, the tick counter is stale. The fix is to make queries read the OS wall clock instead:

```c
uint64_t arch_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}
```

T-Kernel's `knl_current_time` becomes a cached value updated on SIGALRM; *queries* hit `clock_gettime` directly. This is the same pattern Linux itself uses since `NO_HZ` (2007) — the tick is a scheduler hint, not a time source.

### Debug-friendly alternative — CPU-time mode

For deterministic replay and step-debugging, an opt-in mode that uses `CLOCK_PROCESS_CPUTIME_ID` instead:

- Time advances only while our process actually runs.
- Pausing the kernel in a debugger does not cause a flood of expired timers on resume.
- Compile-time switch (`CFN_TIME_DOMAIN_WALL` vs `CFN_TIME_DOMAIN_CPU`) or runtime env var.

The user explicitly asked for this mode to be preserved as an option ("デバッグしづらくなりそう" — exactly why debug mode wants frozen time).

### When to implement

After Session 3c (LP64 allocator fix) lands and `./p-kernel` produces a real boot banner. Adding time domain switching to a non-booting kernel makes no sense; it is a polish layer on a working system.

### Hook points

- `preempt.c`: signal handler gains `timer_getoverrun` + replay loop. Export `tid`.
- `arch/linux/aarch64/time.c` (new): `arch_get_time_ms`, `arch_get_time_ns`, mode switch.
- `kernel/common/timer.c`: where `knl_current_time` is read, replace with `arch_get_time_*` (gated on `_TK_HOSTED_LIBC_` so bare-metal stays tick-based).

Cost: roughly 20-30 lines of new code plus a handful of read-site rewrites in `kernel/common/`. Low risk after the allocator is healthy.
