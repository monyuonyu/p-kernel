# webd-user-space — move the web server out of the substrate

Status: **DESIGNED / not implemented**. This is the privilege-boundary plan for
the next UI wave: `galaxy` stays the honest observation surface, the future
Desktop/WebOS and Imaginary UI ride on the same server family, but the web
server itself must not live inside the ring0/substrate task.

mk_pino decision, 2026-06-27: **the web-server portion belongs in user space.**
Commander proposal accepted by mk_pino: stage the split. Do not over-split
processes first; build one hosted `webd` with clear internal modules, then
split daemons only when interfaces and isolation pressure justify it.

## 0. The Decision

The current shipped UI is useful, but its boundary is wrong for the next stage.
Today `arch/common/galaxy.c` owns too much:

- HTTP request parsing
- routing
- static HTML serving (`galaxy_page.h`)
- SSE
- WebSocket framing
- POST handling for teach/ask/profile/chat
- direct snapshot assembly from substrate tables

That was acceptable for the first "galaxy on the phones" slice. It is not
acceptable for Desktop/WebOS or Imaginary UI, because generated app code and
browser-facing protocol handling are exactly the parts that will change often
and fail often.

The new rule:

> **The substrate exposes bounded UI capabilities. A user-space `webd` owns the
> web protocol and all human-facing surfaces.**

## 1. Target Split

```text
substrate / ring0-equivalent
  - scheduler, p-fs/ARK, world, dnode, swim, dmn, mind state
  - galaxy event producers: galaxy_emit(...)
  - bounded UI syscall/capability surface
  - consent and capability gates
  - no HTTP parser, no WebSocket parser, no generated app runtime

user space: webd, stage 1 process
  - TCP listen / accept
  - HTTP parser and router
  - static asset serving
  - SSE and WebSocket framing
  - internal modules: webd/protocol, webd/desktop, webd/galaxy, webd/apps
  - Galaxy page, Desktop/phone shell, generated-app host
  - crash/restart boundary for UI bugs

browser / WebView
  - rendering and input only
  - no direct substrate memory access
```

For hosted Linux and Android, "user space" means outside the p-kernel substrate
task, in a host process or app-managed service thread that talks through a
bounded bridge. For bare-metal x86/aarch64, "user space" means a real ring3/EL0
program after the required TCP-server substrate exists.

The conceptual final form may split the hosted surface into a small daemon
family:

```text
webd       network/protocol/static assets/routing/session entry point
desktopd  PC desktop and phone shell state
galaxyd   decentralized observability view-model producer
appd      built-in/generated app host and sandbox broker
ui_api    substrate value/capability gate
```

That is a final shape, not the stage-1 implementation requirement. Stage 1 is a
single `webd` process with module boundaries that match the future daemons.

## 2. Daemon Responsibilities

### webd

`webd` is the network and session entry point. It owns:

- TCP listen/accept in hosted builds and the later ring3 `webd.elf` server
  loop;
- HTTP request parsing, response formatting, routing, static assets, cache
  headers, SSE, and WebSocket framing;
- session bootstrap and browser/WebView entry points;
- dispatch to internal stage-1 modules, and later IPC to `desktopd`,
  `galaxyd`, and `appd`;
- compatibility routes such as `/galaxy.json`, `/events`, and `/desktop`.

`webd` must not own substrate truth. It formats bounded values returned by
`ui_api`; it does not read world/dnode/dmn/mind tables directly.

### desktopd

`desktopd` owns human shell state for both PC desktop and phone shell modes:

- windows, cards, app launcher state, focus, active app, z-order, and layout;
- mode selection between desktop and phone shells;
- session-local shell state that should survive a browser reconnect when
  appropriate;
- PC affordances: overlapping/resizable windows, dock/taskbar, keyboard and
  mouse focus;
- phone affordances: full-screen foreground app, app switcher, safe-area
  layout, touch targets, and portrait-first navigation.

In stage 1 this is `webd/desktop`, not a separate process. The module boundary
should still make clear that shell policy is not substrate policy.

### galaxyd

`galaxyd` is the decentralized observability surface. It consumes bounded
world/events data and produces Galaxy view models for browser rendering:

- local event rings and snapshot values;
- explicitly stale gossip-derived knowledge where the source and age are
  visible;
- graph, timeline, health, and module view models;
- overflow/dropped counters and honest absence markers.

`galaxyd` is not a social ranking service. It must not introduce likes,
followers, popularity counters, leaderboards, engagement scores, or human
comparison metrics. Galaxy remains observation of the living system, not a
central social surface.

In stage 1 this is `webd/galaxy`. It may split first if event parsing or view
model bugs need their own crash boundary.

### appd

`appd` owns apps and app containment:

- deterministic built-in shell apps: calculator, notes, clock/timer, file
  manager/p-fs browser, terminal/console, mind chat, Galaxy, modules, settings,
  consent, and capability views;
- template/generated apps and later selfc/germ persisted tools;
- app manifests, provenance, and signed/persisted p-fs object metadata;
- iframe/worker sandbox setup;
- narrow capability-token brokering for app calls into `webd` endpoints.

Generated apps never call the substrate directly. They call the app host, which
checks manifest scope and granted tokens before any write-like operation reaches
`ui_api`.

In stage 1 this is `webd/apps`. It may split first if generated app crashes,
worker lifecycle, or sandbox broker bugs become the dominant isolation risk.

### ui_api / capability gate

`ui_api` is the substrate value and capability boundary. It owns:

- bounded copies from world/dnode/dmn/mind state into value structs or event
  frames;
- consent checks for teach, profile, generated-app persistence, and future
  p-fs writes;
- capability checks and revocation;
- write gates shared by PC desktop, phone shell, built-in apps, generated apps,
  hosted Linux, Android/UMP, and later bare-metal ring3.

The gate stays in the substrate side of the boundary. `webd`, `desktopd`,
`galaxyd`, and `appd` may ask; they do not bypass.

## 3. What Stays In The Substrate

The substrate remains the source of truth. It should keep only state collection
and small, bounded operations:

| Surface | Substrate owns | webd owns |
|---|---|---|
| Snapshot | copy from world/dnode/dmn/mind into a fixed struct | format JSON |
| Events | fixed-width ring events and overflow counters | SSE/WebSocket emission |
| Teach/ask | validate consent, call the existing safe verbs | parse HTTP body, render result |
| Console/log | bounded ring read | text/plain response |
| Modules | bounded module table | JSON response |
| Static UI | none after migration | HTML/CSS/JS assets |
| Generated apps | none | app iframe/worker/runtime sandbox |

The substrate API must return structs/frames, not HTTP bytes. That keeps the
boundary testable and prevents the web layer from becoming a second kernel.

## 3. Minimal UI Capability ABI

First hosted slice can be a C ABI. Bare-metal later maps the same concepts to
syscalls.

```c
int ui_snapshot(UI_SNAPSHOT *out);
int ui_event_read(UI_EVENT *out, unsigned max, unsigned *dropped);
int ui_self_read(char *out, unsigned max);
int ui_console_read(char *out, unsigned max);
int ui_modules_read(UI_MODULE *out, unsigned max, unsigned *count);
int ui_teach(unsigned key, unsigned value, UI_REPLY *out);
int ui_ask(unsigned key, UI_REPLY *out);
int ui_chat(const char *in, unsigned n, ui_emit_fn emit, void *ctx);
int ui_profile_post(const UI_PROFILE_POST *in, UI_REPLY *out);
```

Rules:

- every buffer has an explicit byte or element limit;
- every write-like verb passes through the existing consent/capability gate;
- no path strings from webd are interpreted by the substrate as filesystem paths;
- no generated app can call these APIs directly unless webd grants it a narrow
  capability token;
- the ABI is local-only in v1.

## 4. Migration Plan

### Slice A — freeze the boundary

Create `ui_api.{c,h}` around the current safe internals while leaving
`galaxy.c` behavior unchanged. The first test checks that `/galaxy.json`,
`/log.txt`, `/console.txt`, and `/modules.json` can be produced through the new
API without changing bytes except for allowed timestamps/counters.

Acceptance:

- `galaxy.c` routes still work.
- `ui_snapshot` does not expose raw pointers.
- all API output buffers are bounded.
- no static HTML bytes move yet.

### Slice B — hosted webd

Move the protocol code from `galaxy.c` into a hosted user-space `webd`:

- TCP listen/accept
- HTTP parser
- SSE/WebSocket framing
- static `galaxy.html` serving

The old substrate `galaxy_task` becomes either disabled by default or reduced to
a compatibility shim that uses `ui_api`.

Acceptance:

- `curl http://127.0.0.1:7800/galaxy.json` still returns the same schema.
- `/events` still streams real `galaxy_emit` events.
- killing `webd` does not kill the node.
- restarting `webd` reconnects to the same node state.

### Slice C — Desktop shell

Add `/desktop` served by `webd`, not the substrate. It is a normal human home
surface with a basic deterministic software suite:

- Calculator
- Notes
- Clock/timer
- File manager and p-fs browser, initially metadata-only
- Terminal/console
- Mind chat
- Galaxy and observability
- Modules, settings, consent, and capability inspector

This is distinct from Imaginary UI. Desktop is the stable home; Imaginary UI is
generated inside it.

Acceptance:

- `/` can continue serving Galaxy for compatibility, or redirect by platform.
- `/desktop` loads without adding substrate routes.
- Desktop apps call only the bounded `ui_api` surface.
- `/desktop` chooses the human shell client-side from capabilities/viewport,
  with `?mode=desktop|phone` as an explicit test override. PC-class clients get
  the desktop metaphor; phone-class clients get a phone shell. This is not just
  shrinking CSS: both are human-facing surfaces over the same `webd` and
  `ui_api` capability layer.

### Slice C1 - adaptive WebOS shell

The future `/desktop` route is an adaptive WebOS home, not one responsive
layout. It selects between two human-facing shells over the same bounded
`webd`/`ui_api` substrate:

- PC mode: multi-window desktop, taskbar or dock, resizable app windows,
  keyboard/mouse affordances, focus and window-management state, and dense
  observability.
- Smartphone mode: phone home screen, one foreground app at a time or explicit
  cards, app switcher instead of overlapping windows, bottom navigation, large
  touch targets, safe-area awareness, and portrait-first layout.

For smartphone/UMP, the deployed artifact may still be one Android process and
one WebView-backed app, but the intended experience is that yurikago takes over
the screen as a full phone-like OS shell. The same shell and built-in app suite
must also be able to run later as a bare-metal PC WebOS surface when `webd.elf`
and the TCP server substrate exist.

Mode selection should use measured client facts first: aspect ratio, viewport
resolution in CSS px, available resolution signals, `devicePixelRatio`,
pointer/hover capability, orientation, touch points, and safe-area insets. User
agent sniffing is fallback only. `?mode=desktop|phone` is allowed as an explicit
testing override and must not change endpoint paths, consent gates, or
capability semantics.

The built-in software suite is webd-hosted shell apps first. These apps are
deterministic, audited, and part of the stable home:

- calculator;
- notes;
- clock/timer;
- file manager and p-fs browser;
- terminal/console;
- mind chat with the growing student;
- Galaxy/observability;
- modules, settings, consent, and capability views.

This app model is local and ownerless. There is no central app store, no
popularity ranking, no likes, and no human-comparison layer. Visualization in
these apps means decentralized observability over real local node state, bounded
snapshots, event rings, or explicitly stale gossip-derived knowledge.

Roadmap for apps:

1. Built-in deterministic apps: hand-authored shell apps shipped with `webd`,
   sharing the same capability gates in both PC and phone shells.
2. Template/generated apps: generated or Imaginary UI tools run inside the same
   shell app model, contained by iframes, workers, manifests, and narrow tokens.
3. selfc/germ persisted apps: kept tools become signed/manifested p-fs objects
   with provenance, explicit consent, and bounded capabilities.

Acceptance:

- `/desktop` chooses PC or phone shell client-side without adding substrate
  routes.
- `?mode=desktop|phone` forces deterministic mode selection for tests.
- PC mode exposes multi-window desktop affordances instead of a phone card
  stack.
- Phone mode exposes a phone-like shell instead of overlapping desktop windows.
- Both modes launch the same built-in app suite over the same `webd` endpoints
  and bounded `ui_api` calls.
- Both modes enforce identical consent and capability gates for teach, profile,
  generated-app persistence, and future p-fs writes.
- Hosted browser, Android/UMP WebView, and later bare-metal PC all use the same
  shell/app model, adjusted only at the human surface.
- No shell or app introduces likes, rankings, popularity counters, central app
  store semantics, or human comparison.

### Slice D — generated-app runtime

Generated apps run under `webd`, never in the substrate:

- first as template-instantiated HTML/JS tools;
- later as selfc/germ-produced artifacts;
- "keep this" persists a signed/manifested tool object to p-fs through an
  explicit capability path.

Acceptance:

- malformed generated UI can crash its worker without killing webd or the node;
- generated apps cannot reach teach/profile/p-fs write APIs without a granted
  token;
- every persisted generated tool has provenance and a p-fs object id.

### Slice E — bare-metal parity

After `netstack-tcp-server` exists, build `webd.elf` as a real ring3 program.
It talks to the substrate through the same UI capability ABI, now mapped to
syscalls.

Acceptance:

- a deliberate `webd.elf` fault is reaped like `core_mind.elf`;
- the node keeps swim/world/p-fs alive;
- a restarted `webd.elf` resumes serving the current snapshot.

## 5. Certs And Falsifiers

Required tests before claiming the split is real:

| Cert | What it proves | Falsifier |
|---|---|---|
| `[webd-kill-survives]` | killing webd does not kill the node | old in-substrate route only |
| `[webd-reconnect-state]` | restarted webd reads live state | return cached bootstrap state |
| `[ui-api-bounds]` | all API calls respect output bounds | oversized console/log/module table |
| `[ui-no-rawptr]` | snapshots contain values, not substrate pointers | poison an internal pointer field |
| `[generated-app-contained]` | bad generated app cannot kill node | direct substrate call from app |
| `[desktop-mode-select]` | `/desktop` selects desktop vs phone shell from client capabilities without substrate route changes | backend path split or UA-only selection |
| `[desktop-mode-override]` | `?mode=desktop|phone` forces either shell for tests | override changes capability gates or API paths |
| `[desktop-same-api]` | both shells and built-in apps call the same bounded `ui_api`/webd endpoints | phone shell gets private substrate verbs |
| `[desktop-consent-gates]` | both shells enforce identical consent/capability gates | one shell can teach/write/profile without consent |
| `[webos-builtins-first]` | calculator/notes/clock/files/console/chat/galaxy/modules ship as deterministic apps before generated tools | generated app required for basic OS use |
| `[webos-no-central-store]` | app model is local, ownerless, and p-fs/provenance based | central app store or popularity ranking appears |

The important non-vacuity check: the cert must kill or fault the web server
process/task itself, not just close a browser tab.

## 6. Relationship To Existing Docs

- `galaxy.md` remains the truth for the observation semantics: every photon is
  a real organism event.
- This document now captures the first Desktop/WebOS human environment
  requirements: adaptive PC/phone shells, the deterministic app suite, and
  the staged generated/persisted app model. A later `web-os.md` can split this
  out once implementation starts.
- Imaginary UI is not a replacement for this split. It depends on it.
- `ring3-core.md` remains the precedent: mutable/failure-prone mind code moves
  out of the substrate. `webd` applies the same principle to the human UI layer.

## 7. Current Honest State

Not implemented. The shipped galaxy still serves HTTP from `arch/common/galaxy.c`.
The next implementation wave should start with Slice A, because it gives us a
small API seam without breaking the phone UI.
