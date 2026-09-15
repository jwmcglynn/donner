#pragma once
/// @file
/// \c donner::gpu::metal::MetalSurface - presentation to a Core Animation Metal layer.
///
/// Objective-C++ rather than pure C++, so it is included by the Metal backend's implementation
/// files and not by the `MetalDevice` header that C++ tests include.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <memory>
#include <optional>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"

namespace donner::gpu::metal {

/**
 * Presentation state of one surface: the Core Animation layer it presents to, the configuration
 * it presents under, and the drawable the current frame is holding.
 *
 * A layer hands out a small, fixed number of drawables and takes one back only when it is
 * presented or released, so exactly one frame is held at a time and \ref acquire refuses while
 * one is outstanding. Holding the drawable rather than only its texture is what makes
 * \ref abandon possible: a frame the caller decided not to show is handed straight back to the
 * layer instead of being presented to the screen.
 *
 * Nothing here owns a window. The layer belongs to whoever created the surface, and outlives it;
 * this type only configures the layer, takes drawables from it, and puts them back.
 */
class MetalSurface {
public:
  /**
   * Binds a surface to the Core Animation layer \p descriptor names.
   *
   * @param device Metal device the layer must render with.
   * @param descriptor Label and platform object; the platform object must be a `CAMetalLayer`.
   */
  static Result<std::unique_ptr<MetalSurface>> Create(id<MTLDevice> device,
                                                      const SurfaceDescriptor& descriptor);

  /// Destructor; hands back an outstanding drawable and releases the layer reference.
  ~MetalSurface();

  MetalSurface(const MetalSurface&) = delete;
  MetalSurface& operator=(const MetalSurface&) = delete;

  /// What this layer can present, for choosing a configuration. Every value reported here is
  /// accepted by \ref configure, and every value absent from it is refused.
  SurfaceCapabilities capabilities() const;

  /**
   * Applies \p configuration to the layer, replacing any previous one.
   *
   * Values the layer cannot present are refused rather than substituted: Core Animation silently
   * accepts a pixel format it will not draw, so a configuration outside \ref capabilities fails
   * closed here instead of producing a surface nobody asked for.
   *
   * @param configuration Format, usage, extent, pacing and alpha compositing.
   */
  Status configure(const SurfaceConfiguration& configuration);

  /**
   * Takes the next frame's drawable from the layer.
   *
   * The returned status distinguishes a frame that came back under a configuration the layer has
   * outgrown (\ref SurfaceStatus::Outdated, which still carries a usable texture) from one that
   * did not come back at all. On success \ref currentTexture names the frame until it is
   * presented or abandoned.
   */
  Result<SurfaceStatus> acquire();

  /// Texture of the frame currently held, or nil when none is.
  id<MTLTexture> currentTexture() const;

  /**
   * Schedules the held frame for presentation and releases it.
   *
   * Metal executes the command buffers of one queue in the order they were committed, and a
   * drawable scheduled on a command buffer appears when that buffer completes, so a
   * present-only command buffer committed after the frame's rendering shows the frame only once
   * that rendering has finished.
   *
   * @param commandQueue Queue the frame's work was submitted on.
   */
  Result<SurfaceStatus> present(id<MTLCommandQueue> commandQueue);

  /// Hands the held frame back to the layer without showing it. Does nothing when none is held.
  void abandon();

private:
  /// Constructs a surface bound to \p layer. @param device Metal device. @param layer Layer.
  MetalSurface(id<MTLDevice> device, CAMetalLayer* layer);

  /// Whether the layer's own geometry has moved away from the configured drawable extent, which
  /// is what makes a configuration outdated.
  bool hasOutgrownItsConfiguration() const;

  id<MTLDevice> device_ = nil;          //!< Device the layer renders with.
  CAMetalLayer* layer_ = nil;           //!< Layer this surface presents to; not owned.
  id<CAMetalDrawable> drawable_ = nil;  //!< Frame currently held, or nil.
  std::optional<SurfaceConfiguration> configuration_;  //!< Applied configuration, if any.
};

}  // namespace donner::gpu::metal
