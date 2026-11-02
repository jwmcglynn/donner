#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Device.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"

namespace donner::svg {
namespace {

/// Core Animation presents BGRA8, so the device and surface are created for that format.
constexpr gpu::TextureFormat kPresentationFormat = gpu::TextureFormat::BGRA8Unorm;

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsPositive(const Vector2i& size) {
  return size.x > 0 && size.y > 0;
}

template <typename Container, typename Value>
bool Contains(const Container& container, const Value& value) {
  return std::find(container.begin(), container.end(), value) != container.end();
}

/// Whether \p caps can present BGRA8 render targets with some pacing and alpha mode.
bool SupportsPresentation(const gpu::SurfaceCapabilities& caps) {
  return Contains(caps.formats, kPresentationFormat) &&
         (caps.usages & gpu::TextureUsage::RenderAttachment) != gpu::TextureUsage::None &&
         !caps.presentModes.empty() && !caps.alphaModes.empty();
}

}  // namespace

struct GeodeSurfaceRenderer::Impl {
  std::shared_ptr<geode::GeodeGpuRoot> root;
  std::shared_ptr<geode::GeodeDevice> device;
  gpu::Surface surface;
  gpu::SurfaceConfiguration configuration;
  std::unique_ptr<RendererGeode> renderer;
  Vector2i size;

  gpu::Device& runtime() { return device->runtimeDevice(); }

  /// Opens a Metal root, the device, and a runtime surface over \p metalLayer.
  /// @return An error description, or empty on success.
  std::string open(void* metalLayer) {
    geode::GpuRootSelection selection;
    selection.label = "GeodeSurfaceRenderer";
    // Naming the backend keeps DONNER_GPU_BACKEND from redirecting or aborting selection.
    selection.backend = geode::GpuBackendKind::NativeMetal;
    root = geode::SelectGpuRoot(selection);
    if (root == nullptr) {
      return "Metal is not available on this host";
    }
    device = std::shared_ptr<geode::GeodeDevice>(
        geode::GeodeDevice::CreateOverSelectedRoot(root, kPresentationFormat));
    if (device == nullptr) {
      return "could not create a Geode device over the Metal root";
    }
    gpu::SurfaceDescriptor descriptor;
    descriptor.label = "GeodeSurfaceRenderer";
    descriptor.native.kind = gpu::NativeSurfaceKind::MetalLayer;
    descriptor.native.display = metalLayer;
    gpu::Result<gpu::Surface> created = runtime().createSurface(descriptor);
    if (created.hasError()) {
      return "could not create a surface for the layer: " + created.error().toString();
    }
    surface = std::move(created).result();
    return {};
  }

  /// Chooses BGRA8 with Fifo pacing and opaque alpha when the surface offers them.
  /// @return An error description, or empty on success.
  std::string chooseConfiguration() {
    gpu::Result<gpu::SurfaceCapabilities> capabilities = runtime().surfaceCapabilities(surface);
    if (capabilities.hasError()) {
      return "could not query surface capabilities: " + capabilities.error().toString();
    }
    const gpu::SurfaceCapabilities& caps = capabilities.result();
    if (!SupportsPresentation(caps)) {
      return "the surface cannot present BGRA8 render targets";
    }
    configuration.format = kPresentationFormat;
    configuration.usage = gpu::TextureUsage::RenderAttachment;
    configuration.presentMode = Contains(caps.presentModes, gpu::PresentMode::Fifo)
                                    ? gpu::PresentMode::Fifo
                                    : caps.presentModes.front();
    configuration.alphaMode = Contains(caps.alphaModes, gpu::SurfaceAlphaMode::Opaque)
                                  ? gpu::SurfaceAlphaMode::Opaque
                                  : caps.alphaModes.front();
    return {};
  }

  /// Applies \ref configuration with \p sizePx, keeping the old configuration on failure.
  /// @return An error description, or empty on success.
  std::string configure(Vector2i sizePx) {
    gpu::SurfaceConfiguration next = configuration;
    next.size = gpu::Extent2d{static_cast<uint32_t>(sizePx.x), static_cast<uint32_t>(sizePx.y)};
    if (gpu::Status configured = runtime().configureSurface(surface, next); configured.hasError()) {
      return "could not configure the surface: " + configured.error().toString();
    }
    configuration = next;
    size = sizePx;
    return {};
  }

  /// Maps a surface status that produced no presentable frame.
  static FrameStatus unavailable(gpu::SurfaceStatus status) {
    switch (status) {
      case gpu::SurfaceStatus::Outdated: return FrameStatus::ResizeNeeded;
      case gpu::SurfaceStatus::Timeout: return FrameStatus::Skipped;
      default: return FrameStatus::Failed;
    }
  }

  /// Draws into the acquired \p frame, then presents it. A frame whose draw lost the device is
  /// abandoned rather than presented, so the layer never shows an undrawn drawable.
  FrameStatus drawAndPresent(SVGDocument& document, const gpu::Texture& frame) {
    renderer->setTargetTexture(frame);
    renderer->draw(document);
    renderer->clearTargetTexture();
    if (device->isDeviceLost()) {
      (void)runtime().abandonCurrentTexture(surface);
      return FrameStatus::Failed;
    }
    gpu::Result<gpu::SurfaceStatus> presented = runtime().presentSurface(surface);
    if (presented.hasError()) {
      return FrameStatus::Failed;
    }
    return presented.result() == gpu::SurfaceStatus::Success ? FrameStatus::Presented
                                                             : unavailable(presented.result());
  }

  ~Impl() {
    // Retirement order: renderer, then surface, then the device and root.
    renderer.reset();
    if (surface.isValid() && device != nullptr) {
      if (gpu::Status released = runtime().destroySurface(std::move(surface));
          released.hasError()) {
        std::fprintf(stderr, "GeodeSurfaceRenderer: could not retire surface: %s\n",
                     released.error().toString().c_str());
      }
    }
    device.reset();
    root.reset();
  }
};

GeodeSurfaceRenderer::GeodeSurfaceRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GeodeSurfaceRenderer::~GeodeSurfaceRenderer() = default;

std::unique_ptr<GeodeSurfaceRenderer> GeodeSurfaceRenderer::CreateForMetalLayer(
    void* metalLayer, Vector2i sizePx, std::string* error) {
  if (metalLayer == nullptr) {
    SetError(error, "metalLayer is null");
    return nullptr;
  }
  if (!IsPositive(sizePx)) {
    SetError(error, "drawable size must be positive");
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  std::string failure = impl->open(metalLayer);
  if (failure.empty()) {
    failure = impl->chooseConfiguration();
  }
  if (failure.empty()) {
    failure = impl->configure(sizePx);
  }
  if (!failure.empty()) {
    SetError(error, std::move(failure));
    return nullptr;
  }

  impl->renderer = std::make_unique<RendererGeode>(impl->device);
  return std::unique_ptr<GeodeSurfaceRenderer>(new GeodeSurfaceRenderer(std::move(impl)));
}

bool GeodeSurfaceRenderer::resize(Vector2i sizePx, std::string* error) {
  if (!IsPositive(sizePx)) {
    SetError(error, "drawable size must be positive");
    return false;
  }
  if (sizePx == impl_->size) {
    return true;
  }
  std::string failure = impl_->configure(sizePx);
  if (!failure.empty()) {
    SetError(error, std::move(failure));
    return false;
  }
  return true;
}

Vector2i GeodeSurfaceRenderer::size() const {
  return impl_->size;
}

bool GeodeSurfaceRenderer::isDeviceLost() const {
  return impl_->device->isDeviceLost();
}

GeodeSurfaceRenderer::FrameStatus GeodeSurfaceRenderer::draw(SVGDocument& document) {
  if (isDeviceLost()) {
    return FrameStatus::Failed;
  }
  gpu::Result<gpu::SurfaceTexture> acquired =
      impl_->runtime().acquireCurrentTexture(impl_->surface);
  if (acquired.hasError()) {
    return FrameStatus::Failed;
  }
  gpu::SurfaceTexture frame = std::move(acquired).result();
  if (frame.status != gpu::SurfaceStatus::Success || !frame.texture.isValid()) {
    if (frame.texture.isValid()) {
      (void)impl_->runtime().abandonCurrentTexture(impl_->surface);
    }
    return Impl::unavailable(frame.status);
  }
  return impl_->drawAndPresent(document, frame.texture);
}

std::ostream& operator<<(std::ostream& os, GeodeSurfaceRenderer::FrameStatus status) {
  switch (status) {
    case GeodeSurfaceRenderer::FrameStatus::Presented: return os << "Presented";
    case GeodeSurfaceRenderer::FrameStatus::Skipped: return os << "Skipped";
    case GeodeSurfaceRenderer::FrameStatus::ResizeNeeded: return os << "ResizeNeeded";
    case GeodeSurfaceRenderer::FrameStatus::Failed: return os << "Failed";
  }
  return os << "FrameStatus(" << static_cast<int>(status) << ")";
}

}  // namespace donner::svg
