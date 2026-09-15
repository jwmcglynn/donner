#pragma once
/// @file
/// \c donner::gpu::metal::MetalSurface - presentation to a Core Animation Metal layer.
///
/// Objective-C++ rather than pure C++, so it is included by the Metal backend's implementation
/// files and not by the `MetalDevice` header that C++ tests include.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

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
 * presented or released. Exactly one frame is held at a time, which the runtime enforces before
 * a second acquisition reaches the layer; the check in \ref acquire is a backstop for a caller
 * that reached this type directly. Holding the drawable rather than only its texture is what
 * makes \ref abandon possible: a frame the caller decided not to show is handed straight back
 * to the layer instead of being presented to the screen.
 *
 * Nothing here owns a window. The layer belongs to whoever created the surface, and outlives it;
 * this type only configures the layer, takes drawables from it, and puts them back.
 *
 * Threading: used from the one thread that owns the device, like the rest of the runtime. Core
 * Animation layers are not thread safe, and a layer property set outside an explicit transaction
 * is only published when the setting thread's run loop commits one, so \ref configure wraps its
 * writes in a transaction of its own rather than relying on the caller having a run loop.
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

  /// Destructor; hands back an outstanding drawable and releases this surface's retain on the
  /// layer, which the embedder still owns and which outlives the release.
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
   * Hands the held frame to the layer and releases it.
   *
   * Presents immediately, so the frame's own work must already have finished: a drawable handed
   * over while the GPU is still writing it is shown half drawn. Ordering that is the caller's
   * job - \ref donner::gpu::metal::MetalDevice waits for the frame's submission before calling
   * this - because the layer's own scheduling gives no such guarantee. Scheduling the present on
   * a command buffer would not help: `presentDrawable:` fires when that buffer is *scheduled*,
   * not when it completes, so a present-only buffer committed afterwards can still run first.
   */
  Result<SurfaceStatus> present();

  /// Hands the held frame back to the layer without showing it. Does nothing when none is held.
  void abandon();

private:
  /// Constructs a surface bound to \p layer. @param device Metal device. @param layer Layer.
  MetalSurface(id<MTLDevice> device, CAMetalLayer* layer);

  /// Whether the layer's own geometry has moved away from the configured drawable extent, which
  /// is what makes a configuration outdated.
  bool hasOutgrownItsConfiguration() const;

  id<MTLDevice> device_ = nil;                         //!< Device the layer renders with.
  CAMetalLayer* layer_ = nil;                          //!< Layer presented to; retained, not owned.
  id<CAMetalDrawable> drawable_ = nil;                 //!< Frame currently held, or nil.
  std::optional<SurfaceConfiguration> configuration_;  //!< Applied configuration, if any.
};

}  // namespace donner::gpu::metal
