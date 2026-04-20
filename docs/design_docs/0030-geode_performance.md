# Design: Geode Rendering Performance

**Status:** Design
**Author:** Claude Opus 4.7
**Created:** 2026-04-19

## Summary

Geode's per-draw pipeline allocates fresh WebGPU resources (4 buffers + 1 bind
group) on every `submitFillDraw`, re-runs the full CPU encode path
(`cubicToQuadratic` → `toMonotonic` → band decomposition) on every frame, and
opens a new `CommandEncoder`/`queue().submit()` pair for every group
opacity / filter / mask push. The design doc 0017 (Phase 5) promised
"deep ECS integration for GPU resource caching"; that caching layer is not
yet wired up. This design proposes a concrete set of optimizations, ordered by
measured/suspected impact, that bring Geode's per-frame cost into a range
consistent with Donner's composited-editor targets (`DragFrameOverhead` budgets
in 0025) and unblock Phase 5 of 0017.

Scope: Geode only (`donner/svg/renderer/geode/**`, `RendererGeode.{cc,h}`).
Boundary: no pipeline algorithm changes (Slug stays Slug, MSAA stays 4×, WGSL
stays stable). This is a wiring/caching/batching design, not a new rendering
algorithm.

## Goals

- Stand up a Geode-specific perf harness and **commit budget assertions** per
  the project's debugging discipline (`EXPECT_LT(measured_ms, budget_ms)`), so
  every optimization below lands with a measurable before/after and future
  regressions trip loudly.
- Eliminate per-draw WebGPU resource creation for the steady-state path
  (`submitFillDraw`, `fillPathLinearGradient`, `fillPathRadialGradient`,
  `fillPathIntoMask`, `GeodeTextureEncoder::drawTexturedQuad`). Target: one
  persistent vertex-buffer arena, one uniform ring, one bind-group per frame
  per pipeline layout.
- Cache the CPU-side path-encode result (quadratic conversion → monotonic
  split → band decomposition) on the ECS path entity, invalidated by the
  existing incremental-invalidation dirty flags (0005). Frames where no paths
  change should do zero encode work.
- Batch render work within a frame into a single `CommandEncoder` +
  `queue().submit()`; push/pop of isolated-layer / filter / mask should not
  force a submit.
- Pool transient render-target textures (layer, filter, mask scratch) across
  frames with a size-bucketed free list so redraw-at-the-same-size is
  zero-alloc.
- Eliminate the redundant `setPipeline` on every `submitFillDraw` and retire
  dead state-tracker code.
- Drive filter-engine intermediate textures through the same pool; collapse
  the per-primitive `CommandEncoder` + `submit` into the frame's encoder.

## Non-Goals

- Re-implementing Slug or changing the shader ABI. Shader source in
  `donner/svg/renderer/geode/shaders/**` stays byte-identical for v1 of
  this work.
- Switching WebGPU vendoring (stays on `wgpu-native` v24).
- Replacing MSAA with a new AA scheme, or changing the 1-sample
  alpha-coverage fallback path (#536).
- Expanding the resvg test-suite coverage. Phase 5b's widening of
  `kGeodeDefaultMaxMismatchedPixels = 2000` is orthogonal to perf.
- Adding runtime backend switching or re-plumbing `RendererInterface`.
- GPU timestamp profiling on all platforms. Already listed in 0017 Phase 5
  and stays there as a separate track.
- Any perf work on `RendererTinySkia` or the composited-rendering layer
  (0025 tracks those separately).

## Next Steps

- Port the benchmarking harness from the `geode-dev` branch (commit
  `305685a1`, `donner/svg/renderer/benchmarks/`) onto this branch and land
  it as Milestone 0. This is the repro-first foundation every later
  milestone measures against.
- Add four instrumented counters to `GeoEncoder` (buffer creates,
  bind-group creates, command-buffer submits, path encodes) and surface
  them through `RendererGeode::lastFrameTimings()`. Use these as the
  in-test assertion signal — cheaper and more stable than GPU timestamps,
  directly targets the hot paths identified below.
- Write `GeodePerf_tests.cc` with per-optimization budget tests on
  representative SVGs (`lion.svg`, `Ghostscript_Tiger.svg`,
  `donner_splash.svg`) asserting counter ceilings before any fix lands.

## Implementation Plan

- [x] Milestone 0: Perf harness + counters (repro-first gate for everything
  below). _Landed 2026-04-19._
  - [x] Cherry-picked `305685a1` (`donner/svg/renderer/benchmarks/RendererBench.cc`)
    onto this branch; `bazel run --config=geode -c opt
    //donner/svg/renderer/benchmarks:renderer_bench` runs end-to-end.
  - [x] Added `GeodeCounters` (bufferCreates, bindgroupCreates,
    submits, pathEncodes, textureCreates) on `GeodeDevice` and exposed
    via `RendererGeode::lastFrameTimings()`. Instrumentation covers
    `GeoEncoder`, `GeodeTextureEncoder`, and `RendererGeode`'s own
    render-target + layer/filter/mask/pattern texture allocations and
    `queue().submit()` sites.
  - [x] Added `//donner/svg/renderer/geode:geode_perf_tests`
    (`tests/GeodePerf_tests.cc`) with four tests:
    `SimpleShapes_BaselineCeilings`, `Moderate_BaselineCeilings`,
    `Lion_BaselineCeilings`, `CountersResetBetweenFrames`. Ceilings are
    ~1.3–1.5× current observed values; each assertion carries a
    `// M{N}:` tag naming the milestone that will tighten it.
  - Observed M0 baselines (macOS/Metal, M4 Pro, 2026-04-19):
    - SimpleShapes: pathEncodes=3, bufferCreates=13, bindgroupCreates=3,
      textureCreates=4, submits=2
    - Moderate (incl. `<path opacity="0.8">` layer): pathEncodes=2,
      bufferCreates=10, bindgroupCreates=3, textureCreates=8, submits=4
    - Lion (~132 filled paths): pathEncodes=132, bufferCreates=529,
      bindgroupCreates=132, textureCreates=4, submits=2
  - Left open: `GeodeFilterEngine` is NOT instrumented yet
    (36 allocation/submit sites). Deferred to Milestone 5 since no M0
    fixture exercises filters. Filter-using fixtures will be added when
    Milestone 5 lands.
- [ ] Milestone 1: Per-draw resource pooling (Tier 1 findings).
  - [x] Persistent vertex arena in `GeoEncoder::Impl` — single growable
    `wgpu::Buffer` (`Vertex | CopyDst`), writes advance a bump pointer,
    grown by retaining retired buffers until encoder destruction.
    _Landed 2026-04-19._
  - [x] Persistent band/curve SSBO arenas mirroring the vertex arena
    (bind-group entries carry per-draw 256-byte-aligned offsets into
    a shared buffer). _Landed 2026-04-19._
  - [x] Persistent uniform arena (M1.f.1) — same pattern as SSBOs,
    256-byte-aligned offsets, one Uniform-usage arena covers
    `Uniforms`, `GradientUniforms`, and `MaskUniforms` variants.
    _Landed 2026-04-19._ Lion `bufferCreates`: 529 → 6 (98.9%
    reduction). Caveat: bind groups still rebuilt per draw since
    the bind-group layout is static (not `hasDynamicOffset`); see
    M1.f.2 for the follow-up that collapses `bindgroupCreates` too.
  - [ ] M1.f.2: `hasDynamicOffset = true` bind-group layouts +
    cached-per-pipeline bind group. Drops `bindgroupCreates` to O(1)
    per pipeline per frame.
    - **Investigation 2026-04-19 (abandoned, not shipped):** an
      initial implementation with `hasDynamicOffset = true` on the
      uniform / bands / curves bindings + a signature-tracked
      per-pipeline bind-group cache reached `bindgroupCreates`
      132 → 5 on Lion. However, multi-draw fixtures (`Rect2`,
      `Ellipse1`, `QuadBezier`, `Polyline`) rendered with
      300-2000-pixel differences against goldens — even with the
      cache bypassed. The corruption is in the dynamic-offset
      plumbing itself, not the caching logic. Reverted.
    - Hypotheses to investigate on the next attempt: (a) the
      bind-group entry `size` field interacts with WGSL uniform-block
      layout differently than expected when `size != sizeof(struct)`;
      (b) `queue.writeBuffer` followed by a dynamic-offset bind in
      the same frame may have an ordering/visibility issue on
      wgpu-native Metal; (c) storage-buffer dynamic offsets may
      require a per-slot synchronization barrier we're not emitting.
      Next attempt should enable wgpu-native validation trace mode
      and capture a frame with Metal GPU Capture (Xcode) to confirm
      the actual reads.
  - [x] Delete the redundant `pass.setPipeline` at `GeoEncoder.cc:942`.
    _Landed 2026-04-19._ `submitFillDraw` now relies on
    `bindSolidPipeline` for state tracking — previously the explicit
    `setPipeline` call was issued unconditionally after
    `bindSolidPipeline` returned, defeating the tracker on every solid
    draw. Golden + perf tests green.
  - [x] Persistent dummy view + sampler cached at `GeoEncoder`
    construction. _Landed 2026-04-19._ `ensureDummyResources` now runs
    once in the constructor; removed from the five per-draw entry points
    (`submitFillDraw`, `fillPathIntoMask`, `fillPathLinearGradient`,
    `fillPathRadialGradient`, plus the internal helper). No counter
    change (resources already created once via the idempotent guard);
    saves one function-call + branch per draw.
  - [x] `GeodeTextureEncoder::drawTexturedQuad`: verified that when
    `maskTexture` and `dstSnapshotTexture` are null, the `maskView` /
    `dstView` aliases are ref-counted handle copies of the source view,
    not additional `createView()` calls. Only the single source
    `createView()` runs per blit in the common case — this is already
    optimal. Per-texture view caching would require invasive lifetime
    plumbing for a negligible win; deferred unless a future profile
    shows it's a hot path.
  - [x] Tightened Milestone 0 counter ceilings in `GeodePerf_tests.cc`:
    Lion `bufferCreates` ceiling now 10 (down from 800); Lion
    `bindgroupCreates` ceiling stays 200 pending M1.f.2.
- [x] Milestone 2: `GeodePathCacheComponent` — cache the CPU encode
  (Tier 3 findings, maps to 0017 Phase 5 bullet 1). _Landed
  2026-04-20._ Observed frame-2 `pathEncodes` deltas:
  SimpleShapes 3→0, Moderate 2→0, Lion 132→0,
  Ghostscript_Tiger 305→0. GPU-arena-handle retention (original
  bullet 3) deferred to a follow-up PR; `<use>` instancing
  stays on Milestone 6.
  - Cache only the final `EncodedPath`, not the intermediate
    `quadPath` / `monotonicPath`. The intermediate stages are fused
    inside `GeodePathEncoder::encode` and retaining them buys
    nothing on a pure re-render (geometry didn't change) while
    costing extra memory per path. The stroke slot caches the
    `strokeToFill` output path alongside its encode, since both are
    derived from the same stroke-key inputs.
  - **Entity plumbing.** Widen `RendererInterface::drawPath` to
    carry an `EntityHandle` so the encode sites can look up the
    cache component on the source entity. Touches three
    implementations (`RendererGeode`, `RendererTinySkia`,
    `MockRendererInterface`) and the three `renderer_.drawPath()`
    call sites in `RendererDriver::traverseRange()` (lines 928,
    1373, 1667). Entity is already in scope at those call sites
    (`view.currentEntity()` at `RendererDriver.cc:818`). Non-Geode
    implementations ignore the new parameter.
  - **ShapeSystem content-equality gate (prerequisite).**
    Today, `ShapeSystem::instantiateAllComputedPaths`
    (`ShapeSystem.cc:191-204`) iterates every shape in the
    registry and calls `emplace_or_replace<ComputedPathComponent>`
    unconditionally — it does not per-entity-gate on the
    `DirtyFlags::Shape` bit. That means if any single entity is
    dirty, every shape's `ComputedPathComponent` is rewritten,
    which would invalidate every cached encode. Fix before M2
    caches anything: at each of the 8 write sites under
    `createComputedShapeWithStyle(...)` overloads
    (`ShapeSystem.cc:294,317,334,355,365,391,423,429`), compare
    the newly built `Path` to the existing `ComputedPathComponent::spline`
    (via `try_get`). If equal, return the existing component
    without calling `emplace_or_replace`. Requires a new
    `Path::operator==` in `donner/base/Path.h` (members:
    `commands_`, `points_`). This is a standalone CPU-side perf
    win (skips downstream work keyed on the write) and — most
    importantly — makes entt's `on_update<ComputedPathComponent>`
    signal a precise "geometry actually changed" edge.
  - **Invalidation via entt `on_update` signal.** In
    `RendererGeode::Impl`, connect listeners on
    `registry.on_update<ComputedPathComponent>()` and
    `on_destroy<ComputedPathComponent>()`. Listener:
    `registry.remove<GeodePathCacheComponent>(entity)`. entt
    signals are synchronous, so the wipe happens in-band with the
    write. No polling, no dirty-flag cascade reasoning. Ctor
    connects; dtor disconnects via stored
    `entt::scoped_connection`s.
  - **Component layout.** Under
    `donner/svg/renderer/geode/GeodePathCacheComponent.h`:

    ```cpp
    struct GeodePathCacheComponent {
      std::optional<EncodedPath> fillEncode;
      struct StrokeSlot {
        StrokeStyle strokeKey;               // equality-keyed on stroke inputs
        Path strokedPath;                    // strokeToFill output
        EncodedPath strokedEncode;
      };
      std::optional<StrokeSlot> strokeSlot;
    };
    ```

    Installed lazily via `registry.get_or_emplace` at the Geode
    encode call sites — keeps the component Geode-local, so
    `RendererTinySkia` pays no storage cost.
  - **Cache hit path.** At each encode site
    (`GeoEncoder::submitFillDraw`, `fillPathLinearGradient`,
    `fillPathRadialGradient`, `fillPathIntoMask`) the encoder
    receives the entity handle and takes the fast path:

    ```
    if (auto* cache = registry.try_get<GeodePathCacheComponent>(entity);
        cache && cache->fillEncode.has_value()) {
      // hit — reuse cached EncodedPath, skip encode + countPathEncode.
    } else {
      encode(); store into cache->fillEncode;
    }
    ```

    `countPathEncode()` is *only* called on miss, so the counter
    gates the test assertion directly.
  - **Stroke slot.** Keyed by `StrokeStyle ==`. Geometry changes
    wipe the whole `GeodePathCacheComponent` via the entt signal
    above, so the stroke slot is implicitly invalidated too.
    Stroke-only changes (stroke width/dash/cap/join via CSS)
    don't fire the signal — but they do change `StrokeStyle`, so
    the equality check on the existing stroke slot misses and
    causes a regenerate. Decoupled storage means fill changes
    don't pay for re-strokeToFill and vice versa.
  - **Deferred to a follow-up PR:**
    - GPU-resident arena handle retention (original M2 bullet 3):
      caching the vertex/band/curve offsets from M1's persistent
      arenas so cached paths also skip re-upload. Requires arena
      generation-stamping so cached offsets invalidate when the
      arena grows or recycles. Separate PR.
    - Batching duplicate `<use>` instances onto a single cached
      encode via instanced draws — tracked in M6.
  - **Test plan (repro-first per `CLAUDE.md` debugging discipline).**
    Added to `donner/svg/renderer/geode/tests/GeodePerf_tests.cc`:
    - `Lion_NoDirtyPath_ZeroEncodes` — render twice, assert
      `counters.pathEncodes == 0` on frame 2.
    - `GhostscriptTiger_NoDirtyPath_ZeroEncodes` — same shape on
      Ghostscript_Tiger.svg.
    - `Lion_OneGeometryChange_OneEncode` — mutate one path's `d`,
      assert exactly one re-encode (stretch: only the changed
      path's encode fires).
    Tests land red (pathEncodes > 0 on frame 2) before any
    implementation.
  - **Counter ceiling after M2:** `pathEncodes == 0` on a no-change
    frame for `lion.svg` and `Ghostscript_Tiger.svg`, tightened
    from the M0 observed baselines (Lion frame-1 pathEncodes=132).
- [x] Milestone 3: Single-encoder-per-frame (Tier 2 findings).
  _Landed 2026-04-19._
  - [x] `GeoEncoder` gained a shared-CommandEncoder constructor
    (`finish()` ends the open pass without calling
    `commandEncoder.finish()` / `queue.submit()`).
  - [x] `RendererGeode::Impl` now owns a `frameCommandEncoder` created
    at `beginFrame` and finalised + submitted exactly once at
    `endFrame`. All 11 `GeoEncoder` instantiations (base + layer /
    filter / mask / pattern push+pop sites) use the shared-CE ctor.
  - [x] Blend-mode snapshot copy now records directly into
    `frameCommandEncoder` via `copyTextureToTexture` — the side
    CommandEncoder + submit pair is gone.
  - [x] Filter engine still runs its own CommandEncoder / submit
    (self-contained subsystem). `popFilterLayer` calls a new
    `flushFrameCommandEncoder()` helper so the layer-texture writes
    are visible to the filter engine. Pays one extra submit on
    filter-using frames only; non-filter frames see the full M3 win.
  - [x] Counter delta: Moderate (1 `<g opacity>` layer) drops from
    `submits=4` → `submits=2` (frame + readback). Ceiling tightened
    in `GeodePerf_tests.cc`.
- [x] Milestone 4: Transient render-target pool (Tier 2 findings).
  _Landed 2026-04-20 (M4.2)._
  - [x] M4.1: Reuse `impl_->target` and `impl_->msaaTarget` across frames
    when `pixelWidth`/`pixelHeight` are unchanged. _Landed 2026-04-19._
    Tracks `impl_->targetWidth`/`impl_->targetHeight` in Impl; beginFrame
    only recreates when the new viewport size differs. The
    `CountersResetBetweenFrames` test gates this with
    `EXPECT_LT(secondCounters.textureCreates, firstCounters.textureCreates)`.
  - [x] M4.2: exact-size `(width, height, format, sampleCount, usage)`
    pool on `RendererGeode::Impl` covering `pushIsolatedLayer` /
    `popIsolatedLayer` (including the mix-blend-mode `dstSnapshot`),
    `pushFilterLayer` / `popFilterLayer`, `pushMask` / `popMask`, and
    the clip-mask layers allocated by `pushClip`. Release is deferred
    to `endFrame` (after `queue.submit`) so recorded GPU work
    completes before a subsequent acquire hands the texture back out.
    Observed frame-2 `textureCreates` deltas:
    SimpleShapes 2→0, Moderate (with isolated layer) 8→0,
    Lion 2→0, Ghostscript_Tiger 2→0. Power-of-two size bucketing
    (for viewport-resize scenarios) remains a follow-up.
  - [x] Prerequisite to M4.2: share the two GeoEncoder bind-group
    dummy resources (1×1 RGBA8 pattern + 1×1 R8Unorm clip mask, plus
    views and samplers) on `GeodeDevice`. Prior to this, every
    push/pop encoder allocated its own pair; this alone dropped
    per-frame `textureCreates` by 2 on simple fixtures and by 6+ on
    layered ones.
  - [x] Counter ceiling: `texture_creates == 0` on a repeat-render of
    the same document at the same size. Asserted by
    `{SimpleShapes,Moderate,Lion,GhostscriptTiger}_NoDirtyPath_ZeroTextures`.
- [ ] Milestone 5: Filter engine caching (Tier 5 findings).
  - [ ] Swap `std::unordered_map<std::string, wgpu::Texture> namedBuffers`
    in `GeodeFilterEngine::execute` (`GeodeFilterEngine.cc:964`) for an
    `RcString`-keyed hash map or a small fixed-size array indexed by the
    stable filter-node-index assigned at `FilterGraph` build. Current
    `.str()` copies per result lookup.
  - [ ] Route `createIntermediateTexture` (`GeodeFilterEngine.cc:192-204`)
    through the pool from Milestone 4.
  - [ ] Merge the per-primitive `CommandEncoder` + `queue().submit()` pairs
    (10+ sites, e.g. `:376`, `:425`, `:1156`, `:1313`, `:1383`, `:1556`,
    `:1664`, `:1735`, `:1890`, `:1973`) into the outer frame encoder.
- [ ] Milestone 6: Batch draws sharing pipeline state (0017 Phase 5
  bullet 6).
  - [x] **M6-A: instrumentation.** `drawCalls` + `pipelineSwitches`
    counters added to `GeodeCounters`, wired into every
    `pass.draw(...)` and pipeline-tracker switch site in
    `GeoEncoder` / `GeodeTextureEncoder`. Baseline ceilings
    asserted in `GeodePerf_tests.cc`. _Landed 2026-04-20._

    Key finding: Lion (132 paths) fires `drawCalls=132` /
    `pipelineSwitches=1`. The state tracker already collapses
    contiguous same-pipeline draws onto a single `setPipeline` —
    M6's Bullet 1 ("sort + collapse contiguous same-pipeline
    draws") has zero headroom on solid-fill fixtures, and
    reordering across paint order would break SVG semantics
    anyway. The only lever that moves `drawCalls` on unchanged
    input is Bullet 2 (`<use>` instancing).

  - [ ] Bullet 1 (sort / collapse contiguous same-pipeline draws):
    **deprioritised.** The state tracker already delivers this
    within a single render pass; the only residual win requires
    reordering across paint order, which SVG forbids. Unlikely
    to ship without M1.f.2 (dynamic-offset bind groups) first
    exposing a different bind-group amortization regime.
  - [ ] Bullet 2 (instanced `<use>` draws): **partially landed.**
    Motivating fixture: `docs/img/arch_container.svg` with 1028
    `<use>` elements — today that's 1028 `drawCalls`; target is
    O(distinct-source-entity × distinct-paint) per render pass.
    Scope for the first cut: consecutive `<use>` siblings that
    share the same `dataEntity` AND same resolved solid paint
    AND no stroke/mask/filter/clip → one instanced GPU draw with
    per-instance transforms via a new storage-buffer binding.

    - [x] **M6-B step 1 — detection counter.** Added
      `sameSourceDrawPairs` to `GeodeCounters`. `RendererGeode::Impl`
      tracks `lastDrawSourceEntity`; `drawPath` bumps the counter
      on consecutive same-source calls. Zero on fixtures without
      `<use>`; reports `N − 1` on a run of `N` consecutive
      batchable `<use>` instances (verified by
      `UseHeavy_BaselineCeilings` with 8 `<use>` → 7). Lands
      separately from any actual batching so the signal is
      observable even before the shader/driver pass ships.
    - [x] **M6-B step 2 — shader + bind-group plumbing.** Added
      binding 7 on the Slug-fill pipeline: a per-instance affine
      transforms SSBO (`slug_fill.wgsl` + alpha-coverage variant).
      `vs_main` now takes `@builtin(instance_index)`, fetches the
      instance transform, and composes `effective_mvp = mvp *
      instance_mat`. For non-instanced draws `GeodeDevice` owns
      a 1-element identity buffer — existing draws are no-op
      identity composes, goldens and the resvg suite unchanged.
      Unlocks `fillPathInstanced`, next.
    - [ ] **M6-B step 3 — batching.** Still to write:
      - `GeoEncoder::fillPathInstanced(path, color, rule,
        std::span<Transform2d>, precomputedEncoded)` — uploads
        transforms into an arena slice, sets up the bind group
        with binding 7 pointing at the slice, issues
        `pass.draw(vertexCount, instanceCount=N, 0, 0)`.
      - `RendererInterface::drawPathInstanced(...)` — virtual
        with a default loop fallback for non-Geode backends.
      - Driver lookahead in `RendererDriver::traverseRange`:
        peek ahead from the current entity, accumulate
        consecutive entities with same `dataEntity` / same
        solid paint / no subtree / compatible clip. On the
        accumulator, emit one `drawPathInstanced`; advance
        the view past the group.
      - Update `UseHeavy_BaselineCeilings` to assert
        `drawCalls == 1` + `sameSourceDrawPairs == 0` (the
        batcher eliminates the consecutive same-source runs).
- [ ] Milestone 7: Opportunistic cleanups.
  - [ ] GPU-side `takeSnapshot` unpremultiply (`RendererGeode.cc:2322-2353`)
    via a one-dispatch compute kernel writing into a CopySrc buffer;
    drops the CPU divide-per-pixel loop.
  - [ ] Guard `writeClipPolygonUniforms` (`GeoEncoder.cc:275-279`) on
    `clipPolygonActive` — the zero-polygon case is the norm.
  - [ ] Delete the subpath-count scan at `RendererGeode.cc:2034-2038`
    once Milestone 2's cached `strokeToFill` result plumbs the
    per-subpath count alongside the encoded output.

## Background

### Current state

Geode is Phase 3 + Phase 4 + Phase 3d landed; Phase 5 (ECS cache) and
the "batch draw calls" bullet are open checklist items in
`docs/design_docs/0017-geode_renderer.md:1439-1448`. The backend is
feature-complete enough to be the default for GPU-capable consumers
(0017 Phase 4 text + filter work is green).

Design doc 0017 establishes the *goal* of ECS-resident GPU caches but
leaves the mechanism TBD. 0025's "Perf-gate waivers (v1)" table
documents the current composited-rendering cost gap and attributes it
to three concrete causes (dirty-walk, split-bitmap reblit, style
cascade); none of those three are Geode-internal, but the Geode
steady-state cost is the denominator any future composited-rendering
perf win will divide into. Eliminating per-draw resource churn in Geode
directly benefits the `DragFrameOverhead_*` budgets in 0025.

### Why this is a design, not a patch

Two of the proposed changes — dynamic-offset uniforms (Milestone 1) and
the `GeodePathCacheComponent` (Milestone 2) — are cross-cutting. Getting
the bind-group layout right up front avoids a second rewrite when the
cache lands. Landing the harness first (Milestone 0) lets every later
milestone trade an on-paper claim for a measured counter delta.

### Existing benchmark work

Commit `305685a1` on branch `geode-dev` (unmerged) adds
`donner/svg/renderer/benchmarks/` with a standalone `renderer_bench`
binary measuring parse / draw / snapshot / GPU timestamps over inline
SVGs and testdata fixtures. Milestone 0 ports this forward; no new
harness needs to be written from scratch.

## Requirements and Constraints

- **No exceptions, no malloc.** All pools use size caps + `std::optional`
  for allocation-failure signalling, per
  `feedback_no_exceptions_no_malloc.md`.
- **Repro-first.** Every milestone's budget assertion must land in
  `GeodePerf_tests.cc` before its fix. Per the CLAUDE.md "Debugging
  Discipline" section: no perf fix ships without an automated repro and
  explicit `EXPECT_LT(measured, budget)`.
- **No regressions on goldens.** `renderer_geode_golden_tests`,
  `renderer_tests` under `--config=geode`, and `resvg_test_suite_geode*`
  must stay green across every milestone. Strict identity thresholds
  stay strict.
- **ECS-first.** Caches live on entities (like the ECS components in
  `donner/svg/components/**`) and invalidate through the existing dirty-
  flag channel, not via an ad-hoc side map keyed on `Path*` pointers.
- **Cache invariants documented.** Each cache component specifies what
  invalidates it (which dirty flags, which property changes) — otherwise
  they become stale-bug factories.
- **Pool bounds.** Arena buffers and texture pools have hard size caps
  (configurable via `GeodeConfig`). On cap hit, fall back to the
  current per-draw allocation path — never crash, never grow unbounded.

## Proposed Architecture

### Hot-path taxonomy

Findings map into four classes. Each has its own fix shape:

```
┌────────────────────┬──────────────────────────┬───────────────────────────┐
│ Class              │ Example sites            │ Fix shape                 │
├────────────────────┼──────────────────────────┼───────────────────────────┤
│ Per-draw GPU alloc │ GeoEncoder.cc:956-993    │ Arena / ring / cached BG  │
│                    │ drawTexturedQuad:141-179 │                           │
├────────────────────┼──────────────────────────┼───────────────────────────┤
│ Per-draw CPU work  │ GeodePathEncoder.cc:47+  │ ECS cache component +     │
│                    │ strokeToFill at :2028    │ dirty-flag invalidation   │
├────────────────────┼──────────────────────────┼───────────────────────────┤
│ Per-push submit    │ RendererGeode.cc:1365    │ Single-encoder frame      │
│                    │   :1538, :1654, :1828    │                           │
├────────────────────┼──────────────────────────┼───────────────────────────┤
│ Transient texture  │ pushIsolatedLayer :1328  │ Size-bucketed pool        │
│                    │ pushFilterLayer :1506    │                           │
│                    │ pushMask :1611           │                           │
│                    │ beginFrame :1020-1047    │                           │
└────────────────────┴──────────────────────────┴───────────────────────────┘
```

### Persistent buffers in `GeoEncoder::Impl`

```
struct Impl {
  // Existing: pass, pipeline, device...

  // Milestone 1: per-frame arenas, reset in beginFrame().
  wgpu::Buffer vertexArena;   // Vertex | CopyDst, grown as needed.
  uint64_t vertexArenaOffset;
  wgpu::Buffer bandArena;     // Storage | CopyDst.
  uint64_t bandArenaOffset;
  wgpu::Buffer curveArena;    // Storage | CopyDst.
  uint64_t curveArenaOffset;

  // Uniform ring with dynamic-offset bind groups.
  wgpu::Buffer uniformRing;   // Uniform | CopyDst, ring-allocated per draw.
  uint64_t uniformRingHead;
  wgpu::BindGroup solidDynBG;
  wgpu::BindGroup gradientDynBG;
  wgpu::BindGroup imageDynBG;

  // Cached dummies (currently rebuilt every draw).
  wgpu::Texture dummyTexture;
  wgpu::TextureView dummyView;
  wgpu::Sampler dummySampler;

  // Perf counters (Milestone 0).
  Counters counters;
};
```

Per-draw cost drops from `createBuffer × 4 + createBindGroup × 1 +
writeBuffer × 4` to `writeBuffer × 1 (uniform only) + setBindGroup +
setDynamicOffset`.

### `GeodePathCacheComponent` ownership and invalidation

```
Entity with <path d="...">
  ├── ComputedPathComponent (exists today)
  └── GeodePathCacheComponent (new)
         ├── EncodedPath encoded       # CPU bands/curves/vertices
         ├── ArenaSlot vertexSlot      # handles into GeoEncoder arenas
         ├── ArenaSlot bandSlot        #   (Milestone 2 extension)
         ├── ArenaSlot curveSlot       #
         └── uint64_t cacheEpoch       # bumped by IncrementalInvalidation
```

Invalidation: `IncrementalInvalidationSystem` already emits dirty flags
when a path's `d`/transform/stroke changes (0005). A tiny
`GeodePathCacheSystem` listens for those flags and drops the cache
entry (or just bumps the epoch so the next draw re-uses the slot).

This exactly matches 0017 Phase 5 bullet 1
(`Implement GeodePathCacheComponent: cache encoded band data on path
entities`) and bullet 2 (`Implement cache invalidation via dirty flags`).

### Single-encoder frames + render-target pool

`RendererGeode::beginFrame` creates one `CommandEncoder`; all draws,
layer blits, filter passes, mask passes, and the final resolve go into
that encoder, submitted once at `endFrame`. Layer textures come from
a `GeodeTexturePool` keyed on `(bucket_w, bucket_h, format, sampleCount)`.
Pool size capped in `GeodeConfig`; overflow falls back to per-use
createTexture.

### Counter-based tests over wall-clock tests

Wall-clock budgets are flaky on shared CI runners (see the "Perf-gate
waivers (v1)" table in 0025). Counter assertions (`buffer_creates == 0`,
`path_encodes == 0`, `submits == 1`) are deterministic, so each
milestone's ceiling is a hard invariant — not a 2-3× guard band. We
keep one or two wall-clock tests as smoke checks but *do not* use them
as the milestone gates.

## Performance

Measurement plan:

1. **Geode bench harness** (`renderer_bench`, Milestone 0) reports CPU
   wall-clock draw, snapshot, and GPU timestamps across fixed SVG
   fixtures at N=iterations with warmup. Use as the human-facing
   before/after number on each milestone.
2. **Counter assertions** (`GeodePerf_tests.cc`, Milestone 0) are the
   actual CI gates.
3. **Compositor-level budgets** (`CompositorPerfTest` in
   `donner/svg/compositor/CompositorPerf_tests.cc`) — watch that
   `DragFrameOverhead_10kNodes` improves monotonically. Do *not* tighten
   those budgets as part of this work; the compositor's O(entities)
   dirty walk is the dominant term there (0025 Perf-gate waivers).

Targets (to validate empirically after Milestone 0):

- Steady-state single-frame `buffer_creates` drops from O(paths) to 0.
- Steady-state `bindgroup_creates` drops from O(paths+blits) to O(pipelines).
- Steady-state `path_encodes` drops from O(paths) to 0 after Milestone 2.
- Steady-state `submits` drops from O(1 + push/pop depth) to 1.
- Steady-state `texture_creates` drops from O(layers + masks + filters
  + 2-per-frame) to 0 after Milestone 4.

The above are expressed as invariant ceilings rather than "X% faster"
claims because the actual wall-clock savings depend on the driver, the
GPU, and the SVG. Counter ceilings either hold or they don't.

## Testing and Validation

- `//donner/svg/renderer/geode/tests:geode_perf_tests` (new) — budget
  assertions per milestone.
- `//donner/svg/renderer/geode/tests:geode_device_tests`,
  `:geode_path_encoder_tests`, `:geode_shaders_tests`,
  `:geo_encoder_tests` — existing unit tests stay green; expect some
  changes to `geo_encoder_tests` when the bind-group layout shifts to
  dynamic offsets.
- `//donner/svg/renderer/tests:renderer_geode_golden_tests` —
  strict-identity goldens must stay bit-identical; any drift is a
  correctness regression, not a threshold candidate.
- `//donner/svg/renderer/tests:renderer_tests` under `--config=geode` —
  must stay green at current `geodeOverrides()` tolerances.
- `//donner/svg/renderer/tests:resvg_test_suite_geode*` —
  596-passing / 0-failing baseline must hold.
- `//donner/svg/compositor:compositor_perf_tests` — must stay green
  (thresholds unchanged in this work).
- No new fuzzing surface introduced. Existing XML/CSS/SVG parsers and
  their fuzzers are unaffected.

### Per-milestone validation gate

Before each milestone is marked done:

1. All golden + correctness tests green on `--config=geode` and default
   (`--config=`).
2. `geode_perf_tests` ceiling for that milestone passes with margin.
3. `renderer_bench` wall-clock median does not regress vs the prior
   milestone's baseline on any fixture (hand-reviewed, reported in the
   PR).

## Dependencies

- No new external deps. Uses existing `wgpu-native` v24 API
  (dynamic-offset bind groups are core WebGPU, not an extension).
- ECS dirty-flag plumbing comes from `0005-incremental_invalidation.md`;
  Milestone 2 depends on that system being online for path entities.
  If 0005's path-level invalidation isn't wired up yet, Milestone 2
  can ship a conservative "invalidate on any `ComputedPathComponent`
  replacement" fallback and tighten later.

## Rollout Plan

- All work lands behind the existing `--config=geode` flag. No runtime
  switching.
- Each milestone is a separately-reviewable PR with its own
  before/after `renderer_bench` numbers in the PR description.
- Counter-based `GeodePerf_tests.cc` ceilings are the durable regression
  barrier — they stay green forever after their milestone lands.

## Alternatives Considered

**Switch to a one-big-buffer-suballocator model at the wgpu layer
instead of per-`GeoEncoder` arenas.** Rejected for v1: adds a layer of
indirection, complicates embedder integration (0017 Phase 6), and
offers no measurable win over per-frame arenas in a single-threaded
renderer.

**Cache encoded paths keyed on `Path` pointer identity, not ECS
entity.** Rejected: `Path` is immutable but copied/moved through the
scene-graph build; entity identity is the correct key and aligns with
0017's design intent.

**Skip the harness, land the fixes eyeball-first.** Rejected per the
repro-first rule in CLAUDE.md "Debugging Discipline". Also, the
counter ceilings are the cleanest way to prevent re-regression.

**Move all of `GeodeFilterEngine` to compute-pipeline fusion in one
big kernel.** Out of scope. That's a larger design (cf.
`0014-filter_performance.md` for the CPU-side equivalent) and is not
required to hit the per-frame goals here.

## Open Questions

- Does `IncrementalInvalidationSystem` currently emit a stable
  per-entity dirty flag for path geometry changes, or does it only
  track the composited-layer dirty set? Milestone 2's design choice
  branches on this — answer affects whether we need a new dirty flag
  or just listen to an existing one.
- Should the uniform ring buffer be sized per-pipeline (three rings:
  solid, gradient, image) or one ring with three bind groups pointing
  into it? One ring is simpler; three avoid false sharing if
  pipeline-layout-specific alignment differs. Measure during
  Milestone 1.
- `GeodeTexturePool` bucket granularity: power-of-two (fewer buckets,
  more wasted VRAM) vs nearest-64-pixel (tighter VRAM, more buckets).
  Decide after Milestone 4 with real SVG data.

# Future Work

- [ ] GPU timestamp profiling on all platforms (0017 Phase 5 bullet 4).
  Exposing per-pass GPU time in `lastFrameTimings` lets the compositor
  attribute frame cost to specific draws.
- [ ] Cross-process shader cache (Dawn/wgpu-native supports WGSL → SPIR-V
  caching; currently every process-launch re-runs Tint).
- [ ] Glyph-level path cache (0017 Phase 4 `GeodeGlyphCacheComponent`)
  once text is a measured hotspot in a real editor workload.
- [ ] Investigate whether `populateSharedGradientUniforms` can move
  gradient stops to a texture (freeing the >16-stop cap in
  `slug_gradient.wgsl:28` that's already flagged as Phase 5 work).
