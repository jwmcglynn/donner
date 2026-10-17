#pragma once
/// @file
/// Test-only access to the compositor debug panel's private preview bookkeeping. Defined once so
/// the two test binaries that reach into the panel agree on a single type.

#include <algorithm>
#include <cstddef>
#include <string>

#include "donner/editor/CompositorDebugPanel.h"

namespace donner::editor {

/// Reaches the panel's private thumbnail bookkeeping. The panel befriends this exact type.
struct CompositorDebugPanelTestAccess {
  /// Populate the editable path exactly as text entry would.
  /// @param panel Panel under test.
  /// @param path Editable path, independent of the host's fixed destination.
  static void setTelemetryPath(CompositorDebugPanel& panel, const std::string& path) {
    const std::size_t count = std::min(path.size(), panel.telemetryPathBuffer_.size() - 1u);
    std::copy_n(path.begin(), count, panel.telemetryPathBuffer_.begin());
    panel.telemetryPathBuffer_[count] = '\0';
  }

  /// Invoke the diagnostic Save button's action without frame timing or pointer coordinates.
  /// @param panel Panel under test.
  static bool saveTelemetry(CompositorDebugPanel& panel) { return panel.saveHeuristicTelemetry(); }

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
