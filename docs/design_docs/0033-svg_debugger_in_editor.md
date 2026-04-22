# Design: SVG Debugger integrated into Editor

**Status:** Draft (one-pager)
**Author:** Claude Opus 4.7
**Created:** 2026-04-21

## Summary

Build a RenderDoc-style SVG rendering inspector as a first-class panel inside the in-tree
Donner editor (`//donner/editor`, see design doc [0020](0020-editor.md)). The debugger
lets a developer scrub through a rendered SVG's draw-call timeline, inspect each
intermediate layer, click to pick the SVG element at a point, and compare live output
against the resvg / Skia reference. Solves the pain of "why is this filter producing
the wrong color" without `printf`-driven debugging or bisecting against the
golden-image suite by hand.

**Scope note:** the debugger inspects the **vector** side of the pipeline —
the SVG DOM, the ECS entities, the resolved paint/filter/transform state, and the
draw-op stream emitted by the renderer. It is not a pixel-level / fragment-level
debugger. Pixel-level questions ("what did WebGPU actually draw on this tile?") stay
with RenderDoc / Xcode Metal capture.

Tracks GitHub issue
[#443 — SVG Rendering Inspection Tool (like renderdoc)](https://github.com/jwmcglynn/donner/issues/443),
which already gestures at a record/replay backend + ImGui viewer. This doc names
the concrete pieces and sequences them.

Deliberate analogue: Skia's [`.skp` debugger](https://debugger.skia.org/). Donner's
version runs natively, inside the editor we already build, against both the tiny-skia
and Geode backends.

## Goals

- **Pick-an-element tool.** Click a point → highlight the SVG element(s) at that
  point. Uses the existing `RenderingContext::hitTestEntity` /
  `EditorApp::hitTest` API — same hit-testing logic the select tool already uses.
  Backend-agnostic because it runs against the vector DOM, not the pixel buffer.
- **Draw-call timeline.** Scrub through every draw op the renderer emitted for the
  current frame. At each step, show (a) the incremental framebuffer state, (b) the
  element / CSS rule responsible, (c) the resolved paint / filter / transform state,
  (d) the ECS entity and its attached components.
- **Layer explorer.** Inspect intermediate filter subregions, mask layers, and
  composited offscreens as first-class images. "Save PNG" on any of them.
- **Backend comparison.** Side-by-side tiny-skia vs Geode output for the same frame,
  with a diff overlay. Uses the existing `RendererTestBackendGeode` harness.
- **Golden suite integration.** "Open this resvg golden image in the debugger" — a
  single-click from a failing test, with the expected/actual/diff images already loaded
  and aligned with the draw-call timeline.
- **No production-path cost.** The debugger's instrumentation lives behind a
  `debugger-instrumentation` feature flag and has zero overhead in default builds.

## Non-Goals

- **Not a time-travel debugger.** Single frame at a time; no animation timeline scrub
  across frames. (Revisit once the animation system lands — see doc 0034.)
- **Not a GPU command-level debugger.** For "what did WebGPU actually do", keep using
  RenderDoc / Xcode Metal capture directly against a Geode build. The Donner debugger
  is one layer above: SVG ops → renderer ops, not renderer ops → GPU ops.
- **Not a pixel / fragment debugger.** No per-pixel "who wrote this pixel" answer.
  Picking uses vector-side hit testing — if the vector says the ellipse covers the
  point, that's the answer, even if a filter displaced the pixel elsewhere. For
  per-pixel attribution use a GPU capture tool.
- **Not a visual SVG editor for debugging.** DOM mutations belong to the editor proper;
  the debugger is strictly a **read-only** inspector of a rendered frame.
- **Not a cross-tool artifact format.** Recording files are build-hash-tied and not
  meant for long-term storage or exchange.

## Next Steps

1. Land the draw-call timeline hook: every renderer backend emits a `DebugTrace`
   object (list of steps + intermediate framebuffers) behind the feature flag. This
   gives us a concrete data structure to build UI against.
2. Ship the **pick-an-element** tool as MVP — it's the single feature with the
   highest marginal debugging value and can ride on top of step 1.
3. Prototype the layer explorer for filters (the current pain-point: issue
   [#552](https://github.com/jwmcglynn/donner/issues/552) and the filter-effects
   work in doc [0014](0014-filter_performance.md)) — filters are where this tool
   earns its keep.

## Implementation Plan

- [ ] **Milestone 1: DebugTrace data model**
  - [ ] Define `DebugTrace` (list of `DebugStep`: op, entity, resolved paint state,
        framebuffer snapshot-handle) in `donner/svg/renderer/common`.
  - [ ] Add `RendererDriver::setDebugTraceSink(...)` and populate at each draw-op emit
        site. Behind `//donner:debugger-instrumentation` feature flag.
  - [ ] Golden: an explicit fixture that renders `donner_splash.svg` and asserts
        `DebugTrace` step count / entity coverage is stable.
- [ ] **Milestone 2: Pick-an-element**
  - [ ] Editor panel: click canvas → call `EditorApp::hitTest(point)` (vector-side,
        already wired) → highlight the resulting entity in the tree and show its
        rule chain + resolved paint state in the inspector pane.
  - [ ] "Pick all" mode: return every element whose geometry covers the point
        (not just the topmost hit), surfaced as a hit-stack panel so the user can
        walk occluded elements.
  - [ ] Rect marquee picking reuses `EditorApp::hitTestRect` for multi-select in
        the inspector.
- [ ] **Milestone 3: Draw-call timeline panel**
  - [ ] ImGui timeline UI with step-scrubbing and framebuffer preview.
  - [ ] "Jump to source" — draw op → originating XML line in the text editor pane.
- [ ] **Milestone 4: Layer explorer**
  - [ ] Filter pipeline steps exposed as inspectable intermediates.
  - [ ] Mask / clip offscreens exposed as inspectable intermediates.
- [ ] **Milestone 5: Backend comparison view**
  - [ ] Side-by-side tiny-skia vs Geode. Diff overlay. Threshold match with resvg suite.
- [ ] **Milestone 6: Golden-suite integration**
  - [ ] `bazel test` failure message includes a one-line command to open the failing
        test's SVG in the debugger with expected/actual/diff pre-loaded.

## Proposed Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ Editor (//donner/editor)                                        │
│                                                                 │
│  ┌─────────────┐   ┌──────────────────┐   ┌─────────────────┐  │
│  │ Text editor │   │ Canvas (live)    │   │ Debugger panels │  │
│  │  pane       │◄──┤ + pick cursor    │──►│ - Timeline      │  │
│  └─────────────┘   └────────┬─────────┘   │ - Layer tree    │  │
│                             │             │ - Entity inspector│ │
│                             │             │ - Backend diff  │  │
│                             │             └────────┬────────┘  │
│                             ▼                      │           │
│                     ┌────────────────┐             │           │
│                     │  RenderCoord.  │◄────────────┘           │
│                     │  (AsyncRenderer)│                        │
│                     └────────┬────────┘                        │
│                              │                                 │
│  ┌──────────────────┐        │                                 │
│  │ EditorApp::      │◄───────┴── click → hitTest(point)        │
│  │ hitTest(point)   │           (vector-side; no pixel readback)│
│  │ hitTestRect(r)   │                                          │
│  └──────────────────┘                                          │
└──────────────────────────────┼─────────────────────────────────┘
                               ▼
              ┌──────────────────────────────────┐
              │ RendererDriver                   │
              │  + DebugTraceSink (feature flag) │
              └────┬────────────────────┬────────┘
                   ▼                    ▼
           tiny-skia backend     Geode backend
          (no picking hook)    (no picking hook)
```

Key properties:

- **Picking is backend-free.** Vector-side hit testing lives above the renderer, so
  the debugger needs zero per-backend picking support. tiny-skia and Geode don't
  grow an ID-buffer pass; they stay focused on pixels.
- **Instrumentation is opt-in at link time.** Default consumers of `RendererDriver`
  pay nothing. The debugger binary is the only consumer that sets `DebugTraceSink`.
- **Backend-agnostic interface.** `DebugTrace` is a renderer-level abstraction; both
  tiny-skia and Geode implement it identically.
- **Feeds naturally off the editor's existing seams.** `AsyncRenderer` already owns
  frame orchestration, and `EditorApp` already owns picking; the debugger is a
  consumer of both, not a sibling.

## Testing and Validation

- **Unit tests** for `DebugTrace` population across both backends. A trace count
  regression is a regression.
- **Picking tests** extend the existing `RenderPaneClick_tests.cc` / `SelectTool`
  coverage: for representative SVG fixtures, clicking a known point returns the
  expected entity (and the expected hit-stack for the "pick all" mode).
- **Layer-explorer equivalence** — saving a filter intermediate as PNG and piping it
  through `donner_svg_tool` must give bit-identical output to the in-debugger preview.
- **Editor integration tests** in `donner/editor/tests/` drive the debugger panels
  headlessly (same harness as `AsyncRenderer_tests.cc`).

## Dependencies

- Depends on the editor landing in-tree (doc 0020) — blocks MVP.
- **Co-designed with the perf framework (doc [0035](0035-perf_framework_and_analyzer.md)).**
  The debugger and the perf analyzer are two panes of one tool: `DebugTrace`
  and perf events share a frame ID and an entity ID, so "click a frame → see
  per-element cost" is a straight join, not a fuzzy correlation. Neither doc
  ships its panel without the other.
- **Does not** depend on the IPC framework (doc 0032). If the editor moves to a
  sandboxed out-of-process renderer, the debugger follows along naturally because
  `DebugTrace` is one more IPC interface; until then it's in-process.

## Open Questions

- **Hit-stack API shape.** `hitTestEntity` takes an entity + point; the debugger
  wants "give me *all* elements at this point, topmost-first" without iterating
  every entity from the caller. Is that a new `hitTestAll(point)` method on
  `RenderingContext`, or does the debugger walk the traversal order itself?
- **Hit-testing through invisible regions.** A `<g visibility="hidden">` or
  `pointer-events="none"` subtree is ignored by the select tool but the debugger
  probably wants to *see* it (the whole point is inspecting "what's there?").
  Needs a "debugger mode" flag on the hit-test call that ignores those filters.
- **Recording format stability.** Do we want a "share a reproducer" recording format
  between contributors, or is the debugger strictly live-only for now?
  (Tentative: live-only; record/replay belongs to the IPC framework's file transport.)
- **Discoverability.** Is the debugger a keyboard-shortcut-away panel in the main
  editor, or a separate `donner_svg_tool debug foo.svg` CLI entry point? Probably both.
