# Design: v1.0 Release

**Status:** Draft
**Author:** Jeff McGlynn (with Claude Opus 4.7)
**Created:** 2026-04-17

## Summary

Release checklist and implementation plan for shipping Donner **v1.0** — the production release.
This doc is the execution counterpart to the v1.0 section of
[`docs/ProjectRoadmap.md`](../ProjectRoadmap.md#v10--production-release-in-progress), which
remains the public-facing summary.

v1.0 bundles every workstream that was deferred out of v0.5 plus the retrospective items captured
in [0011-v0_5_release.md](0011-v0_5_release.md#v05-retrospective):

- **Animation** — the 9 phases of SMIL (`<animate>`, `<animateTransform>`,
  `<animateMotion>`, `<set>`, event-based timing).
- **Interactivity** — `EventSystem`, `SpatialGrid` hit testing, the
  `DonnerController` public API.
- **Composited rendering** — the layer-based compositor from
  [0025-composited_rendering.md](0025-composited_rendering.md), in-progress vNext work that
  was never part of v0.5's feature surface. Prerequisite for fluid editor dragging.
- **Interactive SVG editing API** — hybrid structured/freeform editing primitives from the
  ProjectRoadmap: structured API, partial re-parsing, reverse serialization, invalid-region
  tolerance.
- **Editor v1 (the app)** — ship `//donner/editor` at first-release quality, building on the
  in-progress MVP design in [0020-editor.md](0020-editor.md) and the render-pane UX design
  in [0025-editor_ux.md](0025-editor_ux.md). Extend beyond the MVP to include shape and text
  creation tools at minimum.
- **Scripting** — `donner::script` (QuickJS-NG + IDL codegen) per
  [0027-scripting.md](0027-scripting.md).
- **Conformance** — the manifest-driven SVG 1.1 + WPT + scripted conformance program in
  [0026-svg_conformance_testing.md](0026-svg_conformance_testing.md), plus contributing Donner
  back to the upstream resvg test harness.
- **Geode on real GPUs** — finishing [0017-geode_renderer.md](0017-geode_renderer.md) beyond
  software adapters / CI validation.
- **Parser hardening, DOM gaps, and entity lifecycle** — the items listed under each of those
  headings in the ProjectRoadmap.
- **Release-process + docs cleanup** — the retrospective items from 0011: BCR publish,
  Doxygen sidebar grouping, condensed element/filter index, binary-size report rendering,
  public-target visibility audit, Markdown element-list cleanup.
- **Security, optimization, and ecosystem** — AI-assisted security pass, perf/size/memory/
  compile-time audits, the "Donner Tiny" build profile, and the published comparison against
  lunasvg/resvg.

The user's top-of-mind list for v1.0 (in priority order) drives the first phases below:

1. Doxygen sidebar / Markdown element-list cleanup.
2. Composited rendering.
3. Geode renderer on a real GPU.
4. Sandboxing.
5. Scripting.
6. Animation.
7. resvg test-suite cleanup.
8. Conformance test suite.
9. `<a>` and `<switch>` element support.
10. Donner as a backend in the upstream resvg test harness.
11. BCR publish (debug v0.5 failure + re-publish).
12. Binary-size report in Doxygen rendering correctly.
13. **Editor v1** — ship the editor at first-release quality, extending the MVP design in
    [0020-editor.md](0020-editor.md) + [0025-editor_ux.md](0025-editor_ux.md) with shape and
    text creation tools at minimum.

Everything else from the ProjectRoadmap v1.0 section is included and sequenced into later phases —
**option (b)**: keep the full scope rather than deferring items to a v1.1. Scope can be
re-scoped down later if the schedule demands it.

## Goals

- Ship v1.0 with every item listed in the [v1.0 ProjectRoadmap
  section](../ProjectRoadmap.md#v10--production-release-in-progress) closed or explicitly
  deferred with a follow-up tracking issue.
- Meet all **Release Criteria** from the ProjectRoadmap v1.0 section:
  - Stable public API for rendering, editing, and authoring.
  - ≥90% line coverage across production code.
  - CSS3 gap analysis complete; every SVG2-referenced CSS property supported.
  - Published SVG2 conformance report (from the new conformance suite) with known limitations
    documented.
  - Performance and binary-size profiles documented; the "Donner Tiny" tier is usable.
  - Release documentation complete for embedders (embedding guide).
- Close out every item from the [v0.5 retrospective](0011-v0_5_release.md#v05-retrospective).
- Establish scripting and conformance as first-class Donner subsystems, so third-party parity
  checks against browsers and resvg are a routine part of CI.

## Non-Goals

- Shipping scripting features rejected in the scripting design's Non-Goals (HTML, JIT, network
  I/O, `MutationObserver`, etc. — see [0027-scripting.md](0027-scripting.md) §Non-Goals).
- Shipping SVG 1.1-only deprecated features: `enable-background`, `BackgroundImage`/
  `BackgroundAlpha`, `<tref>`, `kerning`, `glyph-orientation-*`. Those remain permanently
  `Params::Skip()`.
- Replacing the resvg test suite. It stays as Donner's primary cross-engine regression lane;
  the new conformance suite supplements it.
- A 100% resvg pass rate. Known architectural gaps (e.g. feImage float rendering path) stay
  tracked in [0021-resvg_feature_gaps.md](0021-resvg_feature_gaps.md).
- Running the full WPT repository in CI. The conformance design vendors a curated subset.
- New rendering backends beyond Skia / tiny-skia / Geode.

## Next Steps

1. Review this doc (DesignReviewBot + user) and freeze the phase list before implementation
   starts. Individual phases are already tracked by their own design docs — this is the
   coordination layer.
2. Socialize phase ordering. The default below is "retrospective bugs → close-out → animation →
   interactivity → scripting → editing → conformance → optimization → release", but several
   phases can run in parallel once phase 1 is done.
3. Pick the first phase to start. The user's stated top priority is the Doxygen/Markdown docs
   cleanup (Phase 1) because it unblocks every later "the sidebar is unusable" complaint.

## Implementation Plan

High-level milestones. Each phase points at the existing design doc that owns the detailed plan —
this doc only tracks "is the v1.0-gating slice of that work done".

- [ ] **Phase 1** — Release-process + docs cleanup (v0.5 retrospective carry-over).
- [ ] **Phase 2** — Finish in-flight vNext workstreams (sandboxing, composited rendering,
  Geode-on-GPU) — never shipped in v0.5, land them for v1.0.
- [ ] **Phase 3** — SVG feature gaps: `<a>`, `<switch>`, and the P0/P1 items from
  [0024-proposed_issues_2026q2.md](0024-proposed_issues_2026q2.md).
- [ ] **Phase 4** — Animation (SMIL Phases 1–9) + animation test suite.
- [ ] **Phase 5** — Interactivity (`EventSystem`, `SpatialGrid`, `DonnerController`).
- [ ] **Phase 6** — Scripting (`donner::script`) per
  [0027-scripting.md](0027-scripting.md).
- [ ] **Phase 7** — Interactive SVG editing API (structured API, partial re-parsing, reverse
  serialization, invalid-region tolerance). The library surface that Phase 8 consumes.
- [ ] **Phase 8** — Editor v1 (the app): graduate `//donner/editor` from MVP to first-release
  quality per [0020-editor.md](0020-editor.md) + [0025-editor_ux.md](0025-editor_ux.md),
  adding shape and text creation tools at minimum.
- [ ] **Phase 9** — Parser hardening + CSS3 gap closure.
- [ ] **Phase 10** — Incremental invalidation completion + entity lifecycle + DOM gaps.
- [ ] **Phase 11** — Conformance program per
  [0026-svg_conformance_testing.md](0026-svg_conformance_testing.md), plus resvg suite cleanup
  and upstream resvg harness contribution.
- [ ] **Phase 12** — Security pass (AI-assisted audit + fuzzer expansion).
- [ ] **Phase 13** — Performance, binary size, memory, compile time, "Donner Tiny" profile.
- [ ] **Phase 14** — Ecosystem: comparison publication against lunasvg / resvg.
- [ ] **Phase 15** — Documentation: design-docs-to-developer-docs conversion, embedding guide,
  in-code docs audit.
- [ ] **Phase 16** — Release: final validation, build report, tag, publish, BCR publish
  verified on the v1.0 tag.

---

### Phase 1: Release-process + docs cleanup

Carry over from the [v0.5 retrospective](0011-v0_5_release.md#v05-retrospective). These are
small, high-visibility fixes that should land before v1.0 feature work to avoid re-surfacing at
the next release cut.

- [ ] **Doxygen sidebar grouping for design docs** — Group all `docs/design_docs/*.md` under a
  single "Design Docs" section in the Doxygen tree instead of flattening them into top-level
  sidebar entries.
- [ ] **Condense filter/element enumeration in Doxygen** — Replace the per-primitive sidebar
  explosion with a single "All Elements" / "Filter Primitives" index page.
- [ ] **Binary-size report renders in Doxygen** — Fix the `docs/build_report.md` integration so
  the hosted Doxygen site shows the binary-size tables and feature matrix instead of an empty
  treeview.
- [ ] **Markdown element-list cleanup** — Shorten the enumerated SVG element lists in README.md
  and other Markdown docs. Collapse the 17 filter primitives and similar categories behind
  named shortcuts with a link to the canonical list.
- [ ] **BCR publish** — Root-cause the v0.5.0 BCR publish failure documented in
  [0018-bcr_release.md](0018-bcr_release.md). Fix the tooling, verify end-to-end on a v0.5.x
  (or v1.0-rc) tag, and re-run before the v1.0 tag.
- [ ] **Public-target visibility audit** — Reduce Bazel `visibility` across the tree to
  `//donner/...`-private by default. The `//donner/editor/...` subtree is the biggest
  offender and should be fixed first. Anything deliberately public gets an explicit
  `visibility = ["//visibility:public"]` and a comment explaining why.

### Phase 2: Finish in-flight vNext workstreams

Three vNext workstreams existed during v0.5 development but were **not** part of v0.5's
feature surface — code landed on `main` but none was enabled/exposed for v0.5 users. They are
the first feature work to finish for v1.0.

- [ ] **Sandboxing** — Finish the remaining milestones in
  [0023-editor_sandbox.md](0023-editor_sandbox.md). Scaffolding (wire-format fuzzer,
  `sandbox_diff` CLI, hardened-child test fixes) already landed; the end-to-end hardened
  editor path still needs to ship.
- [ ] **Composited rendering** — [0025-composited_rendering.md](0025-composited_rendering.md)
  graduates from Draft → Shipped. Layer-based caching that keeps editor dragging fluid.
- [ ] **Geode renderer on a real GPU** — Finish [0017-geode_renderer.md](0017-geode_renderer.md).
  Phase 0–2 and resvg MSAA are green on software adapters; Geode is not yet a user-facing
  backend and has not been validated on physical GPU hardware.

### Phase 3: SVG feature gaps

Small, user-requested element additions plus the P0/P1 queue from 2026 Q2.

- [ ] **`<a>` element support** — Render as a grouping element. Issue S18 in
  [0024-proposed_issues_2026q2.md](0024-proposed_issues_2026q2.md). Small scope, 3 resvg tests.
- [ ] **`<switch>` element + `systemLanguage`** — Conditional processing per
  [SVG2 §5.9](https://www.w3.org/TR/SVG2/struct.html#ConditionalProcessing). Issue S4. Medium
  scope, ~15 resvg tests.
- [ ] **P0 quick wins** — S1 (CSS filter functions at render time, 30 tests) and S2
  (`transform-origin` as a presentation attribute, 20 tests).
- [ ] **P1 text baselines** — S5 (`dominant-baseline` full keyword set) + S7
  (`alignment-baseline` full keyword set).
- [ ] **P1 rendering** — S6 (`context-fill` / `context-stroke`), S8 (`paint-order`).
- [ ] **P1 image / viewport** — S12 (intrinsic sizing), S3 (`<image>` sizing), S11 (`<use>` of
  inline `<svg>`).
- [ ] **`<symbol>` refX/refY units** ([#318](https://github.com/jwmcglynn/donner/issues/318))
  and `<marker>` attribute units
  ([#316](https://github.com/jwmcglynn/donner/issues/316)) — both listed under SVG Feature Gaps
  in the ProjectRoadmap.

### Phase 4: Animation

9 phases of SMIL animation per the ProjectRoadmap. Needs its own design doc once the work
starts (currently only the top-level ProjectRoadmap entry exists).

- [ ] **Animation design doc** — Drop a dedicated `docs/design_docs/00NN-animation.md` before
  implementation begins.
- [ ] **Phases 1–9** — Timing model, interpolation engine, sandwich composition, attribute
  targeting, `<animate>`, `<animateTransform>`, `<animateMotion>`, `<set>`, event-based timing.
- [ ] **Animation test suite** — Comprehensive tests covering timing edge cases, interpolation
  correctness, and event-based triggers.

### Phase 5: Interactivity

- [ ] **Phases 1–6** — `EventSystem`, `SpatialGrid`-accelerated hit testing, event dispatch
  (mouse, pointer), CSS `cursor` property, `DonnerController` public API
  (`addEventListener`, `elementFromPoint`, `findIntersectingRect`, `getWorldBounds`),
  incremental spatial-index updates.

### Phase 6: Scripting

Owned by [0027-scripting.md](0027-scripting.md). This phase's v1.0 gate is completion of
milestones **M1–M7** from that doc.

- [ ] **M1** — QuickJS-NG runtime integration, sandbox, diagnostics, fuzz harness.
- [ ] **M2** — `.donner_idl` compiler and first generated interface.
- [ ] **M3** — Core DOM: `Node`, `Element`, `Document`, `getElementById`, selectors.
- [ ] **M4** — Style object + CSS custom-property unification.
- [ ] **M5** — Event dispatch (capture/bubble, pointer + load + SMIL events).
- [ ] **M6** — SVG geometry IDL types (`SVGLength`, `SVGPoint`, `SVGMatrix`, `SVGNumber`,
  `SVGRect`).
- [ ] **M7** — Real-world corpus + release gate (the M7 gate in the scripting doc is itself
  scoped as the scripting-subsystem release criterion).

### Phase 7: Interactive SVG editing API

The library-surface half of the flagship v1.0 editing feature from the ProjectRoadmap. This
phase ships the Donner-side primitives; Phase 8 consumes them from the editor app.

Builds on Phase 2 (composited rendering), Phase 5 (interactivity), and optionally Phase 6
(scripting, where editor behavior is scriptable).

- [ ] **Structured editing API** — Programmatic DOM mutations propagating through ECS with
  incremental re-render.
- [ ] **Partial re-parsing** — Parse only the changed region when source text is edited and
  splice updates into the live document.
- [ ] **Reverse serialization** — Splice editor-originated operations back into the source text
  while preserving surrounding formatting. Enables source → DOM → visual edit → source
  round-trips.
- [ ] **Invalid-region tolerance** — Editing API does not crash or lose state on temporarily
  invalid SVG during freeform text editing.

### Phase 8: Editor v1 (the app)

Graduate `//donner/editor` from its in-tree MVP (landed during v0.5 development) to a
first-release-quality user-facing editor application. This is the visible half of the
editing story — Phase 7 provides the underlying library surface.

Owned by [0020-editor.md](0020-editor.md) (overall editor design, migration, command queue,
mutation seam) and [0025-editor_ux.md](0025-editor_ux.md) (viewport, zoom, pan, click math,
menu bar, multi-select, marquee, gestures).

- [ ] **Finish the MVP design (0020)** — Close out the remaining milestones from 0020 beyond
  what landed during v0.5 development.
- [ ] **Ship the viewport / interaction rewrite (0025)** — Single-source-of-truth
  `ViewportState`, exact click math at all zoom/pan/DPR combinations, no size-flicker on edit,
  smooth drags, pinch/pan gestures, multi-select, marquee.
- [ ] **Shape creation tools** — Minimum set: rectangle, ellipse/circle, line, polyline,
  polygon, path. Each produces a well-formed DOM mutation via the Phase 7 structured API,
  with preview + commit semantics.
- [ ] **Text creation tools** — Click-to-place text insertion with live font selection,
  size/weight/anchor controls, and editing of existing `<text>` / `<tspan>` nodes.
  Integrates with the text pipeline from [0010-text_rendering.md](0010-text_rendering.md).
- [ ] **Tool UX polish** — Selection tool as the rest state; tool picker in the menu bar and
  keyboard-shortcutted; tool cursors; escape to cancel in-progress creation; undo integrates
  with `UndoTimeline`.
- [ ] **Editor release-gate** — User-facing editor passes a scripted acceptance pass
  (create-edit-save-reload round-trip, at least one shape and one text insertion in every
  supported tool), loads a corpus of real-world SVGs without visible glitches, and has
  headless tests for every invariant pinned in 0025.
- [ ] **Docs** — Editor quick-start + shape/text tool reference as part of the Phase 15
  documentation pass.

### Phase 9: Parser hardening + CSS3 gap closure

- [ ] **`ParseWarning` type** — First-class warning type replacing the current
  `vector<ParseError>` pattern; warnings vs errors distinct at the type level.
- [ ] **Source location audit** — Verify every current parse error reports the correct source
  location.
- [ ] **Full source ranges** — Extend diagnostics to carry full start + end source ranges.
- [ ] **Streaming CSS parser** — Consider making the CSS parser streaming (possibly via C++20
  coroutines). Add source-range and incremental-update support parity with the XML parser.
- [ ] **XML parser conformance** — Fix the known non-conforming `Name` token acceptance
  ([#304](https://github.com/jwmcglynn/donner/issues/304)) and any related bugs.
- [ ] **CSS3 gap closure** — Audit CSS3 property/selector support against every SVG2-referenced
  property. Close gaps in selectors, cascading, specificity, shorthand expansion, value
  parsing.

### Phase 10: Incremental invalidation + entity lifecycle + DOM

- [ ] **Selective per-entity recomputation** — Finish the partially-implemented v0.5 path from
  [0005-incremental_invalidation.md](0005-incremental_invalidation.md). Layout/text/shape/
  paint/filter passes should respect dirty flags and skip clean entities instead of full
  teardown.
- [ ] **CSS differential restyling** — When a stylesheet or class attribute changes, determine
  which selector matches changed and re-resolve only affected elements.
- [ ] **Node removal cleanup** — Destruction hooks tear down components, release resources, and
  remove entities from spatial indices / caches. Currently removed entities leak in the ECS
  registry.
- [ ] **SVG2 DOM gap analysis** — Audit current DOM surface against the full SVG2 DOM spec.
- [ ] **Close DOM gaps** — Implement missing interfaces/attributes/methods, prioritizing those
  needed for Phase 6 (scripting), Phase 7 (editing API), and Phase 8 (editor app).

### Phase 11: Conformance

Owned by [0026-svg_conformance_testing.md](0026-svg_conformance_testing.md). This phase's v1.0
gate is completion of M1–M4 from that doc, plus the two roadmap items on resvg.

- [ ] **M1** — Manifest format + Bazel overlays + SVG 1.1 filter pilot.
- [ ] **M2** — Static SVG 2 / WPT reftest lane.
- [ ] **M3** — CI policy, reporting, and documentation.
- [ ] **M4** — Scripted / DOM / animation conformance on top of `donner::script` (depends on
  Phase 6).
- [ ] **resvg test suite cleanup** — File, triage, and close out issues for current skipped /
  thresholded tests. Tracked via
  [0021-resvg_feature_gaps.md](0021-resvg_feature_gaps.md) and
  [0009-resvg_test_suite_bugs.md](0009-resvg_test_suite_bugs.md).
- [ ] **Add Donner to upstream resvg test harness** — External contribution: Donner as a
  first-class backend in the `linebender/resvg-test-suite` repository.
- [ ] **Publish SVG2 conformance report** — Required by the v1.0 Release Criteria in the
  ProjectRoadmap. Drop output in `docs/` and link from the release notes.

### Phase 12: Security pass

- [ ] **AI-assisted security audit** — Comprehensive review of XML, CSS, SVG attribute, and
  external-reference input paths.
- [ ] **Fuzzer expansion** — Add fuzzers for under-covered surfaces: CSS, filter parameters,
  animation timing, edit/patch paths, scripting IDL surface (codegen-emitted, per
  0027-scripting.md).
- [ ] **Integrate with [0012-continuous_fuzzing.md](0012-continuous_fuzzing.md)** — Make the
  dockerized always-on fuzzing harness the default for the new fuzzers.

### Phase 13: Performance, size, memory, compile time

- [ ] **End-to-end profiling** — Profile parsing, style resolution, layout, rasterization.
  Identify remaining hot paths beyond the filter-SIMD work already landed.
- [ ] **Code / binary size audit** — Ensure text, filters, animation, and JS compile out
  cleanly when disabled. Apply LTO + gc-sections per B9 in
  [0024-proposed_issues_2026q2.md](0024-proposed_issues_2026q2.md).
- [ ] **Memory audit** — ECS component sizes, pixmap allocations, filter intermediate buffers,
  steady-state vs peak.
- [ ] **Compile time** — Forward-declaration headers, modular EnTT (B4), split monolithic
  `svg_core` / renderer files (B11, B14).
- [ ] **"Donner Tiny" build profile** — Minimal-footprint tier stripping text/filters/
  animation/JS. Document size impact per optional module. Pre-defined profiles: `tiny`,
  `standard`, `full`.

### Phase 14: Ecosystem comparison

- [ ] **Lunasvg / resvg comparison** — Publish a detailed comparison covering feature support,
  conformance, performance, API design, binary size, build complexity. Include reproducible
  benchmarks and conformance results.

### Phase 15: Documentation

- [ ] **Design docs → developer docs** — Convert shipped design documents into developer-facing
  architecture documentation. Remove planning/status artifacts, keep how-it-works content.
- [ ] **In-code documentation cleanup** — Audit Doxygen annotations and comments on every
  public header. Resolve the waived v0.5 Doxygen warning backlog.
- [ ] **Embedding guide** — End-to-end guide for integrating Donner into applications: build
  configuration, feature toggles, rendering setup, common workflows.

### Phase 16: Release

Mirrors [0011-v0_5_release.md](0011-v0_5_release.md) Phase 14. The build-report commit is the
tagged commit.

- [ ] **All release-blocking code on `main`** before the build-report commit.
- [ ] **Update `RELEASE_NOTES.md`** with the final v1.0 highlights.
- [ ] **Generate build report** as the dedicated release commit.
- [ ] **CI green on the build-report commit.**
- [ ] **Tag `v1.0.0`** on the build-report commit; push; `gh release create`.
- [ ] **Verify release artifacts** — Linux/macOS binaries, SLSA attestations, BCR publish
  (now expected to work end-to-end after Phase 1's BCR fix).
- [ ] **Post-release** — ProjectRoadmap update, announcement.

---

## Release Criteria

Copied verbatim from
[ProjectRoadmap.md → v1.0 → Release Criteria](../ProjectRoadmap.md#release-criteria), so this
doc is the single source of truth for execution:

- All v1.0 issues closed.
- SVG2 conformance report published with known limitations documented.
- Stable API surface for rendering, editing, and authoring operations.
- ≥90% code coverage across production code.
- CSS3 gap analysis complete, all SVG2-referenced properties supported.
- Performance and binary-size profiles documented.
- Release documentation complete for embedders.

Plus the v0.5 retrospective items (all addressed in Phase 1):

- BCR publish working end-to-end on the v1.0 tag.
- Doxygen sidebar organized; binary-size report rendering; element lists condensed.
- Public-target visibility tightened, particularly under `//donner/editor/...`.

## Proposed Architecture

This is a coordination doc, not a feature design — the architectural changes live in the
per-phase design docs:

- Animation → a forthcoming `00NN-animation.md` (Phase 4).
- Interactivity → ProjectRoadmap §Interactivity (Phase 5), to be promoted to a design doc.
- Composited rendering → [0025-composited_rendering.md](0025-composited_rendering.md).
- Scripting → [0027-scripting.md](0027-scripting.md).
- Conformance → [0026-svg_conformance_testing.md](0026-svg_conformance_testing.md).
- Editor sandbox → [0023-editor_sandbox.md](0023-editor_sandbox.md).
- Geode → [0017-geode_renderer.md](0017-geode_renderer.md).
- Incremental invalidation → [0005-incremental_invalidation.md](0005-incremental_invalidation.md).
- BCR → [0018-bcr_release.md](0018-bcr_release.md).
- Continuous fuzzing → [0012-continuous_fuzzing.md](0012-continuous_fuzzing.md).
- Coverage → [0013-coverage_improvement.md](0013-coverage_improvement.md).

Parallelism: Phases 1, 2, and 3 are mostly independent and can run in parallel. Phase 6
(scripting) blocks Phase 11 M4 (scripted conformance) and parts of Phases 7/8 that need
scripting hooks. Phase 2 (composited rendering) blocks the fluid-dragging parts of Phase 8
(the editor app). Phase 7 (editing API) gates Phase 8 on the structured-mutation and
reverse-serialization primitives — Phase 8 shape/text creation tools can start on the API as
soon as the structured-mutation surface is usable, ahead of partial re-parsing / reverse
serialization.

## Security / Privacy

v1.0 adds two major new trust boundaries beyond v0.5's parser / renderer surface:

- **Scripting.** Untrusted JS running inside Donner. The threat model, sandbox, and fuzz
  strategy are owned by [0027-scripting.md](0027-scripting.md) §Security / privacy.
- **Editor sandbox.** Process isolation for editor parser/renderer. Owned by
  [0023-editor_sandbox.md](0023-editor_sandbox.md).

Donner's global invariant — "must safely handle untrusted input and must never crash" — extends
unchanged across the scripting boundary. Phase 12 (Security pass) is the verification step.

## Testing and Validation

- **Existing lanes stay green**: unit tests, `renderer_tests`, `resvg_test_suite`, all 21+
  fuzzers, CMake and Bazel builds across all backends.
- **New conformance lane** (Phase 11) adds SVG 1.1 filter corpus, static SVG 2 / WPT reftests,
  and — on top of scripting — scripted / DOM / animation conformance.
- **Animation test suite** (Phase 4) covers timing, interpolation, event-based triggers.
- **Scripting fuzz targets** (Phase 6) are emitted by IDL codegen so every new script-exposed
  interface ships with fuzz coverage.
- **Coverage target**: ≥90% line coverage across production code (v0.5 shipped at 81.7%).
- **Binary-size, build-time, and perf profiles** (Phase 13) are captured in the build report
  and become a release-gate artifact.

## Dependencies

- **QuickJS-NG** — new dep introduced by scripting (Phase 6). Licensed MIT; pinned via
  `non_bcr_deps`. See [0027-scripting.md](0027-scripting.md) §Dependencies.
- **WPT subset** — vendored curated snapshot added by the conformance program (Phase 11). See
  [0026-svg_conformance_testing.md](0026-svg_conformance_testing.md) §Dependencies.
- **Existing deps** unchanged.

## Open Questions

- **Phase ordering.** Animation (Phase 4) vs scripting (Phase 6) ordering is the biggest
  open call. Scripting first unblocks scripted conformance (Phase 11 M4) sooner; animation
  first is a cleaner user-facing story. Default here: animation first (matches user's stated
  priority).
- **Editor scope cut.** Phases 7 (API) and 8 (app) are both large. If schedule pressure
  appears, Phase 7's "reverse serialization" and Phase 8's text creation tool are the most
  deferrable items — the rest is load-bearing for the v1.0 interactive-editor story.
- **Deprecate pre-option-(a) scope cut.** Keep revisiting whether v1.0 should be narrowed to
  the user's top-12 list and everything else deferred to v1.1. Default today: full ProjectRoadmap
  scope.

## Future Work

- **v1.1+ ideas** that were debated for v1.0 and intentionally left out today: MutationObserver
  / script-visible change feed, cross-document scripting, HTML support, JIT, additional
  rendering backends beyond Geode, full WPT integration.
