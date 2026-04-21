#pragma once
/// @file
/// Per-frame instrumentation counters for the Geode rendering backend.
///
/// The counters observe steady-state hot paths identified in design doc
/// 0030 (geode_performance). They are the primary regression signal for
/// the perf-optimization milestones: each later milestone tightens one
/// or more of these ceilings, and `GeodePerf_tests.cc` asserts them.
///
/// Counters are free of cost when disabled — hot-path sites check a
/// nullable pointer, so a renderer constructed without counters pays
/// one null-pointer compare per increment site.

#include <cstdint>

namespace donner::geode {

/**
 * Steady-state resource-creation and submission counts for a single frame.
 *
 * Populated by `GeoEncoder` and `RendererGeode` at the sites listed in
 * design doc 0030. Reset at the start of every `RendererGeode::beginFrame`.
 *
 * Invariant: any counter exposed here reflects work done during the
 * previous frame's `beginFrame` → `endFrame` window. Counters do not
 * wrap around; they are `uint64_t` and monotonic within a frame.
 */
struct GeodeCounters {
  /// `wgpu::Device::createBuffer` calls. Steady-state target after
  /// Milestone 1: `== 0` on an unchanged-geometry frame.
  uint64_t bufferCreates = 0;

  /// `wgpu::Device::createBindGroup` calls. Steady-state target after
  /// Milestone 1: `<= number_of_pipelines` per frame (one bind group
  /// per pipeline layout, dynamic offsets for per-draw uniforms).
  uint64_t bindgroupCreates = 0;

  /// `wgpu::Device::createTexture` calls (render targets, layer /
  /// filter / mask scratch, blend snapshots). Steady-state target
  /// after Milestone 4: `== 0` on repeat-render at the same size.
  uint64_t textureCreates = 0;

  /// `wgpu::Queue::submit` calls. Steady-state target after
  /// Milestone 3: `== 1` per frame regardless of layer/filter/mask
  /// push depth.
  uint64_t submits = 0;

  /// `GeodePathEncoder::encode` calls (CPU-side path → bands). Steady-
  /// state target after Milestone 2: `== 0` on an unchanged-geometry
  /// frame (the `GeodePathCacheComponent` serves all paths).
  uint64_t pathEncodes = 0;

  /// `wgpu::RenderPassEncoder::draw` / `drawIndexed` calls. One per
  /// submitted draw call, regardless of instance count. Used to gate
  /// Milestone 6: same-source-entity `<use>` draws collapse to a
  /// single instanced call, so heavy `<use>` fixtures should drop
  /// proportionally.
  uint64_t drawCalls = 0;

  /// `wgpu::RenderPassEncoder::setPipeline` calls that actually
  /// switched the bound pipeline (the GeoEncoder state tracker
  /// deduplicates no-op binds). Gates M6's "sort / collapse
  /// contiguous same-pipeline draws" bullet: on a pure-solid fixture
  /// the steady-state value should converge toward the number of
  /// distinct pipelines the frame touches (typically 1 for Lion-
  /// style many-solid-fill input).
  uint64_t pipelineSwitches = 0;

  /// Number of consecutive `drawPath` calls whose source entity
  /// matches the immediately previous call's source entity — i.e.
  /// the draw-call savings that would be unlocked by M6 Bullet 2
  /// (`<use>` instancing). A run of N consecutive same-source draws
  /// contributes `N - 1` here.
  ///
  /// Zero on fixtures without `<use>` (Lion, Tiger). Non-zero on
  /// fixtures where `<use>` elements reference the same source in
  /// adjacent draw order, which is what the future instancing pass
  /// will collapse into one GPU draw call per group.
  uint64_t sameSourceDrawPairs = 0;

  /// Reset all counters to zero. Called at `RendererGeode::beginFrame`.
  void reset() { *this = {}; }
};

}  // namespace donner::geode
