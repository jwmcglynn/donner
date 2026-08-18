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

- Steady-state frames of an unchanged document render substantially faster on raster-bound
  scenes, measured by `engine_compare_bench`'s `second_ms` phase at `-c opt`. The target was
  10-20x; the measured result is 2.4x on the tiger, and Performance says why the rest is not
  reachable without giving up the identity argument.
- Cold (first) frames stay at parity or better.
- The retained path produces bit-identical output to a fresh render, for every supported scene.
- A change invalidates at least the retained state it affects, and never less. Fine-grained
  classification is a goal only where it can be had without an invalidation rule that has to be
  kept in sync with the property system by hand; today a stroke change rebuilds only stroke
  coverage, and every other change rebuilds the shape.
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

- Decide whether the steady frame's remaining cost (blending covered area, not generating it) is
  worth attacking with opaque-interior stores, and whether that can live inside the shared
  blitter construction so identity still holds by construction rather than by proof.
- Add a colour-only fast path: when a draw does not derive its blitter's paint from the caller's
  (which a hairline stroke and a transformed gradient both do), coverage is independent of the
  paint and a replay could take the fresh paint instead of rebuilding.
- Give corpus-wide retained-versus-fresh identity a CI lane, instead of leaving
  `DONNER_TINYSKIA_RETAINED=compare` as a diagnostic someone has to remember to run.
- Run the SVG parser and render fuzzers with retention enabled, so the invalidation and eviction
  state machines see hostile mutation sequences.
- Make a steady frame allocation-free, which needs paint comparison that does not rebuild a
  gradient's stop list every frame, and then assert it.

## Implementation Plan

- [x] Milestone 1: Span capture and replay inside tiny-skia-cpp
  - [x] `SpanCaptureBlitter` implementing `tiny_skia::Blitter`, recording packed coverage runs.
  - [x] Replay path that feeds captured runs back through the pipeline blitter with a
        caller-supplied `Paint`. The blit surface is wider than this plan first listed; see
        "Milestone 1 decisions" below.
  - [x] Byte-identity unit tests: capture+replay equals direct `Painter::fillPath` /
        `Painter::strokePath` / `Painter::fillRect` for fills, strokes, dashes, and rect
        fast paths, including low-alpha antialiased edge pixels under the pipeline's
        root-surface store rounding.
### Milestone 1 decisions

Milestone 1 landed as `third_party/tiny-skia-cpp/src/tiny_skia/SpanCapture.{h,cpp}` with
`src/tiny_skia/tests/SpanCaptureTest.cpp`, plus an optional `BlitterWrapper` hook on `Painter`
so a capture is the same draw observed rather than a second implementation of it. Three
decisions this document left open are now settled by that code.

**The blit surface is wider than this document first enumerated.** The plan above named
`blitH` / `blitAntiH` / `blitAntiRect`. Reading the scan converters shows the caller-supplied
blitter also receives `blitV`, `blitAntiH2`, `blitAntiV2`, and `blitRect`:

| Blitter method | Emitted by                                                             |
| -------------- | ---------------------------------------------------------------------- |
| `blitH`        | aliased path fill, aliased hairline, antialiased fill's full-coverage runs |
| `blitAntiH`    | antialiased hairline scanlines and antialiased rect partial rows        |
| `blitV`        | antialiased fill's single-column spans, antialiased rect vertical edges, antialiased hairline vertical and near-vertical segments |
| `blitAntiH2`   | antialiased fill's edge coverage pairs, antialiased hairline caps       |
| `blitAntiV2`   | antialiased hairline vertical and near-vertical segments                |
| `blitAntiRect` | antialiased fill's axis-aligned span optimization                       |
| `blitRect`     | aliased rect fill, antialiased rect interior                            |
| `blitMask`     | no scan converter emits it (see below)                                  |

`blitMask` is the one method no scan converter calls: a clip mask reaches the blitter as
pipeline state applied per blit, not as a separate blit, and the only callers are direct ones
such as `MaskOps`. The capture blitter therefore marks the capture invalid if it ever receives
a `blitMask`, so the case fails closed instead of silently losing pixels, and a unit test pins
that. The document's underlying claim, that the `Blitter` interface is the complete pixel
effect of scan conversion, holds: capture covers every method the conversion paths reach.

**Packing.** A run is a fixed 16-byte record (`CapturedSpan`), not the 8-byte
`{x, y, len, coverage}` this document estimated, and the recorded unit is the blit call rather
than a flattened span. The wider interface is why: `blitAntiH2` and `blitAntiV2` carry two
coverages one call must apply together, `blitAntiRect` carries a signed origin plus two edge
coverages, and `blitRect` and `blitV` span a height. Flattening those into single-coverage
spans would enter the blitter a different number of times with different arguments, and the
pipeline blitter's per-call fast paths (row fills, two-pixel coverage runs, whole-column
writes) do not decompose to the same bytes. Recording calls keeps identity true by
construction. The record is `{int32 x, int32 y, uint16 length, uint16 height, uint8 alpha0,
uint8 alpha1, uint8 op, uint8 padding}`; coordinates are signed because the scan converter
calls `blitAntiRect` with a left edge that can be -1, and a value that does not fit marks the
capture invalid rather than truncating.

**Surface binding.** A recorded run is a device-space rectangle that the scan converter had
already clipped to the capture surface, and replay does not re-clip it, so a capture carries
the surface size it was recorded against and a replay onto a different size is refused. That
joins the same fail-closed family as tiled draws and field overflow. It is a memory-safety
boundary, not just bookkeeping: without it a replay onto a smaller surface reaches the pipeline
blitter's rect fast path with an origin past the buffer, where the row-width arithmetic
underflows into an out-of-bounds write (found by review, reproduced under AddressSanitizer, and
now covered by a regression test). The pipeline blitter's `blitRect` also early-outs on an
origin past the surface, so the invariant is enforced at both ends rather than resting on a
silent contract. This matters directly for Milestone 2: a retained node outlives viewport
resizes, and its stale runs must be refused rather than replayed against the new surface.

**Gradient ramp.** Milestone 1 keeps the instantiated shader in the retained `Paint` and adds
no ramp lookup table. Both a cold draw and a replay hand that shader to the same blitter
constructor, so the stops are resolved by one shared step rather than two that would have to be
proven equal. If a quantized ramp is ever introduced it must live inside that shared
construction for the same reason.

**Measured on a 512x512 antialiased path fill** (`render_perf_bench_native`, `-c opt`,
aarch64, 15 repetitions, median): capture costs 5.1 percent on top of the cold fill (377.8 us
to 397.0 us), and the recorded coverage occupies 26.4 KB (1650 runs). Replaying that coverage
takes 297.0 us, 0.79x the cold fill. That last number is the honest early signal: a single
large path spends most of its time blending covered area rather than scan converting, so the
steady-state win this design targets has to come from scenes with many small and medium paths,
where coverage generation dominates and covered area does not. Milestone 5 measures that on
Tiger.

- [x] Milestone 2: Retained node cache in `RendererTinySkia`
  - [x] `RetainedSpansComponent` storage keyed by the render-tree entity, with a validation key
        compared per draw. The key is the draw's own inputs rather than per-property revision
        counters; see "Milestone 2 decisions" below.
  - [x] Steady-frame path: surface clear plus in-order replays. Opaque interior runs are NOT
        straight stores; they go through the same blitter a cold draw builds, which is what
        keeps identity true by construction. That decision is why the measured win is smaller
        than this document projected; see Performance.
  - [x] Retained-versus-fresh identity test, plus a run mode that turns the golden and resvg
        suites into corpus-wide identity runs. The run mode is a diagnostic with no CI lane
        setting it; see Testing and Validation.
  - [x] Per-dirty-class invalidation tests (mutate one property class, assert identity with a
        fresh render).
- [x] Milestone 3: Memory policy
  - [x] Per-document byte budget, coldest-first eviction, whole-document fallback, and a
        runtime enable/disable plus budget setter on the renderer.
  - [x] Memory accounting surfaced through the benchmark's `RETAINED` line.
- [ ] Milestone 4: Structured-concurrency prepare (config-gated, default off)
  - [ ] Joinable retained nodes with idempotent completion wait; zero-thread inline mode.
  - [ ] Worker pool with per-slot scratch pools; slot zero reserved for the caller thread.
  - [ ] Thread-count determinism test (identity across thread counts 0, 2, 8).
- [ ] Milestone 5: Steady-state allocation assertions and performance validation
  - [ ] Zero-allocation steady-prepare test. Not met and not attempted: a steady frame still
        rebuilds each shape's `tiny_skia::Paint` in order to compare it, which allocates for
        gradient paints.
  - [x] Before/after `engine_compare_bench` numbers recorded for Tiger and other raster-heavy
        scenes; see Performance.

### Milestone 2 decisions

**Why validation compares inputs instead of counting revisions.** The alternative is a
per-entity revision counter bumped wherever `DirtyFlagsComponent` is marked, which needs every
mutation site to remember to bump it and turns a missed site into an under-invalidation bug,
where a shape keeps painting its old pixels. Comparing the draw's own inputs has no such site
list: whatever a mutation does to the document, the next frame either builds the same transform,
paint, stroke, clip and surface it built before, or it does not. The one input too large to
compare, the outline, is handled by dropping the entry from a signal that already exists and is
already load-bearing for the Geode encode cache. The cost is granularity, and Invalidation and
validation keys states exactly how coarse.

**Frame identity belongs to the document.** Entries live on the document's registry, so several
renderers can meet the same entry, and each frame takes its identity from a counter on the
document rather than from a renderer-local one. Otherwise two renderers would agree on frame
numbers and each would read the other's draw as a second draw of the same shape, which is the
signal that makes an entity stop retaining.

**Known limitation: several renderers retaining one document thrash.** One entry describes one
draw, so two renderers with different surfaces or transforms drawing the same document take
turns re-capturing it. Output stays correct, because a key that does not match is never
replayed; only the work is wasted. The case that matters in practice, one renderer per document,
is unaffected, and the corpus harness deliberately keeps its reference renderer out of retention
so it cannot perturb the retained one.

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

Retained state lives on the entity a shape's geometry came from, as a `RetainedSpansComponent`
holding two slots, one for the fill pass and one for the stroke pass. Each slot holds:

- **Coverage runs** in device space, exactly the blits the scan converter emitted
  (`CapturedSpans`; see "Milestone 1 decisions" for the record layout). Interior rows compress
  to a few long runs at full coverage; antialiased edges become short runs. Runs are stored in
  emission order, which is what replay depends on.
- **The paint the recorded draw built its blitter from**, which is not always the paint the
  caller passed: a hairline stroke folds its coverage into the shader opacity, and a transformed
  draw transforms the shader. Replaying with this paint is what reproduces the draw. A gradient
  lives inside it as an instantiated shader, so no separate ramp is kept.
- **The validation key**, described below.

Storage keeps its capacity across invalidations, so a shape that is rebuilt repeatedly settles
into allocation-free captures.

The outline itself is not part of this state. A capture takes the shape's `tiny_skia::Path` from
the per-entity conversion cache, and a replay needs no outline at all, so the two caches compose
rather than duplicate: retention never converts a path on its own, and a shape that has to
rasterize again after an invalidation still takes the cached conversion.

Nothing else is retained. There is no cached bounding box (nothing reads one), and axis-aligned
rectangles need no special case: a rect draw's `blitRect` and `blitAntiRect` calls are recorded
by the same packed records as everything else.

### Capture

Capture runs inside the existing draw calls. When retention is active and the current draw is
retainable (see "Retention scope"), `drawPath` routes scan conversion through a
`SpanCaptureBlitter` (a `tiny_skia::Blitter` subclass added to tiny-skia-cpp) instead of
letting `Painter::fillPath` construct only its internal pipeline blitter.
Every scan entry point already accepts a caller-provided blitter, so capture requires no
changes to the scan converters themselves. What Milestone 1 did add is an optional
`BlitterWrapper` hook on `Painter`: each draw builds its pipeline blitter, offers it to the
wrapper, and drives scan conversion with whatever the wrapper returns. That keeps a capture the
same draw observed, rather than a second copy of `Painter`'s transform, tiling, and
blitter-construction logic that could drift out of identity. The hook also reports the paint
the draw actually built its blitter from, which is not always the caller's paint: hairline
strokes fold their coverage into the shader opacity and a transformed draw transforms the
shader. The capture blitter records runs and forwards to the pipeline blitter in the same call,
so the cold frame renders and captures in one pass.

### Steady-frame replay

A steady frame is:

1. Surface clear (as `beginFrame` does today).
2. For each visible shape in paint order: compare the validation key, then replay the fill and
   stroke runs in the shape's fill/stroke/paint-order sequence.

Replay builds the blitter exactly as a direct draw with the retained paint would, and feeds the
recorded runs to it. That is the whole of the identity argument: the same blitter receives the
same calls in the same order, so it performs the same stores.

- **Clip masks** are supplied at replay, not baked into coverage. Scan conversion clips to the
  surface and the mask reaches the blitter as per-blit pipeline state, so recorded coverage is
  mask-independent and a replay is handed whatever mask a fresh draw would get.
- **Edge runs and translucent paints** blend through the normal pipeline stages, reproducing the
  root-surface store rounding of the active pipeline configuration, so low-alpha edge pixels
  keep the bytes the golden corpus depends on (0028-2 documents how observable this rounding
  class is).
- **No separate fast path for opaque interiors.** A `memset`-style store for full-coverage runs
  under an opaque solid paint would be a second implementation of the blitter's arithmetic, and
  identity would stop holding by construction and start needing to be proven. That is the main
  reason the steady frame is faster but not as fast as this document first projected; see
  Performance.

### Invalidation and validation keys

A recording is replayed only while every input the recorded coverage depended on is unchanged.
Rather than tracking which properties were marked dirty, the renderer compares those inputs
directly, so no mutation hook has to be audited for the result to be sound.

The key is the device transform, the `tiny_skia::Paint` the draw was handed, the stroke, the
identity of the clip mask, the surface size, the fill rule, and whether the stroke opened its
dash seams. Paint comparison is real deep equality over the shader, which is why a gradient
stop edit is caught by the same comparison as a colour edit, with nothing to remember to mark.

Geometry is the one input too large to compare per frame. It is handled by dropping the entry
entirely from the resolved-path update and destroy signals, the mechanism the Geode encode
cache already relies on: `ShapeSystem`'s content-equality gate means those signals fire when the
outline actually changed and stay quiet on an idle re-render.

| Change class                                             | Effect on the next frame            |
| -------------------------------------------------------- | ----------------------------------- |
| Geometry                                                  | Entry dropped; shape re-rasterizes  |
| Transform, clip, surface size, fill rule, dash seam       | Key mismatch; shape re-rasterizes   |
| Any paint change, including a solid colour or a gradient stop | Key mismatch; shape re-rasterizes |
| Stroke width, caps, joins, dash array or offset, miter    | Stroke pass re-rasterizes; fill pass replays |
| Paint becomes a pattern, or the shape enters a non-retained context | Entry unused; immediate mode |

This is coarser than the per-property table this design first proposed: a colour change
rebuilds coverage that a colour change cannot affect. The conservative direction is the safe
one, and the case the design exists for, a frame where nothing changed, is unaffected. A
colour-only fast path is possible on top of this and is under Next Steps.

The clip identity is conservatism, not a correctness dependency. Each clip-stack depth remembers
the mask it last held, and a rebuilt mask that compares byte-equal keeps that depth's identity;
any change takes a new one. Identities are renderer-local, so two renderers drawing one document
number their clips independently, and nothing rests on that, because coverage does not depend on
the mask and a replay is handed the mask in effect at replay time.

### Retention scope

Retention covers `drawPath` with solid or gradient paint, emitted directly to the root surface
(`surfaceStack_.empty()`) for a shape that carries a source entity. `drawRect` and `drawEllipse`
are not retained and do not need to be: the driver never emits them, they exist for replaying a
recorded snapshot, and a snapshot has no stable identity to key on. Everything below bypasses
retention and renders through the unchanged immediate-mode code:

- Filter layers (`pushFilterLayer` content and composition).
- Masks (`pushMask` capture and content).
- Pattern tiles (`beginPatternTile` recording and pattern-painted shapes).
- Isolated layers (`pushIsolatedLayer` for group opacity, mix-blend-mode, isolation).
- Text (`drawText`) and images (`drawImage` / `drawBitmap`).
- Draws that also paint a context-paint capture surface, which are two draws sharing one
  outline and cannot be described by one entry.
- Entities drawn more than once in a frame. Shadow-tree instancing (`use`), markers, and a
  `paint-order` that splits fill from stroke all draw one data entity several times, and one
  entry cannot describe several draws. Such an entity stops retaining rather than re-capturing
  on every draw.

Bypassed content is drawn between replays in paint order, so ordering semantics are unchanged.
This split keeps the identity argument simple: retained draws replay the exact blit sequence,
bypassed draws run the exact existing code. Tiger is entirely within the retained set. Widening
the retained set (for example, retaining an isolated layer's composed pixmap) is future work.

### Memory policy

- **Per-shape storage** keeps capacity across invalidations, as above.
- **Cost model**: a run is 16 bytes packed. A shape's run count is approximately (rows x
  interior runs per row) + (antialiased edge pixels), and the analytic scan converter emits long
  interior runs, so the count runs well below one per covered pixel: a 512x512 antialiased path
  measures 1650 runs, 26.4 KB. Measured on real documents, the tiger holds 3.41 MiB across 305
  retained passes at 900x900 and the lion 612 KiB across 132.
- **Budget and eviction**: a per-document retained-memory budget, 32 MiB by default and settable
  on the renderer. It charges the whole of an entry, not only its coverage: the runs plus their
  kept capacity, the two paints beside them, and the entry itself. When the budget is exceeded,
  entries not drawn in the current frame are evicted coldest-first until the document is back
  under it; an evicted shape rasterizes and may re-enter later. If evicting every entry from an
  earlier frame still leaves the document over budget, its working set does not fit at all, so
  retention turns off for the document rather than evicting and re-capturing the same shapes
  every frame. Raising the budget lets it try again.
- **Clip masks a renderer remembers** are not part of that budget. They are bounded instead:
  eight depths at most, one mask per depth, dropped whenever the surface changes size. Past that
  depth a clip takes a fresh identity per draw, so shapes under it rasterize.
- **Opt-out**: retention is off by default and enabled per renderer. Turning it off hands back
  everything the document was holding, since entries outlive the renderer that made them.
- **Per-worker scratch pools** for transient prepare intermediates belong to the concurrency
  stage and do not exist yet.

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
- Any consumer that reads a node's prepared state joins it first.
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
a performance knob. What the budget caps, precisely:

- Every byte a retained entry holds is charged: the recorded coverage including the capacity it
  keeps across a rebuild, the two paints kept beside it (a gradient paint owns its stop list),
  and the entry itself. Exceeding the budget evicts coldest-first; a working set that does not
  fit at all disables retention for the document. Growth is bounded either way.
- Not charged, and bounded by other means: the per-depth clip masks a renderer remembers, which
  are capped at eight depths and dropped when the surface changes size, so a document chooses
  neither how many it holds nor how large they are; and the entry shell of an entity that has
  stopped retaining, which is one small component per entity, bounded like any per-entity
  component by the document's own size.
- Span capture stores only what the scan converter emitted for the clipped device area, so
  coordinates outside the surface cannot inflate storage beyond the surface-bounded run count.
  A capture is bound to the surface size it was recorded against and a replay onto a different
  size is refused, so a retained capture that outlives a viewport resize cannot write outside
  the new surface.
- Retention holds no pixel data of its own: coverage carries alpha, and the paint that colors it
  is supplied at replay, so an evicted or disabled document leaks nothing about what it drew.
- The threaded mode does not exist yet; when it does, workers must only run prepare on data
  owned by the node being prepared, with no cross-document or cross-renderer sharing.

## Testing and Validation

Every invariant below names the CI target that enforces it, per this directory's rule that
claimed invariants trace to CI.

- **Byte identity, retained vs fresh** (the core invariant):
  `//donner/svg/renderer/tests:renderer_retained_spans_tests` renders each of its documents
  several times both ways, once through a renderer that retains nothing and once through a
  warmed retained cache, and asserts byte-equal snapshots frame by frame. Its documents cover
  the retained draw kinds (fills, both fill rules, hairline and thick and dashed strokes,
  zero-length-gap dashes, linear and radial gradients, gradient strokes) and the bypassed ones
  (patterns, masks, isolated layers, markers, `use` instancing, `paint-order` splits), plus
  three real documents from `donner/svg/renderer/testdata` including the tiger.
- **Corpus-wide identity** is a diagnostic, not a CI gate. Setting
  `DONNER_TINYSKIA_RETAINED=compare` makes the tiny-skia test backend render every document a
  suite touches both ways and fail on the first differing byte, so
  `renderer_tests`, `renderer_regression_tests`, `renderer_ascii_tests` and `resvg_test_suite`
  become retained-versus-fresh identity runs over roughly three thousand documents. No CI lane
  sets it today; it is invoked by hand with `--test_env`. Turning it into a lane is the
  follow-up under Next Steps.
- **Per-change-class invalidation**: `renderer_retained_spans_tests` mutates each class in
  isolation (geometry, transform, solid paint, gradient stop, stroke width, clip, surface
  resize), then asserts through the renderer's counters that the next frame rasterized rather
  than replayed, and that its bytes match a fresh render of the mutated document. This catches
  both under-invalidation (stale pixels) and a mutation that quietly does nothing.
- **Fail-closed surface binding**: the same target makes a retained key claim a surface its
  recording did not come from, with every other input identical by construction, and asserts
  the recording is refused, that the refusal falls back to rasterizing rather than dropping the
  shape, and that the frame matches a fresh render.
- **Memory bound**: the same target pins the eviction and whole-document-fallback paths, and
  that turning retention off hands back what the document held.
- **Steady-state allocations**: not implemented. A steady frame still rebuilds each shape's
  `tiny_skia::Paint` to compare it, which allocates for gradient paints, so there is no
  zero-allocation claim to enforce. Listed under Next Steps.
- **Fuzzing with retention enabled**: not wired. The SVG parser and render fuzzers run without
  retention, so the invalidation and eviction state machines do not currently see fuzzer
  mutation sequences. Listed under Next Steps.
- **Thread-count determinism**: not applicable until the concurrency stage exists.
- **Paint and stroke equality**: `//src/tiny_skia/tests:tiny_skia_core_tests` (dual native and
  scalar) covers `PaintTest.cpp`, which pins reflexivity for every shader kind, field
  sensitivity for solid, gradient and pattern paints and for strokes and dashes, and the float
  policy in both directions (a NaN is never equal, signed zeroes are). Equality is what decides
  whether a recording still applies, so an equality that answered wrongly would be the shortest
  path to a stale pixel.
- **tiny-skia-level capture/replay identity**: `SpanCaptureTest.cpp`, in the same dual targets,
  asserts capture+replay equals direct painting for antialiased and aliased fills, both fill
  rules, gradients, transforms, masks, rect fast paths, thick and hairline strokes, and dashes,
  and specifically for low-alpha antialiased edges under the pipeline's root-surface store
  rounding (the failure class 0028-2 documents). It also asserts that the capture pass leaves
  the cold frame byte-identical, that a replay with a different paint matches a direct draw in
  that paint, that the recorded ops cover every blit method the scan converters emit, and that
  the storage accessors a bounded cache depends on report and release what they claim.
  Injecting a one-unit coverage error into replay fails 14 of those cases and a one-pixel rect
  width error fails 3, with the diagnostic naming the first differing pixel.
- **Performance**: `engine_compare_bench --backend=tiny-skia` at `-c opt`, run with and without
  `--retained-spans`, recording `second_ms`, `first_ms`, `mutated_ms`, the `RETAINED` line, ALLOC
  `phase=second`, and RSS fields. Numbers land in this doc only as measured output, never
  estimated.

## Performance

### Measured

`engine_compare_bench --backend=tiny-skia`, `-c opt`, aarch64, one core, medians of three
interleaved passes of five timed iterations each, retention off versus on in the same binary.
`pixels_hash` was identical between the two modes in every run. Both modes run with the
per-entity path and image conversion caches, so these numbers are what retention adds on top of
those.

| Scene (size)                | cold base | cold retained | steady base | steady retained | steady speedup |
| --------------------------- | --------: | ------------: | ----------: | --------------: | -------------: |
| Ghostscript_Tiger (900x900) |  24.10 ms |      26.51 ms |    21.21 ms |         7.67 ms |          2.77x |
| lion (400x400)              |   3.56 ms |       3.87 ms |     2.58 ms |         1.53 ms |          1.69x |
| Edzample_Anim3              |   6.70 ms |       6.79 ms |     5.48 ms |         5.03 ms |          1.09x |
| z0rly_test6                 |   4.79 ms |       4.93 ms |     0.69 ms |         0.69 ms |          1.01x |

Capture costs 10.0 percent of the cold frame on the tiger, 8.7 percent on the lion, and 1 to 3
percent on the other two. That is a larger share than the capture pass costs in isolation,
because the conversion caches took outline conversion out of both sides: the same capture work
is now measured against a cheaper frame.

A frame that follows an edit, timed as a third phase (`--mutate`):

| Scene             | drag base | drag retained | pan base | pan retained |
| ----------------- | --------: | ------------: | -------: | -----------: |
| Ghostscript_Tiger |  22.05 ms |       9.32 ms | 22.38 ms |     23.12 ms |
| lion              |   3.29 ms |       1.92 ms |  2.91 ms |      3.09 ms |
| Edzample_Anim3    |   5.77 ms |       5.28 ms |  5.70 ms |      5.25 ms |
| z0rly_test6       |   4.30 ms |       4.45 ms |  4.31 ms |      4.46 ms |

`drag` moves one shape, so everything else still replays and the frame is close to a settled
one. `pan` moves the outermost group, so every device transform changes, every key misses, and
every shape is captured again: that is the worst case retention can be put in, and it costs 3
percent on the tiger and 6 percent on the lion against not retaining at all. The cost is capture
on top of a full rasterization, plus the comparison that decided nothing could be reused; the
re-rasterization itself still takes its outline from the conversion cache rather than converting
again.

Retained memory on a settled frame, from the benchmark's `RETAINED` line, which reports what the
budget charges (coverage, the paints beside it, and the entries themselves):

| Scene             | retained passes | retained bytes |
| ----------------- | --------------: | -------------: |
| Ghostscript_Tiger |             305 |        3.41 MiB |
| lion              |             132 |         612 KiB |
| Edzample_Anim3    |              49 |         166 KiB |
| z0rly_test6       |              58 |          46 KiB |

The tiger's 3.41 MiB at 900x900 matches this document's "order of a few MiB" estimate, and every
scene sits far under the 32 MiB default budget.

**The 10-20x target was not met, and the reason is structural rather than incidental.** The
tiger's steady frame went from 21.2 ms to 7.7 ms. What retention removes is coverage generation;
what remains is blending that coverage into the surface, which Milestone 1's own measurement
predicted (replaying a 512x512 antialiased fill cost 0.79x the cold fill, because a single large
path spends most of its time blending). The projection of 2-4 ms assumed opaque-interior stores,
which were deliberately not built: a separate store path for the common case is a second
implementation of the blitter's arithmetic, and the byte-identity argument would stop holding by
construction. Closing that gap means putting the fast path inside the shared blitter
construction, where both a cold draw and a replay get it.

### Targets

- **Steady-frame target**: the original 10-20x is not reachable while replay goes through the
  same blitter a cold draw builds, because what remains after coverage generation is removed is
  the blend of that coverage. A target that can be met without giving up the identity argument
  has to come with the fast path that makes it reachable; see Next Steps.
- **Cold-frame target**: parity or better. Capture adds run stores to the cold path. Measured at
  the tiny-skia level it costs 5.1 percent on a 512x512 antialiased path fill, and at the
  renderer level 10.0 percent on the tiger and 8.7 percent on the lion, so the cold-frame budget
  is a real constraint rather than a free assumption.
- **Measurement discipline**: all claims go through `engine_compare_bench`'s phases (`first_ms`,
  `second_ms`, `mutated_ms`, ALLOC per phase, RSS) so numbers are comparable across the design's
  lifetime; within-engine before/after deltas only, per the benchmark's own caveats.

## Risks and Open Questions

- **Memory growth bounds.** The 32 MiB default is a guess: measured documents sit far under it
  (3.41 MiB for the tiger), so nothing has pushed on eviction outside its tests. Open: should
  the budget scale with surface area? Is whole-entry eviction granular enough, or is dropping a
  stroke pass alone worth the complexity?
- **Interaction with the frame storage convention.** Replay must reproduce the exact
  root-surface store rounding of the active pipeline configuration; 0028-2 governs that
  convention and documents how observable the low-alpha rounding class is. The capture/replay
  identity tests target this class directly and must hold across any storage-convention change,
  and any future explicit gradient ramp LUT must be shared by cold and steady paths or identity
  breaks silently.
- **Clip-chain invalidation fan-out.** A clip change invalidates every shape under it; for a
  document-root clip that is everything. Conservative rebuild is correct but makes some
  single-property edits cost a full re-rasterization. Open: drop clip from the key entirely,
  which coverage already permits, once there is a reason to trust that no future change makes
  scan conversion consult the mask.
- **Eviction heuristics.** Least-recently-drawn is simple but a zooming viewport can cycle the
  working set. Open: protect shapes by draw frequency or by rebuild cost.
- **Gradient ramp representation.** The instantiated shader is retained and no quantized ramp
  LUT is introduced, because both paths already share one resolution step. Revisit only if ramp
  resolution shows up in a profile, and only as a stage inside that shared step.
- **Paint comparison is the whole invalidation story for paint.** An equality that answered
  wrongly, on any shader field, would be the shortest path to a stale pixel. `PaintTest.cpp`
  covers reflexivity, field sensitivity and the float policy for that reason, and any field
  added to a shader has to be added to its comparison in the same change.

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
