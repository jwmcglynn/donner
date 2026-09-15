#pragma once
/// @file
/// Registering renderer-produced textures with the UI texture registry.

#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Handles.h"

namespace donner::svg {
class RendererTextureSnapshot;
}  // namespace donner::svg

namespace wgpu {
class Texture;
}  // namespace wgpu

namespace donner::geode {
class GeodeWgpuAdapterDevice;
}  // namespace donner::geode

namespace donner::editor {

/**
 * Runtime handles keeping one UI texture registration's backing reachable.
 *
 * A registration borrows its backing rather than owning it, so whoever registers holds this for
 * as long as the registration is live and drops it when the registration is retired.
 */
struct UiTextureBacking {
  /// Imported texture, null when the source already owned a texture on the UI device.
  gpu::Texture texture;
  /// View the registration samples.
  gpu::TextureView view;
};

/**
 * True when a UI renderer is installed, so a texture registered now will have an identifier draw
 * data can carry. Callers that cache textures before the interface exists use this to tell a
 * failed registration apart from there being nothing to register with yet.
 */
bool HasUiTextureRegistry();

/**
 * Registers \p snapshot with the UI texture registry and returns the identifier draw data carries
 * for it, or zero when there is no UI renderer, \p snapshot is not a backend snapshot, or the
 * registration is refused. The alpha interpretation comes from the snapshot.
 *
 * A snapshot that already owns a texture on the UI device is registered directly; one that does
 * not, including one whose texture belongs to the device its pixels were rendered on rather than
 * the device the interface is drawn on, is imported first.
 *
 * @param snapshot Snapshot to register.
 * @param backing Receives the handles keeping the registration's backing reachable.
 */
ImTextureID RegisterUiSnapshotTexture(const svg::RendererTextureSnapshot& snapshot,
                                      UiTextureBacking* backing);

/**
 * Registers \p texture, a backend texture the caller uploaded itself, with the UI texture
 * registry. Returns zero when there is no UI renderer or the registration is refused.
 *
 * @param texture Backend texture to import and register.
 * @param dimensions Sampled extent in pixels.
 * @param format Runtime format of \p texture.
 * @param alphaMode Alpha interpretation of the sampled texels.
 * @param backing Receives the handles keeping the registration's backing reachable.
 */
ImTextureID RegisterUiImportedTexture(const wgpu::Texture& texture, const Vector2i& dimensions,
                                      gpu::TextureFormat format, UiTextureAlphaMode alphaMode,
                                      UiTextureBacking* backing);

/**
 * Retires the registration \p texture names, so later draw data naming it is refused and its slot
 * is released once the frames a recorded draw can still be in flight have passed. A no-op for a
 * zero identifier or when there is no UI renderer.
 *
 * @param texture Identifier to retire.
 */
void RetireUiTexture(ImTextureID texture);

}  // namespace donner::editor
