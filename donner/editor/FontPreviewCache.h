#pragma once
/// @file

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// Bounded on-disk outlined SVG previews with validated RGBA reloads.
class FontPreviewCache {
public:
  /// @param directory Private per-user cache directory; empty disables persistence.
  /// @param buildIdentity Embedded editor build identity.
  FontPreviewCache(std::filesystem::path directory, std::string buildIdentity);

  /// Load only when the build, font asset, dimensions, and sidecar integrity match.
  /// @param family Font family displayed in the menu.
  /// @param contentIdentity Immutable catalog content id and generation.
  /// @param dimensions Expected output bitmap dimensions.
  [[nodiscard]] std::optional<svg::RendererBitmap> load(std::string_view family,
                                                        std::string_view contentIdentity,
                                                        Vector2i dimensions) const;

  /// Persist a preview after the resolved text was converted to outline paths.
  /// @param family Font family displayed in the menu.
  /// @param contentIdentity Immutable catalog content id and generation.
  /// @param outlinedSvg Standalone SVG containing paths and no text nodes.
  /// @param bitmap Rendered bitmap for immediate subsequent display.
  void store(std::string_view family, std::string_view contentIdentity,
             std::string_view outlinedSvg, const svg::RendererBitmap& bitmap) const;

private:
  [[nodiscard]] std::filesystem::path stem(std::string_view family,
                                           std::string_view contentIdentity,
                                           Vector2i dimensions) const;
  void trim() const;

  std::filesystem::path directory_;
  std::string buildIdentity_;
};

}  // namespace donner::editor
