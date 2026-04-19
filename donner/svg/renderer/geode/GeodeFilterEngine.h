#pragma once
/// @file
/// GPU filter-graph executor for the Geode rendering backend.
///
/// Owns compute pipelines for filter primitives and executes a
/// \ref donner::svg::components::FilterGraph against a source-graphic
/// texture, returning the filtered output texture.
///
/// Phase 7 initial scope: only `feGaussianBlur` is implemented. Other
/// primitives are passed through (the input texture is forwarded
/// unchanged) with a one-shot warning.

#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"

namespace donner::svg::components {
struct FilterGraph;
}  // namespace donner::svg::components

namespace donner::geode {

class GeodeDevice;

/**
 * GPU filter-graph executor.
 *
 * Given a `FilterGraph` and a source-graphic texture (the offscreen layer
 * snapshot captured between `pushFilterLayer` / `popFilterLayer`), executes
 * the graph's primitives on the GPU and returns the final output texture.
 *
 * Intermediate textures between primitives are allocated on demand and
 * keyed by `result` names.
 *
 * Currently supports:
 * - `feGaussianBlur` (two-pass separable Gaussian via compute shader)
 *
 * Unsupported primitives pass the current buffer through unchanged.
 */
class GeodeFilterEngine {
public:
  /// @param device The Geode device (owns wgpu::Device + queue).
  /// @param verbose If true, emit one-shot warnings for unsupported primitives.
  explicit GeodeFilterEngine(GeodeDevice& device, bool verbose = false);

  ~GeodeFilterEngine();

  GeodeFilterEngine(const GeodeFilterEngine&) = delete;
  GeodeFilterEngine& operator=(const GeodeFilterEngine&) = delete;

  /**
   * Execute a filter graph against the source-graphic texture.
   *
   * The source texture must be RGBA8Unorm with `TextureBinding` usage.
   * The returned texture is a freshly-allocated RGBA8Unorm texture sized
   * to the filter region (or the source dimensions if no region is given).
   *
   * @param graph The filter graph to execute.
   * @param sourceGraphic The input texture (layer snapshot).
   * @param filterRegion The filter region in device-pixel coordinates.
   * @return The filtered output texture (RGBA8Unorm, TextureBinding | CopySrc).
   */
  wgpu::Texture execute(const svg::components::FilterGraph& graph,
                        const wgpu::Texture& sourceGraphic, const Box2d& filterRegion);

private:
  /// Two-pass separable Gaussian blur via compute shader.
  /// @param input The input texture.
  /// @param stdDeviationX Standard deviation in X (pixels).
  /// @param stdDeviationY Standard deviation in Y (pixels).
  /// @param edgeMode Edge handling mode (0=None, 1=Duplicate, 2=Wrap).
  /// @return The blurred texture.
  wgpu::Texture applyGaussianBlur(const wgpu::Texture& input, double stdDeviationX,
                                  double stdDeviationY, uint32_t edgeMode);

  /// Run a single blur pass (horizontal or vertical).
  /// @param input Source texture for this pass.
  /// @param width Texture width.
  /// @param height Texture height.
  /// @param stdDeviation Standard deviation for this axis.
  /// @param axis 0 = horizontal, 1 = vertical.
  /// @param edgeMode Edge handling mode.
  /// @return Output texture for this pass.
  wgpu::Texture runBlurPass(const wgpu::Texture& input, uint32_t width, uint32_t height,
                            float stdDeviation, uint32_t axis, uint32_t edgeMode);

  GeodeDevice& device_;
  wgpu::ComputePipeline gaussianBlurPipeline_;
  wgpu::BindGroupLayout blurBindGroupLayout_;
  bool verbose_ = false;
  bool warnedUnsupported_ = false;
};

}  // namespace donner::geode
