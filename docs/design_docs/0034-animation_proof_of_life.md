# Design: Animation Proof-of-Life

**Status:** Draft (one-pager)
**Author:** Claude Opus 4.7
**Created:** 2026-04-21

## Summary

Stand up the **thinnest possible end-to-end animation path** — a single animated
attribute updating every frame — and wire it natively into Donner's existing
display surfaces (`donner_svg_tool --preview` / `--interactive` and the editor's
live canvas). No new `--animate` flag: if the document has animations and the
tool is showing a live view, they play. This is the forcing function that shakes
out Donner's "does the whole stack sustain 60 fps with real mutations?" story.

Reference fixture: **`donner_splash.svg`**, augmented with a spinning
`<text>Animation</text>` element placed in the middle of the canvas, *below a
few of the existing logo layers*. Using the real splash (not a synthetic toy)
means the per-frame cost is representative — gradients, clip paths, and
layered fills are all in the hot path — and putting the animated text *below*
layered content ensures every frame has to re-render elements that stack on
top of the animation, exercising incremental invalidation realistically.

**Reference hardware: Apple M1 MacBook Pro.** Matches the 60 fps targets in
doc [0025](0025-composited_rendering.md) (composited rendering) and
doc [0026](0026-drag_end_latency.md) (drag-end latency), so all three docs
share one hardware anchor and one budget (16.67 ms / frame).

**Primary surface: Wasm.** The editor's primary deployment is the
Wasm/browser build (see doc [0032](0032-teleport_ipc_framework.md)
"Primary surface: Wasm"), so the 60 fps animation gate has to hold
inside a browser tab with the renderer running in a worker, not just
on a native desktop build. The timeline scrubber lives on the main
thread, tick requests cross to the worker via Teleport's
`WorkerTransport`, and the worker ticks `AnimationDriver` +
re-renders. The desktop native path is the dev-time equivalent
(scrubber on main process, renderer in a subprocess). The 16.67 ms
budget is enforced on both.

**Coupled with editor drag perf by design.** Continuous 60 fps animation and
continuous 60 fps dragging are the same problem: a bounded region of the DOM
mutates every frame while the rest of the scene stays identical. Dragging is
"user moves an element"; animation is "clock moves an attribute". The
AnimationDriver reuses the compositor's existing layer-promotion hints,
invalidation tracking, and dirty-region recomputation — no parallel fast path.
Anything that makes drag-at-60 smoother makes animation smoother, and vice
versa.

This is **not** a shipped animation feature. It is a *vehicle* for driving the system
to 60 fps (and eventually 120 fps) on representative SVG content. The goals are
measurement, invalidation correctness, and per-frame budget discipline — not SMIL
spec coverage. The real animation system (`<animate>`, `<animateTransform>`,
`<set>`, event-based timing) lives in a follow-up design doc and uses this scaffolding.

Project context: animation is a named **v1.0** target in
[`docs/ProjectRoadmap.md`](../ProjectRoadmap.md). Today Donner has no per-frame
mutation driver, and the fastest way to find out which systems collapse at 60 fps is
to give them one.

## Goals

- **A single animated attribute drives a full frame loop.** Minimum viable: one
  `<rect>` with an `<animate attributeName="x">` advances its `x` via an explicit
  `AnimationDriver` tick at real wall-clock time. No complex timing spec yet —
  just linear interpolation between two values.
- **Full stack exercised every frame.** DOM mutation → style invalidation →
  incremental layout → composited render → present. Each stage is instrumented
  and budget-asserted.
- **Reuse the compositor's layer system** (doc [0025](0025-composited_rendering.md)).
  The animated `<text>` is promoted into its own compositor layer so the
  surrounding splash geometry stays cached in its existing backing stores —
  the only per-frame work is redrawing the animated layer and the final
  compose pass. Shared infra, shared bugs, shared wins with doc
  [0026](0026-drag_end_latency.md).
- **Simple-span fast path.** Text spans without filters / masks / clip paths /
  non-trivial paint servers skip the render-to-texture detour and composite
  directly from their cached glyph runs. This is the first optimization the
  proof-of-life unblocks: "your animation is one rotating text element, you
  don't need a whole offscreen target for it." Gated on an explicit
  `isSimpleSpan(entity)` predicate so correctness stays the compositor's,
  not the fast path's.
- **Hard per-frame budget gates in CI.** A 60 fps target means a 16.7 ms frame
  budget. The proof-of-life test asserts end-to-end frame time on reference
  hardware against an explicit budget, using `donner_perf_cc_test` so the
  correctness half stays on the PR gate.
- **Surfaces the bottlenecks, doesn't hide them.** If style invalidation is the
  long pole at 12 ms, the test output says so — no silent averaging, no "looks
  about 60 fps in my editor".
- **Animations play natively in any live view.** No new `--animate` flag. The
  existing `donner_svg_tool --preview` (terminal image view) and `--interactive`
  (clickable terminal view) tick the animation driver while rendering successive
  frames to the terminal. The editor's live canvas does the same. Static-output
  modes (default `--output foo.png`, no `--preview`) sample frame 0 only —
  animations are strictly a live-view concern, preserving the reproducibility
  of the default snapshot path.
- **Per-frame timings are free with `--verbose`.** Rather than a dedicated CLI
  sub-tool, the live-view modes dump per-stage timings into the existing
  `--verbose` output. A batch perf-regression run uses a dedicated test
  binary (see M4), not a new CLI entry point.
- **Minimal SMIL subset.** Just enough of `<animate>` to make the proof-of-life
  run: `attributeName`, `from`, `to`, `dur`, linear calcMode, `repeatCount=indefinite`.
  Nothing more.

## Non-Goals

- **Not SMIL spec coverage.** Beyond the minimal `<animate>` +
  `<animateTransform type="rotate">` needed to drive the fixture: no
  `<animateMotion>`, `<set>`, event-based timing, `keyTimes`/`keySplines`,
  begin/end syncbase, restart semantics, additive animation, or CSS
  animations/transitions.
- **Not a declarative timing engine.** No priority queue, no sparse activation
  set, no interval resolution. The proof-of-life driver is a dumb "tick every
  frame, interpolate, write attribute" loop. The real timing engine is a later
  design doc.
- **Not a scripting integration.** `setTimeout` / `requestAnimationFrame` from
  scripts is doc [0027](0027-scripting.md)'s problem, not this one's.
- **Not a design for layer caching.** Layer-based caching for animation is called
  out in ProjectRoadmap; this doc exposes the *need* for it but does not design it.
- **Not "ship animations to v1.0 users"** — this is internal plumbing that makes
  the real animation design defensible.

## Next Steps

1. Land the reference fixture — a copy of `donner_splash.svg` with a spinning
   `<text>Animation</text>` element layered underneath a few of the existing
   logo shapes — as `donner/svg/animation/testdata/splash_animated.svg`.
2. Pick the reference hardware for the 60 fps gate and document the
   self-calibration approach if we go that route.
3. Wire `AnimationDriver::tick(TimePoint)` into the editor's `RenderCoordinator`
   *and* into `donner_svg_tool`'s `--preview` / `--interactive` render loop
   (neither currently ticks; both currently render once per state change).
4. Land the first budget-asserted `donner_perf_cc_test` with a *deliberately
   loose* budget. Tighten only after we measure.

## Implementation Plan

- [ ] **Milestone 0: Reference fixture**
  - [ ] Add `donner/svg/animation/testdata/splash_animated.svg`: a copy of
        `donner_splash.svg` with a `<text>Animation</text>` element placed at
        the visual center, rotated by an `<animateTransform type="rotate">`,
        and inserted in document order *below* a few of the logo layers so
        the layered composite always has content on top of the animated text.
  - [ ] Golden-render frame 0 (static) to confirm the fixture renders
        correctly before any AnimationDriver exists.
- [ ] **Milestone 1: AnimationDriver skeleton**
  - [ ] `AnimationDriver` class in `donner/svg/animation/` with `tick(TimePoint)` +
        list of `ActiveAnimation{entity, target, fromVal, toVal, startTime, durMs}`.
  - [ ] Parse the minimal `<animate>` + `<animateTransform type="rotate">`
        subset needed to drive the fixture into `ActiveAnimation` on document
        load.
  - [ ] `tick()` computes interpolated value and writes back via the normal
        attribute-mutation path so existing invalidation machinery fires.
- [ ] **Milestone 2: Natively drive live views**
  - [ ] `donner_svg_tool --preview`: when the document has active animations,
        the terminal-preview loop re-renders each tick (capped by refresh
        interval) and redraws the sixel/ANSI image. Ctrl-C to exit.
  - [ ] `donner_svg_tool --interactive`: same tick loop, plus the existing
        click-to-inspect flow keeps working (pick is against the vector DOM
        at current frame time).
  - [ ] `--verbose` dumps per-tick budget lines
        (`tick_N_ms: <style, layout, render, present>`).
  - [ ] Deterministic clock source (injectable) so tests of the CLI tick path
        are bit-reproducible.
- [ ] **Milestone 3: Editor integration**
  - [ ] Editor `RenderCoordinator` grows a "live playback" mode: the animation
        driver is ticked from a **timeline scrubber** — the scrubber IS the
        clock source, not the wall clock. Play advances the scrubber at 1×
        real time, pause freezes it, scrub jumps the driver to an arbitrary
        time and re-ticks. This gives deterministic replay inside the editor
        and makes "step one frame" natural.
  - [ ] ImGui timeline widget: play / pause button, scrubber, current-time
        readout, end-time readout.
  - [ ] Layer promotion for the animated entity uses
        `ScopedCompositorHint(entity, Animating)` so the compositor treats
        it the same as a dragged entity.
- [ ] **Milestone 4: Perf gates (shared with doc [0035](0035-perf_framework_and_analyzer.md))**
  - [ ] `donner_perf_cc_test` on `splash_animated.svg`: correctness half asserts
        "animation produces expected frame N transform value" (stays on PR
        gate). Wall-clock half asserts "60 fps end-to-end on M1 MBP"
        (nightly perf lane). Runs a headless render loop, not the CLI —
        CLI mode is for humans, tests hit the driver directly.
  - [ ] Instrumentation uses `DONNER_PERF_SCOPE` macros from doc 0035 —
        **not** raw Tracy calls — so every per-stage zone lands in the
        perf framework's ring buffer and is visible to (a) the editor's
        analyzer panel during live scrubbing, (b) the Perfetto trace
        export, and (c) the `donner_perf_analyzer` regression-diff CLI.
        The animation proof-of-life is doc 0035's first real customer.
  - [ ] Scopes carry the animated entity tag (see 0035's entity-tagged
        scopes) so "which element is the long pole" is answerable without
        re-running with a different instrumentation config.
- [ ] **Milestone 5: Measure-then-fix loop**
  - [ ] Identify the long pole (style? layout? composite? simple-span
        fast-path miss?).
  - [ ] Open a tracked issue per pole. Don't fix in this doc's scope — this
        doc is the driver, not the cure. Fixes that also improve drag perf
        (doc 0026) should land under whichever doc the fix more naturally
        fits; both are beneficiaries.

## Proposed Architecture

```
┌──────────────────┐     tick(t)      ┌───────────────────┐
│ RenderCoord      │ ───────────────► │ AnimationDriver   │
│ (editor canvas)  │                  │  - ActiveAnimation│
└──────────────────┘                  │    list           │
        │                             │  - interpolator   │
        │ OR                          └─────────┬─────────┘
        │                                       │ writes attr
┌──────────────────┐                            ▼
│ donner_svg_tool  │                 ┌───────────────────────┐
│  --preview       │───tick(t)──►    │ SVG DOM (ECS)         │
│  --interactive   │                 └───────────┬───────────┘
└──────────────────┘                             │ incremental invalidation
                                                 ▼
                                     ┌───────────────────────┐
                                     │ Style → Layout →      │
                                     │ Render → Present      │
                                     └───────────────────────┘
                                                 │
                                                 ▼ per-stage Tracy scope
                                          Frame budget gates
```

Key properties:

- **One tick source.** Editor, `--preview`, and `--interactive` all use the same
  `AnimationDriver` — whatever bottleneck the CI test finds is the same
  bottleneck the editor and terminal preview hit.
- **No new CLI flag.** Animation plays wherever there's a live rendering loop;
  static-output invocations ignore it. This mirrors how browsers handle it:
  load the SVG, if it has animations and you're displaying it live, they run.
- **No new invalidation path.** Attribute writes go through the *existing*
  mutation seam (the same one that editor tools use). This is a feature: if
  incremental invalidation can't handle 60 fps of attribute mutations, we want
  to know, and we want to fix it *there* rather than carving out a separate
  "animation-only" fast path that lies about system health.
- **No new compositor path.** Animated entities ride the same layer-promotion
  machinery drag uses (doc [0025](0025-composited_rendering.md)). The only
  animation-specific optimization is the **simple-span fast path** that skips
  render-to-texture for text spans with no filters/masks/clips — gated on an
  `isSimpleSpan` predicate so the compositor still owns correctness.
- **Shared perf instrumentation.** All per-stage scopes go through doc
  [0035](0035-perf_framework_and_analyzer.md)'s `DONNER_PERF_SCOPE` API, so
  the analyzer panel, Perfetto export, and CI regression diff all see the
  same event stream.
- **Hard budget assertions.** Not "usually fast enough" — explicit ms thresholds
  in the test, in line with the project's debugging-discipline rule ("no perf
  fixes without a repro that measures").

## Testing and Validation

- **Correctness.** Given the `splash_animated.svg` fixture rotating the
  "Animation" text at a known rate, at t = 500 ms the resolved transform
  must match the expected angle `± eps`. Run under the PR-gate half of
  `donner_perf_cc_test`.
- **Per-frame budget.** End-to-end frame time for `splash_animated.svg` stays
  under 16.7 ms on reference hardware for ≥ 95% of frames over a 60-frame run.
  Run under the nightly wall-clock lane of `donner_perf_cc_test`.
- **Stage breakdown.** Perf framework (doc 0035) scopes assign frame time to
  (style, layout, composited render, present) with an extra zone for
  compositor-layer vs simple-span fast path. A per-stage budget is also
  asserted so regressions point to the right owner.
- **Coupling with drag perf.** Every fix that moves the drag-end latency
  needle on `donner_splash.svg` (doc 0026) must also move the animation
  60 fps needle on `splash_animated.svg`, and vice versa. Tracked as a
  paired-regression check in the nightly perf lane.
- **CLI smoke test.** A short `donner_svg_tool --preview splash_animated.svg`
  run with an injected clock and a fixed frame cap produces bit-identical
  terminal output across runs — catches regressions in the CLI tick-to-present
  path that the direct-driver perf test can't see.

## Performance

- **Target:** 60 fps sustained (≤ 16.67 ms / frame) on `splash_animated.svg`
  on an **Apple M1 MacBook Pro** by the end of M4. Matches the reference
  hardware and budget used in docs 0025 and 0026.
- **Stretch:** 120 fps on the same fixture on the same hardware, gated to a
  nightly lane.
- **Budget anti-pattern to avoid:** no "total animation time / N frames = mean
  frame time" averages in the gate. Tail latency matters — a 100 ms GC pause on
  one frame out of 60 is a visible stutter and must fail the gate.

## Decisions

- **Reference hardware.** Apple M1 MacBook Pro, matching docs
  [0025](0025-composited_rendering.md) and
  [0026](0026-drag_end_latency.md). CI runners that differ from M1 MBP run
  a self-calibrated budget (target = 1.25× of a known-good M1 baseline
  scaled by a one-shot startup microbenchmark) so regressions stay
  attributable without requiring M1 runners for every PR.
- **Editor clock source.** Timeline scrubber, not wall clock. The scrubber
  widget is the only authority that advances `AnimationDriver` time in
  editor mode. Play = auto-advance at 1× real time; Pause = freeze; Scrub
  = jump. Deterministic replay, step-one-frame, and scrubbing-for-debug
  all fall out of this for free. Wall-clock-driven playback is a
  convenience of the scrubber's "play" state, not a separate path.
- **Terminal-preview frame pacing.** Cap at **30 fps** by default (33 ms
  tick interval), overridable with `--preview-fps=N`. Sixel / ANSI
  redraw cost on a typical terminal is not cheap, and a slow terminal
  must not be able to inflate or deflate the "editor is 60 fps" claim
  — terminal pacing is a display concern, not a perf-measurement
  concern.
- **Coupling with drag perf.** Resolved in favor of **shared
  infrastructure**. Same compositor layers, same invalidation machinery,
  same hardware target, same 16.67 ms budget. Animation is dragging
  driven by a clock.

## Open Questions

- **What happens when we're obviously below 60 fps?** Do we drop frames, or do
  we render "time-warped" (advance animation time but skip frames)? Probably
  "advance animation time, skip frames" to match what browsers do — but that's
  a decision to make once we measure.
- **Simple-span predicate definition.** Where exactly is the cutoff? Plain
  `<text>` with a solid fill is clearly simple; `<text>` with a gradient
  paint server probably still is; `<text>` with an SVG filter is not.
  Sketch the predicate during M2 and pin it during M5 once we measure.
