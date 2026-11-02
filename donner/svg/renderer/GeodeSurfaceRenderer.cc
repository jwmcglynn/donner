#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Device.h"
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

}  // namespace

struct GeodeSurfaceRenderer::Impl {
  std::shared_ptr<geode::GeodeGpuRoot> root;
  std::shared_ptr<geode::GeodeDevice> device;
  gpu::Surface surface;
  gpu::SurfaceConfiguration configuration;
  std::unique_ptr<RendererGeode> renderer;
  Vector2i size;

  gpu::Device& runtime() { return device->runtimeDevice(); }

  /// Applies \ref configuration with \p sizePx. Returns false and keeps the old configuration when
  /// the surface rejects it.
  bool configure(Vector2i sizePx) {
    gpu::SurfaceConfiguration next = configuration;
    next.size = gpu::Extent2d{static_cast<uint32_t>(sizePx.x), static_cast<uint32_t>(sizePx.y)};
    if (runtime().configureSurface(surface, next).hasError()) {
      return false;
    }
    configuration = next;
    size = sizePx;
    return true;
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
  geode::GpuRootSelection selection;
  selection.label = "GeodeSurfaceRenderer";
  impl->root = geode::SelectGpuRoot(selection);
  if (impl->root == nullptr) {
    SetError(error, "no GPU root is available on this host");
    return nullptr;
  }
  impl->device = std::shared_ptr<geode::GeodeDevice>(
      geode::GeodeDevice::CreateOverSelectedRoot(impl->root, kPresentationFormat));
  if (impl->device == nullptr) {
    SetError(error, "could not create a Geode device over the selected GPU root");
    return nullptr;
  }

  gpu::SurfaceDescriptor descriptor;
  descriptor.label = "GeodeSurfaceRenderer";
  descriptor.native.kind = gpu::NativeSurfaceKind::MetalLayer;
  descriptor.native.display = metalLayer;
  gpu::Result<gpu::Surface> created = impl->runtime().createSurface(descriptor);
  if (created.hasError()) {
    SetError(error,
             "could not create a surface for the Metal layer: " + created.error().toString());
    return nullptr;
  }
  impl->surface = std::move(created).result();

  gpu::Result<gpu::SurfaceCapabilities> capabilities =
      impl->runtime().surfaceCapabilities(impl->surface);
  if (capabilities.hasError() || !Contains(capabilities.result().formats, kPresentationFormat) ||
      (capabilities.result().usages & gpu::TextureUsage::RenderAttachment) ==
          gpu::TextureUsage::None ||
      capabilities.result().presentModes.empty() || capabilities.result().alphaModes.empty()) {
    SetError(error, "the surface cannot present BGRA8 render targets");
    return nullptr;
  }
  const gpu::SurfaceCapabilities& caps = capabilities.result();
  impl->configuration.format = kPresentationFormat;
  impl->configuration.usage = gpu::TextureUsage::RenderAttachment;
  impl->configuration.presentMode = Contains(caps.presentModes, gpu::PresentMode::Fifo)
                                        ? gpu::PresentMode::Fifo
                                        : caps.presentModes.front();
  impl->configuration.alphaMode = Contains(caps.alphaModes, gpu::SurfaceAlphaMode::Opaque)
                                      ? gpu::SurfaceAlphaMode::Opaque
                                      : caps.alphaModes.front();
  if (!impl->configure(sizePx)) {
    SetError(error, "could not configure the surface");
    return nullptr;
  }

  impl->renderer = std::make_unique<RendererGeode>(impl->device);
  return std::unique_ptr<GeodeSurfaceRenderer>(new GeodeSurfaceRenderer(std::move(impl)));
}

bool GeodeSurfaceRenderer::resize(Vector2i sizePx) {
  if (!IsPositive(sizePx)) {
    return false;
  }
  if (sizePx == impl_->size) {
    return true;
  }
  return impl_->configure(sizePx);
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
  gpu::Device& device = impl_->runtime();
  gpu::Result<gpu::SurfaceTexture> acquired = device.acquireCurrentTexture(impl_->surface);
  if (acquired.hasError()) {
    return FrameStatus::Failed;
  }
  gpu::SurfaceTexture frame = std::move(acquired).result();
  if (frame.status != gpu::SurfaceStatus::Success || !frame.texture.isValid()) {
    if (frame.texture.isValid()) {
      (void)device.abandonCurrentTexture(impl_->surface);
    }
    switch (frame.status) {
      case gpu::SurfaceStatus::Outdated:
        return impl_->configure(impl_->size) ? FrameStatus::Skipped : FrameStatus::Failed;
      case gpu::SurfaceStatus::Timeout: return FrameStatus::Skipped;
      default: return FrameStatus::Failed;
    }
  }

  impl_->renderer->setTargetTexture(frame.texture);
  impl_->renderer->draw(document);
  impl_->renderer->clearTargetTexture();

  gpu::Result<gpu::SurfaceStatus> presented = device.presentSurface(impl_->surface);
  if (presented.hasError()) {
    return FrameStatus::Failed;
  }
  switch (presented.result()) {
    case gpu::SurfaceStatus::Success: return FrameStatus::Presented;
    case gpu::SurfaceStatus::Outdated:
      return impl_->configure(impl_->size) ? FrameStatus::Skipped : FrameStatus::Failed;
    case gpu::SurfaceStatus::Timeout: return FrameStatus::Skipped;
    default: return FrameStatus::Failed;
  }
}

}  // namespace donner::svg
