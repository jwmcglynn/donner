/// @file
/// Metal presentation: Core Animation layer configuration, drawable acquisition and present.

#include "donner/gpu/metal/MetalSurface.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>
#include <string>
#include <utility>

namespace donner::gpu::metal {

namespace {

/// Names an enum value through its stream operator, so a refusal says which value was refused.
/// @param value Value to name.
template <typename T>
std::string Describe(T value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

/// Metal pixel format for a presentation format, or nothing when the format is not one.
/// @param format Runtime texture format.
std::optional<MTLPixelFormat> PresentationPixelFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::R8Unorm:
    case TextureFormat::RGBA32Float: return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

Result<std::unique_ptr<MetalSurface>> MetalSurface::Create(id<MTLDevice> device,
                                                           const SurfaceDescriptor& descriptor) {
  if (device == nil) {
    return GpuError{GpuErrorType::InvalidState, "createSurface: the Metal device is gone"};
  }
  if (descriptor.native.kind != NativeSurfaceKind::MetalLayer) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("createSurface: the Metal backend presents to a Core Animation "
                                "Metal layer, not to {}",
                                Describe(descriptor.native.kind))};
  }

  // The embedding boundary is trusted to hand over an Objective-C object: sending isKindOfClass:
  // to a pointer that is not one is undefined, so this check catches an object of the wrong
  // class, not an arbitrary pointer. Nothing below is a defence against a hostile embedder.
  NSObject* object = (__bridge NSObject*)descriptor.native.display;
  if (![object isKindOfClass:[CAMetalLayer class]]) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("createSurface: the platform object of '{}' is not a "
                                "CAMetalLayer",
                                std::string_view(descriptor.label))};
  }

  CAMetalLayer* layer = static_cast<CAMetalLayer*>(object);
  layer.device = device;
  return std::unique_ptr<MetalSurface>(new MetalSurface(device, layer));
}

MetalSurface::MetalSurface(id<MTLDevice> device, CAMetalLayer* layer)
    : device_(device), layer_(layer) {}

MetalSurface::~MetalSurface() {
  abandon();
}

SurfaceCapabilities MetalSurface::capabilities() const {
  // Core Animation displays a Metal layer in a fixed set of pixel formats; of the formats this
  // runtime defines, only 8-bit BGRA is one of them. Mailbox pacing is absent because a layer
  // queues its drawables and reclaims them in order, and Inherit alpha compositing is absent
  // because a layer is either opaque or premultiplied with nothing to inherit from.
  return SurfaceCapabilities{{TextureFormat::BGRA8Unorm},
                             TextureUsage::RenderAttachment | TextureUsage::Sampled |
                                 TextureUsage::CopySrc | TextureUsage::CopyDst,
                             {PresentMode::Fifo, PresentMode::Immediate},
                             {SurfaceAlphaMode::Opaque, SurfaceAlphaMode::Premultiplied}};
}

Status MetalSurface::configure(const SurfaceConfiguration& configuration) {
  // Checked against what this surface reports rather than against a second list, so a
  // configuration is accepted exactly when capabilities() said it would be.
  const SurfaceCapabilities supported = capabilities();
  if (std::ranges::find(supported.formats, configuration.format) == supported.formats.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: a Metal layer does not present {}",
                                Describe(configuration.format))};
  }
  if (!HasAllFlags(supported.usages, configuration.usage)) {
    return GpuError{GpuErrorType::Unsupported,
                    "configureSurface: a Metal layer's drawables do not carry every requested "
                    "usage"};
  }
  if (std::ranges::find(supported.presentModes, configuration.presentMode) ==
      supported.presentModes.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: a Metal layer does not pace frames as {}",
                                Describe(configuration.presentMode))};
  }
  if (std::ranges::find(supported.alphaModes, configuration.alphaMode) ==
      supported.alphaModes.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: a Metal layer does not composite alpha as {}",
                                Describe(configuration.alphaMode))};
  }

  const std::optional<MTLPixelFormat> pixelFormat = PresentationPixelFormat(configuration.format);
  if (!pixelFormat.has_value()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: no Metal pixel format presents {}",
                                Describe(configuration.format))};
  }

  // A frame taken under the configuration being replaced is sized and formatted for a layer that
  // will not exist a line from now, so it goes back before the layer changes shape.
  abandon();

  // Setting a layer property opens an implicit transaction that is committed by the run loop of
  // the thread that opened it. A renderer thread has no run loop, so an implicit transaction is
  // never committed and the layer keeps its old drawable extent; an explicit transaction is what
  // makes a reconfiguration take effect wherever this runs.
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  layer_.device = device_;
  layer_.pixelFormat = *pixelFormat;
  layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(configuration.size.width),
                                   static_cast<CGFloat>(configuration.size.height));
  // A drawable used only as a render attachment can stay in the form Core Animation displays
  // directly; sampling from it, or copying either way, needs the general form instead.
  layer_.framebufferOnly = configuration.usage == TextureUsage::RenderAttachment;
  layer_.opaque = configuration.alphaMode == SurfaceAlphaMode::Opaque;
  layer_.displaySyncEnabled = configuration.presentMode != PresentMode::Immediate;
  [CATransaction commit];

  configuration_ = configuration;
  return OkStatus();
}

Result<SurfaceStatus> MetalSurface::acquire() {
  if (!configuration_.has_value()) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the Metal layer has not been configured"};
  }
  if (drawable_ != nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the layer is still holding the frame it handed out"};
  }
  if (layer_.device != device_) {
    // The layer was rebound to another device, or to none, so the frames it hands out are no
    // longer ours to draw with. A new surface over a fresh layer is the only way back.
    return SurfaceStatus::Lost;
  }

  const SurfaceStatus status =
      hasOutgrownItsConfiguration() ? SurfaceStatus::Outdated : SurfaceStatus::Success;

  id<CAMetalDrawable> drawable = [layer_ nextDrawable];
  if (drawable == nil) {
    // A layer hands out a small fixed number of drawables and waits for one to come back rather
    // than blocking a frame loop indefinitely; the caller retries on the next frame.
    return SurfaceStatus::Timeout;
  }
  if (drawable.texture == nil) {
    // A frame with nothing to draw into is not a frame that has not arrived yet, and retrying
    // would not produce one.
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the layer handed out a frame with no texture"};
  }

  // The runtime describes this frame to every later range check using the configured extent, so
  // a layer whose drawable extent moved behind the configuration must not yield a frame that is
  // a different size from the one the runtime is about to claim it is.
  const NSUInteger width = drawable.texture.width;
  const NSUInteger height = drawable.texture.height;
  if (width != configuration_->size.width || height != configuration_->size.height) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("acquireCurrentTexture: the layer handed out a {}x{} frame under a {}x{} "
                    "configuration",
                    static_cast<uint64_t>(width), static_cast<uint64_t>(height),
                    configuration_->size.width, configuration_->size.height)};
  }

  drawable_ = drawable;
  return status;
}

id<MTLTexture> MetalSurface::currentTexture() const {
  return drawable_ == nil ? nil : drawable_.texture;
}

Result<SurfaceStatus> MetalSurface::present() {
  if (drawable_ == nil) {
    return GpuError{GpuErrorType::InvalidState, "presentSurface: no frame is being held"};
  }

  id<CAMetalDrawable> drawable = drawable_;
  // The layer owns the frame from here, matching the runtime's rule that presenting ends the
  // frame whatever it reports.
  drawable_ = nil;
  [drawable present];

  return layer_.device == device_ ? SurfaceStatus::Success : SurfaceStatus::Lost;
}

void MetalSurface::abandon() {
  // Releasing the drawable is what returns it: a layer reclaims a frame that was never shown as
  // soon as nothing holds it. That makes the release have to be the last one, so no caller may
  // hold this surface's frames alive in an autorelease pool it has not drained; the runtime
  // hands the drawable to nobody, which is what keeps that true here.
  drawable_ = nil;
}

bool MetalSurface::hasOutgrownItsConfiguration() const {
  const CGSize bounds = layer_.bounds.size;
  if (bounds.width <= 0.0 || bounds.height <= 0.0) {
    // An empty bounds rectangle means the layer is not laid out in a window: its drawable extent
    // is the whole of its geometry, so there is nothing for the configuration to have drifted
    // from.
    return false;
  }

  const CGFloat scale = layer_.contentsScale;
  const double width = std::round(static_cast<double>(bounds.width) * static_cast<double>(scale));
  const double height = std::round(static_cast<double>(bounds.height) * static_cast<double>(scale));
  return width != static_cast<double>(configuration_->size.width) ||
         height != static_cast<double>(configuration_->size.height);
}

}  // namespace donner::gpu::metal
