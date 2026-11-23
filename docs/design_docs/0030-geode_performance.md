# Design: Geode Rendering Performance

**Status:** Design. On an unchanged frame, resident solid, gradient and pattern fills and strokes,
including painted text, re-encode, create and re-upload nothing, including the second and later
draws of one entity in a frame such as marker placements and draws under a path clip mask, clip
shapes fill their masks from cached, resident geometry, filter passes upload and bind nothing new,
opacity, blend-mode, filter-result and `<mask>` composites and decoded images, also under a path
clip mask, write and create nothing, and ordered batching draws runs of resident draws as
instanced draws. Steady-state residency for draws under a clip polygon (among them turned marker
placements) is still open (Milestone 8).
`geode_counter_corpus_tests` records the corpus scenes measured to miss the steady state.
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

- Start the remaining Milestone 8 item: residency under a clip polygon (8.9).
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
    to `kGeodeFillVariantCount` device-derived tolerance buckets and up to
    `kGeodeStrokeVariantCount` (8) stroke variants, each holding a `strokeToFill` outline and its
    encode. A stroke variant is keyed by the stroke style (width, cap, join, miter limit, dash
    array, dash offset, and `pathLength`), the device-derived flattening tolerance, which must
    match exactly, and the host transform of a `non-scaling-stroke` centerline. One stroke style
    keeps at most `kStrokeVariantsPerStyle` (2) outlines across tolerance buckets and host
    transforms, so a zoom through further scale buckets replaces the style's own least recently
    used outline (`ZoomingKeepsTwoStrokeOutlinesPerStyle`). Otherwise a new key takes an empty
    variant or replaces the least recently used one. A variant is replaced only when no open frame
    on the device may still read it: neither the current frame nor an outer frame that an
    offscreen pass runs inside, whose pending batch can still point at it; when every candidate
    may still be read, the draw uses a per-frame outline or encode. Fill variants are replaced
    under the same rule (`NestedPassKeepsTheStrokeOutlineAnOuterFrameLeftPending`,
    `NestedPassKeepsTheFillEncodeAnOuterFrameLeftPending`). Each variant's bytes are charged to the
    document's cache-byte budget, a replacement taking over the evicted variant's charge, and each
    has its own resident slots, released with the variant, the entity, or the device
    (`ReplacedStrokeOutlinesHandOverTheirChargeAndResidence`,
    `StrokeOutlinesGoWithTheirPathDeviceAndDocument`).
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
    recorded GPU work completes before a texture is handed out again. A bucket hands its textures
    out in the order the most recent frame took them, whatever order they came back in, so a frame
    that repeats the previous frame's acquisitions gets back the same texture for each one. Within
    a bucket, queuing a returned texture, handing one out, and taking an eviction victim each cost
    O(log n) (`TextureHandOutQueue`, `LargeBucketMovesEachItemAConstantNumberOfTimes`); choosing the
    bucket to evict from scans every bucket (#2619).
  - [x] `{SimpleShapes,Moderate,Lion,GhostscriptTiger,GaussianBlur}_NoDirtyPath_ZeroTextures`
    assert `textureCreates == 0` on an unchanged second frame.
- [x] Milestone 5: Filter engine caching.
  - [x] Filter results are keyed by node index. Once admission has bounded a graph to
    `kMaximumFilterGraphNodes` (32) primitives, a filter execution resolves each named input to
    the latest earlier primitive with that result name, or to the previous result when no earlier
    primitive has it, and records the last primitive that reads each result; this costs at most 31
    name comparisons per input, once per execution, not per tile. Results are then stored and
    looked up by the producing primitive's index and released after their last reader. The
    subregion history that input-derived default subregions read (`FilterRegionContext`, shared
    with the CPU filter path) lists the named primitives in recording order and is searched from
    the latest. Neither copies a name's characters nor uses a map, so naming results adds no heap
    allocation to an execution (`NamedResultsAllocateNoMoreThanImplicitChaining` in
    `//donner/svg/renderer/benchmarks:geode_filter_allocation_tests`, an allocation probe over a
    recording device in its own binary, which the sanitizer lanes exclude because it replaces the
    global allocator). A repeated name, read both as a result and as the default subregion it sets,
    an input named like its primitive's own result, a name defined only by a later primitive, a
    name no primitive defines read by the first primitive (the source graphic), a result named like
    a standard input keyword (the keyword wins), a result read after several later primitives, and
    a graph of 80 inputs, which moves the input table to the heap, each render like the same graph
    with every result named once, whole and split into tiles
    (`NamedInputsReadTheLatestEarlierPrimitiveWithThatName`). The tables hold 32 primitives and 64
    inputs in the execution's own storage; an admitted graph with more inputs, possible only
    through `feMerge` children, moves the input table to the heap for that one execution; nothing
    outlives the execution, so this retains no memory across frames.
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
- [ ] Milestone 8: Steady-state residency for the remaining draw classes. Each item removes one
  cause of the named `kKnownViolations` entries in `geode_counter_corpus_tests`; an entry is
  deleted once every item it names has landed. Every entry listed here misses the steady state in
  the shipping configuration, with ordered batching on.
  - [x] 8.1: Stroke outlines cached per stroke key (see Milestone 2), so `<use>` instances that
    stroke one source path with different styles keep one outline each and re-encode nothing on an
    unchanged frame (`UseInstancesWithDistinctStrokeStyles_NoDirtyPath_ZeroEncodes`,
    `StrokeStylesPastTheCache_ReEncodeOnlyTheOverflow`; staleness under an edit to each key
    component and to the path: `RendererGeodeStrokeKeyTest` in `RendererGeode_tests.cc`). It
    removes the path encodes of `poker_chips`, `stroking_complex`, and `nested_svg_aspectratio`.
    The remaining uploads and bind groups of all three come from same-frame repeats of one
    resident slot: several `<use>` instances draw the same fill or stroke variant at different
    transforms or paints, and every draw after the first went through the per-frame arena. 8.2
    gives those repeats resident records, and with both items landed the three entries meet the
    steady state and are deleted.
  - [x] 8.2: Marker instance residency. A marker's content is one entity drawn once per placement;
    its first placement of a frame draws from its resident slot's uniform, and every later one
    takes a persistent record of its own (`GeodeResidentSlot::repeatRecords`) over the same
    resident geometry and joins ordered batches. The record's existing transform, paint, and clip
    rectangle carry the placement, so the `SlugFillInstance` layout is unchanged.
    `MarkerInstances_SecondFrameIsSteadyState` asserts the unchanged frame for placements whose
    marker viewport clip is an axis-aligned rectangle. A turned placement (`orient="auto"`) is not
    resident: a marker's viewport clip turns with the marker, so it reaches the renderer as a clip
    polygon, under which no draw is resident (8.9). `marker` stays listed for that reason, and
    `RotatedClippedMarkerPlacementsMatchSinglePlacementMarkers` covers those placements' pixels.
    `marker_segments` and `marker_spec_example` put `opacity` on the marker content, so each
    placement also composites an isolated layer; they clear with 8.3. Repeats of other entities
    hold the steady state the same way, each stroke variant of a path keeping its own records:
    `image_external_svg_par` and `image_external_svg_viewbox`, which draw one external SVG's paths
    twice per frame, clear here, and with 8.1 so do `poker_chips`, `stroking_complex`, and
    `nested_svg_aspectratio` (`UseStrokeStylesRepeated_SecondFrameIsSteadyState`).
  - [x] 8.3: Isolated-layer and filter-result composites. Each opacity or blend-mode composite,
    and each composite of a filter result, keeps a uniform buffer and a bind group across frames
    (`GeodeTextureEncoder::RetainedQuadBinding`), indexed by draw order within the frame, with
    layers and filter results in separate sequences. The bind group is reused only with the
    textures it was built from, matched by runtime identity (slot and generation), so it never
    binds a destroyed texture or one whose slot now holds another texture
    (`LayerCompositeRebuildsWhenItsTextureWasDestroyed`), and a composite whose texture another
    renderer holds rebuilds (`LayerCompositeRebuildsWhenAnotherRendererHoldsItsTexture`). The
    uniform is rewritten only when its bytes change
    (`LayerCompositeRewritesItsUniformWhenOpacityChanges`). Because the texture pool hands each
    size's textures out in the previous frame's acquisition order, an unchanged frame gives every
    composite the textures it had before, also when another renderer on the device draws between
    frames (`LayerCompositesKeepTheirTexturesAcrossUnchangedFrames`); a filter result keeps its
    output texture and binding when its content is edited
    (`FilterResultCompositeKeepsItsBindingAcrossContentEdits`). Each sequence keeps at most 256
    entries, the most surfaces a frame's surface budget admits at once; composites past that in
    one frame allocate their uniform and bind group for that frame only. The surface budget alone
    does not bound the entries, because a frame split by a memory-limited filter budget returns
    the surfaces it already composited (`LayerCompositeRetainedStateIsCappedAcrossFrameSplits`).
    An entry holds a 224-byte uniform buffer on the GPU, a 224-byte CPU mirror of it, and the
    handles of that buffer, at most three texture views (source, `<mask>` and blend backdrop) and
    one bind group; its host record is at most 512 bytes (a `static_assert` on
    `RetainedCompositeSequence::Entry`). The two composite sequences therefore retain at most 512
    entries: 112 KiB of uniform buffers on the GPU, 256 KiB of host records, and the runtime
    objects behind 512 buffers, at most 1024 views (a layer or filter composite views at most two
    textures) and 512 bind groups. These bytes are not charged to a byte budget. Entries the frame
    did not reach are dropped once it is submitted; each keeps its uniform buffer, with no view or
    bind group, for the next entry a frame claims, so a renderer whose frames alternate between
    documents or compositor layers does not create those buffers again
    (`RetainedCompositesKeepTheirBuffersAcrossFramesWithoutThem`), and entries and kept buffers
    together stay within the cap. A texture pool trim to nothing releases every entry and kept
    buffer, and all are released with the renderer
    (`LayerCompositeEntriesFollowTheLastFrame`). An entry's textures go back to the device's
    shared pool at each frame end while its views stay, so entries release their views and bind
    group, keeping the uniform buffer, before any texture they view is destroyed. The pool reports
    each texture to every renderer on the device before destroying it; tested for a trim by
    another renderer, a stale bucket and a budget eviction caused by another renderer's large frame
    (`RetainedCompositesReleaseViewsBeforeThePoolDestroysTheirTextures`). A frame that ends without
    submitting, because it never reached `endFrame` or its recording failed, releases every entry's
    views, including bind groups naming its clip-mask views
    (`RetainedCompositesReleaseViewsWhenAFrameIsAbandoned`). An entry under a path clip mask names
    the clip mask's view, which the renderer keeps across frames (see 8.7), and the renderer
    releases every entry naming a clip-mask view before it drops that view: when the view's texture
    is destroyed (`ClippedImageBlitsKeepTheirClipMaskViewUntilItGoes`, where another renderer's
    pool trim destroys only the clip mask's texture) and when a frame starts that the view went
    unused for. Covered only structurally: a release the pool rejects goes through the same
    reporting destroy; a texture a failed frame hands to fenced destruction, and one destroyed with
    no pool, are reported the same way; the pool destroys textures in its own destructor only after
    every renderer has unregistered; teardown releases the entries before destroying any texture;
    and the release at a frame start, which only an entry left undrawn after its claim reaches (a
    pass that failed to open, a refused uniform publish, or a singular texel mapping). Such an
    entry's bind group is never reused, since the identity of a dropped view never matches a live
    view. The runtime refuses any use of a view whose texture is gone in any case; this ordering
    keeps a retained view from outliving the request to destroy its texture. The `<mask>` texture,
    the blend backdrop, the clip-mask view and the sampler are part of the bind group's key, each
    changed alone in `RetainedQuadBindingRebuildsWhenAnyBoundInputChanges`, and a binding reports
    each texture and clip-mask view it binds to the release paths above
    (`RetainedQuadBindingReportsWhatItBinds`); the source texture is covered by the tests above.
    The bind group layout and the device are keyed too, defensively and untested: a device has one
    image pipeline layout per target format. Blend-mode and filter-region edits and a
    canvas rescale match a fresh renderer (`CompositeEditsMatchAFreshRenderer`).
    `LayerComposites_UnchangedFrameWritesAndCreatesNothing` and
    `FilterResultComposites_UnchangedFrameWritesAndCreatesNothing` gate the counters.
    `Edzample_Anim3`, `donner_icon`, the five filter scenes, and with 8.2 `marker_segments` and
    `marker_spec_example` meet the steady state: in
    `donner_icon` every counter came from its twenty `opacity: .5` spark outlines, each its own
    isolated layer, while its gradient-painted paths are resident and batched. Composites under a
    path clip mask and composites through a `<mask>` hold the steady state with 8.7.
  - [x] 8.4: Image residency. An image drawn from its element's `LoadedImageComponent` keeps its
    premultiplied RGBA8 texture on each device that draws it (`GeodeResidentImageComponent`,
    `geode/GeodeResidentImageComponent.h`), and the blit of a kept texture draws through a retained
    quad binding claimed in draw order, a third sequence beside the two 8.3 composite sequences. An
    unchanged frame therefore creates, uploads and writes nothing for an image and creates no bind
    group (`DecodedImages_UnchangedFrameWritesAndCreatesNothing`). Without it, each unchanged frame
    created and uploaded a texture per image (512 bytes for a 2x2 or 3x2 image, rows padded to 256
    bytes), wrote the blit uniform through the per-frame arena and built a bind group; there is no
    image quad geometry, the quad is expanded from the uniform. A kept texture is keyed by the
    component's revision and the image size, both compared before every draw, so new pixels upload
    once, whether `href` changed or the decoded pixels were rewritten in place under a new revision
    (`ImageEditsReachTheNextFrameOfAWarmRenderer`); the component's update and destroy listener
    drops the textures of an image whose pixels go away. Every writer of the component removes and
    re-emplaces it, which the destroy path covers; a component replaced in place reaches the update
    path (both in `ImageTexturesGoWithTheirElementDeviceAndDocument`). Each texture charges width x
    height x 4 bytes to the document's resident limit
    (`GeodeDocumentGeometryBudget::reserveImageBytes`, counted in `imageBytes()`); an image the
    limit refuses uploads a texture for each frame that draws it
    (`KeptImageTexturesStayWithinTheDocumentResidentLimit`). A replaced texture stays charged until
    it is released, so near the limit the first frame after an edit can refuse the new texture and
    upload it for that frame only; the next frame keeps it. A dropped or replaced texture waits in
    the document's resident slab for its device until no open frame on the device can still sample
    it (`NestedPassKeepsTheImageTextureAnOuterFrameLeftPending`). A frame that `RendererGeode::draw`
    draws live, under the document's write access, takes back at its start what the document's slabs
    released before it, so a texture goes at the next such frame even when it draws nothing that
    looks the slabs up, as in a document whose only drawable was the image
    (`ImageTexturesGoWhenTheDocumentHasNothingElseToDraw`). A concurrent-DOM document's frames
    replay a snapshot, which must not touch the live document, and leave its residence alone
    (`ConcurrentDomReplayLeavesTheDocumentsResidenceAlone`); there, in frames driven through
    `RendererDriver` directly, and in a document that is not drawn again, a released texture waits
    for the document's next resident lookup or its destruction. The texture's handle then goes to
    its device's handle retirement, which releases it on that device's thread
    (`RetiredTexturesWaitForTheOwnerAndForeignOnesStayWithTheCaller`). Before releasing a texture,
    the retirement reports it to the device's texture pool, whose destroy listeners are the 8.3
    ones, so every renderer on the device drops the views and bind groups of blit entries built from
    it; tested when the pixels change, when they go away, when the resident limit refuses the new
    pixels, and when the document is destroyed, each with the entry held by a renderer other than
    the one whose frame releases the texture
    (`ImageBlitsReleaseViewsBeforeTheirTexturesAreDestroyed`). A frame that ends without submitting
    releases every image entry's views (`ImageBlitsReleaseViewsWhenAFrameIsAbandoned`), and an image
    under a path clip mask keeps the bind group naming the clip mask's view until that view goes
    (`ClippedImageBlitsKeepTheirClipMaskViewUntilItGoes`). A device that goes takes its renderers
    and their entries with it; its textures stop counting against the document, and a document
    destroyed outside a frame hands its textures to the retirement
    (`ImageTexturesGoWithTheirElementDeviceAndDocument`). The image blit sequence keeps at most 256
    entries of the 8.3 entry shape (56 KiB of uniform buffers, 128 KiB of host records), uncharged
    like the composite entries; with the `<mask>` composite sequence of 8.7, a renderer's four
    sequences retain at most 1024 entries (224 KiB of uniform buffers, 512 KiB of host records);
    blits past it allocate for their frame, and its
    entries follow the last frame and a pool trim
    (`ImageBlitEntriesStayCappedAndFollowTheLastFrame`). Placement, size, sampling and transform
    edits match a fresh renderer (`ImageBlitEditsMatchAFreshRenderer`). Each draw of a kept texture
    takes its own entry, so `<use>` repeats of one image share the texture. The host record of a
    kept texture, two shared pointers per device and the texture's record, is fixed per drawn image
    element and device and is not charged. The device check on a kept texture is defensive and
    untested, since every device keeps its own entry. A render snapshot clears the image's entity
    handle at capture and replays a copy of its pixels, so replay keeps nothing and neither reads
    nor writes the document (`ReplayedImageSnapshotLeavesTheDocumentUntouched`).
    `image_data_url_opacity` and `image_data_url_pixelated` meet the steady state.
    `image_external_svg_par` and `image_external_svg_viewbox`, which draw one external SVG at two
    sizes, meet it with 8.2: their second draws of each path were same-frame repeats.
  - [x] 8.5: Pattern residency. A pattern fill or pattern stroke draws from its entity's resident
    fill or stroke slot (`GeoEncoder::fillPathPatternResident`), whose cached bind group binds the
    tile's view. The pool hands a bucket's textures out in the order the previous frame took them,
    so on an unchanged frame each tile draws into the texture it used before, and the frame uploads
    nothing and creates no bind group
    (`PatternFills_UnchangedFrameWritesAndCreatesNothing`). Clears `geode_pattern_checker`,
    `geode_pattern_nonrect`, `geode_pattern_offset`, `geode_pattern_solid`. The tile itself is
    rendered again every frame, which costs one render pass and its content draws but no gated
    counter; skipping that pass is Future Work. A pattern painted by several elements renders its
    tile once per element, and its content entities are then drawn more than once in the frame, so
    the draws after the first take the arena path, the repeat-draw limit 8.2 also meets.
  - [x] 8.6: Filter-pass steady state. A pass's parameters land in the slot they had the frame
    before, and an upload of bytes the slot already holds is skipped
    (`ReusedFilterSlotsUploadEditedParameters` checks that an edit still reaches the GPU). Texture
    views and bind groups are cached in the filter engine under the identities (slot and generation)
    of the textures and buffers they name, so a lookup returns only an object built from the live
    resources being bound, and a texture destroyed and replaced in its runtime slot misses
    (`CachedFilterBindingsFollowTextureReuseAndReplacement`). Entries unused since the previous
    sweep are destroyed at the next one; that test also checks the cache returns to one frame's
    entries after textures are replaced. The sweep, like the parameter cursor's rewind, runs at a
    renderer's beginFrame only when no frame on the device is open, so it never destroys an entry a
    recorded frame still names. While frames overlap nothing is swept and the cache grows with the
    passes recorded meanwhile; the scratch those passes grow is retained bytes that each filter
    admission counts (`retainedBufferBytes`, with per-execution growth bounded by
    `ParameterGrowthIsBoundedAfterEarlierExecutions`), so admission also bounds the cache, but that
    bound is structural and no test overlaps frames to measure it. The pool's frame counter and the
    sweep advance at every renderer's beginFrame on the device: one renderer drawing filters reaches
    the steady state (`UnchangedFilterPassesUploadAndBindNothing`), while renderers that alternate
    frames on one device sweep each other's bindings and rebuild them, which no test measures. The
    cache holds no texture, so pooled textures stay inside the pool's byte budget and per-key cap
    (structural). Each 64 KiB parameter block keeps a CPU copy of its uploaded bytes (64 KiB) and 2
    KiB of per-slot lengths; `retainedBufferBytes` counts the GPU blocks, and the copies add the
    same amount of CPU memory, which no counter reports. Keying on texture identity rather than pass
    role works because the pool hands textures out in the previous frame's acquisition order: in
    release order, the intermediates two filters share in one frame come back reversed on alternate
    frames, and every pass misses under either key (the two-filter case of
    `UnchangedFilterPassesUploadAndBindNothing`). A filter region that changes size takes new
    textures and rebuilds its bindings once.
  - [x] 8.7: Clip, mask, and nested-viewport geometry. Clip geometry, draws under a path clip,
    composites under a path clip and composites through a `<mask>` meet the steady state. Draws
    inside a `<mask>`'s content or mask capture were already resident: the clips are suspended while
    a mask is captured. `nested_svg_aspectratio`'s per-frame uploads were same-frame repeats of
    resident slots, not nested-viewport geometry, and cleared with 8.1 and 8.2.
    - Clip shapes. `ResolvedClip::sourceEntity` names the entity whose computed clip paths produced
      the clip, and a `GeodeClipPathCacheComponent` on it keeps each shape's encode at one
      device-derived tolerance and, per device, a resident slot with the shape's geometry, its
      224-byte mask uniform and its mask bind group (`GeoEncoder::fillPathIntoMaskResident`). An
      unchanged frame encodes, uploads and creates nothing for a shape; the uniform is rewritten
      only when its bytes change, and the bind group is rebuilt only when the nested clip mask it
      binds is a different view. A clip group (a `<clipPath>` with a `clip-path` of its own) unions
      its coverage by filling a unit square scaled to the target, kept like a shape under the
      group's index.
    - Invalidation. Each draw compares the shape's path with the path its encode was built from, and
      its tolerance, and encodes again on any difference; a clip with a different shape count drops
      all of its shapes. The fill rule and the transforms reach the GPU through the uniform. The
      component itself is removed when the entity's computed clip paths are created, replaced or
      destroyed, so its memory and budget charges go with its clip
      (`ClipGeometryCacheGoesWithItsClip`). Every render-tree rebuild clears every computed clip
      path in the document, so any edit anywhere that rebuilds the render tree drops every clip
      cache of the document: the first frame after such an edit encodes and uploads every clip shape
      again. The compositor's transform-only fast path for a dragged layer skips that rebuild
      (`CompositorController::renderFrameImpl`), so a drag keeps the clip caches.
      `ClipEditsReachTheNextFrameOfAWarmRenderer` compares a warm renderer with a fresh one after an
      edit of the clip shape's geometry and transform, its clip-rule on the shape and on the clipped
      element, clipPathUnits, the clipped element's bounds and transform, a nested clip, the clip
      reference, and the device scale at two zooms. `ClipShapeCacheFollowsTheDeviceScale` checks
      that a shape drawn at a new scale under the same cache is encoded again for it, and
      `ClipShapeCacheFollowsAPathOfTheSameSize` changes a shape to a path of the same size under the
      same entity: the new encode takes the old one's storage, and the slot recognizes it by the
      identity each encode gets when it is made.
    - Clipped draws. A solid, gradient or pattern fill or stroke under a path clip mask stays
      resident. Its uniform carries the clip flag exactly as the arena draw's does, and its cached
      bind group binds the mask's view (`GeodeResidentSlot::boundClipMaskView`), rebuilt when the
      draw is clipped by another view (`ResidentClippedDrawsMatchTheArenaDraws`). A clip polygon or
      an open mask pass still takes the arena, also together with a clip mask: residence under a
      clip polygon is 8.9, which this item does not cover. A same-frame repeat of a slot under a
      clip mask takes the arena too, since repeat records are not written under any clip state; the
      arena draw binds the same mask and carries the same clip flag.
    - Composites. A `<mask>` composites its content through its mask with a retained quad binding
      (8.3) claimed when the mask is popped, in a fourth sequence beside the layer, filter-result
      and image sequences; the binding views the content and the mask texture and keys its bind
      group on both, and its uniform carries the mask mode, color interpolation and region bounds,
      so it is rewritten only when one of them changes. A layer, filter-result, image or mask
      composite under a path clip mask keeps the bind group naming the clip mask's retained view
      from frame to frame. An unchanged frame writes and creates nothing for either
      (`MaskComposites_UnchangedFrameWritesAndCreatesNothing`,
      `MaskCompositesKeepTheirBindingsAcrossUnchangedFrames`, also when another renderer draws on
      the device between frames, and again after a canvas resize). Edits of the mask's content, the
      masked content, the mask's color interpolation, type and region, and a mask removed and
      restored match a fresh renderer (`MaskCompositeEditsMatchAFreshRenderer`). Mask composite
      entries follow the 8.3 lifetime rules and release their views before the pool destroys either
      texture, after a trim, a stale bucket and a budget eviction caused by another renderer, and
      after an abandoned frame (`MaskAndClippedCompositesReleaseViewsWithTheirTextures`). Each
      entry has the 8.3 entry's bytes, counted with the other sequences under 8.4, and views two
      textures, its content and its mask, so the sequence keeps the runtime objects behind at most
      256 buffers, 512 views and 256 bind groups. The composites belong to the renderer, not the
      document, so a replayed concurrent-DOM frame keeps them too
      (`SnapshotReplayKeepsMaskCompositeBindings`) and the change adds nothing to a render
      snapshot.
    - Mask views. The renderer keeps the view of each clip-mask texture across frames
      (`RetainedTextureViews`, the mechanism pattern tile views use), and the pool hands a clip mask
      the texture it had the frame before, so an unchanged frame binds the same views. A view is
      dropped at the start of the second frame of its renderer that does not use its texture
      (`ClipMaskViewsStayBoundedBetweenFrames`), and as soon as the texture is destroyed: the pool's
      destroy notification and the device's texture retirement reach every renderer's retained
      views, pattern tile views included, through the same release path the retained composites use,
      and a frame that ends without submitting drops every view
      (`TileAndClipMaskViewsGoWithTheirTextures`, for a trim, a stale bucket and a budget eviction
      caused by another renderer, and an abandoned frame). A view owns no memory, and the textures
      stay in the pool's budget. A resident slot's cached bind group that names a dropped view is
      not released with it: it is replaced at the slot's next draw, which finds a different view, or
      released with the slot. The GPU layer refuses any use of a view whose texture is gone, so no
      use of such a group is reachable; the group and its view's backend object stay until then, the
      remaining part of #2632 for these caches.
    - Snapshot replay. A document in concurrent-DOM mode is drawn by recording a render snapshot and
      replaying it (`RendererDriver::draw`), and a snapshot holds no handle into the live document:
      its paths and clips carry no source entity. A replayed frame therefore keeps no per-entity
      state at all, neither these clip caches nor the resident fill, stroke, gradient and pattern
      slots, and encodes and uploads its geometry every frame
      (`SnapshotReplayKeepsNoClipStateOnTheDocument`). Single-threaded documents, the interruptible
      draw the editor uses, and the compositor's layer rasterization draw from the live document and
      keep the steady state.
    - Lifetime. A slot carries one fill's uniform, so a slot an open frame on the device already
      drew takes the arena; a shape encoded again resets its slot, whose old range and bind group
      stay parked in the slab until no frame that may have recorded them is open
      (`NestedPassKeepsTheClipMaskAnOuterFrameLeftPending`). Slots are kept per device, released
      with the component, the entity or the device, and swept with every other per-device component
      when a device goes away.
    - Accounting. Per clip shape and device: the encode (`EncodedPath::retainedBytes()`) and the
      kept copy of its path (`Path::retainedBytes()`) are charged to the document's cache-byte
      budget, the resident geometry (the shape's band, curve and grid regions plus a
      256-byte-aligned uniform region) to its resident-byte budget, and the 224-byte CPU copy of the
      uniform to the cache budget. A shape either budget refuses fills from a per-frame encode or
      from the arena, and renders the same (`ClipGeometryPastTheDocumentBudgetStillRenders`). The
      host records, at most 576 bytes per clip shape and 576 bytes per shape and device (checked by
      `static_assert`s), are uncharged; their number is linear in the clip shapes the driver copies
      into each frame's clips, which its text-materialization budget already admits.
    - `ClippedDraws_UnchangedFrameEncodesWritesAndCreatesNothing` gates the counters. Not covered:
      the rectangle a transformed `<mask>` region is clipped by has no source entity and is encoded
      every frame; no corpus scene has one. Nested clip chains (#2499) are unchanged: the cache is
      indexed by shape, so each shape costs a constant per frame, and the superlinear cost #2499
      measures is in the driver's clip collection.
    - Corpus. `donner_splash` and `geode_splash` meet the steady state, and their entries are
      deleted from `kKnownViolations`.
  - [x] 8.8: Painted-text geometry. Solid, unwarped glyph fills draw the glyph cache's resident
    encodings. Every other shape a text element paints (a placed glyph outline filled with a
    gradient or a pattern, or with a solid along a text path; a joined glyph stroke outline; a
    decoration line and its stroke) is kept on the element by a `GeodeTextShapeCache`
    (`geode/GeodeGlyphResidency.h`), one per device, with its encode and resident slots, and draws
    through `fillPathResident`, `fillPathPatternResident` or the resident gradient fills. Shape `i`
    is the element's `i`th such draw, and is reused only when the path about to be painted, its fill
    rule and the encode tolerance are bitwise identical to what it holds; anything else replaces it
    with fresh residence. An unchanged frame therefore encodes, uploads and binds nothing for
    painted text (`PaintedTextUnchangedFrameEncodesWritesAndCreatesNothing`,
    `GeodeGlyphInstancing_tests.cc`), and a warm renderer draws an edit of the text content, font
    size, pattern content, size or transform, gradient, span boundaries, decoration, device scale,
    element transform, ancestor transform, solid stroke color, fill opacity, opacity or
    `currentColor` exactly as a fresh renderer does
    (`TextPaintEditsReachTheNextFrameOfAWarmRenderer`, `GeodeDocumentResidency_tests.cc`). The
    shapes are built and drawn in the element's space, as the per-frame draws are, so the resident
    draw paints the same pixels. An element keeps at most `kDefaultMaximumShapes` (4096) shapes per
    device; a draw past it, or one the document's cache budget refuses, encodes for the frame
    (`TextShapesPastTheCapDrawFromPerFrameEncodes`) and leaves its ordinal empty, so the shapes
    after it keep their ordinals; a draw whose new geometry is refused also drops the old shape at
    that ordinal and its charge (`AShapeAfterAnEmptyOrdinalIsKeptAndReleaseReturnsItsCharge`,
    `GeodeGlyphResidency_tests.cc`). The document budget refuses every later charge that would grow
    its total once it refuses one (Requirements and Constraints), so after a budget refusal the
    element keeps only a new shape that replaces an old one without growing the charge, as every
    other cache under that budget does. A joined stroke group the encoder refuses whole takes no
    ordinal: its glyph strokes are drawn and kept in its place, and an unchanged frame repeats only
    the refused encode attempt (`RefusedJoinedStrokeKeepsTheGlyphStrokesDrawnInItsPlace`). Each kept
    shape charges `sizeof(Shape)` (at most 2 KiB by a `static_assert`; 1,400 bytes measured in a
    64-bit Linux build) plus its path and encode capacities, and the shape owner vector its
    capacity, to the document's cache budget; its GPU geometry and uniform mirrors are charged
    through the slab as for any resident slot. Shapes past the last one an element's draw reaches
    are dropped at the end of that draw, and the rest go with a device that goes and with the
    document (`TextShapesGoWithTheirElementDeviceAndDocument`), or with the element when it is
    destroyed; an element that is detached or not drawn keeps its shapes until it is destroyed or
    drawn again. The per-element component is created at the element's first attempt to keep a
    shape, so text that paints only solid glyph fills carries none, and a first attempt the cap or
    budget refuses leaves an empty, uncharged component; the frame that creates it claims it at once
    (`NestedPassLeavesTheTextShapesTheOuterFrameJustKept`). Kept shapes draw solo from slots of
    their own and take no instance record: the shared occurrence records (`GeodeOccurrenceRecords`)
    serve the glyph cache's solid fills and the resident slots' repeat draws, and a kept text shape
    is drawn at most once per frame. A shape is replaced or dropped only by a draw of the element
    that no unsubmitted frame on the device has drawn; a second draw of the element in one frame, or
    an offscreen pass inside a frame that drew it, draws from per-frame encodes, so the shapes the
    first draw keeps survive it (`NestedPassLeavesTheTextShapesAnOuterFrameDrew`: the outer frame
    draws what a fresh renderer draws, and its next unchanged frame encodes nothing). That replaced
    and dropped residence returns through the slab's deferred free and bind-group retirement, so a
    recorded draw never loses its geometry, is structural; no test replaces a shape under an open
    frame. A glyph run painted with a gradient of more than 16 stops keeps its encodes but draws
    them through the per-frame arena, so its glyphs share one stop-ramp lookup through the run's
    resolution (`GradientTextRunLooksUpItsStopRampOnce`); a decoration line resolves its own
    gradient for each draw and looks its ramp up once either way, so it keeps its gradient
    residence. Glyph and decoration strokes are still expanded on the CPU every frame
    (`strokeOutlineFlattens`, which no gate checks). Clears `geode_text_decoration_underline`,
    `geode_text_pattern_fill`, `geode_text_span_gradient_over_pattern`,
    `geode_text_span_gradient_over_pattern_stroke`.
  - [ ] 8.9: Residency under a clip polygon. A clip polygon reaches the shader only through the
    draw-level uniform, and no resident path writes one: a slot's solo resident draw
    (`GeoEncoder::submitResidentOrArenaFill`) and a repeat's record (`tryAppendRepeatDraw`) both
    fall back to the per-frame arena while a clip polygon is active. A turned marker placement
    draws under one, because its viewport clip turns with it. Clears `marker`. Tracked in #2644
    (Geode draws under a clip polygon are never resident).

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
  `GeodeDocumentGeometryBudget` bounds the encodes a document keeps across frames (64 MiB) and
  its resident geometry, which includes kept stop ramps and image textures (64 MiB). Once it
  refuses a cache charge it refuses every later charge that would grow the document's total; a
  replacement that does not grow it is still accepted.
  From then on an encode that would add bytes, such as an outline for a new stroke key, draws
  from a per-frame encode while the encodes already cached keep serving. A document's stroke
  outlines count against that budget: up to two per stroke style and eight per path.

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
  +-- GeodePathCacheComponent      (CPU encodes: fill variants per tolerance, stroke variants
  |                                 per stroke key)
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
  pool texture; because the pool hands a bucket's textures out in the order the previous frame took
  them, an unchanged frame draws each tile into the texture it used before. The renderer keeps a
  view of each tile texture for one frame after its last use
  (`PatternTileViewsStayBoundedBetweenFrames`), and drops it as soon as the texture is destroyed
  (`TileAndClipMaskViewsGoWithTheirTextures`). A view owns no memory of its own; the tile textures
  go back to the pool at every frame end and are charged to the pool's byte budget. Because every
  tile is rendered again, a warm renderer's next frame matches a fresh render after an edit of a
  shape inside the pattern, the pattern's size or transform, a gradient or nested pattern its
  content paints with, the device scale, or an edit that makes tiles of one size trade textures
  (`PatternEditsReachTheNextFrameOfAWarmRenderer`). Two gaps outside the pattern path are not
  covered: a warm renderer keeps a marker's old context paint after the context element's paint
  changes (#2617), and setting a pattern's `width` with `setAttribute` does not reach the tile
  (#2616).
- **Residency gates.** Residence is taken only when no clip polygon or mask pass is active. A
  draw under a path clip mask is resident and binds the mask's view, which the renderer keeps
  across frames. Draws that fail a gate use the per-frame arena path.
- **Repeats within a frame.** A slot's uniform and primary record serve its first draw of a frame.
  Its second and later draws in the frame (each marker placement, a repeated `<use>` source) take
  persistent records from `GeodeResidentSlot::repeatRecords`: the `i`-th repeat writes record `i`,
  only when its bytes change, so an unchanged frame writes none of them
  (`MarkerInstances_SecondFrameIsSteadyState`, `UseRepeatsMatchPlainGeometryAndFollowEdits`) and an
  edit that moves or repaints a placement rewrites the records it changed
  (`MarkerPlacementsFollowEveryEditThatMovesOrRepaintsThem`). A slot keeps at most 4096 such
  records, each a 256-byte GPU record plus a few hundred bytes of CPU bookkeeping charged to the
  document's geometry budget, so under 3 MiB per slot (`MarkerRepeatRecordsStopAtTheSlotLimit`).
  Repeats past that, or refused by the budget, write a record that lives for one frame, and repeats
  that cannot have even that draw through the arena
  (`MarkerRepeatsPastTheRecordLimitDrawTheSamePicture`,
  `MarkerRepeatsFallBackWhenTheDocumentRefusesTheirRecords`). An offscreen pass inside an open
  frame that already wrote a slot's records takes one-frame records instead
  (`OffscreenPassLeavesTheOuterFramesRepeatRecordsAlone`). Gradient-painted repeats keep the arena
  path, because a gradient's parameters live in the slot's single paint block; pattern-painted
  draws take their own resident path and keep no repeat records. Only the solid fill and solid
  stroke slots hold repeat records: per path and device, two fill slots and eight stroke slots, a
  bound fixed by the slot arrays rather than by a test, so a path retains under about 30 MiB of
  repeat records, all charged to the document's geometry budget. The records are kept at each
  slot's high-water mark and reused: a slot reset for new geometry, including a replaced stroke or
  fill variant, keeps its records (structural: `reset()` leaves them in place), and they are
  released only when the path's residence component is removed
  (`MarkerRepeatRecordsAreReplacedWhenTheContentChanges`), when the device is dropped
  (`MarkerRepeatRecordsGoWithTheDeviceThatKeptThem`), or with the document
  (`MarkerRepeatRecordsAreKeptAcrossFramesAndGoWithTheDocument`). Repeat detection keys on the
  slot itself, so a draw of another slot of the same path, such as a stroke after its own pending
  fill, is a first draw (`ADrawOfAnotherSlotOfThePathIsNotARepeat`), and each stroke variant counts
  its own repeats (`EachStrokeVariantKeepsTheRecordsOfItsOwnRepeats`).
- **Ordered batching.** An eligible resident draw publishes its instance record into the
  `GeodeRecordSlab` and joins the pending batch; the batch flushes as one instanced draw in painter
  order.
- **Images.** A decoded image keeps its texture per device on its element
  (`GeodeResidentImageComponent`), keyed by the loaded image's revision and size and charged to
  the document's resident limit (Milestone 8, item 8.4).
- **Glyphs.** `GeodeGlyphCache` (`geode/GeodeGlyphResidency.h`) caches each glyph outline once per
  glyph identity (font, glyph, scale, stretch, rotation, and tolerance), bounded by entry and byte
  budgets. Solid-filled glyph occurrences draw from those resident outlines through instance records
  and join ordered batches. Every other shape a text element paints is kept on the element by a
  `GeodeTextShapeCache` (same header), one per device, with its encode and resident slots (8.8).
- **Invalidation.** Path geometry invalidates through `on_update<ComputedPathComponent>`, which
  fires only when `ShapeSystem` changes the geometry; no separate dirty flag is involved.
- **Uniform storage.** Resident slots hold their own uniforms, including each clip shape's mask
  uniform, and each isolated-layer or filter-result composite and each blit of a kept image texture
  keeps a uniform buffer and bind group of its own across frames. Per-frame draws bump-allocate
  from per-encoder arenas, other image blits from the encoder's uniform scratch, and filter passes
  from the filter engine's scratch blocks, which skip uploading a slot that already holds the same
  bytes. Nothing uses a uniform ring or dynamic offsets.
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

`kKnownViolations` lists the corpus scenes measured to miss one or more of these, all of them
covered by Milestone 8. An entry names the counters it breaks and their measured values. The list
gates only the built configuration, which always compiles in ordered batching.

## Testing and Validation

- `//donner/svg/renderer/geode:geode_perf_tests`: per-fixture first-frame ceilings and
  unchanged-frame invariants, including snapshot readback cost through
  `consumeReadbackStats()`.
- `//donner/svg/renderer/geode:geode_counter_corpus_tests`: the corpus-wide steady-state gate.
  `kKnownViolations` records each corpus scene measured to miss an invariant, the counters it
  breaks, their measured ceilings in the built configuration, and the reason. A listed scene must
  still break every counter its entry names, so fixing a counter fails the suite until it is
  removed from the entry, and a listed scene cannot get worse. Every scene must also render visible
  pixels unless listed in `kDeliberatelyInert`.
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
