# Design: Geode Rendering Performance

**Status:** Implemented. On an unchanged frame within its stated caps and budgets, Geode encodes no
path, creates no texture, writes no GPU buffer and creates at most six bind groups, and every scene
of the counter corpus meets that steady state with an empty known-violations list. The current
documentation is [Geode Frame Cost and Residency](../geode_performance.md); the full design is in
Git history.\
**Author:** Claude Opus 4.7\
**Created:** 2026-04-19\
**Updated:** 2026-11-23

## What shipped

This design removed Geode's per-frame GPU resource, CPU encoding and submission costs on frames
whose document did not change, without changing the coverage algorithm: Geode keeps the
single-sample analytic dual-ray coverage fill of [0041](0041-geode_analytical_aa.md). It added:

- Counter instrumentation (`GeodeCounters`), targeted counter tests (`geode_perf_tests`) and a
  corpus-wide gate (`geode_counter_corpus_tests`) that holds every render-corpus scene to the
  steady state and fails in both directions.
- A CPU encode cache on each path entity, with fill variants per device-derived tolerance bucket
  and stroke variants per stroke key.
- GPU residence per document and device: geometry and record slabs, resident fill, stroke,
  gradient and pattern slots, repeat records for same-frame repeats such as marker placements,
  clip uniforms for repeats under a clip polygon, kept stop ramps, kept image textures, a glyph
  cache, a per-element text shape cache and a clip geometry cache.
- Ordered batching of resident draws into instanced draws in painter order.
- Renderer-owned retained composite bindings for layer, filter-result, image and `<mask>`
  composites, and retained views of pattern tile and clip-mask textures.
- Frame recording through one command encoder at a time, with the frame's command buffers
  submitted together at its end unless a 16-buffer bound or a filter-memory split submits part of
  them earlier; a shared transient texture pool; and filter passes that reuse their parameter
  slots and cached bindings.
- Per-document cache and resident byte budgets that bound what a document can make the renderer
  retain, and stated caps for the state no budget charges.

## Documentation

- [Geode Frame Cost and Residency](../geode_performance.md), covering the steady-state contract
  and the tests that enforce it, every kind of retained state with its invalidation, lifetime and
  accounting, which draw paths keep residency, command buffer submission, frame-boundary releases,
  the draws that still write buffers every frame, and the known limits.
- [Embedding Geode](../guides/embedding_geode.md#EmbeddingGeodeTexturePool), covering the
  texture pool's budget and how a host trims it.
- [GPU runtime reference](../gpu_runtime.md), covering ownership and backends below the renderer.

## Original design

The full design, with its goals, milestone plan, per-item notes and alternatives, is in
[Git history at `040ea59d`](https://github.com/jwmcglynn/donner/blob/040ea59d5e825a5f6ac94107de10bc77957ef505/docs/design_docs/0030-geode_performance.md).

## Future Work

- GPU timestamp profiling on all platforms, as a separate track. The GPU runtime exposes no
  timestamp queries, `RendererGeode::enableTimestamps` has no effect, and the
  `FrameTimings::renderPassNs` and `totalGpuNs` fields stay zero.
- Skip the render pass of a pattern tile whose pixels cannot have changed. The renderer cannot tell
  that from the draws it receives, so the key has to come from the driver and cover everything the
  tile's content reads: elements outside the pattern subtree (`<use>` targets, referenced gradients
  and nested patterns), the pattern element's attributes and content transform, context paint, the
  patterns already open around the tile, document time, loaded fonts and images, and the tile's
  raster size and device.
- Decide whether the texture pool should bucket sizes. It keys textures by exact size, so a
  viewport that changes size every frame misses the pool for every size it has not kept; a counter
  test over a sequence of new viewport sizes would show whether coarser buckets pay for their
  memory.

## Related Designs

- [0017: Geode renderer](0017-geode_renderer.md)
- [0025: Composited rendering](0025-composited_rendering.md)
- [0041: Geode analytical anti-aliasing](0041-geode_analytical_aa.md)
- [0053: Native GPU runtime](0053-native_gpu_hal.md)
