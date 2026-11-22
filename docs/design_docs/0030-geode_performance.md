# Design: Geode Rendering Performance

**Status:** Design. On an unchanged frame, resident solid, gradient and pattern fills and strokes
re-encode, create and re-upload nothing, and ordered batching draws runs of resident draws as
instanced draws. Steady-state residency for markers, images, filter passes, layer composites, clip
and mask geometry, and painted text is still open (Milestone 8), as are index-keyed filter results
(Milestone 5). `geode_counter_corpus_tests` records the corpus scenes measured to miss the steady
state.
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
  (8.3), and filter-pass steady state (8.6), together with the Milestone 5 named-result keys.
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
- [x] Milestone 3: Frame command buffers and submission.
  - [x] `RendererGeode::Impl` records a frame through one runtime command encoder at a time. When
    the active `GeoEncoder` finishes (`retireActiveEncoder`, at layer, filter, mask, and
    pattern-tile boundaries and at end of frame) and before a filter executes, it closes that
    encoder into a command buffer of the frame and opens a fresh one; empty encoders produce no
    command buffer. A filter execution appends command buffers of its own, closing one every
    `kMaxPassesPerCommandBuffer` (64) passes so a pathological graph cannot build one unbounded
    command buffer.
  - [x] `endFrame` submits all of the frame's command buffers together, in recording order, with
    two exceptions. A frame that already holds `kMaxFrameCommandBuffersPerSubmission` (16)
    command buffers splits before the next one (`splitFrameSubmission`): it submits what it holds
    and, on Vulkan, waits for the queue to idle before recording continues. A memory-limited filter
    budget that must free retained bytes before admitting another filter splits the same way
    (`submitFilterBudgetChunk`).
  - [x] For frames below the 16-buffer bound, `GaussianBlur_UsesSingleFrameSubmission` and
    `MultipleBoundedMorphologyNodesStillChunkCommandBuffers` assert at most two submissions, the
    frame's plus the snapshot readback's; the latter also asserts 3 to 16 command buffers.
    `AFramePastTheSubmissionBoundSplitsAndStillRenders`,
    `AFrameSplitTakesTheCrossSubmitCompletionWait`, and
    `FilterFrameRecoveredThroughAMemoryChunkReportsNoRefusal` (`RendererGeode_tests.cc`) cover the
    splits; `AFrameChunkClosesOnlyWhenPassSixtyFiveCrossesTheBoundary` and
    `ChunksPastTheFramesBoundSplitAcrossSubmissions` (`GeodeFilterEngine_tests.cc`) cover filter
    chunks.
  - [x] Snapshot readback is an explicit capture with its own submission and counters, outside
    the frame's submissions.
- [x] Milestone 4: Transient render-target pool.
  - [x] The render target is reused across frames while the viewport size is unchanged.
  - [x] `RendererGeodeTexturePool`, shared by the renderers of one device, reuses layer, mix-blend
    destination snapshot, filter, mask, clip-mask, and pattern-tile textures by exact
    `(width, height, format, usage, sampleCount)`. Release is deferred to the frame boundary so
    recorded GPU work completes before a texture is handed out again.
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
    (`SlugFillInstance` in `donner/gpu/shader/programs/SlugFill.h`, mirrored by the shader's
    `InstanceRecord` in `SlugFillSource.h`) with its transform, paint, fill rule, and any
    rectangular clip, which the fragment stage applies. Draws under a clip polygon, a clip mask,
    or an open mask pass do not join a batch. The pending batch flushes at every state change it
    cannot carry, including clip, layer, filter, and mask push and pop, pattern tiles, images,
    text that cannot join it, and end of frame.
    `kEnableSceneBatching` in `RendererGeode.cc` gates the feature and is on.
  - [x] Repeated `<use>` instances of one source draw as one instanced draw
    (`UseHeavy_BaselineCeilings` asserts `drawCalls == 1`). Lion's 132 paths draw as one batch
    (`Lion_BaselineCeilings`).
- [x] Milestone 7: Opportunistic cleanups.
  - [x] Band-selection and band-fill scratch storage stays on the stack, bounded by `kMaxBands`.
  - [x] Curve maxima stay inclusive when assigning band references, so a curve ending exactly on an
    internal band boundary is referenced by both adjacent bands
    (`CurveEndingOnBandBoundaryIsReferencedByFollowingBand`).
  - [x] The TinySkia showcase golden renders through `RendererTinySkia` directly
    (`ShowcaseAssetFixture.GeneratedBadgeAndOverlayMatchGolden`), so a Geode test configuration
    does not compare GPU output with a TinySkia image.
  - [x] Snapshot unpremultiplication runs as a GPU compute pass (`SnapshotUnpremultiply`) where
    the target format allows it (`GpuAndCpuPathsAreByteIdentical`).
- [ ] Milestone 8: Steady-state residency for the remaining draw classes. Each item clears the
  named `kKnownViolations` entries in `geode_counter_corpus_tests`; every entry listed here misses
  the steady state in the shipping configuration, with ordered batching on.
  - [ ] 8.1: Stroke cache keyed by resolved stroke style. `<use>` instances that share one source
    path with different stroke styles thrash the single stroke slot and re-encode every frame.
    Clears `poker_chips`, `stroking_complex`. Bound the extra slots with the retained-geometry
    budget.
  - [ ] 8.2: Marker instance residency. Every drawn marker re-uploads its geometry through the
    per-frame arena and builds its own bind group. Clears `marker`, `marker_segments`,
    `marker_spec_example`. A design that batches markers through instance records and needs new
    record fields changes the `SlugFillInstance` layout.
  - [ ] 8.3: Isolated-layer composites. The composite blit rewrites its uniform and creates a bind
    group every frame (`GeodeTextureEncoder::drawTexturedQuad`). Clears `Edzample_Anim3`. The
    corpus also records `donner_icon`, whose gradient-painted paths under an opacity group
    re-upload geometry and build bind groups per draw; why those draws miss gradient residency is
    not traced, so this item starts by tracing it.
  - [ ] 8.4: Image residency. Decoded images allocate a texture and re-upload their quad every
    frame, and the nested-document raster cache holds one entry per source, so the same external
    SVG drawn at two sizes re-uploads one of them. Clears `image_data_url_opacity`,
    `image_data_url_pixelated`, `image_external_svg_par`, `image_external_svg_viewbox`. Depends on
    8.3 for the opacity case.
  - [x] 8.5: Pattern residency. A pattern fill or pattern stroke draws from its entity's resident
    fill or stroke slot (`GeoEncoder::fillPathPatternResident`), whose cached bind group binds the
    tile's view, and each tile takes back from the pool the texture the same tile used the frame
    before, so an unchanged frame uploads nothing and creates no bind group
    (`PatternFills_UnchangedFrameWritesAndCreatesNothing`). Covers `geode_pattern_checker`,
    `geode_pattern_nonrect`, `geode_pattern_offset`, `geode_pattern_solid`. The tile itself is
    rendered again every frame, which costs one render pass and its content draws but no gated
    counter; skipping that pass is Future Work. A pattern painted by several elements renders its
    tile once per element, and its content entities are then drawn more than once in the frame, so
    the draws after the first take the arena path, the repeat-draw limit 8.2 also meets.
  - [ ] 8.6: Filter-pass steady state. Filter passes rewrite their per-pass uniforms and create a
    bind group per pass every frame. Keep uniform allocations across unchanged frames and reuse
    pass bind groups. How to key the bind-group cache is open: the texture pool hands back the most
    recently released texture of a key, but whether a pass sees the same textures on an unchanged
    frame is not established. Measuring texture identity per pass across two unchanged frames
    decides between keying on texture identity and keying on pass role. Clears
    `filter_displacement_map`, `filter_drop_shadow`, `filter_fill_paint`, `filter_spot_light`,
    `filter_stroke_paint`.
  - [ ] 8.7: Clip, mask, and nested-viewport geometry. Residency and batching skip draws under a
    clip polygon, a clip mask, or an open mask pass, so clip-path and mask geometry re-encodes and
    re-uploads every frame, and nested viewports re-encode their subtree. Clears `donner_splash`,
    `geode_splash`, `nested_svg_aspectratio`. Depends on 8.3, on 8.6 for `donner_splash`'s
    filters, and on 8.1 for `nested_svg_aspectratio`'s `<use>` instances. That scene also carries
    text inside nested viewports; whether its text needs 8.8 is not traced.
  - [ ] 8.8: Painted-text geometry. Glyph outlines are cached per glyph (`GeodeGlyphCache`) and
    solid-filled glyphs draw from resident outlines, but pattern- and gradient-painted glyphs and
    text decorations are placed and encoded as transient geometry every frame. Clears
    `geode_text_decoration_underline`, `geode_text_pattern_fill`,
    `geode_text_span_gradient_over_pattern`, `geode_text_span_gradient_over_pattern_stroke`. The
    three pattern-painted scenes depend on 8.5. The corpus reason strings for these entries say
    placed text has no path cache or residency; correcting them is a follow-up to the corpus test.

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
  +-- GeodeResidentPathComponent   (GPU residence: solid/pattern and gradient slots per variant)
         +-- slot range in GeodeResidentSlab  (document-scoped chunks, first-fit free list)
         +-- cached bind group
         +-- record slot in GeodeRecordSlab   (instance record for ordered batching)
```

- **Resident slots.** A solid or gradient fill or stroke uploads its geometry once into a range of
  the document's `GeodeResidentSlab` and caches its bind group. Later frames rewrite only the slot's
  uniform, and only when it changed; an unchanged frame writes nothing. Freed ranges return to the
  slab at the next frame boundary, so a re-upload never overwrites a range a recorded draw still
  reads. Up to 16 stops travel inline in a gradient's parameters; a gradient with more reads its
  stops from a stop-ramp texture (`GeodeGradientRamp`), which resident slots keep across frames, so
  an unchanged frame uploads no ramp (`ManyStopGradient_OneRampThenZeroWrites`).
- **Pattern fills.** A pattern fill or stroke draws from the entity's solid fill or stroke slot,
  with a bind group that binds the pattern tile's view instead of the placeholder texture and is
  rebuilt when the tile's view changes. The renderer renders every tile again each frame, into a
  pool texture: the Nth tile of a size in a frame asks the pool for the texture the Nth tile of
  that size used the frame before, and the renderer keeps a view of each tile texture for one
  frame after its last use (`PatternTileViewsStayBoundedBetweenFrames`). Tile textures go back to
  the pool at every frame end, so the pool's byte budget bounds them, and an edit anywhere a
  tile's pixels come from reaches the next frame
  (`PatternEditsReachTheNextFrameOfAWarmRenderer`).
- **Residency gates.** Residence is taken only when no clip polygon, clip mask, or mask pass is
  active, and once per slot per frame. Draws that fail a gate use the per-frame arena path.
- **Ordered batching.** An eligible resident draw publishes its instance record into the
  `GeodeRecordSlab` and joins the pending batch; the batch flushes as one instanced draw in painter
  order.
- **Glyphs.** `GeodeGlyphCache` (`geode/GeodeGlyphResidency.h`) caches each glyph outline once per
  glyph identity (font, glyph, scale, stretch, rotation, and tolerance), bounded by entry and byte
  budgets. Solid-filled glyph occurrences draw from those resident outlines through instance records
  and join ordered batches.
- **Invalidation.** Path geometry invalidates through `on_update<ComputedPathComponent>`, which
  fires only when `ShapeSystem` changes the geometry; no separate dirty flag is involved.
- **Uniform storage.** Resident slots hold their own uniforms. Per-frame draws bump-allocate from
  per-encoder arenas, image blits from the encoder's uniform scratch, and filter passes from
  per-frame scratch buffers. Nothing uses a uniform ring or dynamic offsets.
- **Eviction.** Geometry changes remove the entity's cache and residence components; document
  teardown destroys them and returns their slab ranges. Residence is tracked per device, so a
  document drawn by another device uploads there instead of borrowing this device's buffers
  (`GpuResidence_ReUploadsWhenDeviceChanges`, `GeodeDocumentResidency_tests.cc`).

### Frames, encoders, and pools

A frame records through one runtime command encoder at a time and closes it into a command buffer
at each boundary listed in Milestone 3; `endFrame` submits the frame's command buffers together
unless the 16-buffer bound or a filter-memory split submitted part of them earlier. Layer, filter,
mask, clip-mask, and pattern textures come from `RendererGeodeTexturePool`; arena buffers come from
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

`kKnownViolations` lists the corpus scenes measured to miss one or more of these. An entry can
carry a separate mask and ceilings for the build with ordered batching, and two entries
(`svg2_e_use_003`, `svg2_e_use_004`) violate only with batching off; ordered batching is always
compiled in, so they already meet the steady state. Milestone 8 covers the other 23.

## Testing and Validation

- `//donner/svg/renderer/geode:geode_perf_tests`: per-fixture first-frame ceilings and
  unchanged-frame invariants, including snapshot readback cost through
  `consumeReadbackStats()`.
- `//donner/svg/renderer/geode:geode_counter_corpus_tests`: the corpus-wide steady-state gate.
  `kKnownViolations` records each corpus scene measured to miss an invariant, the counters it
  breaks, their measured ceilings, and the reason, with an optional separate mask and ceilings for
  the build with ordered batching. A listed scene must still break the counters its entry
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
  64-pixel buckets would trade VRAM for reuse. A counter test that renders a sequence of new
  viewport sizes and asserts `textureCreates` decides whether bucketing pays. The existing
  `TransientTexturePoolStaysWithinGlobalMemoryBudgetAcrossSizeChurn` bounds retained memory under
  size churn and checks reuse of a size already seen, but does not measure creates across new
  sizes.

# Future Work

- [ ] GPU timestamp profiling on all platforms, as a separate track. The GPU runtime exposes no
  timestamp queries today, `RendererGeode::enableTimestamps` has no effect, and the
  `FrameTimings::renderPassNs` and `totalGpuNs` fields stay zero. Per-pass GPU time in
  `lastFrameTimings` would let the compositor attribute frame cost to specific draws.
- [ ] Skip the render pass of a pattern tile whose pixels cannot have changed. The renderer cannot
  tell that from the draws it receives, so the key has to come from the driver, and it has to
  cover everything the tile's content reads: elements outside the pattern subtree (`<use>`
  targets, referenced gradients and nested patterns), the pattern element's own attributes and
  its content transform, context paint, the patterns already open around the tile (a pattern that
  reaches itself draws nothing at the inner level), document time, loaded fonts and images, and
  the tile's raster size and device. Today every tile renders each frame, which is always current
  and costs one render pass plus the tile's resident content draws.
