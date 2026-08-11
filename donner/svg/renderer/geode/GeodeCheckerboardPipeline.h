#pragma once
/// @file
/// Render pipeline for the framebuffer checkerboard underlay (fullscreen triangle).

#include <webgpu/webgpu.hpp>

namespace donner::geode {

/**
 * Caches the compiled `wgpu::RenderPipeline` for the checkerboard underlay the
 * editor's direct framebuffer presentation draws behind transparent document
 * regions, plus its bind group layout.
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
     * any document pixels land on top of it (the desktop framebuffer underlay).
     */
    Replace,
    /**
     * Composite the checkerboard *under* the target's existing premultiplied
     * content: `result = dst + checker * (1 - dst.alpha)`. Used when the
     * document pixels are already in the target and the checkerboard has to
     * fill whatever alpha they left behind (the browser worker surface, which
     * receives an already-composed full-canvas texture).
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

}  // namespace donner::geode
