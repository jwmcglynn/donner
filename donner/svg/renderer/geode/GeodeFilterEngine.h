#pragma once
/// @file
/// GPU filter-graph executor for the Geode rendering backend.
///
/// Owns compute pipelines for filter primitives and executes a
/// \ref donner::svg::components::FilterGraph against a source-graphic
/// texture, returning the filtered output texture.
///
/// Implemented primitives: `feGaussianBlur`, `feOffset`, `feColorMatrix`,
/// `feFlood`, `feMerge`. Other primitives are passed through (the input
/// texture is forwarded unchanged) with a one-shot warning.

#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"

namespace donner::svg::components {
struct FilterGraph;
struct FilterNode;

namespace filter_primitive {
struct Offset;
struct ColorMatrix;
struct Flood;
struct Merge;
}  // namespace filter_primitive
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
 * - `feOffset` (pixel shift via compute shader)
 * - `feColorMatrix` (4x5 matrix transform via compute shader)
 * - `feFlood` (constant color fill via compute shader)
 * - `feMerge` (alpha-over composite of N inputs via compute shader)
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

  /// Shift pixels by (dx, dy) via compute shader.
  /// @param input The input texture.
  /// @param primitive The feOffset parameters.
  /// @return The offset texture.
  wgpu::Texture applyOffset(const wgpu::Texture& input,
                            const svg::components::filter_primitive::Offset& primitive);

  /// Apply a 4x5 color matrix to each pixel via compute shader.
  /// @param input The input texture.
  /// @param primitive The feColorMatrix parameters.
  /// @return The transformed texture.
  wgpu::Texture applyColorMatrix(const wgpu::Texture& input,
                                 const svg::components::filter_primitive::ColorMatrix& primitive);

  /// Fill the output with a constant flood color via compute shader.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @param primitive The feFlood parameters.
  /// @return The flood-filled texture.
  wgpu::Texture applyFlood(uint32_t width, uint32_t height,
                           const svg::components::filter_primitive::Flood& primitive);

  /// Alpha-over composite of N input textures via sequential compute dispatches.
  /// @param node The feMerge filter node (inputs resolve to merge children).
  /// @param namedBuffers Named intermediate textures.
  /// @param currentBuffer The "previous" output buffer.
  /// @param sourceGraphic The original source-graphic texture.
  /// @return The composited texture.
  wgpu::Texture applyMerge(
      const svg::components::FilterNode& node,
      const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
      const wgpu::Texture& currentBuffer, const wgpu::Texture& sourceGraphic);

  /// Run a single alpha-over composite pass (src over dst → output).
  /// @param src Source texture.
  /// @param dst Destination texture.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @return The composited texture.
  wgpu::Texture runMergePass(const wgpu::Texture& src, const wgpu::Texture& dst, uint32_t width,
                             uint32_t height);

  GeodeDevice& device_;

  // Gaussian blur pipeline (reused from Phase 7 initial scope).
  wgpu::ComputePipeline gaussianBlurPipeline_;
  wgpu::BindGroupLayout blurBindGroupLayout_;

  // feOffset pipeline.
  wgpu::ComputePipeline offsetPipeline_;
  wgpu::BindGroupLayout offsetBindGroupLayout_;

  // feColorMatrix pipeline.
  wgpu::ComputePipeline colorMatrixPipeline_;
  wgpu::BindGroupLayout colorMatrixBindGroupLayout_;

  // feFlood pipeline.
  wgpu::ComputePipeline floodPipeline_;
  wgpu::BindGroupLayout floodBindGroupLayout_;

  // feMerge alpha-over blit pipeline.
  wgpu::ComputePipeline mergePipeline_;
  wgpu::BindGroupLayout mergeBindGroupLayout_;

  bool verbose_ = false;
  bool warnedUnsupported_ = false;
};

}  // namespace donner::geode
