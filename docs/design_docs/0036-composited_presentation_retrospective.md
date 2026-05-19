# Retrospective: Composited Presentation Repro and Flat-Mode Removal

**Status:** Retrospective
**Type:** Retrospective
**Author:** Codex GPT-5
**Created:** 2026-05-19

## Summary

The editor now presents document pixels through a single composited path. The
former flat texture display path is removed from the UI and MCP mirrors; full
document snapshots still exist only as internal inputs for a single
full-canvas composited tile. The filtered O-to-R drag repro is covered by a
GUI-scheduled `.rnr` replay and by GL readback screenshot tooling that captures
the same canvas area the operator sees.

The main retrospective finding is that the team spent too many iterations
debugging plausible internal state instead of proving the visible frame. The
representative test became trustworthy only after it compared the presented
bitmap, used the editor's asynchronous frame ordering, and could produce a
human-checkable screenshot of the exact failing frame.

## Scope

Reviewed stack:

- Editor presentation refactor: `AsyncRenderer`, `RenderCoordinator`,
  `RenderPanePresenter`, `GlTextureCache`, `CompositedPresentation`, and
  `SelectTool`.
- MCP mirror: `EditorControlSession`, display-frame JSON, pixelmatch diff
  artifacts, `.rnr` recording/replay, and editor-shell schedule simulation.
- Regression tests: `.rnr` replay tests, MCP session tests, async renderer
  tests, drag-release pop-back tests, GL replay screenshots, and compositor
  tests affected by safe fallback behavior.
- Docs and process rules: `AGENTS.md`, `CLAUDE.md`, design docs 0025, 0029,
  0034, and 0035.

This retrospective does not redesign the compositor's layer resolver. It does
call out where the current stack still falls short of the new "all visible
content is presentable through compositing" policy.

## Outcome

Current decisions:

1. **Composited presentation is the only editor display path.** A pre-drag
   render is represented as one full-canvas composited tile, not as a separate
   flat texture.
2. **`--experimental` remains a no-op CLI contract.** The editor accepts it for
   existing developer scripts. `--no-experimental` is removed. New `.rnr`
   recordings default legacy `exp` metadata to `false`; old recordings still
   decode the field.
3. **MCP diff output uses pixelmatch only.** Raw private pixel comparison is
   removed from the MCP path; mismatches write `actual_*`, `expected_*`, and
   `diff_*` artifacts.
4. **The representative repro is a presented-frame assertion.** Metadata such
   as "cached entity id matches active entity" is useful diagnostics, but the
   acceptance criterion is that the visible bitmap pixelmatches the eventual
   correct frame.
5. **Editor and MCP replay share render-posting policy.**
   `PresentationRenderScheduler` owns the common decision for regular renders,
   composited prewarm, active-drag capture, settled-selection refresh, and the
   interaction hint attached to the next `RenderRequest`.

## Code Review Findings

### Fixed During Retrospective

- **Vestigial toggle test removed.** `SelectTool_tests` still exercised
  `CanToggleCompositedRendering`, even though the menu toggle and helper were
  removed. The test is deleted.
- **Dead state removed.** `RenderCoordinator::lastSetCanvasSize_` became
  write-only after `maybeRequestRender` switched to live document-size
  comparison. The field and assignment are deleted.
- **Vestigial presentation shim removed.** `CompositedPresentation::shouldDisplayCompositedLayers` returned only `hasCachedTextures` and had no
  production callers. Tests now assert `hasCachedTextures` directly.
- **Misleading naming cleaned up.** MCP's artifact-label helper no longer uses
  a `flatLabel` local after flat presentation removal.
- **Stale comments trimmed.** The most visible changed-code comments now state
  current invariants instead of narrating the bugs that led to them.

### Resolved During First Action Tranche

- **Direct compositor promotion no longer encodes filter/clip/mask descendants
  as refusal.** `CompositorController::promoteEntity` now returns an explicit
  result; valid descendants under compositing ancestors request a full-canvas
  composited preview plan. Relevant targets:
  `//donner/svg/compositor:compositor_golden_tests` and
  `//donner/editor/tests:async_renderer_tests`.
- **Replay pixel tests use explicit pixel-diff policy.** `GlRnrReplay_tests`
  uses `PixelmatchIdentityParams()`. `RnrReplay_tests` keeps the pre-existing
  non-identity tolerances as documented `ApprovedPixelToleranceParams(...)`
  exceptions; no tolerance was widened.
- **The tight-bounded-segments control is test/diagnostic-only.** The editor
  View-menu toggle and MCP option/status field are removed. The compositor
  config and `AsyncRenderer` setter remain for focused tests and diagnostic
  code.

### Resolved During Second Action Tranche

- **`RenderRequest` carries a non-null render lease.** Renderer/document
  handoff now uses `RenderLease` references constructed with each request, and
  `AsyncRenderer` stores pending work as `std::optional<RenderRequest>` instead
  of a default request with nullable raw pointers. This removes the invalid
  "busy with null renderer/document" state while preserving the existing UI
  lifetime contract.
- **Compositor upload snapshots own their presentation data.**
  `snapshotTilesForUpload` now returns value snapshots with owned bitmap
  payloads and explicit source dimensions. Intermediate drag previews request
  drag-target pixels only, so the no-copy non-drag metadata path remains
  available without leaking pointers into compositor-owned storage.

### Resolved During Third Action Tranche

- **`CompositorController` stores its required dependencies as references.**
  The controller now holds non-null `SVGDocument` and `RendererInterface`
  references and uses an optional cancellation-token reference only during
  cancellable render calls. This removes the unused "constructed but detached"
  state and keeps cancellation scoped to one render attempt.

### Resolved During Fourth Action Tranche

- **Changed-code history comments were trimmed into local invariants.** The
  longest debugger-narrative comments in `AsyncRenderer.h`,
  `CompositorController.{h,cc}`, and `RnrReplay_tests.cc` now state the current
  contract or the enforcing test instead of preserving the investigation log.

### Resolved During Fifth Action Tranche

- **`AsyncRenderer` has one closed worker-state payload.** The worker state is
  now a mutex-protected `std::variant` that carries pending requests,
  intermediates, and final results in the active state payload. `isBusy()` is
  derived from the variant, `Done` remains busy until the final result is
  drained, and superseded renders cannot publish stale intermediate or final
  results.

### Resolved During Sixth Action Tranche

- **`CompositedPresentation` is a closed state machine.** Presentation state now
  uses explicit `NoCache`, `Cached`, `SettlingForRender`, and
  `WaitingForChromeRefresh` phases. Production scheduling uses behavior helpers
  instead of raw fields, while tests and MCP display snapshots read a copied
  diagnostics snapshot.

### Resolved During Seventh Action Tranche

- **`RenderCoordinator` no longer relies on top-level member order for worker
  teardown.** The renderer and async worker are held in a small
  `RenderWorkerBundle` whose member order makes the worker join before the
  renderer is destroyed. The editor-close teardown regression test now asserts
  that invariant without carrying the old crash history in production code.

### Resolved During Eighth Action Tranche

- **Editor and MCP replay share presentation render scheduling.**
  `PresentationRenderScheduler` now owns the common "post a render or not"
  decision, last-rendered version/canvas bookkeeping, and compositor interaction
  hint selection. `RenderCoordinator` and MCP's editor-shell replay path still
  own their environment-specific request construction, canvas flushing, overlay
  work, and structural remap consumption. The direct scheduler unit test covers
  first render, up-to-date no-op, active-drag capture, and settled-selection
  refresh behavior.

### Resolved During Ninth Action Tranche

**Editor and MCP replay share presented-frame geometry.**
`PresentedFrameComposer` now owns presentation geometry, not texture ownership.
It is a backend-neutral editor library that both `RenderPanePresenter` and MCP
replay call. It does not know about GL texture ids, ImGui draw lists, JSON, MCP
protocol details, bitmap cache entries, or CPU bitmap blending.

The shared API describes the pieces both callers already have:

- `PresentedFrameTileGeometry`
  - `canvasOffsetDoc`
  - `bitmapDimsDoc`
  - `dragTranslationDoc`
  - `isDragTarget`
- `PresentedDragPreview`
  - `entity`
  - `translationDoc`
- `PresentedTileRect`
  - `topLeft`
  - `bottomRight`
  - `effectiveDragTranslationDoc`

The core functions are:

- `ResolvePresentedTileDragTranslation(...)`
- `ComputePresentedTileRect(...)`, taking a dest-from-source
  `outputFromCanvasTransform` so editor screen presentation and MCP
  canvas-pixel presentation use the same document-to-output math.
- `RoundPresentedTileRectToPixelRect(...)` for MCP's explicit integer rounding
  before calling its bitmap blending path.

The editor caller adapts `GlTextureCache::TileView` into
`PresentedFrameTileGeometry`, receives a rectangle, and passes it to
`ImDrawList::AddImage`. The MCP caller adapts `DisplayTileView`, rounds the
returned rectangle for the headless bitmap, and keeps `BlendBitmapOver` local.
`GlTextureCache::TileView` still owns GL upload state; MCP `DisplayTileView`
still owns replay metadata and bitmap-cache lookup keys. Sharing those would
merge unrelated lifetimes and make the boundary harder to reason about.

Focused coverage moved the geometry-only `RenderPanePresenter_tests` cases into
`//donner/editor/tests:presented_frame_composer_tests` and added the missing
MCP-shaped cases:

- active drag target uses live active translation when the displayed preview
  entity matches;
- idle, target switch, and non-drag tiles keep cached `dragTranslationDoc`;
- output rectangles are computed from `canvasOffsetDoc`, `bitmapDimsDoc`,
  drag translation, and `outputFromCanvasTransform`;
- invalid tile dimensions or invalid output transforms are rejected;
- non-zero canvas/document origins match MCP replay's `viewBox.topLeft`;
- integer rounding is covered by the round-to-pixel helper.

`RenderPanePresenter_tests` was removed because it only covered the moved
geometry helpers.
`//tools/mcp-servers/editor-control:editor_control_session_tests` continues to
assert that replayed presented frames match final frames.

### Must Fix Before Landing

- **The flat-removal stack should be commit-split before review.** The local
  branch now has tranche commits instead of one mixed working tree. Before
  opening the PR, verify the final stack still preserves red-to-green evidence
  where possible: representative failing test first, implementation second,
  cleanup third.

### Cleanup Follow-Ups

- **Long comments still exist in older compositor/editor paths.** The worst
  changed-code history blocks in the active compositor/editor/replay paths have
  been trimmed, but older comments should still be reviewed opportunistically
  when their surrounding code changes. Production comments should state the
  invariant, the local gotcha, and the enforcing test target.
- **Matcher coverage is uneven.** Several new tests use raw `EXPECT_TRUE` over
  JSON or state-machine fields where a gMock matcher would make the invariant
  clearer. This is especially visible in MCP replay assertions and
  `CompositedPresentation` state tests.

## Fragility and Refactoring Opportunities

- **Optional compositor callbacks still rely on scoped raw pointers.**
  `CompositorController` keeps `intermediateCallback_` as an optional pointer
  during the 3-argument `renderFrame` overload. This is still valid by call
  scope, but any future callback expansion should prefer a scoped render-attempt
  object so callback, cancellation, and exit publication live in one payload.
- **MCP replay still composes display frames separately from the editor.** The
  render-posting scheduler and presented-frame geometry are now shared, but MCP
  still performs the final CPU bitmap blending locally. Keep GL texture upload
  lifetime and MCP bitmap-cache lifetime separate unless a second non-MCP
  caller needs CPU bitmap composition.
- **Pixel-diff policy still depends on adoption.** Replay tests now use central
  identity and approved-tolerance helpers, but future pixel tests can still add
  private comparison paths unless reviewers hold the line. New visible-frame
  regressions should use `BitmapGoldenCompare` and default to identity
  pixelmatch.
- **Long comments are compensating for missing types.** Several comments still
  describe sequencing protocols that should be encoded by leases, closed state
  variants, or result types. Keep history in retrospectives; production
  comments should describe current invariants and the enforcing test target.

## Non-Solutions That Made Debugging Worse

- **Flat fallback.** It made the UI appear safe when tile metadata was stale,
  but the stale presented pixels still flashed in the real editor.
- **Raw pixel counts and percentage thresholds.** They made small but visible
  repro differences look acceptable and forced manual interpretation.
- **Synchronous `.rnr` replay only.** It skipped the frame where the user saw
  the flash because it waited for render completion at the wrong times.
- **Testing tile-entity identity instead of visible pixels.** The first tests
  proved a useful internal invariant, not the operator-visible bug.
- **Manual screenshots of the wrong frame.** The first screenshot response was
  not generated from the harness and did not prove the repro. The later GL
  readback, cropped to the canvas, closed that gap.
- **Feature gates for correctness paths.** `experimentalMode`,
  `enable_composited_drag_preview`, and `--no-experimental` multiplied states
  without representing real product choices.

## Testing Review

Why the repro took several iterations:

1. The first MCP replay assertions returned early when the display path was
   `flat`, so they missed stale presented pixels.
2. The synchronous replay path advanced input only after render completion,
   while the real editor buffers clicks, cancels busy work, polls async results
   at frame start, and presents the currently cached texture set.
3. The headless display comparison did not initially compose exactly what the
   user saw. GL readback exposed the gap between CPU-side replay state and
   real presented pixels.
4. The screenshot harness first captured too much UI or the wrong frame. Hiding
   source/panels and cropping to the canvas made the artifact reviewable.
5. Manual operator validation was needed because the automation did not yet
   prove that its screenshot was the repro frame. Future repro harnesses should
   emit frame number, click index, crop mode, and diff artifacts together.

Infrastructure advances to propagate:

- Make GUI-scheduled `.rnr` replay the default for editor-visible bugs.
- Keep GL readback capture available from MCP and from Bazel tests when the bug
  depends on the real present path.
- Share pixelmatch artifact writing through `bitmap_golden_compare`; MCP should
  not grow private comparison logic again.
- Add fixture annotations for important `.rnr` files: expected click index,
  expected frame window, target selector, and representative crop.
- Add a reusable "presented frame must equal eventual final" helper so future
  tests do not re-implement the JSON walk.

## Process Review

What worked:

- The eventual test matched the operator-visible symptom: first presented UI
  frame after the R click pixelmatches the eventual correct frame.
- The MCP and editor screenshot tooling converged on the same cropped canvas
  view, which made human validation concrete.
- Cleanup removed real extra paths: flat upload/draw, experimental gates,
  `--no-experimental`, raw MCP compare, stale toggle tests, and write-only
  state.

What did not work:

- Several attempted fixes landed mentally before the red test was
  representative. The operator had to ask for proof of repro multiple times.
- Tests were initially too implementation-centric. Tile ids, cached entities,
  and generation counters are diagnostics; visible pixels are the contract.
- Some tests are not ToTT-readable enough. The MCP JSON assertions are powerful
  but hard to scan. They need small helpers with domain language such as
  `ExpectPresentedFrameMatchesFinalAfterClick(...)`, backed by gMock matchers
  for JSON fields and preview tile signatures.
- The branch now has tranche commits, but the final PR stack still needs one
  audit pass so representative tests, implementation, and cleanup remain easy
  to review independently.

## Actions

- [x] Replace direct promote-refusal tests for filter/clip/mask descendants
      with "safe boundary or full-canvas composited preview" tests, or rename
      the low-level API so refusal is not exposed as valid presentation
      behavior. Enforced by `//donner/svg/compositor:compositor_golden_tests`
      and `//donner/editor/tests:async_renderer_tests`.
- [x] Convert thresholded replay pixel tests in `RnrReplay_tests` to identity
      pixelmatch or document explicit exceptions. Enforced by
      `//donner/editor/tests:rnr_replay_tests`.
- [x] Factor MCP replay assertions into readable helper functions that describe
      editor-visible behavior instead of raw JSON traversal. Enforced by
      `//tools/mcp-servers/editor-control:editor_control_session_tests`.
- [x] Add gMock matchers for presented-frame JSON, pixelmatch summaries, and
      composited preview tile signatures.
- [x] Add `.rnr` fixture annotations for frame/click/crop expectations.
- [x] Move remaining changed-code history comments into this retrospective or
      the relevant design doc; keep production comments present-tense and local.
- [x] Decide the fate of the tight-bounded-segments menu toggle: product
      feature, test-only diagnostic, or removed path.
- [x] Replace nullable raw pointers in `RenderRequest` with a render lease or
      other non-null lifetime-enforcing handoff. Enforced by
      `//donner/editor/tests:async_renderer_tests`.
- [x] Replace raw-pointer compositor upload snapshots with owned/value
      presentation snapshots.
- [x] Make compositor controller dependencies non-null by construction.
      Enforced by `//donner/svg/compositor:compositor_tests`,
      `//donner/svg/compositor:compositor_golden_tests`, and
      `//donner/editor/tests:async_renderer_tests`.
- [x] Collapse `AsyncRenderer` busy/cancel/result bookkeeping into one closed
      worker-state model. Enforced by `//donner/editor/tests:async_renderer_tests`
      and `//donner/editor/tests:editor_layer_stress_tests`.
- [x] Convert `CompositedPresentation` into a closed state machine with an
      immutable diagnostics snapshot for tests and MCP. Enforced by
      `//donner/editor/tests:composited_presentation_tests`.
- [x] Extract a shared presentation render scheduler for editor and MCP replay.
      Enforced by `//donner/editor/tests:presentation_render_scheduler_tests`,
      `//donner/editor/tests:async_renderer_tests`, and
      `//tools/mcp-servers/editor-control:editor_control_session_tests`.
- [x] Extract a shared presented-frame composer for editor and MCP replay if
      display-frame semantics keep changing. The current MCP path still
      composes headless tile textures separately from the GL presenter, but it
      now consumes the same drag-translation and document-to-output rectangle
      policy. Enforced by `//donner/editor/tests:presented_frame_composer_tests`,
      `//donner/editor:render_pane_presenter`, and
      `//tools/mcp-servers/editor-control:editor_control_session_tests`.
- [x] Add a central pixel-diff expectation API so identity pixelmatch is the
      default and tolerances require explicit approval.
- [x] Remove `RenderPanePresenter`'s full-pane sizing fallback for invalid tile
      geometry once tests assert every visible tile has positive document
      dimensions.
