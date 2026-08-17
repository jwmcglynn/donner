#pragma once
/// @file
/// Per-frame instrumentation counters for the Geode rendering backend.
///
/// The counters observe the steady-state hot paths of the Geode backend.
/// They are the primary regression signal for its performance work: each
/// optimization tightens one or more of these ceilings, and
/// `GeodePerf_tests.cc` asserts them.
///
/// Counters are free of cost when disabled - hot-path sites check a
/// nullable pointer, so a renderer constructed without counters pays
/// one null-pointer compare per increment site.

#include <cstdint>

namespace donner::geode {

/**
 * Steady-state resource-creation and submission counts for a single frame.
 *
 * Populated by `GeoEncoder` and `RendererGeode` at their per-frame resource
 * creation and submission sites. Reset at the start of every
 * `RendererGeode::beginFrame`.
 *
 * Invariant: any counter exposed here reflects work done during the
 * previous frame's `beginFrame` → `endFrame` window. Counters do not
 * wrap around; they are `uint64_t` and monotonic within a frame.
 */
struct GeodeCounters {
  /// `wgpu::Device::createBuffer` calls. Steady-state target with the
  /// cross-frame buffer pool: `== 0` on an unchanged-geometry frame.
  uint64_t bufferCreates = 0;

  /// `wgpu::Device::createBindGroup` calls. Steady-state target:
  /// `<= number_of_pipelines` per frame (one bind group per pipeline
  /// layout, dynamic offsets for per-draw uniforms).
  uint64_t bindgroupCreates = 0;

  /// `wgpu::Device::createTexture` calls (render targets, layer /
  /// filter / mask scratch, blend snapshots). Steady-state target with
  /// render-target reuse and the transient texture pool: `== 0` on
  /// repeat-render at the same size.
  uint64_t textureCreates = 0;

  /// `wgpu::Queue::submit` calls. Steady-state target with the shared
  /// CommandEncoder: `== 1` per frame regardless of layer/filter/mask
  /// push depth.
  uint64_t submits = 0;

  /// `GeodePathEncoder::encode` calls (CPU-side path → bands). Steady-
  /// state target with the path-encode cache: `== 0` on an unchanged-
  /// geometry frame (the `GeodePathCacheComponent` serves all paths).
  uint64_t pathEncodes = 0;

  /// `wgpu::RenderPassEncoder::draw` / `drawIndexed` calls. One per
  /// submitted draw call, regardless of instance count. Used to gate
  /// `<use>` instancing: same-source-entity `<use>` draws collapse to a
  /// single instanced call, so heavy `<use>` fixtures should drop
  /// proportionally.
  uint64_t drawCalls = 0;

  /// `wgpu::RenderPassEncoder::setPipeline` calls that actually
  /// switched the bound pipeline (the GeoEncoder state tracker
  /// deduplicates no-op binds). Gates the "sort / collapse contiguous
  /// same-pipeline draws" work: on a pure-solid fixture
  /// the steady-state value should converge toward the number of
  /// distinct pipelines the frame touches (typically 1 for Lion-
  /// style many-solid-fill input).
  uint64_t pipelineSwitches = 0;

  /// Number of consecutive `drawPath` calls whose source entity
  /// matches the immediately previous call's source entity - i.e.
  /// the draw-call savings that would be unlocked by `<use>`
  /// instancing. A run of N consecutive same-source draws
  /// contributes `N - 1` here.
  ///
  /// Zero on fixtures without `<use>` (Lion, Tiger). Non-zero on
  /// fixtures where `<use>` elements reference the same source in
  /// adjacent draw order, which is what the future instancing pass
  /// will collapse into one GPU draw call per group.
  uint64_t sameSourceDrawPairs = 0;

  /// `wgpu::Queue::writeBuffer` calls. Together with `bufferWriteBytes`
  /// this measures the CPU -> GPU buffer-upload traffic of a frame.
  /// Steady-state today: one write per arena region per draw (vertex,
  /// bands, curves, vBands, vCurves, hGrid, vGrid, uniforms), because
  /// cached `EncodedPath` data has no persistent GPU residence and is
  /// re-uploaded every frame.
  uint64_t bufferWrites = 0;

  /// Total payload bytes passed to `wgpu::Queue::writeBuffer`. The
  /// per-frame GPU transfer volume for buffer data. An unchanged-
  /// geometry frame should converge toward 0 once encoded path data
  /// gains persistent GPU residence.
  uint64_t bufferWriteBytes = 0;

  /// Total payload bytes passed to `wgpu::Queue::writeTexture` (image
  /// decode uploads, gradient ramps, filter LUTs, dummy textures).
  /// Steady-state target: 0 on an unchanged frame (texture uploads are
  /// already cached; this counter verifies that claim).
  uint64_t textureWriteBytes = 0;

  /// `Path::strokeToFill` calls that actually rebuilt a stroke outline this
  /// frame (cache hits do not count). Zero on an unchanged-geometry,
  /// unchanged-zoom frame; one per stroked draw whose stroke params or
  /// device-scale bucket changed.
  uint64_t strokeOutlineFlattens = 0;

  /// Total point count across the stroke outlines rebuilt this frame.
  ///
  /// The stroke pipeline flattens curves with a tolerance derived from the
  /// draw-time device transform (see `GeodeStrokeTolerance.h`), so this count
  /// grows with the view scale. That makes it the observable signal that the
  /// flattening tracked the device scale: scale-blind flattening yields the
  /// same point count at 1x and at 32x, which is exactly the defect that makes
  /// zoomed-in curves render as visible polygons.
  uint64_t strokeOutlinePoints = 0;

  /// Reset all counters to zero. Called at `RendererGeode::beginFrame`.
  void reset() { *this = {}; }
};

}  // namespace donner::geode
