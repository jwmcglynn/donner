#pragma once
/// @file
/// Feature vocabulary shared by every bakeoff adapter: which `RendererInterface` capabilities a
/// backend implements natively, emulates, or cannot render, and which of them a scene used.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace donner::bakeoff {

/// One renderer capability that a scene can exercise through `RendererInterface`.
enum class Feature : uint8_t {
  Transform,       ///< Affine transforms set or pushed on the renderer.
  SolidPaint,      ///< Solid color fill or stroke.
  LinearGradient,  ///< Linear gradient paint server.
  RadialGradient,  ///< Radial gradient paint server, including a focal point.
  PatternPaint,    ///< Pattern paint server recorded through a pattern tile.
  EvenOddFill,     ///< Path fill with the even-odd fill rule.
  Stroke,          ///< Stroked geometry with joins and caps.
  StrokeDash,      ///< Dashed stroke.
  ClipRect,        ///< Rectangular clip.
  ClipPath,        ///< Single-level `clip-path` geometry.
  ClipPathNested,  ///< `clip-path` whose children carry their own clip paths.
  IsolatedLayer,   ///< Group opacity composited as an isolated layer.
  BlendMode,       ///< Non-normal `mix-blend-mode`.
  MaskLuminance,   ///< Luminance mask.
  MaskAlpha,       ///< Alpha mask.
  Filter,          ///< Filter effect layer.
  Image,           ///< Raster image draw.
  Text,            ///< Text draw.
};

/// Number of \ref Feature values.
inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::Text) + 1;

/// How a backend adapter implements a feature.
enum class Support : uint8_t {
  Native,       ///< The backend API has a direct primitive for the feature.
  Emulated,     ///< The adapter composes the feature from other backend primitives.
  Unsupported,  ///< The adapter cannot render the feature; draws using it are dropped.
};

/// Stable lower_snake_case name for \p feature, used in JSON results and reports.
/// @param feature Feature to name.
std::string_view FeatureName(Feature feature);

/// Stable lower-case name for \p support: `native`, `emulated` or `unsupported`.
/// @param support Support level to name.
std::string_view SupportName(Support support);

/// Per-feature support levels of one adapter, with an optional note per feature.
struct CapabilityTable {
  /// Support level indexed by \ref Feature.
  std::array<Support, kFeatureCount> support{};
  /// Short explanation per feature, empty when the level speaks for itself.
  std::array<std::string_view, kFeatureCount> notes{};

  /// Creates a table with every feature at \p level.
  /// @param level Initial support level.
  static CapabilityTable All(Support level);

  /// Sets the level and note for one feature, returning this table for chaining.
  /// @param feature Feature to set.
  /// @param level Support level.
  /// @param note Optional explanation; must have static storage duration.
  CapabilityTable& set(Feature feature, Support level, std::string_view note = {});

  /// Support level of \p feature.
  /// @param feature Feature to query.
  [[nodiscard]] Support get(Feature feature) const {
    return support[static_cast<std::size_t>(feature)];
  }
};

/// Counts how often a scene used each feature, and how often an adapter had to drop it.
class FeatureUsage {
public:
  /// Records one use of \p feature.
  /// @param feature Feature used.
  void recordUse(Feature feature) { ++uses_[static_cast<std::size_t>(feature)]; }

  /// Records one dropped use of \p feature.
  /// @param feature Feature the adapter could not render.
  void recordDropped(Feature feature) { ++dropped_[static_cast<std::size_t>(feature)]; }

  /// Number of uses of \p feature.
  /// @param feature Feature to query.
  [[nodiscard]] std::uint64_t uses(Feature feature) const {
    return uses_[static_cast<std::size_t>(feature)];
  }

  /// Number of dropped uses of \p feature.
  /// @param feature Feature to query.
  [[nodiscard]] std::uint64_t dropped(Feature feature) const {
    return dropped_[static_cast<std::size_t>(feature)];
  }

  /// True when no use of any feature was dropped.
  [[nodiscard]] bool rendersFully() const;

  /// Clears all counts.
  void reset();

private:
  std::array<std::uint64_t, kFeatureCount> uses_{};
  std::array<std::uint64_t, kFeatureCount> dropped_{};
};

}  // namespace donner::bakeoff
