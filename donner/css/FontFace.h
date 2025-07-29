#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"

namespace donner::css {

/// How text is rendered while the web-font is loading (§4.9 font-display).
enum class FontDisplay { Auto, Block, Swap, Fallback, Optional };

/// Enumerates the basic style buckets; oblique may carry an angle.
enum class FontStyle { Normal, Italic, Oblique };

/// A single entry listed in `src:`—either a local face, a URL, or inline data.
struct FontFaceSource {
  enum class Kind { Local, Url, Data };

  /// Font source kind.
  Kind kind;

  /// The payload of the source, which can be a URL or raw data (already parsed from the data URL).
  std::variant<RcString, std::vector<uint8_t>> payload;

  /// Format hint, if provided, e.g. "woff2" or "opentype".
  std::string formatHint;

  /// Technology hints, if provided, e.g. {"variations","color-COLRv1"}.
  std::vector<std::string> techHints;
};

/// Numeric range helpers for variable-font axes.
template <typename T>
struct Range {
  T min;
  T max;
};

/**
 * In-memory representation of a single @font-face rule.
 */
struct FontFace {
  std::string familyName_;               ///< font-family descriptor
  std::vector<FontFaceSource> sources_;  ///< ordered src list

  std::optional<Range<int>> weightRange_;      ///< font-weight (100-900)
  std::optional<int> weight_;                  ///< single weight
  std::optional<Range<double>> stretchRange_;  ///< font-stretch 50-200 %
  std::optional<double> stretchPercent_;       ///< single stretch
  FontStyle style_{FontStyle::Normal};
  std::optional<Range<double>> obliqueAngle_;

  std::string unicodeRange_;
  FontDisplay display_{FontDisplay::Auto};
  std::string featureSettings_;
  std::string variationSettings_;
  std::string namedInstance_;
  std::optional<float> sizeAdjust_;
  std::optional<float> ascentOverride_;
  std::optional<float> descentOverride_;
  std::optional<float> lineGapOverride_;
  std::string languageOverride_;
};

}  // namespace donner::css
