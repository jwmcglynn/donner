#include "donner/editor/gui/UiTextureRegistration.h"

#include <utility>

#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::editor {

namespace {

/// The device the UI renderer draws on, which every UI texture must be registered against. The
/// editor always draws through the transition adapter, so the cast holds for as long as an
/// imported texture is how a backend texture reaches the interface.
/// @param renderer Installed UI renderer.
geode::GeodeWgpuAdapterDevice& UiRendererDevice(ImGuiRuntimeRenderer& renderer) {
  return static_cast<geode::GeodeWgpuAdapterDevice&>(renderer.device());
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
ImTextureID Register(ImGuiRuntimeRenderer& renderer, gpu::Result<gpu::TextureView>&& view,
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
  const uint64_t uiDeviceId = renderer->device().deviceId();
  if (runtimeTexture != nullptr && runtimeTexture->deviceId() == uiDeviceId) {
    return Register(*renderer,
                    renderer->device().createTextureView(*runtimeTexture,
                                                         gpu::TextureViewDescriptor{"uiSnapshot"}),
                    dimensions, UiAlphaModeOf(geodeSnapshot.alphaType()), backing);
  }

  if (!geodeSnapshot.runtimeFormat().has_value()) {
    return 0;
  }
  return RegisterUiImportedTexture(geodeSnapshot.texture(), dimensions,
                                   *geodeSnapshot.runtimeFormat(),
                                   UiAlphaModeOf(geodeSnapshot.alphaType()), backing);
}

ImTextureID RegisterUiImportedTexture(const wgpu::Texture& texture, const Vector2i& dimensions,
                                      gpu::TextureFormat format, UiTextureAlphaMode alphaMode,
                                      UiTextureBacking* backing) {
  ImGuiRuntimeRenderer* renderer = CurrentImGuiRuntimeRenderer();
  if (renderer == nullptr || dimensions.x <= 0 || dimensions.y <= 0) {
    return 0;
  }

  gpu::Result<gpu::Texture> imported = UiRendererDevice(*renderer).importExternalTexture(
      texture, {static_cast<uint32_t>(dimensions.x), static_cast<uint32_t>(dimensions.y)}, format,
      gpu::TextureUsage::Sampled);
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

void RetireUiTexture(ImTextureID texture) {
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  if (texture == 0 || registry == nullptr) {
    return;
  }
  const gpu::Status retired = registry->retire(UiTextureId::FromImTextureId(texture));
  (void)retired;
}

}  // namespace donner::editor
