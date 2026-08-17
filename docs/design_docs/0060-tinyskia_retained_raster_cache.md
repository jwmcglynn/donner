# Design: TinySkia Retained Raster Caching

**Status:** Draft
**Author:** Claude Fable 5
**Created:** 2026-08-17

## Summary

The TinySkia CPU backend re-rasterizes every shape on every frame. A steady-state frame of an
unchanged document costs the same as its first frame: on the Ghostscript Tiger
(`donner/svg/renderer/testdata/Ghostscript_Tiger.svg`) at 900x900, a `-c opt` build on a
reference aarch64 host measures about 41 ms per frame via
`//donner/svg/renderer/benchmarks:engine_compare_bench --backend=tiny-skia`, and the second-frame
phase (`second_ms`) is essentially identical to the first because nearly all of the time is
re-rasterization. The interactive editor and animation workloads redraw mostly-unchanged scenes,
so this is exactly the frame we should be making cheap.

This design proposes per-shape retained render state for `RendererTinySkia`: each shape's
rasterized coverage is kept as run-length-encoded spans, invalidated by fine-grained dirty bits,
and replayed on steady frames as a span blit in paint order. A steady frame becomes a surface
clear plus per-shape span blits instead of a full outline/stroke/scan pass per shape. Retained
span caching is a well-established technique in production 2D engines; the design below derives
it from Donner's own pipeline structure and correctness constraints.

An optional, config-gated structured-concurrency prepare stage parallelizes per-shape span
(re)generation while keeping compositing strictly sequential in paint order, so pixel output is
thread-count-invariant by construction. The single-threaded default is byte-identical to today's
renderer and the threaded mode is off by default everywhere, including the Wasm build.

Scope: the TinySkia CPU backend only (`donner/svg/renderer/RendererTinySkia.cc` and targeted
additions to `third_party/tiny-skia-cpp`). The Geode GPU backend is untouched.

## Goals

- Steady-state frames of an unchanged document render 10-20x faster on raster-bound scenes
  (Tiger: from about 41 ms to 2-4 ms), measured by `engine_compare_bench`'s `second_ms` phase at
  `-c opt`.
- Cold (first) frames stay at parity or better.
- The retained path produces bit-identical output to a fresh render, asserted by a
  mode-comparison test over the golden and resvg corpora, for every supported scene.
- Property changes invalidate only the retained state they affect: a fill color change rebuilds
  nothing; a gradient stop change rebuilds only the color ramp; a stroke property change rebuilds
  only stroke spans; geometry, transform, or clip changes rebuild outline and spans.
- Steady-state prepare performs zero heap allocations, asserted by an allocation-count test.
- With the concurrency feature enabled, output is identical for every thread count, including the
  zero-thread inline mode.

## Non-Goals

- No change to the GPU backend's architecture. Geode already replays resident geometry; this
  design gives the CPU backend an analogous steady-state shape without touching Geode.
- No partial or damage rendering. Steady frames still clear the surface and blit every visible
  shape; skipping unchanged screen regions is future work layered on top of this cache.
- No relaxation of full SVG feature semantics. Filters, masks, patterns, and clip paths must
  render identically. Initially the following bypass retention entirely and re-render through the
  existing immediate-mode code: filter layers, masks, pattern tiles, isolated layers
  (group opacity / mix-blend-mode / isolation), text, and images. See "Retention scope" below.
- No changes to tiny-skia-cpp's pixel pipeline math or frame storage conventions. This design
  inherits whatever storage convention the pipeline uses (governed by
  [0028-2](0028-2-tinyskia_premul_internal.md)), and the retained path must be byte-identical
  under it.
- No multi-threaded blitting or tiled parallel compositing. Only the prepare stage may run
  concurrently; pixels are always written sequentially in paint order.

## Next Steps

- Land the span-capture seam in tiny-skia-cpp (a capturing `Blitter` plus a replay entry point)
  with byte-identity tests at the tiny-skia level.
- Build the retained-node cache and validation keys in `RendererTinySkia` behind a runtime mode
  flag, with the mode-comparison identity test running both modes over the golden corpus.

## Implementation Plan

- [ ] Milestone 1: Span capture and replay inside tiny-skia-cpp
  - [ ] `SpanCaptureBlitter` implementing `tiny_skia::Blitter`, recording packed coverage runs.
  - [ ] Replay path that feeds captured runs back through the pipeline blitter
        (`blitH` / `blitAntiH` / `blitAntiRect`) with a caller-supplied `Paint`.
  - [ ] Byte-identity unit tests: capture+replay equals direct `Painter::fillPath` /
        `Painter::strokePath` / `Painter::fillRect` for fills, strokes, dashes, and rect
        fast paths, including low-alpha antialiased edge pixels under the pipeline's
        root-surface store rounding.
- [ ] Milestone 2: Retained node cache in `RendererTinySkia`
  - [ ] `RetainedShapeNode` storage keyed by render-tree entity; validation keys per
        property class.
  - [ ] Steady-frame path: surface clear plus in-order span blits; opaque interior runs as
        straight stores.
  - [ ] Mode-comparison identity test over the renderer golden corpus and resvg suite.
  - [ ] Per-dirty-class invalidation tests (mutate one property class, assert identity with a
        fresh render).
- [ ] Milestone 3: Memory policy
  - [ ] Per-document span budget, eviction, whole-document fallback, and opt-out API.
  - [ ] Memory accounting counters surfaced through the benchmark output.
- [ ] Milestone 4: Structured-concurrency prepare (config-gated, default off)
  - [ ] Joinable retained nodes with idempotent completion wait; zero-thread inline mode.
  - [ ] Worker pool with per-slot scratch pools; slot zero reserved for the caller thread.
  - [ ] Thread-count determinism test (identity across thread counts 0, 2, 8).
- [ ] Milestone 5: Steady-state allocation assertions and performance validation
  - [ ] Zero-allocation steady-prepare test.
  - [ ] Before/after `engine_compare_bench` numbers recorded for Tiger and the resvg-derived
        raster-heavy scenes.

## Background

### The current pipeline, per frame

`RendererTinySkia::draw` constructs a fresh `RendererDriver` and traverses the prepared render
tree every frame (`RendererTinySkia.cc`, `RendererDriver.cc`). For each shape instance the
driver calls `renderer_.setTransform(...)`, `renderer_.setPaint(...)`, and
`RendererDriver::drawPathWithPaintOrder`, which emits `renderer_.drawPath(pathShape, stroke)`
(or `drawRect` / `drawEllipse` for the axis-aligned fast paths).

Inside `RendererTinySkia::drawPath`, every call re-does the full pipeline:

1. `toTinyPath` converts the `PathSpline` into a `tiny_skia::Path` (allocation + copy).
2. `makeFillPaint` / `makeStrokePaint` rebuild the `tiny_skia::Paint`, including
   `instantiateGradientShader` re-resolving gradient stops for gradient paints.
3. For strokes, dash arrays are re-normalized and `tiny_skia::Stroke` state is rebuilt; the
   stroker and dasher re-expand the outline.
4. `tiny_skia::Painter::fillPath` / `strokePath` run edge building (`EdgeBuilder`,
   `AnalyticEdge`) and the scan converter (`scan::PathAa::fillPath`), which produces per-scanline
   coverage as `AlphaRuns` (run-length encoded alpha) and hands it to a pipeline `Blitter` via
   `blitH` / `blitAntiH` / `blitAntiRect`.
5. The pipeline blitter applies the paint (solid color, gradient, pattern) and blends into the
   frame pixmap, storing pixels under the pipeline's frame storage convention (governed by
   [0028-2](0028-2-tinyskia_premul_internal.md)), whose low-alpha rounding behavior is
   observable in the golden corpus.

Also relevant: `beginFrame` recreates the frame pixmap each frame, and every piece of
intermediate state (paths, edges, alpha runs, gradient shaders) is transient. The coverage
information that took nearly all of the 41 ms to compute is thrown away at the end of step 4.

The key structural observation: the `tiny_skia::Blitter` interface is the complete pixel effect
of scan conversion. Steps 1-4 exist only to produce the sequence of blit calls; step 5 is the
only step that touches pixels. If the blit-call sequence for a shape is recorded once and
replayed with the same paint state, the pixel output is the same stores in the same order. That
is the retention seam this design uses, and it is what makes bit-identity a checkable property
rather than an aspiration.

### Why the frame is raster-bound

Coverage generation cost scales with path complexity (curve flattening, edge sorting, scanline
accumulation) while replaying coverage scales with covered area. For scenes like Tiger, which
is hundreds of filled and stroked paths, coverage generation dominates. The
`engine_compare_bench` header documents the production asymmetry this design removes: the Geode
backend replays resident geometry on the second frame while TinySkia re-rasterizes every path.

### Relationship to incremental invalidation (0005)

[Design 0005](0005-incremental_invalidation.md) gives the document side of this problem:
`DirtyFlagsComponent` tracks per-entity `Style` / `Layout` / `Transform` / `WorldTransform` /
`Shape` / `Paint` / `Filter` / `RenderInstance` staleness so `instantiateRenderTree()` can skip
recomputation. This design is the renderer-side counterpart: even with a perfectly clean render
tree, today's backend re-rasterizes everything. The two compose: 0005 makes prepare-the-tree
cheap on unchanged frames; this design makes rasterize-the-tree cheap.

The dirty flags themselves are consumed (cleared) by the compute systems during
`instantiateRenderTree()`, so the renderer cannot read them directly at draw time. Section
"Invalidation and validation keys" describes the per-category revision counters that carry the
same information across to the renderer.

### Prior art

Two-stage rasterization, where an outline is scan-converted once into a compact coverage
representation and then applied to pixels separately, is the structure of the FreeType
rasterizer algorithm family, and retained span/RLE coverage caching is a well-established
technique in production 2D engines. tiny-skia-cpp already represents coverage this way
transiently (`AlphaRuns`); this design makes the representation durable per shape.

## Proposed Architecture

### Retained state per shape

Each retained shape node (`RetainedShapeNode`, keyed by the render-tree entity of the
`RenderingInstanceComponent`) stores:

- **Fill coverage spans**: packed `{x, y, len, coverage}` runs in device space, exactly the runs
  the scan converter emitted. Interior rows compress to a few long runs at full coverage;
  antialiased edges become short runs (the same shape `AlphaRuns` produces today). Runs are
  stored sorted by `y` then `x`.
- **Stroke coverage spans**: the same representation for the stroked outline, with dashing,
  caps, and joins already baked in by the stroker/dasher at capture time.
- **Gradient color ramp**: for gradient paints, the instantiated `tiny_skia::Shader` (the
  product of `instantiateGradientShader`), including any stop-ramp lookup table the gradient
  pipeline stage precomputes. If an explicit ramp LUT stage is introduced, the cold path must
  use the identical LUT so identity holds; this is called out under Open Questions.
- **Device-space bounding box** of the union of fill and stroke spans, used for clipped blits
  and for bounds queries without touching span data.
- **Fast-path flag** for axis-aligned rectangles: `drawRect` fills that go through
  `Scan::fillRect` / `fillRectAa` retain the rect plus its edge alphas instead of spans and
  replay via `blitRect` / `blitAntiRect`.
- **Validation keys** described below.

Span storage is per-shape `std::vector`-backed with capacity retention: invalidation resets the
count but keeps the allocation, so a shape that oscillates between two geometries settles into
zero-allocation rebuilds.

### Capture

Capture runs inside the existing draw calls. When retention is active and the current draw is
retainable (see "Retention scope"), `drawPath` routes scan conversion through a
`SpanCaptureBlitter` (a `tiny_skia::Blitter` subclass added to tiny-skia-cpp) instead of
letting `Painter::fillPath` construct only its internal pipeline blitter.
`scan::PathAa::fillPath(path, fillRule, clip, blitter)` already accepts a caller-provided
blitter, so capture requires no changes to the scan converter itself. The capture blitter
records runs and forwards to the pipeline blitter in the same call, so the cold frame renders
and captures in one pass; capture overhead is the run stores only.

### Steady-frame replay

A steady frame is:

1. Surface clear (as `beginFrame` does today).
2. For each visible shape in paint order: validate the node (cheap key compare), then blit its
   fill spans and stroke spans in the shape's fill/stroke/paint-order sequence.

Replay feeds the recorded runs to the same pipeline blitter configuration the cold path uses,
constructed from the shape's current `Paint`:

- **Opaque interior spans** (coverage 255, opaque solid paint, source-over) are straight stores:
  a `memset`-style row fill of the paint color. This is the dominant case on raster-bound
  scenes.
- **Edge spans and translucent paints** blend through the normal pipeline stages, reproducing
  the exact root-surface store rounding of the active pipeline configuration so low-alpha edge
  pixels keep the bytes the golden corpus depends on (0028-2 documents how observable this
  rounding class is).
- **Color at blit**: because spans are pure coverage, the paint color is an input to replay, not
  to capture. A solid paint color change therefore rebuilds nothing.
- **Clipped blits**: spans are sorted by `y`, so a clip band or tight viewport selects its span
  sub-range by binary search on `y` rather than walking the whole array.

### Invalidation and validation keys

Retained state is invalidated by property class, mirroring `DirtyFlagsComponent` categories.
Because the compute systems clear the dirty flags before the renderer runs, each category
increments a small per-entity revision counter when marked (a `RenderRevisionsComponent`
extension of the 0005 machinery); retained nodes record the revisions they were built from and
compare on validation.

| Change class                                  | Rebuilds                                     |
| --------------------------------------------- | -------------------------------------------- |
| Geometry (`Shape`), transform (`Transform` /  | Outline conversion, fill spans, stroke spans |
| `WorldTransform`), clip chain                 |                                              |
| Solid paint color, `fill-opacity`,            | Nothing (applied at blit)                    |
| `stroke-opacity`, `currentColor`              |                                              |
| Gradient stop list or stop colors             | Gradient ramp only (spans untouched)         |
| Stroke properties (width, caps, joins, dash   | Stroke spans only (fill spans untouched)     |
| array/offset, miter limit)                    |                                              |
| Paint server kind change (solid to gradient,  | Paint object only; spans untouched           |
| gradient to solid)                            |                                              |
| Paint becomes a pattern, or shape enters a    | Node leaves retention; immediate mode        |
| non-retained context                          |                                              |

Clip participates conservatively in the first iteration: the node key includes the identity of
the clip chain affecting the shape, and any clip change rebuilds the node's spans. Because the
clip mask is actually applied at blit time (the pipeline blitter takes the mask), a later
refinement can drop clip from the rebuild set; that is listed under Open Questions rather than
assumed.

### Retention scope

Retention initially covers shape draws (`drawPath`, `drawRect`, `drawEllipse`) with solid or
gradient paint, emitted directly to the root surface (`surfaceStack_.empty()`). Everything else
bypasses retention and renders through the unchanged immediate-mode code:

- Filter layers (`pushFilterLayer` content and composition).
- Masks (`pushMask` capture and content).
- Pattern tiles (`beginPatternTile` recording and pattern-painted shapes).
- Isolated layers (`pushIsolatedLayer` for group opacity, mix-blend-mode, isolation).
- Text (`drawText`) and images (`drawImage` / `drawBitmap`).

Bypassed content is drawn between retained blits in paint order, so ordering semantics are
unchanged. This split keeps the identity argument simple (retained draws replay the exact blit
sequence; bypassed draws run the exact existing code) while covering the raster-bound scenes
that motivate the design. Tiger is entirely within the retained set. Widening the retained set
(for example, retaining an isolated layer's composed pixmap) is future work.

### Memory policy

- **Per-shape storage** keeps capacity across invalidations, as above.
- **Per-worker scratch pools** hold the transient prepare intermediates (path conversion
  buffers, edge lists, stroke/dash output, scanline cells), sized high-water per worker slot,
  so steady-state prepare performs zero heap allocations once pools are warm. Slot zero belongs
  to the caller thread and is the only slot used in single-threaded mode.
- **Cost model**: a run is 8 bytes packed (`x:u16, y:u16` row-relative, `len:u16, coverage:u8`
  plus padding/format tag; exact packing decided in Milestone 1). A shape's run count is
  approximately (rows x interior runs per row) + (antialiased edge pixels). For Tiger at
  900x900 this is on the order of a few hundred thousand runs total, in the low single-digit
  MiB. A pathological document (many thousands of small shapes) is bounded by the budget below,
  not by hope.
- **Budget and eviction**: a per-document retained-memory budget (default on the order of
  32 MiB, configurable). When exceeded, nodes are evicted coldest-first (least-recently-blitted)
  until under budget; an evicted shape simply renders immediate-mode and may re-enter the cache
  later. If eviction thrashes (a tracked counter crosses a threshold within a frame window),
  retention disables for the document and the renderer reverts to today's behavior wholesale.
- **Opt-out**: a document-level API disables retention explicitly for embedders that prefer the
  immediate-mode memory profile.

## Structured-Concurrency Prepare Stage

This stage is optional, config-gated, and off by default. It exists because span regeneration
after a broad invalidation (for example, a zoom changes every device transform) is
embarrassingly parallel per shape, while pixel writes are not.

### Unit of concurrency

The unit is per-shape **prepare**: outline conversion, stroke expansion, dashing, span
generation, and gradient ramp construction. Compositing and blitting stay strictly sequential
in paint order on the caller thread. Because prepare only writes into its own node and its own
worker-slot scratch pool, and every pixel is written by the sequential blit pass, pixel output
is thread-count-invariant by construction, not by testing luck.

### Joinable nodes

Each retained render node is itself the joinable unit: it carries completion state and an
idempotent wait ("join") that is safe to call any number of times from the consumer side.
Consumers join before reading:

- The blit pass joins each node immediately before blitting it (in paint order, so the caller
  naturally overlaps blitting early shapes with preparing later ones).
- Bounds queries join the node before reading its device-space bounding box.
- Clip dependencies do not schedule concurrently: a prepare that depends on another node's clip
  output joins that dependency at submission time, on the submitting thread, rather than
  deferring the join into the worker. This keeps the dependency graph trivially acyclic at the
  scheduler level and makes a stuck worker impossible by construction.

### Scheduler shape

- A small set of work-stealing queues, one per worker, plus the caller thread.
- **Zero-thread mode runs the identical code path inline**: submission with zero workers
  executes the prepare immediately on the caller thread through the same node/join structure.
  There is no separate single-threaded implementation to drift.
- C++20 coroutines are the candidate mechanism for expressing prepare tasks and joins, with a
  plain task-queue (function-object) fallback if coroutine codegen or debuggability disappoints
  on any supported toolchain. The node/join API is written so either substrate fits behind it.
- Scratch pools are indexed by worker slot; slot zero is reserved for the caller thread, which
  is what the zero-thread mode and the join-at-submission path use.

### Configuration gating

A `bool_flag` + `config_setting` in `donner/svg/renderer/BUILD.bazel`, following the existing
`:filters` / `:text` / `:renderer_backend` pattern (for example
`--//donner/svg/renderer:tiny_skia_prepare_threads`), compiles the worker pool in or out. With
the flag off (the default), the scheduler is the inline zero-thread path and no threading
primitives are linked.

### Wasm

The browser build keeps the threaded configuration off: threaded Wasm requires
SharedArrayBuffer and COOP/COEP cross-origin isolation, and the TinySkia CPU backend must not
add a second dependency on that deployment constraint (design
[0056](0056-geode_only_web_editor_runtime.md) tracks the isolation work the editor's pthread
build needs; this backend stays usable without it). The cooperative single-threaded structure
(nodes, idempotent joins, slot-zero scratch pool) must still compile and run in the Wasm build
unchanged, and does, because the zero-thread mode is the same code path.

## Security / Privacy

SVG input is untrusted. This design adds no new parsing surfaces, but it adds retained state
whose size is attacker-influenced, so the memory policy above is a security boundary, not just
a performance knob:

- The per-document budget caps retained memory regardless of shape count or coordinates;
  overflow degrades to immediate mode, never to unbounded growth. The budget/eviction paths get
  negative tests with adversarial documents (many shapes, giant device boxes, degenerate spans).
- Span capture stores only what the scan converter emitted for the clipped device area, so
  coordinates outside the surface cannot inflate storage beyond the surface-bounded run count.
- The threaded mode is compiled out by default and, when enabled, workers only run prepare on
  data owned by the node being prepared; there is no cross-document or cross-renderer sharing.
- Fuzzing: the existing SVG parser/render fuzzers run with retention enabled so the
  invalidation and eviction state machines see hostile mutation sequences; a mode-flip fuzz
  cycle (render, mutate, render, compare against fresh) doubles as a correctness oracle.

## Testing and Validation

Every invariant below names the CI target that enforces it, per this directory's rule that
claimed invariants trace to CI.

- **Byte identity, retained vs fresh** (the core invariant): a new
  `//donner/svg/renderer/tests:renderer_retained_identity_tests` renders every case twice with
  a shared corpus driver: once with a fresh renderer, once through a warmed retained cache
  (render, then render again), and asserts byte-equal snapshots. It runs over the renderer
  golden corpus and, on lanes where `--//donner/svg/renderer:resvg_test_suite_available=True`,
  over the resvg suite corpus. A retained-mode configuration of the existing golden suites
  (`renderer_tests`, `resvg_test_suite`) additionally pins retained output to the checked-in
  goldens.
- **Per-dirty-class invalidation**: in the same target, each property class from the
  invalidation table is mutated in isolation (color, gradient stops, stroke width, dash array,
  transform, geometry, clip), then the steady frame is compared byte-equal against a fresh
  render of the mutated document. This catches both under-invalidation (stale pixels) and
  mis-classified rebuilds.
- **Steady-state allocations**: `renderer_retained_identity_tests` includes cases wrapping the
  steady frame in the allocation-counting scope from
  `donner/svg/renderer/benchmarks/AllocationTracker.h` and asserting zero prepare-path
  allocations after warmup (the frame clear and snapshot are excluded and measured separately).
- **Thread-count determinism**: with the concurrency config enabled, the identity test
  parameterizes thread counts {0, 2, 8} and asserts byte-equal output across all of them. This
  runs on a CI lane that builds with `--//donner/svg/renderer:tiny_skia_prepare_threads=true`
  so the threaded configuration is compiled and exercised, per the rule that a green default
  build does not verify flag-gated code.
- **tiny-skia-level capture/replay identity**: unit tests in
  `third_party/tiny-skia-cpp` assert capture+replay equals direct painting for fills, strokes,
  dashes, rect fast paths, masks, and specifically low-alpha antialiased edges under the
  pipeline's root-surface store rounding (the failure class 0028-2 documents).
- **Performance**: before/after `engine_compare_bench --backend=tiny-skia` runs at `-c opt` on
  Tiger and raster-heavy resvg-derived scenes, recording `second_ms`, `first_ms`, ALLOC
  `phase=second`, and RSS fields. Numbers land in this doc's status updates only as measured
  output, never estimated.

## Performance

- **Steady-frame target**: 10-20x on raster-bound scenes; Tiger from about 41 ms to 2-4 ms
  `second_ms` at 900x900, `-c opt`, reference aarch64 host. The floor is the surface clear plus
  covered-area blend cost, which is why opaque-interior straight stores matter.
- **Cold-frame target**: parity or better. Capture adds run stores to the cold path (a strict
  subset of the work the pipeline blitter already does per pixel); the concurrency stage, where
  enabled, can make cold frames faster than today. Regression bar: cold `first_ms` within noise
  of the pre-change baseline on the benchmark corpus.
- **Measurement discipline**: all claims go through `engine_compare_bench`'s existing phases
  (`first_ms`, `second_ms`, ALLOC per phase, RSS) so numbers are comparable across the design's
  lifetime; within-engine before/after deltas only, per the benchmark's own caveats.

## Risks and Open Questions

- **Memory growth bounds.** The budget/eviction policy is designed but its default (32 MiB
  order) is a guess until measured across the resvg corpus and editor documents. Open: should
  the budget scale with surface area? What eviction granularity (whole node vs stroke-only)
  pays for its complexity?
- **Interaction with the frame storage convention.** Replay must reproduce the exact
  root-surface store rounding of the active pipeline configuration; 0028-2 governs that
  convention and documents how observable the low-alpha rounding class is. The capture/replay
  identity tests target this class directly and must hold across any storage-convention change,
  and any future explicit gradient ramp LUT must be shared by cold and steady paths or identity
  breaks silently.
- **Clip-chain invalidation fan-out.** A clip path change invalidates every shape it affects;
  for a document-root clip that is everything. Conservative rebuild is correct but makes some
  single-property edits cost a full re-rasterization. Open: apply clip masks purely at blit
  time (the pipeline already takes the mask per blit) so clip changes rebuild nothing, at the
  cost of keying blits by mask identity; decide after measuring real editor clip-edit
  frequency.
- **Eviction heuristics.** Least-recently-blitted is simple but a zooming viewport can cycle
  the working set. Open: protect shapes by blit frequency, or by rebuild cost, and what the
  thrash-detection window should be.
- **Gradient ramp representation.** Whether to keep retaining the instantiated shader object or
  introduce an explicit quantized ramp LUT stage in tiny-skia-cpp (shared by both paths) is a
  Milestone 1 decision; the LUT is only worth it if the shared-stage identity constraint holds.
- **Revision-counter plumbing.** The `RenderRevisionsComponent` extension must be marked by the
  same mutation hooks that mark `DirtyFlagsComponent`; a missed hook is an under-invalidation
  bug. The per-dirty-class tests are the net, but the hook audit is real work.

## Alternatives Considered

- **Retain the frame, diff the document (full-frame cache).** Keep the last frame and re-render
  only when anything changed. Rejected as the primary mechanism: it collapses to all-or-nothing
  (any change pays 41 ms) and offers nothing for animation or drag workloads, which change a
  few properties per frame. It falls out of this design for free anyway: an unchanged document
  validates every node and the frame rebuild is pure blits.
- **Retain rasterized per-shape pixmaps instead of spans.** Simpler replay (image blit), but
  memory scales with bounding-box area rather than covered area, translucent overlap double
  counts, and per-shape pixmaps cannot do color-at-blit without re-tinting. Spans keep the
  color/coverage separation that makes the invalidation table cheap.
- **Retain tiny_skia::Path / edge lists instead of coverage.** Saves only outline conversion
  and edge building; scan conversion (the accumulation loop) still runs per frame, and that is
  where the time goes. Measured option if span memory proves prohibitive, but it caps the win
  well below the target.
- **Parallel tiled blitting for the steady frame.** Would parallelize the remaining 2-4 ms, but
  makes pixel-write order a function of tile scheduling, so identity across thread counts would
  need per-tile determinism arguments instead of holding by construction. Out of scope while
  the sequential blit meets the target.

## Future Work

- [ ] Damage/partial rendering on top of retained nodes (dirty-region union of invalidated
      nodes' device boxes), explicitly out of scope here.
- [ ] Widen the retained set: isolated-layer result pixmaps, retained mask alpha, pattern tile
      reuse across shapes.
- [ ] Blit-time clip masks to remove clip from the rebuild set (see Open Questions).
- [ ] Evaluate enabling the prepare stage for cold frames in the editor's background render
      workers once the determinism lane has soak time.
