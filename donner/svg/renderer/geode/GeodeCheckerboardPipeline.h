#pragma once
/// @file
/// Render pipeline and draw pass for the transparency checkerboard (fullscreen triangle).

#include <array>
#include <cstdint>
#include <optional>
#include <webgpu/webgpu.hpp>

#include "donner/base/Vector2.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;

/**
 * Appearance of the transparency checkerboard drawn behind see-through
 * document pixels.
 *
 * Shared by every presentation path so the same document lands on identical
 * cells. The size is in *logical* pixels, so the pattern is anchored to device
 * pixels and never scales with document zoom.
 *
 * @{
 */
inline constexpr double kTransparencyCheckerboardCellLogicalPx = 16.0;
/// RGBA in the 0-1 range for odd cells.
inline constexpr std::array<float, 4> kTransparencyCheckerboardDarkColor = {
    40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f};
/// RGBA in the 0-1 range for even cells.
inline constexpr std::array<float, 4> kTransparencyCheckerboardLightColor = {
    60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f};
/// @}

/// Device-pixel sub-rectangle of the target a checkerboard pass is confined to.
struct CheckerboardScissorPx {
  std::uint32_t x = 0;       //!< Left edge in device pixels.
  std::uint32_t y = 0;       //!< Top edge in device pixels.
  std::uint32_t width = 0;   //!< Width in device pixels.
  std::uint32_t height = 0;  //!< Height in device pixels.
};

/**
 * Placement and appearance of a transparency-checkerboard pass over a render target.
 *
 * @see GeodeCheckerboardPass::draw
 */
struct CheckerboardUnderlayParams {
  /// Device pixels per logical pixel. Cells are \ref
  /// kTransparencyCheckerboardCellLogicalPx logical pixels across regardless of
  /// document zoom.
  double devicePixelRatio = 1.0;
  /**
   * Device-pixel offset from the pattern's anchor origin to the target's
   * top-left corner.
   *
   * Zero anchors the pattern at the target's own origin, which is what a window
   * framebuffer wants: the pattern is fixed to the window and the document
   * slides over it. A target that is itself positioned somewhere on screen
   * passes its top-left screen position in device pixels, which reproduces the
   * window-anchored pattern that would have been drawn in the same place.
   * Without it the checkerboard would slide with the document instead of
   * staying put behind it.
   */
  Vector2d originOffsetPx;
  /// Cell size in logical pixels.
  double cellSizeLogicalPx = kTransparencyCheckerboardCellLogicalPx;
  /// RGBA in the 0-1 range for odd cells.
  std::array<float, 4> darkColor = kTransparencyCheckerboardDarkColor;
  /// RGBA in the 0-1 range for even cells.
  std::array<float, 4> lightColor = kTransparencyCheckerboardLightColor;
  /// Region of the target to draw into; unset covers the whole target.
  std::optional<CheckerboardScissorPx> scissorPx;
};

/**
 * Caches the compiled `wgpu::RenderPipeline` that draws the transparency
 * checkerboard behind see-through document regions, plus its bind group layout.
 *
 * Owned by `GeodeDevice` (lazily, via `GeodeDevice::checkerboardPipeline()` and
 * `GeodeDevice::checkerboardUnderlayPipeline()`) so every consumer sharing the
 * device reuses one compiled pipeline per blend mode - wgpu-native retains every
 * pipeline ever constructed, so per-consumer construction leaks (issue #575).
 *
 * Bind group layout:
 * - binding 0: uniform buffer (\ref Uniforms)
 *
 * The pipeline takes no vertex buffer - the shader emits a fullscreen triangle
 * from `@builtin(vertex_index)`. A draw call is `pass.draw(3, 1, 0, 0)`. The
 * caller owns its uniform buffer and bind group; \ref Uniforms pins the layout
 * contract between the two.
 */
class GeodeCheckerboardPipeline {
public:
  /// How the emitted checkerboard combines with whatever is already in the target.
  enum class BlendMode {
    /**
     * Overwrite the target. Used when the checkerboard is drawn first, before
     * any document pixels land on top of it, which is what the editor's window
     * framebuffer does.
     */
    Replace,
    /**
     * Composite the checkerboard *under* the target's existing premultiplied
     * content: `result = dst + checker * (1 - dst.alpha)`. For a target that
     * already holds composed document pixels, where the checkerboard has to
     * fill whatever alpha they left behind instead of going in first.
     */
    DestinationOver,
  };

  /// Uniform block consumed by the checkerboard shader.
  struct Uniforms {
    float targetSize[2];      //!< Render-target size in device pixels.
    float devicePixelRatio;   //!< Device pixels per logical pixel.
    float checkerSize;        //!< Checker cell size in logical pixels.
    float darkColor[4];       //!< RGBA for odd cells.
    float lightColor[4];      //!< RGBA for even cells.
    float originOffsetPx[2];  //!< Device-pixel offset of the target's top-left from the anchor.
    float padding[2];         //!< WGSL uniform struct tail padding; must be zero.
  };
  static_assert(sizeof(Uniforms) == 64);

  /**
   * Create the checkerboard pipeline for the given device and target format.
   *
   * @param device The WebGPU device.
   * @param colorFormat The pixel format of the render target this pipeline
   *   will draw into. Must match the target texture's format at draw time.
   * @param blendMode How the emitted checkerboard combines with the target.
   */
  GeodeCheckerboardPipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat,
                            BlendMode blendMode = BlendMode::Replace);

  ~GeodeCheckerboardPipeline() = default;
  GeodeCheckerboardPipeline(const GeodeCheckerboardPipeline&) = delete;
  GeodeCheckerboardPipeline& operator=(const GeodeCheckerboardPipeline&) = delete;
  /// Move constructor.
  GeodeCheckerboardPipeline(GeodeCheckerboardPipeline&&) noexcept = default;
  /// Move assignment operator.
  GeodeCheckerboardPipeline& operator=(GeodeCheckerboardPipeline&&) noexcept = default;

  /// True when the shader, bind group layout, and pipeline all compiled.
  bool valid() const { return static_cast<bool>(pipeline_) && static_cast<bool>(bindGroupLayout_); }

  /// The compiled render pipeline.
  const wgpu::RenderPipeline& pipeline() const { return pipeline_; }

  /// Bind group layout used by the pipeline.
  const wgpu::BindGroupLayout& bindGroupLayout() const { return bindGroupLayout_; }

private:
  wgpu::BindGroupLayout bindGroupLayout_;
  wgpu::RenderPipeline pipeline_;
};

/**
 * Draws the transparency checkerboard over a Geode-owned render target.
 *
 * This is the single checkerboard draw in the project. It is deliberately
 * Geode-only: the checkerboard exists so see-through document pixels reach a
 * GPU presentation surface with something behind them, and there is no software
 * presentation path that needs one. Putting it here instead of on
 * `RendererInterface` keeps a GPU-only concern out of the backend-neutral
 * interface every renderer has to implement.
 *
 * One instance per consumer: the compiled pipeline and its bind group layout
 * are shared device-wide (issue #575), but the uniform buffer and bind group
 * are per-consumer and are built on the first draw, so a consumer that never
 * draws a checkerboard never allocates them.
 */
class GeodeCheckerboardPass {
public:
  GeodeCheckerboardPass() = default;

  GeodeCheckerboardPass(const GeodeCheckerboardPass&) = delete;
  GeodeCheckerboardPass& operator=(const GeodeCheckerboardPass&) = delete;
  /// Move constructor.
  GeodeCheckerboardPass(GeodeCheckerboardPass&&) noexcept = default;
  /// Move assignment operator.
  GeodeCheckerboardPass& operator=(GeodeCheckerboardPass&&) noexcept = default;

  /**
   * Submit one checkerboard pass over @p target.
   *
   * With \ref GeodeCheckerboardPipeline::BlendMode::Replace the pass overwrites
   * the scissored region, so it must run before any document pixels land in the
   * target. With \ref GeodeCheckerboardPipeline::BlendMode::DestinationOver it
   * goes underneath premultiplied content already in the target: fully-opaque
   * pixels are unchanged, fully-transparent pixels become checkerboard, and
   * partial alpha blends as `destination-over` does.
   *
   * @param device Device that owns @p target and the shared pipeline.
   * @param target Render target. Needs `RenderAttachment` usage and the
   *   device's texture format.
   * @param targetSizePx Target size in device pixels.
   * @param params Checkerboard placement and appearance.
   * @param blendMode How the checkerboard combines with the target's contents.
   * @return True when the pass was submitted. Degenerate parameters and a
   *   failed pipeline or resource creation return false and leave @p target
   *   untouched.
   */
  [[nodiscard]] bool draw(GeodeDevice& device, const wgpu::Texture& target, Vector2i targetSizePx,
                          const CheckerboardUnderlayParams& params,
                          GeodeCheckerboardPipeline::BlendMode blendMode);

private:
  /// Build the uniform buffer and bind group, or reuse the ones already built
  /// for @p blendMode.
  [[nodiscard]] bool ensureResources(GeodeDevice& device, const GeodeCheckerboardPipeline& pipeline,
                                     GeodeCheckerboardPipeline::BlendMode blendMode);

  ScopedWgpuHandle<wgpu::Buffer> uniformBuffer_;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup_;
  /// Blend mode \ref bindGroup_ was built against; a different one rebuilds it
  /// because each blend mode has its own pipeline and bind group layout.
  std::optional<GeodeCheckerboardPipeline::BlendMode> bindGroupBlendMode_;
};

}  // namespace donner::geode
