#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"

namespace donner::css {

/// A single entry listed in `src:`—either a local face, a URL, or inline data.
struct FontFaceSource {
  /**
   * Specifies the source type for a font face declaration.
   */
  enum class Kind : uint8_t {
    Local,  ///< Font is loaded from a local system font by name (local() function)
    Url,    ///< Font is loaded from a remote URL or file path (url() function)
    Data    ///< Font is embedded as inline data using a data URI scheme
  };

  /// Font source kind.
  Kind kind;

  /// The payload of the source, which can be a URL or raw data (already parsed from the data URL).
  std::variant<RcString, std::vector<uint8_t>> payload;

  /// Format hint, if provided, e.g. "woff2" or "opentype".
  RcString formatHint;

  /// Technology hints, if provided, e.g. {"variations","color-COLRv1"}.
  std::vector<RcString> techHints;
};

/**
 * In-memory representation of a single @font-face rule.
 */
struct FontFace {
  RcString familyName;                  ///< font-family descriptor
  std::vector<FontFaceSource> sources;  ///< ordered src list
  std::optional<RcString> style;        ///< font-style descriptor
  std::optional<RcString> weight;       ///< font-weight descriptor
  std::optional<RcString> stretch;      ///< font-stretch descriptor
  std::optional<RcString> display;      ///< font-display descriptor
};

}  // namespace donner::css
