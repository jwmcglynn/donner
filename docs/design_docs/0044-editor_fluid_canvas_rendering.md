# Design: Editor Fluid Canvas Rendering

**Status:** Implementation in progress
**Author:** Codex
**Created:** 2026-05-27

## Summary

The editor still falls out of interactive budgets on real `donner_splash.svg` workloads after the
first compositor-responsiveness pass. The remaining pattern is consistent: we rasterize or upload
work proportional to the document canvas, selected element count, or full paint-order span even when
the user can only see a small viewport or only needs coarse interaction feedback.

This design proposes a second responsiveness pass:

- render high zooms against the visible viewport, not the whole document texture;
- keep cheap paint-order spans and editor chrome in immediate mode with viewport culling;
- use selection level-of-detail so selecting all elements never asks the overlay renderer to outline
  every path before the next frame;
- promote tiles, overlays, and source-pane ropes to bounded, clipped, prioritized work.

## Goals

1. **Every input produces visible feedback within one frame** on `donner_splash.svg`: select,
   select-all, drag, zoom, pan, source hover, and source-focus ropes.
2. **Steady interaction stays under 16 ms/frame p50 and 33 ms/frame p99** on an M-series Mac at
   DPR=2, including high zoom and multi-selection.
3. **High zoom never renders a whole-document 8192 px texture just because the viewport is zoomed
   in.** Raster work is bounded by the pane size plus a small predictive margin.
4. **Selecting all or a large group is O(visible chrome), not O(all selected paths x full canvas).**
   The first frame may show combined bounds and handles; individual path outlines refine only when
   they are visible and cheap.
5. **No zoom-out artifacts.** If a user zooms back out before high-resolution tiles finish, the
   editor displays a coherent lower-resolution overview rather than missing or stale tile holes.
6. **Performance regressions are measurable.** Each milestone adds either a deterministic counter
   test or a manual/nightly wall-clock perf test.

## Non-Goals

- Replacing tiny-skia with Geode as the default renderer for the whole library. The editor now
  targets Geode by default, while non-editor renderer targets keep their existing backend defaults.
- Pixel-perfect high-resolution output during active gestures. The interaction frame can be lower
  fidelity as long as it is coherent and refines promptly after idle.
- A full retained-mode scene graph rewrite. Reuse `CompositorController`, `RenderCoordinator`,
  `GlTextureCache`, and `ViewportState` where possible.
- Rendering arbitrary offscreen parts of an infinite canvas. This covers the active SVG document
  viewBox and the editor panes.

## Next Steps

- Rebaseline with current Geode offline and live-editor telemetry before changing the next
  architectural piece. The replay telemetry now shows the cached-span zoom+drag reraster fixed; the
  remaining live signal is WGPU surface acquisition/backpressure plus any manual frame-graph spikes.
- Capture manual telemetry for the regressed flow next: far zoom in/out followed by drag on a
  Donner letter, especially after the worker is busy or the surface has just resized. Record the
  frame graph, terminal WGPU timing logs, and Layers-panel heuristic history JSONL from the default
  path `/tmp/donner-compositor-heuristics.jsonl` or an explicit run-specific path.
- Keep select-all and source-reference ropes in the queue, but treat them as secondary until the
  zoom+drag regression is explained.
- Add the missing M1 offline fixtures for the manual regressions: high-zoom pan/drag near the old
  8192 px clamp, zoom-then-drag click-to-first-feedback latency, and then large-selection/select-all
  real-splash feedback.
- Finish M5's explicit low-resolution overview refresh, then move to M6 tile pyramid/prioritization.
  Start M7 Geode chrome migration after the live telemetry confirms ropes/chips are still a visible
  part of the frame budget.

## Implementation Plan

- [ ] **M1: Instrument the remaining hotspots.**
  - [x] Add `FrameCostBreakdown` counters for overlay capture, overlay draw, overlay upload bytes,
        source rope layout/update/draw, composited tile coverage, texture upload bytes, and
        full-document canvas commits.
  - [x] Render the known per-frame costs as a stacked color-bar profiler on the frame graph.
  - [x] Track presentation-cache memory buckets and render them as a stacked memory graph beside
        the frame-time profiler.
  - [x] Add worker-side content tile raster phase counters.
  - [x] Add a Layers-panel JSONL history export for per-segment immediate-vs-cached heuristic
        telemetry across zooms and interactions.
  - [x] Emit structured frame-budget-miss JSONL telemetry with the same contributor regions used
        by the color-bar profiler. Set `DONNER_EDITOR_FRAME_MISS_LOG=stderr` or a file path; setting
        `DONNER_EDITOR_RESOURCE_LOG` also enables stderr miss records.
  - [ ] Extend `async_renderer_wallclock_tests` with a large-selection/select-all real-splash case.
  - [ ] Add a replay/perf fixture for zooming to the 8192 px clamp and panning while selected.
- [x] **M2: Immediate-mode editor chrome.**
  - [x] Replace full-document overlay bitmap upload for path chrome with a viewport-sized or direct
        presenter path clipped to the render pane.
  - [x] Cull path outlines, per-element AABBs, and handles against the visible document rect.
  - [x] Add large-selection LOD: combined bounds first, visible path outlines during idle.
- [x] **M3: Clip and cull source-pane ropes.**
  - [x] Push a source-pane clip rect around `renderFocusReferenceLinks`.
  - [x] Skip rope simulation/draw for links whose source/target/route AABB is outside the visible
        text region.
  - [x] Cap animated rope count and fall back to static straight connectors for overflow.
- [x] **M4: Immediate-mode cheap compositor spans.**
  - [x] Add a `StaticSpanPlan` for each paint-order gap: `CachedTile` or `Immediate`.
  - [x] Use a conservative cheapness heuristic: simple geometry, no filters/masks/patterns/text,
        estimated redraw cost below cached-texture overhead, plus measured timing against a 120 Hz
        frame-budget slice. Static-cheap spans stay immediate; dynamically-expanded spans fall back
        to cached presentation after an over-budget immediate render.
  - [x] Emit immediate spans into the active composition render target via
        `RendererDriver::drawEntityRangeIntoCurrentFrame`; keep transient tile payloads only for
        the current editor split-tile presentation handoff.
- [ ] **M5: Viewport-bounded high-zoom rendering.**
  - [x] Split display transform from raster target: high-zoom raster target is pane-sized plus
        margin, not full document viewBox sized.
  - [x] Render the visible document rect with an explicit `outputFromDocument` transform while
        keeping the SVG semantic canvas at the full-document size.
  - [x] Carry the raster viewport through worker results and scheduler state so same-size pan/zoom
        results cannot be reused after the camera moves.
  - [x] Cull render instances against the bounded output surface before backend submission, after
        composing entity-to-canvas and canvas-to-surface transforms in the correct order.
  - [x] Keep the most recent unbounded whole-document tile set as an overview fallback under
        viewport-bounded renders.
  - [ ] Add an explicit low-resolution overview refresh when no unbounded overview exists or it is
        too stale for zoom-out fallback.
- [ ] **M6: Tile pyramid and prioritization.**
  - [ ] Add fixed-size content tiles keyed by `(paint span, tile coord, scale band, generation)`.
  - [ ] Prioritize visible tiles, selection tiles, one-tile margin, then predicted pan/zoom tiles.
  - [ ] Add memory caps, LRU eviction, and stale-but-coherent fallback rules.
- [ ] **M7: Geode-rendered source-pane flair and UI chrome.**
  - [x] Render selection/path chrome through `OverlayRenderer` + `RendererGeode` directly into the
        editor framebuffer after ImGui submits the editor draw data. This path uses a single-sample
        alpha-coverage Geode device and `LoadOp::Load`, so it appends to the swapchain instead of
        allocating an overlay texture or re-tessellating curves through ImGui.
  - [ ] Move source-reference ropes from ImGui path commands to a clipped Geode screen-space chrome
        layer. Keep the existing ImGui hit testing and tooltips as invisible interaction owners.
  - [ ] Draw chip backgrounds, borders, glows, and connector flair through Geode. Keep chip text in
        ImGui until Geode has UI-grade text rendering.
  - [ ] Share the same clip/cull policy as M3 so ropes and chips cannot bleed outside the source
        text area or canvas pane.
  - [ ] Treat this as a migration path for non-text editor UI from ImGui to Geode, starting with
        decorative/high-churn visuals before moving stateful controls.
- [ ] **M8: Validation and rollout.**
  - [ ] Add CI counter gates for culling, tile reuse, and no full-document high-zoom commits.
  - [ ] Add manual perf gates for select-all, high-zoom pan, high-zoom drag, and source-focus ropes.
  - [ ] Remove obsolete full-overlay and full-document high-zoom paths after the new path is stable.

## Profiling Snapshot

Updated on `main` at `7cb6fb30` on 2026-05-28 on an Apple M1 Pro, using
`-c opt --config=geode` for Geode-specific targets.

Commands:

```sh
tools/llm-bazel-wrap.sh test -c opt --config=geode \
  //donner/editor/tests:async_renderer_wallclock_tests \
  --test_filter='AsyncRendererE2ETest.ClickThenDragOnSplashShapeMeetsLatencyBudget:AsyncRendererE2ETest.EndToEndDragHarnessOnRealSplash:AsyncRendererE2ETest.FaithfulFrameDragOnRealSplashBreaksDownPerFrameCost:AsyncRendererE2ETest.MultiShapeClickDragHiDpiRepro' \
  --test_output=all

tools/llm-bazel-wrap.sh test -c opt --config=geode \
  //donner/editor/tests:filter_drag_repro_tests_wallclock \
  //donner/editor/tests:async_renderer_filter_group_perf_tests_wallclock \
  --test_output=all

tools/llm-bazel-wrap.sh test -c opt \
  //donner/svg/compositor:compositor_perf_tests \
  --test_output=all

tools/llm-bazel-wrap.sh run -c opt --config=geode \
  //donner/svg/renderer/benchmarks:renderer_bench -- \
  --iterations=5 --warmup=1 donner_splash.svg

tools/llm-bazel-wrap.sh test -c opt --config=geode \
  //donner/svg/renderer/geode:geode_perf_tests \
  --test_output=all

tools/llm-bazel-wrap.sh test -c opt --config=geode \
  //donner/editor/tests:gl_rnr_replay_tests \
  --test_filter='GlRnrReplayTest.GeodeDragZoomRerasterizesDonnerDOverlayEveryPresentedFrame:GlRnrReplayTest.GeodeZoomThenDragKeepsDonnerDOverlayLockedToPresentedContent:GlRnrReplayTest.GeodeZoomThenDragDoesNotFreezeLiveDragPreviewWhileWorkerBusy:GlRnrReplayTest.GeodeFarZoomThenDragKeepsDonnerNOverlayLockedToPresentedContent' \
  --test_output=all

tools/llm-bazel-wrap.sh test -c opt --config=geode \
  //donner/editor/tests:rnr_replay_tests \
  --test_filter='RnrReplayTest.DragStartAfterZoomAsyncHarnessDoesNotHang' \
  --test_output=all
```

Headline results:

| Scenario                                  |                                    Current measurement | Why it matters                                                                |
| ----------------------------------------- | -----------------------------------------------------: | ----------------------------------------------------------------------------- |
| Real splash worker-only drag              |                               1.51 ms steady avg / max | Compositor worker is not the current broad drag-frame bottleneck.             |
| Faithful real splash drag frame           |                             10.18 ms avg, 23.70 ms max | Misses the 120 Hz frame budget and occasionally exceeds 60 Hz.                |
| Faithful frame worker portion             |                              1.51 ms avg, 10.56 ms max | Worker spikes exist, but the average is small.                                |
| Faithful frame overlay portion            |                              8.66 ms avg, 22.19 ms max | Overlay raster/upload dominates the faithful offline frame.                   |
| Overlay upload, natural canvas            |                                         1.74 MiB/frame | Still a real per-frame payload in the faithful harness.                       |
| HiDPI multi-shape repro at 1784x1024      |         9-11 ms click/promote, ~1.51 ms repeated drags | The old offline 100+ ms repeated-drag repro is green.                         |
| Filter drag replay worker frames          |                 0.03 ms avg first drag, 0.02 ms second | The filter drag worker fast path remains green.                               |
| Filter group subtree drag                 |                                      1.51 ms avg / max | Filter-group drag is not reproducing the live regression offline.             |
| Mock compositor drag overhead, 10k nodes  |                                          0.05 ms/frame | Warm compositor traversal is cheap after the recent immediate-span work.      |
| Mock compositor click-to-first, 10k nodes |              464 ms prewarm, 0.067 ms first drag frame | Cold prewarm is still linear and worth keeping out of interactive gestures.   |
| RendererGeode `donner_splash.svg`         |        6.59 ms parse, 11.15 ms draw, 29.87 ms snapshot | Full-document Geode snapshot is over interactive budgets if forced per frame. |
| Geode no-dirty perf counters              |                              0 path encodes / textures | No-dirty renderer counters are green; counter regressions are not obvious.    |
| Geode zoom+drag GL replay correctness     |                                 4 focused tests passed | Lockstep/freeze correctness is green, but this is not a frame-time gate.      |
| GL replay surface acquisition             |                           392 ms and 579 ms log spikes | Strong signal for surface acquisition/GPU backpressure during zoom+drag.      |
| Async zoom-after-drag replay              | max worker 131.9 ms; click-to-drag 19.8 ms and 43.6 ms | The second zoom+drag click misses 60 Hz/120 Hz feedback budgets offline.      |

Interpretation:

- The broad async worker/filter paths do not reproduce the user's current live-editor FPS miss.
- The nearest offline miss is presentation chrome: the faithful harness spends most of its frame in
  overlay rasterization/upload and misses 120 Hz even though the worker averages about 1.5 ms.
- Raw `RendererGeode` whole-document snapshot is far over a 120 Hz or 60 Hz interactive budget for
  `donner_splash.svg`. Any live path that still snapshots whole-document chrome/content during an
  interaction will be visible.
- Zoom+drag needs its own lane. The correctness replays are green, but they already show slow
  `surface.getCurrentTexture` calls and one async click-to-drag path over 40 ms. That matches the
  user's report better than the generic drag/filter benchmarks.
- The next measurement needs to happen in the live editor with the color-bar profiler and Layers
  panel heuristic telemetry, because final ImGui composition, source-pane chrome, and live
  immediate-span decisions are not fully covered by the offline worker tests.

### Zoom+Drag Telemetry Update

The first replay telemetry pass on `donner/editor/tests/geode_drag_zoom_o_pop.rnr` isolated a
specific live-frame regression: zooming while dragging a cached selection repeatedly committed a
new document canvas size, which invalidated the render tree and rerasterized every cached
compositor span before the next pointer frame.

The policy after this pass:

- If an active drag has a matching cached composited entity, `PresentationRenderScheduler` keeps the
  interaction on presenter-side transforms and suppresses regular renders even when the desired
  canvas size or raster viewport changes.
- `RenderCoordinator` keeps `SVGDocument::setCanvasSize()` debounced and additionally defers
  zoom-driven canvas commits while that active drag is live. The crisp refresh happens after the
  drag settles.
- `CompositedPresentation` treats active-drag cache validity as entity-based. A canvas-size
  mismatch during drag is acceptable because the interaction invariant is coherent, lockstep
  presentation; high-resolution canvas freshness is an idle/settle refinement.

Measured on the same replay window, active drag frames `39-81` changed from:

| Metric                                           |   Before | After |
| ------------------------------------------------ | -------: | ----: |
| Cached compositor raster, average                | 30.64 ms |  0 ms |
| Cached compositor raster, max                    | 99.64 ms |  0 ms |
| Zoom-driven document canvas commits in window    |       15 |     0 |
| Cached compositor tiles rerendered in bad frames | 12/frame |     0 |

The Geode GL replay lane still logged a `surface.getCurrentTexture` stall during one focused test,
so GPU surface acquisition remains a separate investigation item. This update removes the repeated
cached-span reraster from the zoom+drag hot path; it does not claim to fix every WGPU backpressure
source.

## Background and Prior Art

The previous responsiveness plan is
[0033-editor_design_tool_responsiveness](0033-editor_design_tool_responsiveness.md). It landed
important pieces: intrinsic layer rasters, async cancellation, preemptive result wakeups, selection
chrome snapshots, re-drag bypasses, and demotion hysteresis. It still leaves full-canvas overlay,
full-document high-zoom rasterization, and large-selection LOD open.

[0034-progressive_rendering](0034-progressive_rendering.md) is deliberately **not** revived here.
That experiment published intermediate frames with stale canvas-sized tile geometry and caused
unrelated layers to jump. This design keeps intermediate display coherent by using either the
current active tile set, an explicit low-resolution overview, or visible tiles whose geometry
belongs to the current viewport request.

Production rendering systems use the same broad pattern:

- Figma built its editor around a low-level GPU rendering interface for a smooth infinite canvas and
  later moved that interface toward WebGPU while benchmarking against WebGL. See Figma's WebGPU
  renderer write-up: <https://www.figma.com/blog/figma-rendering-powered-by-webgpu/>.
- Chromium does not rasterize entire layers when only part is visible. Its compositor uses tiles,
  prioritizes visible/soon-visible content, and may activate lower-resolution content before the
  high-resolution version is ready. See Chromium's compositor docs:
  <https://www.chromium.org/developers/design-documents/gpu-accelerated-compositing-in-chrome/>
  and <https://developer.chrome.com/docs/chromium/renderingng-data-structures>.
- Chromium's compositor thread architecture budgets GPU memory by visibility, distance from the
  viewport, animation status, and velocity: <https://www.chromium.org/developers/design-documents/compositor-thread-architecture/>.
- Photoshop treats zoom/pan-style canvas interaction as GPU-accelerated user-interface work; Adobe
  documents zoom as noticeably slower without GPU support and lists Scrubby Zoom among GPU-dependent
  features: <https://helpx.adobe.com/uk/photoshop/desktop/get-started/technical-requirements-installation/photoshop-and-graphics-processor-gpu-card-usage.html>.
- Inkscape's historical filter/tiling discussion is a warning: tiling can make high-zoom filters
  worse if each tile recalculates the same filter. Donner's tile plan must cache filter/layer
  results above the tile level when a filter spans many tiles:
  <https://lists.inkscape.org/hyperkitty/list/inkscape-devel%40lists.inkscape.org/thread/APAYXLML372X7P666ZC5DAEXSCTN3OF3/>.

## Proposed Architecture

### Frame Model

```
UI input
  |
  v
Interaction state update (selection, zoom, pan, drag)       <= 1 frame
  |
  +--> Immediate chrome presenter
  |     - combined selection bounds
  |     - visible path outlines
  |     - handles / marquee
  |     - source ropes clipped to text viewport
  |
  +--> Presented content cache
  |     - low-resolution whole-document overview
  |     - visible high-res tiles
  |     - cheap immediate spans
  |     - cached expensive spans/layers
  |
  +--> Async refinement queue
        - visible tile current scale
        - one-tile margin / predicted direction
        - idle path-outline refinement
        - stale low-priority tiles
```

The display path always has something coherent to show:

1. current high-resolution visible tiles if present;
2. current lower-resolution tiles or overview scaled into place;
3. immediate cheap spans and chrome clipped to the viewport;
4. a scheduled refinement if the visible quality is below target.

### Viewport-Bounded Raster Targets

The original `ViewportState::desiredCanvasSize()` returned
`documentViewBox.size() * zoom * DPR`, clamped per axis. On the splash, high zoom turned a 892x512
document into an 8192x4708-ish target even though the pane only showed a fraction of the document.
The M5 implementation splits the editor raster target into two sizes:

- `semanticCanvasSizePx`: the full-document canvas size used for SVG layout semantics;
- `outputSizePx`: the actual presentation surface size. At high zoom it is bounded to the pane plus
  margin instead of the full document.

The worker renders with an explicit `outputFromDocument` transform. `RendererDriver` applies the
derived surface transform before backend submission, and the async result carries the originating
`EditorRasterViewport`. `PresentationRenderScheduler` treats raster-viewport changes as regular
render invalidations, and `RenderCoordinator` rejects landed results whose viewport no longer
matches the current UI camera. This prevents a same-size pan at high zoom from presenting stale
pixels from the previous document window.

`GlTextureCache` retains the most recent unbounded whole-document composited tile set separately
from the active viewport-bounded tiles. `RenderPanePresenter` draws those overview tiles underneath
the active bounded tiles, so zooming out or panning into a not-yet-refined region shows coherent
older document pixels instead of empty tile holes while the next render lands.

The remaining M5/M6 work should introduce explicit raster modes:

- **Document overview mode:** render the full viewBox at a low or fit-to-pane scale. Used for zoomed
  out views, initial load, and fallback when high-res tiles are missing.
- **Visible viewport mode:** render only `visibleDocumentRect + margin` into a pane-sized target.
  The renderer gets an explicit transform from document coordinates into the viewport target.
- **Tile mode:** split the visible target into 512 or 1024 device-pixel tiles once the visible target
  exceeds a threshold or pan/zoom velocity predicts reuse.

`SVGDocument::canvasSize()` should stop being the only raster identity. A render request needs:

```cpp
struct EditorRasterViewport {
  Box2d documentRect;
  Vector2i outputSizePx;
  Vector2i semanticCanvasSizePx;
  Transform2d outputFromDocument;
  bool viewportBounded = false;
};
```

The full-document `canvasFromDocumentTransform()` remains valid for export-style renders. Editor
presentation should use `outputFromDocument` for high-zoom interactive rendering.

### Immediate-Mode Compositor Spans

The compositor already splits paint order into static segments and promoted layers. Today a static
segment becomes an offscreen bitmap if it is dirty. That is not always the right tradeoff:

- a small solid path may be cheaper to redraw than to allocate, upload, and retain as a texture;
- an offscreen bitmap cannot be culled per shape once it exists;
- at high zoom, full-span textures burn memory even when most of the span is offscreen.

Add a `StaticSpanPlan`:

```cpp
enum class StaticSpanMode {
  CachedTile,
  Immediate,
};

struct StaticSpanPlan {
  StaticSpanMode mode = StaticSpanMode::CachedTile;
  Entity firstEntity = entt::null;
  Entity lastEntity = entt::null;
  Box2d boundsDocument;
  int estimatedDrawOps = 0;
  int estimatedPathVerbs = 0;
  bool hasExpensiveEffect = false;
  uint64_t estimatedRetainedBytes = 0;
  double estimatedRedrawCost = 0.0;
  double estimatedCacheOverheadCost = 0.0;
  double measuredRasterizeMs = 0.0;
  double immediateBudgetMs = 0.0;
  bool staticHeuristicImmediate = false;
  bool dynamicHeuristicImmediate = false;
};
```

The baseline heuristic should choose immediate mode only when all are true:

- no filter, mask, clip-path, pattern, marker, image, or text in the span;
- projected visible bounds intersect the viewport;
- estimated redraw cost is lower than the texture setup and retained-memory overhead;
- no active animation or transform invalidation that would make command capture stale.

Measured raster time is asymmetric. Spans that miss the baseline heuristic may still switch to
immediate mode when their most recent rasterize is below a 120 Hz frame-budget slice and the
cumulative dynamic immediate work for the frame stays within that slice. If one of those
dynamically-expanded immediate spans later renders over budget, the freshly-rendered payload is
retained and the span returns to cached presentation on the next frame. Slow-machine timing still
does not demote spans that the baseline heuristic already classified as cheap enough.

Immediate spans are drawn in paint order during presentation with a clip rect equal to the render
pane or tile. Cached spans keep the current offscreen texture path.

The important behavior is not the exact threshold. It is the ability to choose "redraw this cheap
thing now and cull it" instead of "allocate a texture and upload it forever."

### Immediate-Mode Editor Chrome

Selection chrome is UI, not document content. It should not use a full-document raster target.

Replace the current full-canvas overlay texture with:

- immediate Geode drawing for handles, AABBs, marquee, and path outlines directly into the
  framebuffer;
- a viewport-sized transient overlay only for software-backend fallback cases that cannot draw
  directly;
- a strict clip rect around the render pane;
- culling against the visible document rect before path transformation/draw.

In Geode editor builds, the immediate chrome path must use Donner renderer calls, not ImGui path
commands. The render-pane presenter still uses ImGui to present cached canvas tiles, then
`EditorWindow` invokes a post-ImGui direct-render callback before surface presentation/readback.
That callback points `RendererGeode` at the current swapchain texture, preserves existing
framebuffer contents, pushes a framebuffer-space clip rect for the artboard, and calls
`OverlayRenderer::drawChromeFromSnapshot`.

The overlay is rebuilt every frame from the current viewport and current interaction state. It is
not retained behind the async document-content version gate and it is not reprojected from a cached
overlay texture during pan, zoom, drag, or transform handles. This keeps the chrome aligned with the
content even when the document tiles are still refining.

The path overlay has an iron lockstep rule: it must represent the same transform as the document
tiles actually presented underneath it in that frame. If a high-zoom or worker-busy frame cannot
present a fresh drag-target tile yet, the overlay must be projected back to the presented content
transform instead of displaying a newer transform over stale pixels.

Large selections use LOD:

| Selection size               | First interactive frame                                     | Idle refinement                                     |
| ---------------------------- | ----------------------------------------------------------- | --------------------------------------------------- |
| 1 element                    | path outline + AABB + handles                               | rerender current chrome at full detail              |
| 2-32 visible elements        | visible path outlines + per-element AABBs + combined bounds | rerender current visible outlines                   |
| >32 elements or "select all" | combined bounds + handles + count                           | visible outlines after interaction settles          |
| Group with many descendants  | group bounds first                                          | descendant outlines only when zoomed in and visible |

`SelectionChromeSnapshot` is a per-frame transfer object, not a retained cache. It should keep
capture and draw separate so we can count work, cull before draw, and choose large-selection LOD,
but it should be discarded after the current overlay has been drawn or uploaded.

### Source-Pane Ropes

`TextEditor::renderFocusReferenceLinks` already draws directly through ImGui. The missing pieces are
clip and cull:

- push a clip rect for the source text content area before rope drawing;
- compute a cheap route AABB from source endpoint, target endpoint, and slack bounds;
- skip simulation and draw when the route AABB is outside the visible text region;
- cap active simulated ropes, e.g. 64 visible ropes, and draw overflow as static straight lines or a
  chip count;
- sleep ropes immediately when the source pane is not hovered, not scrolling, and no rope is near
  the pointer.

This keeps source-focus visuals rich without letting them become an unbounded per-frame simulation.

### Tile Pyramid and Zoom-Out Coherence

A visible-viewport renderer alone can checkerboard when zooming out: the viewport suddenly covers
more document area than the high-zoom tiles contain. Use a pyramid:

- Level 0: overview texture for the full document at fit/low resolution.
- Level N: visible high-resolution tiles at scale bands, e.g. powers of two or half-stop bands.
- Presentation chooses the highest available level per region.
- Missing high-res regions display overview pixels until tile refinement lands.
- Tiles are requested in priority order: visible center, visible edges, one-tile margin, predicted
  pan direction, then idle overview refresh.

This is similar to browser compositor tiling, but with a filter-specific rule: if a filter's
inflated bounds intersects many tiles, rasterize the filtered layer once into an intrinsic cached
layer and let tiles sample/blit it. Do not recalculate the same filter per tile.

## API / Interfaces

Initial internal interfaces:

```cpp
struct VisibleDocumentWindow {
  Box2d documentRect;
  Vector2i outputSizePx;
  Transform2d outputFromDocument;
  double devicePixelRatio = 1.0;
};

struct ChromeLodPolicy {
  int maxImmediatePathOutlines = 32;
  int maxImmediatePathVerbs = 4096;
  int maxRopes = 64;
};

struct ContentTileKey {
  uint64_t spanId = 0;
  int scaleBand = 0;
  int tileX = 0;
  int tileY = 0;
  uint64_t documentGeneration = 0;
};
```

Likely touched modules:

- `ViewportState`: expose visible document rect and raster window helpers.
- `RenderCoordinator`: choose overview vs visible viewport vs tile requests.
- `CompositorController`: classify static spans and emit immediate/cached tile plans.
- `RenderPanePresenter`: draw immediate spans/chrome with clip rects in paint order.
- `OverlayRenderer`: split per-frame capture from draw and add large-selection LOD.
- `TextEditor`: clip/cull `renderFocusReferenceLinks`.
- `GlTextureCache`: support tile-pyramid keys, quality bands, and memory accounting.

## Data and State

New caches should be explicitly budgeted:

- content tile cache: memory cap in bytes, LRU by visibility and recent interaction;
- overview texture: one per document generation and scale band;
- source rope state: visible-link state only, with inactive ropes evicted when they leave the
  viewport.

Cached content must carry:

- document generation and structural remap generation;
- transform/scale band used to rasterize;
- document-space bounds and inflated filter bounds;
- visible tile coverage and fallback quality.

## Performance Targets

| Operation                                   |                         Current |                                         Target |
| ------------------------------------------- | ------------------------------: | ---------------------------------------------: |
| Real splash steady drag at natural size     |                     33.8 ms avg |                         <16 ms avg, <33 ms p99 |
| Real splash overlay rasterize               | 5.4 ms avg plus 1.74 MiB upload |          <1 ms direct draw or viewport overlay |
| HiDPI repeated drag at 1784x1024            |                   ~108 ms/frame |                                     <33 ms p99 |
| Zoom-after-drag second click-to-drag render |                          1.78 s | <100 ms visible feedback, <500 ms crisp refine |
| Select-all first feedback                   |         no current counter gate |                         <16 ms combined bounds |
| Source-focus ropes                          |         no current counter gate |                            <2 ms visible ropes |
| High-zoom raster dimensions                 |     up to 8192 px full document |                        pane size + tile margin |

## Testing and Validation

- `//donner/editor/tests:async_renderer_wallclock_tests`
  - Add `SelectAllRealSplashChromeBudget`.
  - Add `HighZoomViewportBoundedRasterBudget`.
  - Keep `FaithfulFrameDragOnRealSplashBreaksDownPerFrameCost`.
- `//donner/editor/tests:rnr_replay_tests`
  - Tighten `DragStartAfterZoomAsyncHarnessDoesNotHang` with a click-to-first-feedback budget once
    immediate chrome/overview is available.
- `//donner/editor/tests:overlay_renderer_tests`
  - Assert large-selection LOD emits combined bounds before individual outlines.
  - Assert offscreen selected paths are culled.
  - Assert render-pane clip prevents overlay bleed.
- `//donner/editor/tests:text_editor_tests`
  - Add rope clip/cull tests with links outside the visible source region.
- `//donner/svg/compositor:compositor_tests`
  - Assert cheap spans choose `Immediate`.
  - Assert expensive spans with filters/masks choose `CachedTile`.
  - Assert immediate/cached mixed paint order matches full render.
- `//donner/svg/compositor:compositor_golden_tests`
  - Golden compare mixed immediate/cached spans on splash-like documents.
  - Golden compare tile-pyramid fallback during zoom out.

Per design-doc invariant policy:

- "High zoom does not request an 8192 px full-document texture for viewport interaction" must be
  enforced by a counter test in `async_renderer_wallclock_tests`.
- "Large selection first feedback is combined bounds only" must be enforced by
  `overlay_renderer_tests`.
- "Immediate spans preserve paint order" must be enforced by compositor golden tests.
- "Ropes cannot draw outside the source pane" must be enforced by a text-editor visual/unit test.

## Security / Privacy

No new external input surface is introduced. The work changes scheduling and cache retention for
already-loaded SVG documents. Resource caps are still security-relevant because hostile SVGs can
contain many paths, filters, or references:

- enforce tile-cache byte caps and overlay per-frame LOD caps;
- treat oversized path-outline selections as bounds-only during interaction and full-detail only
  after idle;
- avoid unbounded source rope state;
- keep cancellation points between tile/span raster tasks so large documents cannot monopolize the
  worker indefinitely.

## Rollout Plan

1. Ship M1 counters first and leave current behavior unchanged.
2. Enable immediate chrome by default after it passes pixel/clip tests.
3. Land immediate compositor spans directly in the composition render target; use the heuristic to
   decide immediate vs cached behavior, not a feature flag.
4. Replace full-document high-zoom rendering directly above a zoom threshold where full-document
   rasterization exceeds `2 x pane pixels`; use tests and counters as the rollback boundary, not a
   dormant alternate implementation.
5. Remove the full-canvas high-zoom overlay/content paths only after the replay corpus and manual
   perf targets are green.

## Alternatives Considered

- **Only increase async cancellation.** This helps stale work, but does not reduce the size of the
  next requested render. The 1.64 s worker render after zoom would still exist.
- **Only lower `kMaxCanvasDim`.** It bounds memory but makes high zoom blurrier and still rasterizes
  the wrong area.
- **Revive progressive intermediate frames from 0034.** Rejected because stale canvas tile geometry
  caused layer jumps. The new plan uses coherent overview/tile fallbacks.
- **Tile everything immediately.** Too risky for filters; Inkscape's filter tiling history shows
  repeated filter recalculation can make high-zoom filtered documents slower. Start with viewport
  rendering and filter-aware cached layers.

## Open Questions

- What scale bands are acceptable before visible blur feels worse than a short refinement delay?
- Can the editor presentation path move split-tile composition onto Geode so immediate spans no
  longer need transient tile payloads for ImGui presentation?
- Can `RenderingInstanceComponent` expose enough effect/bounds metadata to classify cheap spans
  without a second tree walk?
- Should select-all mean every renderable geometry element, or should it select top-level editable
  objects and expand only on demand?
