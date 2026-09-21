#pragma once
/// @file
/// Test-only access to the compositor debug panel's private preview bookkeeping. Defined once so
/// the two test binaries that reach into the panel agree on a single type.

#include <cstddef>
#include <string>

#include "donner/editor/CompositorDebugPanel.h"

namespace donner::editor {

/// Reaches the panel's private thumbnail bookkeeping. The panel befriends this exact type.
struct CompositorDebugPanelTestAccess {
  /// Runs one thumbnail upload and returns the identifier the panel publishes for the tile.
  /// @param panel Panel under test.
  /// @param tile Composite tile to upload.
  static auto upload(CompositorDebugPanel& panel,
                     const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
    return panel.uploadThumbnail(tile);
  }

  /// Number of tiles the panel currently holds a preview resource for.
  /// @param panel Panel under test.
  static std::size_t registrationCount(const CompositorDebugPanel& panel) {
    return panel.textures_.size();
  }

  /// Installs a registration with no backing texture, so a release path can be observed without a
  /// live graphics context.
  /// @param panel Panel under test.
  /// @param id Composite tile identifier to register.
  static void seedRegistrationWithoutTexture(CompositorDebugPanel& panel, const std::string& id) {
    panel.textures_[id] = CompositorDebugPanel::ThumbnailTexture{};
  }
};

}  // namespace donner::editor
