# Design: Geode Rendering Performance

**Status:** Design. An unchanged Geode frame re-encodes no path, creates no texture and re-uploads
no geometry for solid and gradient fills and strokes, and ordered batching draws runs of resident
draws as instanced draws. Steady-state residency for markers, patterns, images, filter passes, layer
composites, clip and mask passes, and some text paints is still open (Milestone 8), as are
index-keyed filter results (Milestone 5). `geode_counter_corpus_tests` lists every scene that
still misses the steady state.
**Author:** Claude Opus 4.7
**Created:** 2026-04-19

## Summary

This design addresses Geode's per-frame GPU resource, CPU encoding, and submission costs. It adds
caching, residency, batching, and counter instrumentation, ordered by their impact on the
composited editor's `DragFrameOverhead` budgets in Design 0025.

Scope: Geode only (`donner/svg/renderer/geode/**`, `RendererGeode.{cc,h}`). Boundary: no change to
the coverage algorithm. Geode keeps the single-sample analytic dual-ray coverage fill from 0041;
this is a wiring, caching, and batching design, not a new rendering algorithm.

## Goals

- Measure every optimization with deterministic counter assertions (`GeodeCounters`) so each
  change lands with a before/after and regressions fail CI.
- Create no GPU resources per draw on an unchanged frame: geometry lives in persistent,
  suballocated GPU storage and bind groups are cached rather than rebuilt per draw.
- Cache the CPU path encode on the ECS path entity, invalidated when the geometry actually changes.
  Frames where no paths change do zero encode work.
- Record a frame's render work into one command encoder and one submission. Layer, filter, and
  mask push/pop does not force a submit.
- Reuse transient render-target textures (layer, filter, mask, pattern, and clip scratch) across
  frames so a redraw at the same size creates no texture.
- Draw runs of compatible draws as one instanced draw without changing SVG painter order.

## Non-Goals

- Changing the coverage algorithm or the shader programs' interfaces beyond what a batching or
  residency change needs.
- Changing native GPU backend selection or the Linux-only test reference. The
  [GPU runtime reference](../gpu_runtime.md) describes backend selection and that no backend
  request or default selects the reference; [Design 0053](0053-native_gpu_hal.md) states how the
  no-Rust verifier and the dependency audits keep the reference out of every product.
- Changing the anti-aliasing scheme. Geode renders analytic dual-ray coverage at one sample per
  pixel on every adapter (0041).
- Expanding the resvg test-suite coverage or changing pixelmatch thresholds;
  [Design 0021](0021-resvg_feature_gaps.md) owns those policies.
- Adding runtime backend switching or re-plumbing `RendererInterface`.
- GPU timestamp profiling. It is a separate track (see Future Work); counters are this design's
  regression signal.
- Perf work on `RendererTinySkia` or the composited-rendering layer (0025 tracks those
  separately).

## Next Steps

- Start the independent Milestone 8 items: keyed stroke cache (8.1), layer-composite uniforms
  (8.3), pattern residency (8.5), and filter-pass steady state (8.6), together with the Milestone 5
  named-result keys.
- Each item deletes the `kKnownViolations` entries it clears from
  `donner/svg/renderer/geode/tests/GeodeCounterCorpus_tests.cc` and adds a focused
  `GeodePerf_tests.cc` case that fails before the change.

## Implementation Plan

- [x] Milestone 0: Perf harness and counters.
  - [x] `//donner/svg/renderer/benchmarks:renderer_bench` reports per-phase wall-clock medians
    (parse, first draw and snapshot, unchanged-frame redraw, and a per-frame mutation) over inline
    SVGs and testdata fixtures.
  - [x] `GeodeCounters` (`geode/GeodeCounters.h`) counts buffer, bind-group, and texture creates;
    submits and command buffers; path encodes; draw calls and pipeline switches; buffer upload
    calls and bytes and texture upload bytes; gradient-stop work; stroke-outline flattening; and
    glyph residency hits, uploads, and evictions. The GPU runtime device counts resource creation,
    so filter-engine and texture-encoder work is included. `RendererGeode::lastFrameTimings()`
    returns the last frame's counters; `RendererGeode::consumeReadbackStats()` reports snapshot
    readback work separately.
  - [x] `//donner/svg/renderer/geode:geode_perf_tests` (`geode/tests/GeodePerf_tests.cc`) asserts
    first-frame ceilings and unchanged-frame invariants on inline fixtures, `lion.svg`, and
    `Ghostscript_Tiger.svg`, including filter, gradient, image-blit, and `<use>` fixtures.
  - [x] `//donner/svg/renderer/geode:geode_counter_corpus_tests`
    (`geode/tests/GeodeCounterCorpus_tests.cc`) renders every SVG in the render corpus twice and
    asserts the steady-state invariants on the second frame (see Testing and Validation).
- [x] Milestone 1: Per-draw resource pooling.
  - [x] Per-encoder bump arenas for vertex, storage, and uniform data. `GeodeBufferPool` recycles
    arena buffers across frames, keyed by size and usage.
  - [x] The solid pipeline is bound through the encoder's pipeline state tracker; contiguous
    same-pipeline draws issue one `setPipeline`.
  - [x] The bind-slot placeholders (1x1 pattern and clip-mask textures, their samplers, a
    one-element identity instance record, and a zero-filled gradient paint block) are shared per
    `GeodeDevice` instead of created per encoder or per draw.
  - [x] Bind groups are cached on each resident slot and created once per ordered batch, so a
    resident or batched draw creates no bind group on an unchanged frame. Draws that still go
    through the per-frame arena create one bind group per draw; Milestone 8 lists them.
- [x] Milestone 2: `GeodePathCacheComponent`, the CPU encode cache.
  - [x] `GeodePathCacheComponent` (`geode/GeodePathCacheComponent.h`) holds the encoded fill for up
    to `kGeodeFillVariantCount` device-derived tolerance buckets and one stroke slot holding the
    `strokeToFill` outline and its encode, keyed by the stroke style and the device-derived
    stroke tolerance.
  - [x] The driver passes the source entity to the backend in `PathShape::sourceEntity`
    (`RendererInterface.h`); backends that keep no per-entity state ignore it.
  - [x] `ShapeSystem`'s `emplaceComputedPathIfChanged` keeps the existing `ComputedPathComponent`
    when the rebuilt `Path` compares equal (`Path::operator==`), so
    `on_update<ComputedPathComponent>` fires only on a real geometry change.
  - [x] `RendererGeode::Impl::onComputedPathChanged`, connected to `on_update` and `on_destroy` of
    `ComputedPathComponent`, removes both `GeodePathCacheComponent` and
    `GeodeResidentPathComponent`.
  - [x] `{SimpleShapes,Moderate,Lion,GhostscriptTiger}_NoDirtyPath_ZeroEncodes` assert
    `pathEncodes == 0` on an unchanged second frame.
- [x] Milestone 3: Single encoder per frame.
  - [x] `RendererGeode::Impl` owns one frame command encoder from `beginFrame` to `endFrame`;
    every `GeoEncoder` for the base target and for layer, filter, mask, and pattern passes records
    into it, and blend-mode snapshot copies record into it as well.
  - [x] Filter render, compute, copy, and composite passes record into the frame. The filter engine
    closes a command buffer every `kMaxPassesPerCommandBuffer` (64) passes so a pathological graph
    cannot build one unbounded command buffer; inside a frame those command buffers join the frame's
    single submission (`MultipleBoundedMorphologyNodesStillChunkCommandBuffers`,
    `AFrameChunkClosesOnlyWhenPassSixtyFiveCrossesTheBoundary`);
    `GaussianBlur_UsesSingleFrameSubmission` pins the ordinary case.
  - [x] Snapshot readback is an explicit capture with its own submission and counters, outside
    the frame's submission.
- [x] Milestone 4: Transient render-target pool.
  - [x] The render target is reused across frames while the viewport size is unchanged.
  - [x] `RendererGeodeTexturePool`, shared by the renderers of one device, reuses layer, mix-blend
    destination snapshot, filter, mask, clip-mask, and pattern-tile textures by exact
    `(width, height, format, usage)`. Release is deferred to the frame boundary so recorded GPU
    work completes before a texture is handed out again.
  - [x] `{SimpleShapes,Moderate,Lion,GhostscriptTiger,GaussianBlur}_NoDirtyPath_ZeroTextures`
    assert `textureCreates == 0` on an unchanged second frame.
- [ ] Milestone 5: Filter engine caching.
  - [ ] Key filter results by node index instead of string. A filter execution in
    `GeodeFilterEngine.cc` keeps named results in `std::unordered_map<std::string, FilterTexture>`
    and copies the result name with `.str()` on every lookup and store. No counter covers this; it
    needs an allocation or encode-time probe as its repro.
  - [x] Filter intermediate textures come from the Milestone 4 pool and return to it after frame
    submission; an unchanged repeat blur creates no texture.
  - [x] Gaussian blur and morphology alternate two scratch textures instead of allocating one per
    pass.
  - [x] Filter-pass uniforms come from per-frame scratch buffers rather than a buffer per pass
    (`MigratedFilterPassesShareOneUniformScratch`).
- [x] Milestone 6: Batch draws sharing pipeline state.
  - [x] `drawCalls`, `pipelineSwitches`, and `sameSourceDrawPairs` counters.
  - [x] Ordered batching: consecutive eligible draws, including different entities and different
    paints, join one instanced draw in painter order. Each instance carries a 256-byte record
    (`InstanceRecord`, matching `SlugFillInstance` in
    `donner/gpu/shader/programs/SlugFillSource.h`) with its transform, paint, fill rule, and any
    rectangular clip, which the fragment stage applies. Draws under a clip polygon, a clip mask,
    or an open mask pass do not join a batch, and the pending batch flushes at clip, layer,
    filter, mask, and pattern-tile push and pop, at images, and at end of frame.
    `kEnableSceneBatching` in `RendererGeode.cc` gates the feature and is on.
  - [x] Repeated `<use>` instances of one source draw as one instanced draw
    (`UseHeavy_BaselineCeilings` asserts `drawCalls == 1`). Lion's 132 paths draw as one batch
    (`Lion_BaselineCeilings`).
- [x] Milestone 7: Opportunistic cleanups.
  - [x] Band-selection and band-fill scratch storage stays on the stack, bounded by `kMaxBands`.
  - [x] Curve maxima stay inclusive when assigning band references, so a curve ending exactly on an
    internal band boundary is referenced by both adjacent bands.
  - [x] TinySkia-only golden tests are pinned to `RendererTinySkia`.
  - [x] Snapshot unpremultiplication runs as a GPU compute pass (`SnapshotUnpremultiply`) where
    the target format allows it (`GpuAndCpuPathsAreByteIdentical`).
- [ ] Milestone 8: Steady-state residency for the remaining draw classes. Each item clears the
  named `kKnownViolations` entries in `geode_counter_corpus_tests`.
  - [ ] 8.1: Stroke cache keyed by resolved stroke style. `<use>` instances that share one source
    path with different stroke styles thrash the single stroke slot and re-encode every frame, and
    styled `<use>` stroke outlines draw through the per-frame arena. Clears `poker_chips`,
    `stroking_complex`, `svg2_e_use_003`, `svg2_e_use_004`. Bound the extra slots with the
    retained-geometry budget.
  - [ ] 8.2: Marker instance residency. Every drawn marker re-uploads its geometry through the
    per-frame arena and builds its own bind group. Clears `marker`, `marker_segments`,
    `marker_spec_example`. A design that batches markers through instance records and needs new
    record fields changes the `SlugFillInstance` layout.
  - [ ] 8.3: Isolated-layer composites. The composite blit rewrites its uniform and creates a bind
    group every frame (`GeodeTextureEncoder::drawTexturedQuad`), and gradient-painted paths under
    an opacity group re-upload their geometry and build bind groups per draw. Clears
    `Edzample_Anim3`, `donner_icon`.
  - [ ] 8.4: Image residency. Decoded images allocate a texture and re-upload their quad every
    frame, and the nested-document raster cache holds one entry per source, so the same external
    SVG drawn at two sizes re-uploads one of them. Clears `image_data_url_opacity`,
    `image_data_url_pixelated`, `image_external_svg_par`, `image_external_svg_viewbox`. Depends on
    8.3 for the opacity case.
  - [ ] 8.5: Pattern residency. Cache each pattern tile per paint server across frames and give the
    tiled fill a resident slot. Clears `geode_pattern_checker`, `geode_pattern_nonrect`,
    `geode_pattern_offset`, `geode_pattern_solid`.
  - [ ] 8.6: Filter-pass steady state. Filter passes rewrite their per-pass uniforms and create a
    bind group per pass every frame. Keep uniform allocations across unchanged frames and cache
    bind groups by pass role rather than texture identity, because pooled textures change identity
    between frames. Clears `filter_displacement_map`, `filter_drop_shadow`, `filter_fill_paint`,
    `filter_spot_light`, `filter_stroke_paint`.
  - [ ] 8.7: Clip, mask, and nested-viewport geometry. Residency and batching skip draws under a
    clip polygon, a clip mask, or an open mask pass, so clip-path and mask geometry re-encodes and
    re-uploads every frame, and nested viewports re-encode their subtree. Clears `donner_splash`,
    `geode_splash`, `nested_svg_aspectratio`. Depends on 8.3, on 8.6 for `donner_splash`'s
    filters, and on 8.1 for nested `<use>` instances.
  - [ ] 8.8: Placed-text paint residency. Pattern- and gradient-painted glyphs and text
    decorations re-encode and re-upload every frame. Clears `geode_text_decoration_underline`,
    `geode_text_pattern_fill`, `geode_text_span_gradient_over_pattern`,
    `geode_text_span_gradient_over_pattern_stroke`. Depends on 8.5.

## Background

Geode is feature-complete enough to be the default for GPU-capable consumers. This design tracks
its cache, residency, and batching work; the
[original Geode rationale](0017-geode_renderer.md#original-rationale) remains in Git history.

0025's "Perf-gate waivers (v1)" table attributes the composited-rendering cost gap to three
causes (dirty walk, split-bitmap reblit, style cascade). None of them is Geode-internal, but
Geode's steady-state cost is the denominator any composited-rendering win divides into, so
removing per-draw resource churn in Geode directly benefits the `DragFrameOverhead_*` budgets.

## Requirements and Constraints

- **No exceptions.** Allocation and admission failures return status values and take the bounded
  failure paths.
- **Repro first.** Every change lands with a counter assertion in `GeodePerf_tests.cc` or a
  corpus entry deletion that fails before the change.
- **No regressions on goldens.** `renderer_geode_golden_tests`, `renderer_tests` under
  `--config=geode`, and the Geode resvg suites stay green. Strict identity thresholds stay strict;
  the arena, resident, and batched paths share their uniform and record builders so a draw renders
  the same on any of them (`RecordSourcedAndUniformSourcedDrawsAgree`).
- **ECS first.** Caches live on entities and invalidate through `ComputedPathComponent` signals,
  not through a side map keyed on `Path` pointers.
- **Documented invariants.** Each cache component documents what invalidates it.
- **Bounded pools.** The texture pool's retained bytes follow the largest working set of recent
  frames between a 64 MiB floor and a 256 MiB native or 96 MiB WebAssembly ceiling, with at most
  `kMaxPoolEntriesPerKey` (16) free textures per key
  ([texture memory between frames](../guides/embedding_geode.md#EmbeddingGeodeTexturePool)).
  `GeodeFrameGeometryBudget` (`geode/GeodeResourceBudget.h`) bounds the encoded geometry a frame
  admits; once a frame exceeds it, the frame refuses further geometry.

## Proposed Architecture

### Encoded path data

`GeodePathEncoder::encode` produces an `EncodedPath` (`geode/GeodePathEncoder.h`): canonical
monotonic quadratic curves stored once per ray axis, compact per-band references into them for
the horizontal and vertical rays, dense band grids, and a conservative bounding polygon of at most
eight vertices. The vertex stage generates the polygon's triangle fan from `vertex_index`, so no
vertex buffer is bound.

### Residency and batching

```
Entity with <path d="...">
  +-- ComputedPathComponent        (ShapeSystem; on_update fires only on real changes)
  +-- GeodePathCacheComponent      (CPU encodes: fill variants per tolerance, stroke slot)
  +-- GeodeResidentPathComponent   (GPU residence: solid and gradient slots per variant)
         +-- slot range in GeodeResidentSlab  (document-scoped chunks, first-fit free list)
         +-- cached bind group
         +-- record slot in GeodeRecordSlab   (instance record for ordered batching)
```

- **Resident slots.** A solid or gradient fill or stroke uploads its geometry once into a range of
  the document's `GeodeResidentSlab` and caches its bind group. Later frames rewrite only the slot's
  uniform, and only when it changed; an unchanged frame writes nothing. Freed ranges return to the
  slab at the next frame boundary, so a re-upload never overwrites a range a recorded draw still
  reads. Gradients with more than 16 stops also keep their stop ramp resident.
- **Residency gates.** Residence is taken only when no clip polygon, clip mask, or mask pass is
  active, and once per slot per frame. Draws that fail a gate use the per-frame arena path.
- **Ordered batching.** An eligible resident draw publishes its instance record into the
  `GeodeRecordSlab` and joins the pending batch; the batch flushes as one instanced draw in painter
  order. Text uses the same mechanism for resident glyph outlines shared across occurrences
  (`GeodeGlyphCache`, `geode/GeodeGlyphResidency.h`).
- **Eviction.** Geometry changes remove the entity's cache and residence components; document
  teardown destroys them and returns their slab ranges. Residence is tracked per device, so a
  document drawn by another device uploads there instead of borrowing this device's buffers
  (`GpuResidence_ReUploadsWhenDeviceChanges`, `GeodeDocumentResidency_tests.cc`).

### Frames, encoders, and pools

`RendererGeode::beginFrame` creates one command encoder; draws, layer blits, filter passes, mask
passes, and target writes record into it, and `endFrame` submits it. Layer, filter, mask,
clip-mask, and pattern textures come from `RendererGeodeTexturePool`; arena buffers come from
`GeodeBufferPool`.

### Counter-based tests over wall-clock tests

Wall-clock budgets are flaky on shared CI runners (0025's "Perf-gate waivers (v1)" table). Counter
assertions (`bufferWrites == 0`, `pathEncodes == 0`, `textureCreates == 0`) are deterministic, so
each invariant is a hard ceiling rather than a guard band.

## Performance

- `renderer_bench` reports wall-clock draw and snapshot times for human before/after comparison.
- `geode_perf_tests` and `geode_counter_corpus_tests` are the CI gates.
- `CompositorPerfTest` (`donner/svg/compositor/CompositorPerf_tests.cc`) watches
  `DragFrameOverhead_*`. This work does not change those budgets; the compositor's dirty walk is
  the dominant term there (0025 Perf-gate waivers).

Steady-state invariants on an unchanged second frame, asserted per corpus scene:

- `pathEncodes == 0`: the encode cache serves every path.
- `textureCreates == 0`: targets and scratch textures come from the pool.
- `bufferWrites == 0`: resident geometry and unchanged uniforms are not re-uploaded.
- `bindgroupCreates <= 6`: bind groups are cached per slot and per batch, not built per draw.

The scenes listed in `kKnownViolations` miss one or more of these; Milestone 8 clears them.

## Testing and Validation

- `//donner/svg/renderer/geode:geode_perf_tests`: per-fixture first-frame ceilings and
  unchanged-frame invariants, including snapshot readback cost through
  `consumeReadbackStats()`.
- `//donner/svg/renderer/geode:geode_counter_corpus_tests`: the corpus-wide steady-state gate.
  `kKnownViolations` records each scene that misses an invariant, the counters it breaks, their
  measured ceilings, and the reason. A listed scene must still break the counters its entry
  names, so fixing a scene fails the suite until its entry is deleted, and a listed scene cannot
  get worse. Every scene must also render visible pixels unless listed in `kDeliberatelyInert`.
- `//donner/svg/renderer/geode:geode_device_tests`, `:geode_path_encoder_tests`,
  `:geode_shaders_tests`, `:geo_encoder_tests`: existing unit tests stay green.
- `//donner/svg/renderer/tests:renderer_geode_golden_tests`: strict-identity goldens stay
  bit-identical; any drift is a correctness regression, not a threshold candidate.
- `//donner/svg/renderer/tests:renderer_tests` under `--config=geode`: stays green at current
  tolerances.
- The Geode resvg suites keep their current results.
- `//donner/svg/compositor:compositor_perf_tests`: stays green with thresholds unchanged.
- No new fuzzing surface. Existing XML, CSS, and SVG parsers and their fuzzers are unaffected.

### Per-milestone validation gate

1. All golden and correctness tests are green on `--config=geode` and the default configuration.
2. The milestone's `geode_perf_tests` assertions pass and its corpus entries are deleted.
3. The `renderer_bench` wall-clock median does not regress on any fixture (reported in the PR).

## Dependencies

- No new external dependencies. Uses Donner's GPU runtime buffer, bind-group, and command APIs;
  the [GPU runtime reference](../gpu_runtime.md) describes backend selection, and
  [Design 0053](0053-native_gpu_hal.md) states how the test-only reference is kept out of every
  product.
- Encode-cache invalidation depends on `ShapeSystem` leaving an unchanged `ComputedPathComponent`
  in place (`emplaceComputedPathIfChanged`).

## Rollout Plan

- All work lands behind the existing `--config=geode` flag. No runtime switching.
- Each milestone item is a separately reviewable PR with before/after counters and
  `renderer_bench` numbers in the PR description.
- Counter ceilings and corpus invariants stay green after their milestone lands.

## Alternatives Considered

**Dynamic-offset bind groups over one uniform ring.** Not used. Per-slot cached bind groups over
suballocated resident storage reach zero bind-group creation on unchanged frames without
dynamic-offset plumbing, and ordered batching amortizes the remaining binding cost across a batch.

**A suballocator at the GPU runtime layer instead of renderer-owned arenas and slabs.** Rejected:
it adds a layer of indirection, complicates the
[native embedding contract](../guides/embedding_geode.md), and offers no measured win over
renderer-owned pools in a single-threaded renderer.

**Cache encoded paths keyed on `Path` pointer identity, not ECS entity.** Rejected: `Path` is
copied and moved through the scene-graph build; entity identity is the correct key for the
renderer's per-document ownership model.

**Reorder draws across paint order to lengthen batches.** Rejected: SVG painter order affects
output. Ordered batching groups draws only in their existing order.

**Move all of `GeodeFilterEngine` to compute-pipeline fusion in one big kernel.** Out of scope.
That is a larger design (cf. `0014-filter_performance.md` for the CPU-side equivalent) and is not
required for the per-frame goals here.

## Open Questions

- `RendererGeodeTexturePool` keys textures by exact size. A viewport that changes size every
  frame (resize, zoom) misses the pool for every size it has not seen, while power-of-two or
  64-pixel buckets would trade VRAM for reuse. A counter test that renders a sequence of viewport
  sizes and asserts `textureCreates` decides whether bucketing pays; no such test exists yet.

Resolved by the as-built design:

- Path-geometry invalidation uses `on_update<ComputedPathComponent>`, which fires only when
  `ShapeSystem` changes the geometry; no new dirty flag is needed.
- Uniform storage: resident slots hold their own uniforms and per-frame draws bump-allocate from
  per-encoder arenas and a uniform scratch buffer. There is no uniform ring and no dynamic offset,
  so the ring-sizing question does not arise.

# Future Work

- [ ] GPU timestamp profiling on all platforms, as a separate track. The GPU runtime exposes no
  timestamp queries today, `RendererGeode::enableTimestamps` has no effect, and the
  `FrameTimings::renderPassNs` and `totalGpuNs` fields stay zero. Per-pass GPU time in
  `lastFrameTimings` would let the compositor attribute frame cost to specific draws.
- [x] Glyph-level geometry cache: resident glyph outlines are shared across occurrences and
  bounded by entry and byte budgets (`GeodeGlyphCache`).
