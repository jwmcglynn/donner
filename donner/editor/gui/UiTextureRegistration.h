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
 * A texture of the device the interface is drawn on is registered directly; one rendered on a
 * different device is registered from the export its producer took, so a snapshot that only
 * borrows a producer's frame target cannot be registered at all.
 *
 * @param snapshot Snapshot to register.
 * @param backing Receives the handles keeping the registration's backing reachable.
 */
ImTextureID RegisterUiSnapshotTexture(const svg::RendererTextureSnapshot& snapshot,
                                      UiTextureBacking* backing);

/**
 * Retires the registration \p texture names and transfers \p backing to the renderer until that
 * exact registration generation is released. Returns false without consuming \p backing when the
 * UI renderer is unavailable or the registry refuses the retirement.
 *
 * @param texture Identifier to retire.
 * @param backing Backing handles to retain through the registration's retirement window.
 */
[[gnu::noinline]] bool RetireUiTexture(ImTextureID texture, UiTextureBacking* backing);

}  // namespace donner::editor
