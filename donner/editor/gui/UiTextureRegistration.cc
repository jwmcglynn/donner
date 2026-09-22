#include "donner/editor/gui/UiTextureRegistration.h"

#include <cstdio>
#include <utility>

#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::editor {

namespace {

/// \p renderer's import device when it is the one \p renderer draws on, or null. A renderer with
/// no import path is not an error: it means a backend texture cannot reach the interface on that
/// device, which the caller reports as a refused registration.
/// @param renderer Installed UI renderer.
geode::GeodeWgpuAdapterDevice* ImportDeviceFor(ImGuiRuntimeRenderer& renderer) {
  geode::GeodeWgpuAdapterDevice* device = renderer.importDevice();
  if (device == nullptr || device->deviceId() != renderer.device().deviceId()) {
    return nullptr;
  }
  return device;
}

/// The UI alpha interpretation matching \p alphaType.
/// @param alphaType Snapshot's alpha interpretation.
UiTextureAlphaMode UiAlphaModeOf(svg::AlphaType alphaType) {
  return alphaType == svg::AlphaType::Premultiplied ? UiTextureAlphaMode::Premultiplied
                                                    : UiTextureAlphaMode::Straight;
}

/// Registers \p view for \p dimensions, filling in \p backing's view on success.
/// @param renderer Installed UI renderer. @param view View to register.
/// @param dimensions Sampled extent in pixels. @param alphaMode Alpha interpretation.
/// @param backing Receives the view.
[[gnu::noinline]] ImTextureID Register(ImGuiRuntimeRenderer& renderer,
                                       gpu::Result<gpu::TextureView>&& view,
                                       const Vector2i& dimensions, UiTextureAlphaMode alphaMode,
                                       UiTextureBacking* backing) {
  if (view.hasError()) {
    return 0;
  }
  const gpu::Result<UiTextureId> registered =
      renderer.registry().registerTexture(UiTextureDescriptor{
          view.result(),
          {static_cast<uint32_t>(dimensions.x), static_cast<uint32_t>(dimensions.y)},
          alphaMode});
  if (registered.hasError()) {
    return 0;
  }
  backing->view = std::move(view).result();
  return registered.result().imTextureId();
}

}  // namespace

bool HasUiTextureRegistry() {
  return CurrentUiTextureRegistry() != nullptr;
}

ImTextureID RegisterUiSnapshotTexture(const svg::RendererTextureSnapshot& snapshot,
                                      UiTextureBacking* backing) {
  ImGuiRuntimeRenderer* renderer = CurrentImGuiRuntimeRenderer();
  if (renderer == nullptr || snapshot.backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }

  const auto& geodeSnapshot = static_cast<const svg::RendererGeodeTextureSnapshot&>(snapshot);
  const Vector2i dimensions = geodeSnapshot.dimensions();
  if (dimensions.x <= 0 || dimensions.y <= 0) {
    return 0;
  }

  const gpu::Texture* runtimeTexture = geodeSnapshot.runtimeTexture();
  if (runtimeTexture == nullptr) {
    return 0;
  }
  const UiTextureAlphaMode alphaMode = UiAlphaModeOf(geodeSnapshot.alphaType());
  if (runtimeTexture->deviceId() == renderer->device().deviceId()) {
    return Register(*renderer,
                    renderer->device().createTextureView(*runtimeTexture,
                                                         gpu::TextureViewDescriptor{"uiSnapshot"}),
                    dimensions, alphaMode, backing);
  }

  // Rendered on a different device than the interface is drawn on, so it reaches the interface
  // only by being registered on the drawing device - which needs the producing device to still
  // own it, and is why a snapshot that only borrows a frame target cannot be registered here.
  geode::GeodeWgpuAdapterDevice* importDevice = ImportDeviceFor(*renderer);
  const std::shared_ptr<geode::GeodeDevice>& owner = geodeSnapshot.owningDevice();
  if (importDevice == nullptr || owner == nullptr) {
    return 0;
  }
  gpu::Result<gpu::Texture> imported =
      importDevice->importTextureFrom(owner->adapterDevice(), *runtimeTexture);
  if (imported.hasError()) {
    return 0;
  }

  const ImTextureID handle =
      Register(*renderer,
               renderer->device().createTextureView(imported.result(),
                                                    gpu::TextureViewDescriptor{"uiImported"}),
               dimensions, alphaMode, backing);
  if (handle != 0) {
    backing->texture = std::move(imported).result();
  }
  return handle;
}

bool RetireUiTexture(ImTextureID texture, UiTextureBacking* backing) {
  ImGuiRuntimeRenderer* renderer = CurrentImGuiRuntimeRenderer();
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  if (texture == 0 || backing == nullptr || renderer == nullptr || registry == nullptr) {
    return false;
  }
  const UiTextureId id = UiTextureId::FromImTextureId(texture);
  const gpu::Status retired = registry->retire(id);
  if (retired.hasError()) {
    // The registry reports a double release; swallowing it would hide the producer bug it exists
    // to name.
    std::fprintf(stderr, "UI texture retire failed: %s\n", retired.error().toString().c_str());
    return false;
  }
  renderer->retainTextureBackingUntilReleased(id, std::move(backing->texture),
                                              std::move(backing->view));
  return true;
}

}  // namespace donner::editor
