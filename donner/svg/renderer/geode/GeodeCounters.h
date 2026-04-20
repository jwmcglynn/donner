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

  /// Reset all counters to zero. Called at `RendererGeode::beginFrame`.
  void reset() { *this = {}; }
};

}  // namespace donner::geode
