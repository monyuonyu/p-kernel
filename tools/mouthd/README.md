# mouthd — the Frontier Mouth host companion

A small host-side daemon (like `tools/relay`) run **only by the node operator who
opts in**. It holds the API key, speaks TLS to the provider (CONSULT) or plain
HTTP to a localhost model server (TEACH), and talks to the kernel over localhost
**FM1** datagrams. See `docs/architecture/30-module/frontier-mouth.md` and
`frontier_mouth_design.md` §4.

The key is an **owned** credential — it lives here, in the operator's process,
**never** inside the ownerless kernel or its p-fs. No mouthd running ⇒ the node
is a byte-honest baseline node (the kernel degrades honestly to the student's
own voice).

## Build

```
make            # -> ./mouthd  (host tool, NEVER in CI)
```

Includes only `arch/common/llm/frontier.h` (the FM1 op bytes + `fr_echo_of` +
the `teacher_kind` enum). Links no kernel object. TLS is done by invoking the
system `curl`, not by linking one into the kernel.

## Run

CONSULT (the labeled borrowed voice):

```
export ANTHROPIC_API_KEY=sk-...
export PKERNEL_MOUTHD_PORT=7801         # the kernel finds mouthd on this port
./mouthd --provider anthropic --model claude-3-5-haiku-latest --port 7801
# then, in the galaxy chat, the "consult" verb sends the human's words here.
```

TEACH (open-license teacher text only — the license line is enforced by the
kernel's `teacher_kind` enum; there is NO `FRONTIER_API` kind):

```
./mouthd --teach http://127.0.0.1:8080 --model qwen2-7b-instruct --license apache-2.0
```

## Status / honesty

- CONSULT is fully wired (FM1 REQ → curl → the Anthropic Messages API → CHUNK/
  DONE with the anti-theater nonce echo).
- TEACH validates the license line and defines the wire; the full harvest loop
  (the `student_harvest_diverse.c` prompt-diversity discipline over a volunteer's
  local server) is a `[live]` follow-up.
- This tool is **[live]-only** — never in CI (secret + cost + nondeterminism).
  The kernel-side `[frontier-*]` certs run against an in-process **mock** mouthd;
  mouthd is the real socket leg a human exercises by hand on the ThinkPad.
- Binds `127.0.0.1` only (hard-coded), like `relay`/`galaxy_posix`.
