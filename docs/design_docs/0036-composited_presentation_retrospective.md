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

The follow-up bug work found one more category: drag presentation math needs an
explicit baseline. A cached drag tile may already include the currently
displayed drag translation, while a newly started re-drag may need only the
residual live delta on top of an older cached offset. Tests now cover those
unit cases, but future visible-frame repros should still prove mid-drag
alignment before mouseup instead of relying on the final settled frame.

The later unrelated-layer generation bug clarified an important non-cause:
redundant transform writes were noisy, but they were not the root reason every
promoted layer rerasterized. The actual failure was a latched full-rebuild
signal. A selection/hit-test path rebuilt the render tree and consumed local
dirty flags, but `RenderTreeState::needsFullRebuild` stayed true. The next
compositor frame interpreted that global flag as a full cache invalidation and
bumped every promoted layer generation.

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

### Resolved During Follow-Up Bug Fixes

- **Second drag of the same object no longer snaps the composited tile back to
  the original position.** The first shared-composer implementation replaced
  cached drag-target tile translation with only the live active preview. During
  a second drag, the cached tile still needed its previous document offset plus
  the new active drag delta. `ResolvePresentedTileDragTranslation(...)` now
  keeps the cached tile translation in the composition. Enforced by
  `//donner/editor/tests:presented_frame_composer_tests`.
- **Click/drag immediately after zoom no longer parks behind a stale prewarm
  render.** The render pane now handles the pending click before posting the
  next render request, cancels dispensable busy prewarm work when the click
  needs the slow hit-test path, and the async worker fires its wake callback
  when cancellation returns the worker to idle. The GL replay harness also
  exposes the final selected element label so the zoom-before-reraster repro
  can assert behavior, not just capture pixels. Enforced by
  `//donner/editor/tests:async_renderer_tests` and
  `//donner/editor/tests:gl_rnr_replay_tests`.
- **Active drag presentation no longer double-applies the displayed drag
  translation.** The first second-drag fix added the active preview delta
  unconditionally. When a newly landed drag tile already represented the same
  displayed preview, the composited pixels moved twice as far as the overlay.
  `ResolvePresentedTileDragTranslation(...)` now adds only the residual delta:
  tile offset plus active preview translation minus displayed preview
  translation. Enforced by
  `//donner/editor/tests:presented_frame_composer_tests`.
- **Unrelated promoted layers no longer rerasterize after drag-writeback plus
  selection.** The tempting workaround was to suppress dirty flags for semantic
  no-op transform writes. That guard was removed after the true root cause was
  isolated. `RenderingContext::instantiateRenderTree(...)` now clears
  `RenderTreeState::needsFullRebuild` after a successful render-tree
  instantiation, so query paths that rebuild render instances do not leave a
  global full-rebuild signal for the compositor. Enforced by
  `//donner/svg/renderer/tests:renderer_driver_tests` and
  `//donner/editor/tests:async_renderer_tests`.

### Working Tree Audit After Root Cause

Audited the local changes after the `needsFullRebuild` latch was understood:

- **Removed as unnecessary:** the transform equality/no-op dirty suppression in
  `SVGElement` and `LayoutSystem`. It hid this repro but did not address the
  global invalidation leak.
- **Kept as independently justified:** metadata-only tile publication and
  generation-stability tests. They are the intended upload contract: unchanged
  tiles keep their generation and GL texture even when presentation metadata
  moves.
- **Kept as independently justified:** removal of progressive intermediate
  results. Stale canvas-sized intermediate frames were an invalid presentation
  state, and the feature had no remaining correct caller after composited
  presentation became the only display path.
- **Kept as independently justified:** worker-cancel/liveness, layer-inspector
  freshness, and GL replay coverage for zoom/drag bugs. Those tests prove UI
  progress and presented-frame behavior, not just this generation-counter
  symptom.
- **No new compositor dirty-range rewrite is needed for this bug.** The
  existing per-layer/per-segment dirty mapping was not what dirtied every layer;
  the global rebuild flag bypassed it.

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

- **Progressive intermediate frames were removed.** The callback-based
  two-stage render path was only correct when cached compositor tiles already
  matched the request canvas size, which excluded the zoom/canvas-resize case it
  was meant to help. `AsyncRenderer` now publishes only complete render results
  with fresh tile pixels.
- **MCP replay still composes display frames separately from the editor.** The
  render-posting scheduler and presented-frame geometry are now shared, but MCP
  still performs the final CPU bitmap blending locally. Keep GL texture upload
  lifetime and MCP bitmap-cache lifetime separate unless a second non-MCP
  caller needs CPU bitmap composition.
- **Drag presentation baseline is now explicit, but still caller-selected.**
  `PresentedDragBaseline` names the represented and active translations, and
  `//donner/editor/tests:presented_frame_composer_tests` covers the known
  baseline cases. Future callers still need to build the baseline only for
  matching active/displayed entities; keep that adapter logic small and local.
- **Visible overlay alignment coverage is frame-pair based.**
  `//donner/editor/tests:gl_rnr_replay_tests` now compares the active drag
  frame with the same-cursor mouseup frame. This catches the reported drift
  without splitting document pixels and overlay chrome into separate readback
  layers.
- **The layer-inspector status has replay liveness coverage.**
  `//donner/editor/tests:layer_inspector_diagnostics_tests` covers the pure
  canvas-freshness classification, and
  `//donner/editor/tests:gl_rnr_replay_tests` asserts the stale-compositor
  status is not present at the bounded final replay frame.
- **Render-tree rebuild state must be consumed by the rebuilder.** A render
  tree rebuild is allowed during rendering, hit testing, and bounds queries.
  After it succeeds, `RenderTreeState::needsFullRebuild` must be false. Leaving
  the flag latched turns a later compositor frame into a full cache
  invalidation even when no local dirty entities remain.
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
- For drag bugs, capture and assert the frame before mouseup. The settled frame
  can be correct even when active composited pixels and overlay chrome are
  visibly divergent.
- For zoom/reraster bugs, assert both progress and final behavior: worker wake
  count or idle transition, final selected element, and absence of a persistent
  stale-compositor status across a bounded frame window.

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
- Generation counters still have value as a secondary invariant. Once a repro
  is visibly understood, a generation-stability test is a good way to pin the
  performance contract that unrelated layer textures are not re-uploaded.
- Some tests are not ToTT-readable enough. The MCP JSON assertions are powerful
  but hard to scan. They need small helpers with domain language such as
  `ExpectPresentedFrameMatchesFinalAfterClick(...)`, backed by gMock matchers
  for JSON fields and preview tile signatures.
- The second-drag and zoom-before-reraster bugs both initially looked solved
  because the final settled state was correct. For interaction bugs, final
  state is only one assertion; the active frame that the operator reports must
  be part of the repro contract.
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
- [x] Add regression coverage for repeated same-entity drag presentation and
      for active drag tiles that already represent the displayed preview
      translation. Enforced by
      `//donner/editor/tests:presented_frame_composer_tests`.
- [x] Add zoom-before-reraster replay and worker-cancel wake coverage so a
      click/drag after zoom can supersede stale prewarm work. Enforced by
      `//donner/editor/tests:async_renderer_tests` and
      `//donner/editor/tests:gl_rnr_replay_tests`.
- [x] Add a GL replay assertion for second-drag overlay alignment before
      mouseup. The test should capture a mid-drag frame, compare composited
      document pixels against the path overlay or an equivalent geometric
      signature, and fail when the document pixels move at a different scale
      than the overlay. Enforced by
      `//donner/editor/tests:gl_rnr_replay_tests`.
- [x] Rename or restructure the drag presentation API so the baseline is
      encoded in the type, not inferred from two optional previews. Implemented
      as `PresentedDragBaseline`, carrying `entity`,
      `representedTranslationDoc`, and `activeTranslationDoc`. Enforced by
      `//donner/editor/tests:presented_frame_composer_tests` and
      `//tools/mcp-servers/editor-control:editor_control_session_tests`.
- [x] Extend `.rnr` expectations for interaction bugs with active-frame
      requirements: before-mouseup frame, expected selection/status after a
      bounded frame window, and whether the test is proving pixels, selection,
      or worker liveness. Enforced by
      `//donner/editor/repro:repro_file_tests` and
      `//donner/editor/tests:gl_rnr_replay_tests`.
- [x] Add a replay/status assertion for the layer inspector's
      `"compositor not yet re-rasterized"` diagnostic so it cannot persist
      indefinitely after zoom/cancel input if the UI treats the status as
      user-visible correctness. Enforced by
      `//donner/editor/tests:layer_inspector_diagnostics_tests` and
      `//donner/editor/tests:gl_rnr_replay_tests`.
- [x] Fix the root cause of unrelated layer-generation bumps instead of
      suppressing transform dirty flags. A successful
      `RenderingContext::instantiateRenderTree(...)` now consumes
      `RenderTreeState::needsFullRebuild`, and the transform no-op workaround
      is removed. Enforced by
      `//donner/svg/renderer/tests:renderer_driver_tests` and
      `//donner/editor/tests:async_renderer_tests`.
