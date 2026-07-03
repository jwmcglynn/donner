#pragma once
/// @file
/// GPU filter-graph executor for the Geode rendering backend.
///
/// Owns compute pipelines for filter primitives and executes a
/// \ref donner::svg::components::FilterGraph against a source-graphic
/// texture, returning the filtered output texture.
///
/// Implemented primitives: `feGaussianBlur`, `feOffset`, `feColorMatrix`,
/// `feFlood`, `feMerge`, `feComposite`, `feBlend`, `feMorphology`,
/// `feComponentTransfer`, `feConvolveMatrix`, `feTurbulence`,
/// `feDisplacementMap`, `feDiffuseLighting`, `feSpecularLighting`,
/// `feDropShadow`, `feImage`, `feTile`. Other primitives are passed
/// through (the input texture is forwarded unchanged) with a one-shot
/// warning.

#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::svg::components {
struct FilterGraph;
struct FilterNode;

namespace filter_primitive {
struct Offset;
struct ColorMatrix;
struct Flood;
struct Merge;
struct Composite;
struct Blend;
struct Morphology;
struct ComponentTransfer;
struct ConvolveMatrix;
struct Turbulence;
struct DisplacementMap;
struct DiffuseLighting;
struct SpecularLighting;
struct DropShadow;
struct Image;
struct Tile;
}  // namespace filter_primitive
}  // namespace donner::svg::components

namespace donner::geode {

class GeodeDevice;
struct FilterResourceArena;

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
 * - `feComposite` (Porter-Duff compositing of two inputs via compute shader)
 * - `feBlend` (W3C Compositing 1 blend modes via compute shader)
 * - `feMorphology` (erode / dilate via min / max rectangular kernel)
 * - `feComponentTransfer` (per-channel LUT transform via compute shader)
 * - `feConvolveMatrix` (NxM kernel convolution via compute shader)
 * - `feTurbulence` (Perlin noise / fractal noise via compute shader)
 * - `feDisplacementMap` (per-pixel channel-driven displacement via compute shader)
 * - `feDiffuseLighting` (Lambertian shading with distant/point/spot lights)
 * - `feSpecularLighting` (Phong shading with distant/point/spot lights)
 * - `feDropShadow` (blur alpha + offset + flood-tint + source-over)
 * - `feImage` (Mitchell-Netravali bicubic placement of external raster / in-document fragment)
 * - `feTile` (wraparound tiling of input subregion across filter region)
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
   * @param filterRegion The filter region in user-space coordinates.
   * @param deviceFromFilter The combined transform from filter/user-space to
   *   device-pixel coordinates, captured at `pushFilterLayer` time. Used to
   *   derive per-axis scale factors and to project directional parameters
   *   (e.g. feOffset dx/dy) through rotation/skew.
   * @return The filtered output texture (RGBA8Unorm, TextureBinding | CopySrc).
   */
  wgpu::Texture execute(const svg::components::FilterGraph& graph,
                        const wgpu::Texture& sourceGraphic, const Box2d& filterRegion,
                        const Transform2d& deviceFromFilter);

private:
  /// Two-pass separable Gaussian blur via compute shader.
  /// @param input The input texture.
  /// @param stdDeviationX Standard deviation in X (pixels).
  /// @param stdDeviationY Standard deviation in Y (pixels).
  /// @param edgeMode Edge handling mode (0=None, 1=Duplicate, 2=Wrap).
  /// @return The blurred texture.
  wgpu::Texture applyGaussianBlur(FilterResourceArena& arena, const wgpu::Texture& input,
                                  double stdDeviationX, double stdDeviationY, uint32_t edgeMode);

  /// Run a single blur pass (horizontal or vertical).
  /// @param input Source texture for this pass.
  /// @param width Texture width.
  /// @param height Texture height.
  /// @param stdDeviation Standard deviation for this axis.
  /// @param axis 0 = horizontal, 1 = vertical.
  /// @param edgeMode Edge handling mode.
  /// @return Output texture for this pass.
  wgpu::Texture runBlurPass(FilterResourceArena& arena, const wgpu::Texture& input, uint32_t width,
                            uint32_t height, float stdDeviation, uint32_t axis, uint32_t edgeMode);

  /// One pass of a 3-pass box blur (used to approximate a Gaussian for sigma
  /// >= 2.0, matching tiny-skia's behaviour).
  /// @param input The input texture.
  /// @param width Texture width.
  /// @param height Texture height.
  /// @param boxLeft Number of samples on the negative side of the centre tap.
  /// @param boxRight Number of samples on the positive side of the centre tap.
  /// @param axis 0 = horizontal, 1 = vertical.
  /// @param edgeMode Edge handling mode.
  /// @return Output texture for this pass.
  wgpu::Texture runBoxBlurPass(FilterResourceArena& arena, const wgpu::Texture& input,
                               uint32_t width, uint32_t height, int32_t boxLeft, int32_t boxRight,
                               uint32_t axis, uint32_t edgeMode);

  /// Shift pixels by (dx, dy) via compute shader.
  /// @param input The input texture.
  /// @param primitive The feOffset parameters.
  /// @return The offset texture.
  wgpu::Texture applyOffset(FilterResourceArena& arena, const wgpu::Texture& input,
                            const svg::components::filter_primitive::Offset& primitive);

  /// Apply a 4x5 color matrix to each pixel via compute shader.
  /// @param input The input texture.
  /// @param primitive The feColorMatrix parameters.
  /// @return The transformed texture.
  wgpu::Texture applyColorMatrix(FilterResourceArena& arena, const wgpu::Texture& input,
                                 const svg::components::filter_primitive::ColorMatrix& primitive);

  /// Extract SourceAlpha (0,0,0,A) from a SourceGraphic texture.
  /// @param input The source-graphic texture.
  /// @return A texture whose RGB are zero and alpha matches the input alpha.
  wgpu::Texture applySourceAlpha(FilterResourceArena& arena, const wgpu::Texture& input);

  /// Fill the output with a constant flood color via compute shader.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @param primitive The feFlood parameters.
  /// @return The flood-filled texture.
  wgpu::Texture applyFlood(FilterResourceArena& arena, uint32_t width, uint32_t height,
                           const svg::components::filter_primitive::Flood& primitive);

  /// Alpha-over composite of N input textures via sequential compute dispatches.
  /// @param node The feMerge filter node (inputs resolve to merge children).
  /// @param namedBuffers Named intermediate textures.
  /// @param currentBuffer The "previous" output buffer.
  /// @param sourceGraphic The original source-graphic texture.
  /// @param linearRGB If true, composite in linearRGB: convert each input
  ///   sRGB→linear before the alpha-over passes and convert the result
  ///   linear→sRGB (matches tiny-skia's `color-interpolation-filters` handling).
  /// @return The composited texture.
  wgpu::Texture applyMerge(FilterResourceArena& arena, const svg::components::FilterNode& node,
                           const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
                           const wgpu::Texture& currentBuffer, const wgpu::Texture& sourceGraphic,
                           const wgpu::Texture* sourceAlpha, bool linearRGB);

  /// Run a single alpha-over composite pass (src over dst → output).
  /// @param src Source texture.
  /// @param dst Destination texture.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @return The composited texture.
  wgpu::Texture runMergePass(FilterResourceArena& arena, const wgpu::Texture& src,
                             const wgpu::Texture& dst, uint32_t width, uint32_t height);

  /// Porter-Duff compositing of two inputs via compute shader.
  /// @param in1 First input texture (source).
  /// @param in2 Second input texture (destination/backdrop).
  /// @param primitive The feComposite parameters (operator + k1..k4).
  /// @return The composited texture.
  wgpu::Texture applyComposite(FilterResourceArena& arena, const wgpu::Texture& in1,
                               const wgpu::Texture& in2,
                               const svg::components::filter_primitive::Composite& primitive);

  /// W3C Compositing 1 blend of two inputs via compute shader.
  /// @param in1 First input texture (source).
  /// @param in2 Second input texture (backdrop).
  /// @param primitive The feBlend parameters (blend mode).
  /// @return The blended texture.
  wgpu::Texture applyBlend(FilterResourceArena& arena, const wgpu::Texture& in1,
                           const wgpu::Texture& in2,
                           const svg::components::filter_primitive::Blend& primitive);

  /// Morphological erode / dilate via min / max rectangular kernel.
  /// @param input The input texture.
  /// @param primitive The feMorphology parameters (operator, radiusX, radiusY).
  /// @param pixelRadiusX Horizontal radius in pixels.
  /// @param pixelRadiusY Vertical radius in pixels.
  /// @return The morphed texture.
  wgpu::Texture applyMorphology(FilterResourceArena& arena, const wgpu::Texture& input,
                                const svg::components::filter_primitive::Morphology& primitive,
                                int pixelRadiusX, int pixelRadiusY);

  /// Per-channel LUT transform (feComponentTransfer).
  /// @param input The input texture.
  /// @param primitive The feComponentTransfer parameters (4 channel functions).
  /// @return The transformed texture.
  wgpu::Texture applyComponentTransfer(
      FilterResourceArena& arena, const wgpu::Texture& input,
      const svg::components::filter_primitive::ComponentTransfer& primitive);

  /// NxM kernel convolution (feConvolveMatrix).
  /// @param input The input texture.
  /// @param primitive The feConvolveMatrix parameters.
  /// @return The convolved texture.
  wgpu::Texture applyConvolveMatrix(
      FilterResourceArena& arena, const wgpu::Texture& input,
      const svg::components::filter_primitive::ConvolveMatrix& primitive);

  /// Perlin noise / fractal noise generator (feTurbulence).
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @param primitive The feTurbulence parameters.
  /// @param deviceFromFilter Transform from filter/user space into device space.
  /// @return The noise texture.
  wgpu::Texture applyTurbulence(FilterResourceArena& arena, uint32_t width, uint32_t height,
                                const svg::components::filter_primitive::Turbulence& primitive,
                                const Transform2d& deviceFromFilter);

  /// Per-pixel channel-driven displacement (feDisplacementMap).
  /// @param in1 Source image texture.
  /// @param in2 Displacement map texture.
  /// @param primitive The feDisplacementMap parameters.
  /// @param pixelScale Displacement scale in pixels.
  /// @return The displaced texture.
  wgpu::Texture applyDisplacementMap(
      FilterResourceArena& arena, const wgpu::Texture& in1, const wgpu::Texture& in2,
      const svg::components::filter_primitive::DisplacementMap& primitive, double pixelScale);

  /// Lambertian diffuse lighting (feDiffuseLighting).
  /// @param input The input texture (alpha = height map).
  /// @param primitive The feDiffuseLighting parameters.
  /// @param scaleX User-to-pixel scale X.
  /// @param scaleY User-to-pixel scale Y.
  /// @param linearRGB If true, convert the lighting color sRGB→linear and the
  ///   output linear→sRGB (matches tiny-skia's linearRGB color-interpolation).
  /// @return The lit texture.
  wgpu::Texture applyDiffuseLighting(
      FilterResourceArena& arena, const wgpu::Texture& input,
      const svg::components::filter_primitive::DiffuseLighting& primitive,
      const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
      const Box2d& sampleSubregion, bool linearRGB);

  /// Phong specular lighting (feSpecularLighting).
  /// @param input The input texture (alpha = height map).
  /// @param primitive The feSpecularLighting parameters.
  /// @param scaleX User-to-pixel scale X.
  /// @param scaleY User-to-pixel scale Y.
  /// @param linearRGB If true, convert the lighting color sRGB→linear and the
  ///   output linear→sRGB (matches tiny-skia's linearRGB color-interpolation).
  /// @return The lit texture.
  wgpu::Texture applySpecularLighting(
      FilterResourceArena& arena, const wgpu::Texture& input,
      const svg::components::filter_primitive::SpecularLighting& primitive,
      const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
      const Box2d& sampleSubregion, bool linearRGB);

  /// Drop-shadow composite (feDropShadow): blur alpha, offset, flood, source-over.
  /// @param input The input texture.
  /// @param primitive The feDropShadow parameters.
  /// @param pixelStdDevX Blur standard deviation in pixel units (X).
  /// @param pixelStdDevY Blur standard deviation in pixel units (Y).
  /// @param pixelDx Offset in pixel units (X).
  /// @param pixelDy Offset in pixel units (Y).
  /// @return The drop-shadowed texture.
  wgpu::Texture applyDropShadow(FilterResourceArena& arena, const wgpu::Texture& input,
                                const svg::components::filter_primitive::DropShadow& primitive,
                                double pixelStdDevX, double pixelStdDevY, double pixelDx,
                                double pixelDy);

  /// Blit an external image into a freshly-allocated filter-sized texture (feImage).
  /// @param primitive The feImage parameters (imageData, preserveAspectRatio, fragmentId, ...).
  /// @param width Output texture width (filter-region pixels).
  /// @param height Output texture height (filter-region pixels).
  /// @param graph The enclosing filter graph (for bbox / scale / subregion resolution).
  /// @param node The enclosing filter node (for primitive subregion overrides).
  /// @param deviceFromFilter The combined CTM from filter/user-space to device pixels.
  ///   Used by fragment references to project through rotation/skew.
  /// @param placementRegionUser The feImage placement rectangle in user space,
  ///   already resolved for percent/OBB units and with absent x/y/width/height
  ///   defaulted to the filter region. The image is fit into this rect per
  ///   preserveAspectRatio.
  /// @return The placed-image texture.
  wgpu::Texture applyImage(FilterResourceArena& arena,
                           const svg::components::filter_primitive::Image& primitive,
                           uint32_t width, uint32_t height,
                           const svg::components::FilterGraph& graph,
                           const svg::components::FilterNode& node,
                           const Transform2d& deviceFromFilter, const Box2d& placementRegionUser);

  /// Wraparound tile of an input subregion across the full output (feTile).
  /// @param input The input texture.
  /// @param srcX Source rectangle X origin in pixels.
  /// @param srcY Source rectangle Y origin in pixels.
  /// @param srcW Source rectangle width in pixels.
  /// @param srcH Source rectangle height in pixels.
  /// @return The tiled texture.
  wgpu::Texture applyTile(FilterResourceArena& arena, const wgpu::Texture& input, int32_t srcX,
                          int32_t srcY, int32_t srcW, int32_t srcH);

  /// Clip a primitive's output to its user-space subregion via the inverse CTM.
  /// Pixels whose center maps outside the subregion are zeroed.
  /// @param input The primitive's output texture.
  /// @param filterFromDevice Inverse of the deviceFromFilter transform.
  /// @param usrX0 User-space subregion left edge.
  /// @param usrY0 User-space subregion top edge.
  /// @param usrX1 User-space subregion right edge.
  /// @param usrY1 User-space subregion bottom edge.
  /// @return A new texture with out-of-subregion pixels cleared.
  wgpu::Texture applySubregionClip(FilterResourceArena& arena, const wgpu::Texture& input,
                                   const Transform2d& filterFromDevice, double usrX0, double usrY0,
                                   double usrX1, double usrY1);

  /// Convert a texture between sRGB and linearRGB color spaces.
  /// Used to implement `color-interpolation-filters: linearRGB` (the SVG default).
  /// @param input The input texture in premultiplied sRGB (or linear, for the reverse).
  /// @param srgbToLinear True to convert sRGB→linear, false for linear→sRGB.
  /// @return A new texture in the target color space.
  wgpu::Texture applyColorSpaceConversion(FilterResourceArena& arena, const wgpu::Texture& input,
                                          bool srgbToLinear);

  GeodeDevice& device_;

  // Gaussian blur pipeline (reused from Phase 7 initial scope).
  ScopedWgpuHandle<wgpu::ComputePipeline> gaussianBlurPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> blurBindGroupLayout_;

  // feOffset pipeline.
  ScopedWgpuHandle<wgpu::ComputePipeline> offsetPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> offsetBindGroupLayout_;

  // feColorMatrix pipeline.
  ScopedWgpuHandle<wgpu::ComputePipeline> colorMatrixPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> colorMatrixBindGroupLayout_;

  // feFlood pipeline.
  ScopedWgpuHandle<wgpu::ComputePipeline> floodPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> floodBindGroupLayout_;

  // feMerge alpha-over blit pipeline.
  ScopedWgpuHandle<wgpu::ComputePipeline> mergePipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> mergeBindGroupLayout_;

  // feComposite Porter-Duff pipeline (two inputs + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> compositePipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> compositeBindGroupLayout_;

  // feBlend W3C blend-mode pipeline (two inputs + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> blendPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> blendBindGroupLayout_;

  // feMorphology erode/dilate pipeline (input + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> morphologyPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> morphologyBindGroupLayout_;

  // feComponentTransfer LUT pipeline (input + output + storage buffer).
  ScopedWgpuHandle<wgpu::ComputePipeline> componentTransferPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> componentTransferBindGroupLayout_;

  // feConvolveMatrix kernel pipeline (input + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> convolveMatrixPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> convolveMatrixBindGroupLayout_;

  // feTurbulence noise pipeline (output + storage buffer, no input texture).
  ScopedWgpuHandle<wgpu::ComputePipeline> turbulencePipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> turbulenceBindGroupLayout_;

  // feDisplacementMap pipeline (two inputs + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> displacementMapPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> displacementMapBindGroupLayout_;

  // feDiffuseLighting pipeline (input + output + storage buffer).
  ScopedWgpuHandle<wgpu::ComputePipeline> diffuseLightingPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> diffuseLightingBindGroupLayout_;

  // feSpecularLighting pipeline (input + output + storage buffer).
  ScopedWgpuHandle<wgpu::ComputePipeline> specularLightingPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> specularLightingBindGroupLayout_;

  // feDropShadow compose pipeline (two inputs + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> dropShadowPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> dropShadowBindGroupLayout_;

  // feImage placement pipeline (input texture + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> imagePipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> imageBindGroupLayout_;

  // feTile wraparound pipeline (input + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> tilePipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> tileBindGroupLayout_;

  // Per-primitive subregion clipping pipeline (input + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> subregionClipPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> subregionClipBindGroupLayout_;

  // sRGB↔linearRGB color space conversion pipeline (input + output + uniform).
  ScopedWgpuHandle<wgpu::ComputePipeline> colorSpaceConvertPipeline_;
  ScopedWgpuHandle<wgpu::BindGroupLayout> colorSpaceConvertBindGroupLayout_;

  bool verbose_ = false;
  bool warnedUnsupported_ = false;
};

}  // namespace donner::geode
