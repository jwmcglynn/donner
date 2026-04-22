# Design: Perf Framework and Analyzer

**Status:** Draft (one-pager)
**Author:** Claude Opus 4.7
**Created:** 2026-04-21

## Summary

Unify Donner's currently-scattered performance instrumentation (Tracy zones, the
`donner_perf_cc_test` correctness/wall-clock split, ad-hoc `chrono` timers, Geode
GPU-counter probes, and the resvg-suite drag-latency harness) into a single
**PerfFramework** with one canonical API, one canonical event stream, and one
primary surface: **the in-editor analyzer panel, tightly coupled to the SVG
debugger** (doc [0033](0033-svg_debugger_in_editor.md)). Offline analysis is a
supporting surface — Perfetto export lives behind a button in that same panel,
and a regression-diff CLI consumes the same event stream for CI.

**Editor-first is P0.** Perfetto export, the `donner_perf_analyzer` CLI, and
any other offline surface are P1. The one-second feedback loop of "tick the
animation, click a frame, see what was slow, see which element was slow, change
the source, re-tick" only closes inside a single tool — and we already have
that tool (the editor + the debugger). Shipping Perfetto-first would tell the
perf culture "perf work is an offline activity", which is the culture this doc
is explicitly trying to change.

**Primary surface: Wasm.** The editor's primary deployment is the
Wasm/browser build (see doc [0032](0032-teleport_ipc_framework.md)
"Primary surface: Wasm"), so the analyzer panel's primary home is
in-browser. On Wasm the per-thread ring buffers live in their
respective workers and the main-thread analyzer panel consumes
*flushes* of those ring buffers delivered via Teleport's
`WorkerTransport` — a batched frame of events per N ms, not a
per-event crossing. The desktop sandboxed equivalent works the same
way (per-subprocess ring buffer, pipe-transport flushes to the
editor). Unified ring buffer semantics across surfaces is what lets
one analyzer UI work everywhere.

Today "perf work" in Donner means opening the Tracy client for the editor, running
a shell script for the CLI, reading test-log CSVs for the resvg suite, and stitching
them together by hand. Three data-formats, three viewers, one problem. This doc
collapses that to one.

## Goals

- **One instrumentation API.** `DONNER_PERF_SCOPE(name)` and
  `DONNER_PERF_COUNTER(name, value)` macros. A single compile-time switch chooses
  whether they emit to the in-process ring buffer, Tracy, Perfetto, or nothing.
  No more `#if DONNER_TRACY_ENABLED` sprinkled across the codebase.
- **Live in-editor analyzer panel, coupled to the SVG debugger** (doc
  [0033](0033-svg_debugger_in_editor.md)) — this is the P0 surface. Per-frame
  flamegraph, per-system histogram, counter plots, and per-entity attribution
  — all driven off the same ring buffer the tests consume, and all wired into
  the debugger's draw-call timeline so picking a frame in the debugger reveals
  what it cost, element by element.
- **Collect-and-analyze in-place.** Traces are captured, visualized, and
  diffed inside the editor. A developer should never need to alt-tab into
  an external tool to answer "why was that frame slow" for live debugging.
- **Example integration (the marquee demo):** click a frame on the debugger's
  draw-call timeline → the analyzer panel lights up with that frame's
  flamegraph, its per-stage ms breakdown, and a **per-element cost list**
  (each ECS entity rendered this frame, sorted by accumulated scope time).
  Click a row in the cost list → the debugger's entity inspector highlights
  that entity and shows its resolved paint state. One click, root cause in
  view. This is what P0 has to deliver.
- **Trace export surfaced in the debugger UI.** The same panel has a
  "Save trace…" button (→ Perfetto / Chrome-tracing JSON) and a
  "Compare against…" button (→ loads a reference trace and diffs). The CLI
  form (`donner_svg_tool --trace=perf.json`) exists for automation, but the
  debugger UI is the primary way humans produce a trace file.
- **Offline regression analyzer** — P1. A small `donner_perf_analyzer` CLI
  consumes two recordings and flags per-scope regressions above a noise
  floor. Plugs into the nightly perf lane and the `donner_perf_cc_test`
  wall-clock half.
- **Zero cost when off.** Release / default builds pay nothing — scopes compile
  to no-ops, counters to nothing. The framework is opt-in at link time.
- **Attribution down to ECS entity.** A scope can carry an entity tag so the
  analyzer can say "this filter cost came from this `<feGaussianBlur>`" — exactly
  what the resvg golden suite wants when a test regresses by 3 ms, and exactly
  what the per-element cost list above needs.
- **Tracy backend stays**, as a parallel sink — see [Decisions](#decisions)
  below.

## Non-Goals

- **Not a replacement for GPU-vendor tools.** RenderDoc / Xcode Metal capture /
  Tracy's own GPU-timing view stay authoritative for "what did WebGPU actually
  do, cycle by cycle". The PerfFramework's GPU view is coarse (queue submit /
  encode / present timestamps), not fine-grained GPU-profile data.
- **Not a CPU sampling profiler.** For "what code path is hot" at instruction
  resolution keep using perf / Instruments / samply. PerfFramework is for
  *instrumented* timings, not sampled ones.
- **Not continuous production telemetry.** Donner has no production telemetry
  channel and this doc is not proposing one. All of this is developer-facing
  tooling.
- **Not a reinvention of Perfetto/Tracy.** Framework is an *adapter*. We emit
  into the formats they already understand; we don't try to out-build them.
- **No tests that "just check perf didn't get worse" in wall-clock without an
  explicit budget.** Every perf test names its budget and its reference hardware.

## Decisions

- **Editor integration is P0. Perfetto export is P1.** All P0 milestones
  (M1–M3) ship a useful editor-first perf workflow. Perfetto / Chrome-tracing
  export (M4a) and the regression-diff CLI (M4b) are P1 and ship after
  the editor loop is closed.
- **Trace export lives in the debugger UI.** The "Save trace…" /
  "Compare against…" buttons are part of the analyzer panel that is part
  of the debugger (doc [0033](0033-svg_debugger_in_editor.md)). The
  `donner_svg_tool --trace=...` CLI form exists for CI / automation only.
- **Tracy backend stays, as a parallel sink for ≥ one release after M1
  lands.** `DONNER_PERF_SCOPE` feeds both the in-process ring buffer and
  the Tracy sink by default. Rationale: (a) Tracy's out-of-process profiler
  has features the in-editor panel won't match on day one (full-program
  capture across the whole process tree, its own flamegraph, long-capture
  replay); (b) existing users — including the editor prototype today — have
  Tracy in their fingers and we don't want to rip that away before the
  in-editor replacement is proven; (c) Tracy stays our escape valve if the
  in-editor panel needs to be taken apart for any reason. Tracy gets
  dropped only once the in-editor panel covers the remaining workflows
  we actually use it for.
- **Coupling with the SVG debugger (doc 0033).** Perf framework and
  debugger are one UI, built on shared infrastructure. The debugger's
  `DebugTrace` per-frame structure and the perf framework's ring buffer
  are populated from the same call sites in the same frame — the debugger
  owns "what was drawn", the perf framework owns "how long it took",
  and they're joined by entity ID on display.

## Next Steps

1. Survey existing call sites: how many Tracy scopes do we have today,
   and what semantics do they encode? The migration must not lose
   information.
2. Land the ring buffer + macro rename first (M1) — that's the unlock for
   every downstream piece (analyzer panel, Perfetto export, regression tool),
   and it's a pure in-place refactor of existing Tracy call sites.
3. Prototype the "click a debugger frame → see per-element cost list"
   integration against a canned event stream before building the live
   wiring. If the UI shape feels right on fake data, live data is a
   plumbing exercise. If it doesn't, we find out before spending M3 on
   it.

## Implementation Plan

### P0 — in-editor perf loop

- [ ] **Milestone 1: Unified API + ring buffer + Tracy co-sink**
  - [ ] Define `donner::perf::Event{name, ts_ns, duration_ns, thread_id, payload}`
        + lock-free per-thread ring buffer in `donner/base/perf/`.
  - [ ] `DONNER_PERF_SCOPE`, `DONNER_PERF_COUNTER`, `DONNER_PERF_INSTANT` macros.
        All compile to nothing when the framework is disabled.
  - [ ] Migrate existing Tracy call sites to the new macros. Tracy stays on as
        a parallel sink (see Decisions) — migrate the *interface*, keep the
        *surface*.
- [ ] **Milestone 2: Ring-buffer + Tracy sinks wired, debugger integration
      seam**
  - [ ] Ring-buffer sink (consumed live by editor panel).
  - [ ] Tracy sink (unchanged external surface, fed via the new API).
  - [ ] Define the join with doc 0033's `DebugTrace`: both structures share
        a frame ID and an entity ID, so "click a frame → per-element cost
        list" is a straight pivot, not a fuzzy correlation.
- [ ] **Milestone 3: In-editor analyzer panel (P0 surface)**
  - [ ] Flamegraph view (per-frame, zoomable).
  - [ ] Counter time-series plots.
  - [ ] Per-ECS-entity attribution tab (pivots the ring buffer by entity tag).
  - [ ] **Frame-click → per-element cost list.** Picking a frame in the
        debugger's draw-call timeline displays the per-entity cost
        breakdown for that frame in the analyzer panel; clicking a row
        highlights the entity in the debugger's scene tree.
  - [ ] In-panel trace controls: "Save trace…" (Chrome-tracing JSON),
        "Compare against…" (load a reference trace, show per-scope
        regression deltas in-panel).

### P1 — offline / CI

- [ ] **Milestone 4a: Perfetto / Chrome-tracing export**
  - [ ] JSON writer shared by the in-panel "Save trace…" button and the
        `donner_svg_tool --trace=perf.json` CLI flag.
- [ ] **Milestone 4b: `donner_perf_analyzer` CLI**
  - [ ] Two-run regression diff: "scope X got 1.3× slower, p95 13.2 ms → 17.4 ms".
  - [ ] Plugs into `donner_perf_cc_test` wall-clock-lane CI output. Same diff
        engine as the in-panel "Compare against…" button — same semantics
        whether you're in the editor or in CI.
- [ ] **Milestone 5: GPU integration**
  - [ ] Geode backend emits queue-submit / encode / present timestamps via the
        same API. Editor panel shows CPU + GPU side-by-side.

## Proposed Architecture

```
    Call sites   ─── DONNER_PERF_SCOPE / COUNTER / INSTANT ──┐
                                                             │
                                                             ▼
                                          ┌──────────────────────────┐
                                          │ donner::perf::RingBuffer │
                                          │ (per-thread, lock-free)  │
                                          └──┬───────────┬───────────┘
                                             │           │
                                             ▼           ▼
                                        Tracy sink    (always: in-process
                                        (parallel,    event stream for the
                                         ≥ 1 release) editor analyzer)
                                                         │
              ┌──────────────────────────────────────────┘
              │
   ┌──────────┴──────────────────────────────────────────────────────┐
   │ Editor (//donner/editor) ──── P0                                │
   │                                                                 │
   │   ┌─────────────────────────┐    ┌──────────────────────────┐  │
   │   │ SVG debugger panel      │◄──►│ Perf analyzer panel      │  │
   │   │ (doc 0033)              │    │ - Flamegraph             │  │
   │   │ - Draw-call timeline    │    │ - Per-entity cost list   │  │
   │   │ - Entity inspector      │    │ - Counter plots          │  │
   │   │ - Layer explorer        │    │ - Save trace… button     │  │
   │   │                         │    │ - Compare against… button│  │
   │   │  click a frame ─────────┼───►│  shows that frame's cost │  │
   │   │  click a cost row ◄─────┼────┤  highlights that entity  │  │
   │   └─────────────────────────┘    └──────────────────────────┘  │
   └─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼ (Save trace / --trace=file.json)
                            Perfetto / Chrome JSON ──── P1
                                    │
                                    ▼
                            donner_perf_analyzer CLI ──── P1
                                    │
                                    ▼
                            Regression diff in CI output
```

Key properties:

- **Single source of truth (the ring buffer).** Editor panel, Tracy, Perfetto
  export, and CI diffs all consume identical event streams. No "it looks fine
  in the editor but CI says it regressed" divergence.
- **Tightly coupled to the SVG debugger (doc [0033](0033-svg_debugger_in_editor.md)).**
  Per-frame `DebugTrace` (0033) and per-frame perf events (this doc) share a
  frame ID and entity ID and are presented as two panes of one tool. Click a
  frame in one, see costs in the other; click a cost row, see the element in
  the other.
- **Collect-and-analyze in-place.** The primary workflow — tick the animation
  (doc [0034](0034-animation_proof_of_life.md)), click a frame, see what was
  slow, see which element was slow, change the source, re-tick — lives
  entirely inside the editor. Exporting traces is a secondary workflow.
- **Tracy stays alive, in parallel.** `DONNER_PERF_SCOPE` drives both the
  in-process ring buffer and Tracy by default. Tracy's out-of-process
  profiler remains the escape valve and the "whole-program capture" tool
  for as long as the in-editor panel hasn't proven out that workflow.
- **Compile-time opt-in.** Default builds pay zero. Developer builds and
  perf-lane CI turn it on. Production consumers (BCR) never see it.
- **Entity-tagged scopes.** A scope can carry an ECS entity — the analyzer
  pivots on that to drive the per-element cost list (the marquee feature)
  and to answer "which element is the slow one?" without `printf`-driven
  bisection.
- **Plays well with doc [0034](0034-animation_proof_of_life.md) (animation
  proof-of-life).** The 60 fps budget gate uses PerfFramework scopes for the
  per-stage breakdown. Animation is the first real customer.

## Data and State

- **Per-thread ring buffers** (no cross-thread contention on the fast path).
  Consumers walk them either live (editor) or at flush time (trace export).
- **Bounded memory.** Ring buffer is sized at init and drops oldest events on
  overflow — never allocates on the hot path. Default 4 MiB/thread.
- **Cross-thread correlation via nanosecond timestamps.** All threads stamp
  from `std::chrono::steady_clock`; the analyzer merges by timestamp. Good
  enough for ms-scale SVG rendering work.

## Testing and Validation

- **Correctness of scopes.** A unit test drives a known workload and asserts
  the resulting event stream contains the expected scopes with the expected
  nesting. This catches "someone removed the scope" regressions.
- **Zero-cost discipline.** A build-size and symbol-count test on release
  builds asserts that PerfFramework contributes 0 bytes when disabled. A
  release build containing the string `donner::perf::RingBuffer` fails the
  test.
- **Sink equivalence.** A recorded workload flushed to Tracy and to Perfetto
  must produce the same scope names, durations, and counters (formats
  differ, semantics don't).
- **Analyzer regression.** The `donner_perf_analyzer` tool itself is tested —
  two synthetic traces with a known 1.3× regression in one scope must be
  flagged; a known ±2% noise traces must not be flagged.

## Dependencies

- Tracy (already vendored) — stays as a parallel sink indefinitely until the
  in-editor panel covers every workflow we actually use Tracy for.
- A Perfetto / Chrome-trace writer — minimal custom JSON emit; no new
  third-party dependency needed for v1. P1.
- Doc [0020](0020-editor.md) (editor in-tree) — hard dependency for the P0
  analyzer panel.
- Doc [0033](0033-svg_debugger_in_editor.md) (SVG debugger) — **co-designed**,
  not a loose dependency. Shared UI, shared frame/entity IDs on events, shared
  feature-flag gate (`debugger-instrumentation`). Neither doc ships its
  analyzer/debugger panel without the other.
- Doc [0032](0032-teleport_ipc_framework.md) (Teleport IPC) — **hard
  dependency on the primary (Wasm) surface.** Per-worker ring buffers
  flush batched event frames to the main-thread analyzer panel via
  Teleport's `WorkerTransport`. The same path holds on desktop with a
  pipe/shared-memory transport. In-process builds skip Teleport (direct
  ring-buffer consumption) as an escape-hatch path.

## Alternatives Considered

### A. Editor-integrated only, no Perfetto export (ever)

**Pros.** Smallest surface. No JSON-writer to maintain.
**Cons.** Can't share a trace with another contributor, can't archive across
CI runs, can't hand a trace to anyone who doesn't have a Donner checkout.
**Rejected** — we keep Perfetto export, we just do it as P1.

### B. Perfetto-first / Perfetto-only (no editor panel)

**Pros.** Zero new UI work. Perfetto is already great.
**Cons.** "Reproduce a stutter, open a trace, alt-tab, drag the file in" is a
10-second loop; we want a ≤1-second "glance at the panel" loop for live
debugging. More importantly, the marquee workflow — click a frame in the
debugger, see per-element cost — only works if the perf UI lives in the same
process as the debugger. Putting it in Perfetto cuts the join. **Rejected.**

### C. Just keep using Tracy directly

**Pros.** Already works. Zero new code.
**Cons.** Doesn't integrate with CI regression gates (Tracy is interactive-only).
Doesn't give us entity-tag pivots. Doesn't give us the debugger join. Doesn't
integrate with `donner_svg_tool` for CLI traces. **Rejected — but Tracy stays
as one sink**, in parallel, for at least a release past M1 (see Decisions).

## Open Questions

- **When does Tracy retire?** Tracy stays in parallel until the in-editor
  panel covers the workflows we actually use Tracy for. Pick a concrete
  retirement criterion during M3 (e.g. "drop Tracy the first release after
  the in-editor panel has shipped a full-program capture mode") so the
  parallel-sink situation doesn't become load-bearing indefinitely.
- **Per-thread ring-buffer size default.** 4 MiB/thread is a guess. Profile
  real editor workloads to size correctly.
- **Backwards compat for existing Tracy call sites.** Do we keep the `TracyZone*`
  macros as aliases for one release, or break them in a single PR? Project norm
  is "break them" — no deprecation shims — but confirm.
- **GPU-counter integration on Geode.** The Geode design doc (0017) already
  wires up some WebGPU timestamp queries. Do we consume those here, or does
  Geode keep its own parallel perf surface? Strong preference: consume here.
- **Ring-buffer flush cadence over Teleport.** Per-thread ring buffers
  exist on Wasm workers and sandboxed desktop subprocesses; the
  main-thread analyzer panel pulls them via Teleport's transport. Open
  question is the flush cadence — every N frames? Every N ms? On panel
  demand? Per-event crossing would dominate the cost the framework is
  trying to measure, so batched-flush is the rule; the open question is
  the batch size. Pin during M3.
