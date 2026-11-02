#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Device.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#if defined(DONNER_GEODE_BROWSER_BACKEND)
#include "donner/svg/renderer/geode/GeodeBrowserRoot.h"
#else
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#endif

namespace donner::svg {
namespace {

/// Core Animation presents BGRA8, and every browser canvas can be configured with it, so the device
/// is first created for BGRA8. A canvas whose preferred format differs reopens with that format.
constexpr gpu::TextureFormat kDefaultFormat = gpu::TextureFormat::BGRA8Unorm;

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsPositive(const Vector2i& size) {
  return size.x > 0 && size.y > 0;
}

/// Describes why \p size cannot be a frame target, or returns empty when it can. The frame
/// target must fit the renderer's surface budget, or the renderer would skip drawing it.
std::string CheckFrameSize(const Vector2i& size) {
  if (!IsPositive(size)) {
    return "drawable size must be positive";
  }
  constexpr std::uint64_t kBytesPerPixel = 4;
  const std::uint64_t bytes =
      static_cast<std::uint64_t>(size.x) * static_cast<std::uint64_t>(size.y) * kBytesPerPixel;
  if (bytes > RendererSurfaceBudget::kMaximumBytes) {
    return "drawable size exceeds the renderer's surface budget";
  }
  return {};
}

template <typename Container, typename Value>
bool Contains(const Container& container, const Value& value) {
  return std::find(container.begin(), container.end(), value) != container.end();
}

/// Whether \p caps can present render targets with some pacing and alpha mode.
bool CanPresent(const gpu::SurfaceCapabilities& caps) {
  return !caps.formats.empty() &&
         (caps.usages & gpu::TextureUsage::RenderAttachment) != gpu::TextureUsage::None &&
         !caps.presentModes.empty() && !caps.alphaModes.empty();
}

}  // namespace

/// What one factory asks \ref GeodeSurfaceRenderer::Impl::open for.
struct GeodeSurfaceRenderer::SurfaceRequest {
  geode::GpuBackendKind backend;    //!< Backend the platform object belongs to.
  gpu::NativeSurfaceHandle native;  //!< The host's platform object.
};

struct GeodeSurfaceRenderer::Impl {
  std::shared_ptr<geode::GeodeGpuRoot> root;
  std::shared_ptr<geode::GeodeDevice> device;
  gpu::Surface surface;
  gpu::SurfaceConfiguration configuration;
  std::unique_ptr<RendererGeode> renderer;
  Vector2i size;
  gpu::TextureFormat format = kDefaultFormat;

  gpu::Device& runtime() { return device->runtimeDevice(); }

  /// Opens the requested backend's root, a device for \ref format, and a runtime surface.
  /// @return An error description, or empty on success.
  std::string open(const SurfaceRequest& request) {
    geode::GpuRootSelection selection;
    selection.label = "GeodeSurfaceRenderer";
    // Naming the backend keeps DONNER_GPU_BACKEND from redirecting or aborting selection.
    selection.backend = request.backend;
    root = geode::SelectGpuRoot(selection);
    if (root == nullptr) {
      return "no GPU is available for this surface";
    }
    return openDeviceAndSurface(request.native);
  }

  /// Opens a device for \ref format over \ref root and a runtime surface for \p native.
  /// @return An error description, or empty on success.
  std::string openDeviceAndSurface(const gpu::NativeSurfaceHandle& native) {
    device = std::shared_ptr<geode::GeodeDevice>(
        geode::GeodeDevice::CreateOverSelectedRoot(root, format));
    if (device == nullptr) {
      return "could not create a Geode device over the GPU root";
    }
    gpu::SurfaceDescriptor descriptor;
    descriptor.label = "GeodeSurfaceRenderer";
    descriptor.native = native;
    gpu::Result<gpu::Surface> created = runtime().createSurface(descriptor);
    if (created.hasError()) {
      return "could not create a surface: " + created.error().toString();
    }
    surface = std::move(created).result();
    return {};
  }

  /// Releases the surface and device, keeping the root, so a device can reopen in another format.
  void closeDeviceAndSurface() {
    if (surface.isValid()) {
      (void)runtime().destroySurface(std::move(surface));
    }
    device.reset();
  }

  /// Queries what the surface supports.
  /// @return An error description, or empty on success.
  std::string capabilities(gpu::SurfaceCapabilities& caps) {
    gpu::Result<gpu::SurfaceCapabilities> queried = runtime().surfaceCapabilities(surface);
    if (queried.hasError()) {
      return "could not query surface capabilities: " + queried.error().toString();
    }
    caps = queried.result();
    return CanPresent(caps) ? std::string() : "the surface cannot present render targets";
  }

  /// Ensures the device format is one the surface accepts, reopening the device and surface for
  /// the surface's own format when it does not offer BGRA8 (a browser canvas whose preferred
  /// format is RGBA8).
  /// @return An error description, or empty on success.
  std::string matchSurfaceFormat(const gpu::NativeSurfaceHandle& native,
                                 gpu::SurfaceCapabilities& caps) {
    if (Contains(caps.formats, format)) {
      return {};
    }
    format = caps.formats.front();
    closeDeviceAndSurface();
    std::string failure = openDeviceAndSurface(native);
    if (failure.empty()) {
      failure = capabilities(caps);
    }
    if (failure.empty() && !Contains(caps.formats, format)) {
      failure = "the surface offers no format the device can render";
    }
    return failure;
  }

  /// Chooses the surface format, Fifo pacing and opaque alpha when the surface offers them.
  /// @return An error description, or empty on success.
  std::string chooseConfiguration(const gpu::NativeSurfaceHandle& native) {
    gpu::SurfaceCapabilities caps;
    std::string failure = capabilities(caps);
    if (failure.empty()) {
      failure = matchSurfaceFormat(native, caps);
    }
    if (!failure.empty()) {
      return failure;
    }
    configuration.format = format;
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

std::unique_ptr<GeodeSurfaceRenderer> GeodeSurfaceRenderer::Create(const SurfaceRequest& request,
                                                                   Vector2i sizePx,
                                                                   std::string* error) {
  if (std::string invalid = CheckFrameSize(sizePx); !invalid.empty()) {
    SetError(error, std::move(invalid));
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  std::string failure = impl->open(request);
  if (failure.empty()) {
    failure = impl->chooseConfiguration(request.native);
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

std::unique_ptr<GeodeSurfaceRenderer> GeodeSurfaceRenderer::CreateForMetalLayer(
    void* metalLayer, Vector2i sizePx, std::string* error) {
#if defined(DONNER_GEODE_BROWSER_BACKEND)
  (void)metalLayer;
  (void)sizePx;
  SetError(error, "Metal layers are not available in browser builds");
  return nullptr;
#else
  if (metalLayer == nullptr) {
    SetError(error, "metalLayer is null");
    return nullptr;
  }
  SurfaceRequest request{geode::GpuBackendKind::NativeMetal, {}};
  request.native.kind = gpu::NativeSurfaceKind::MetalLayer;
  request.native.display = metalLayer;
  return Create(request, sizePx, error);
#endif
}

std::unique_ptr<GeodeSurfaceRenderer> GeodeSurfaceRenderer::CreateForCanvas(
    std::string_view canvasSelector, Vector2i sizePx, std::string* error) {
#if defined(DONNER_GEODE_BROWSER_BACKEND)
  if (canvasSelector.empty()) {
    SetError(error, "canvasSelector is empty");
    return nullptr;
  }
  SurfaceRequest request{geode::GpuBackendKind::Browser, {}};
  request.native.kind = gpu::NativeSurfaceKind::CanvasSelector;
  request.native.selector = RcString(canvasSelector);
  return Create(request, sizePx, error);
#else
  (void)canvasSelector;
  (void)sizePx;
  SetError(error, "canvas presentation requires a WebAssembly build with the browser backend");
  return nullptr;
#endif
}

bool GeodeSurfaceRenderer::resize(Vector2i sizePx, std::string* error) {
  if (std::string invalid = CheckFrameSize(sizePx); !invalid.empty()) {
    SetError(error, std::move(invalid));
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
