#pragma once
/// @file

#include <cstdint>
#include <memory>
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

  /// The payload of the source, which can be a URL or shared font data bytes. Using shared_ptr
  /// for the data variant avoids deep-copying font bytes (~13MB per font set) when FontFace
  /// objects are copied across documents, preventing glibc heap fragmentation in test suites.
  std::variant<RcString, std::shared_ptr<const std::vector<uint8_t>>> payload;

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
  int fontWeight = 400;                 ///< font-weight descriptor (100-900, 400=normal, 700=bold)
  int fontStyle = 0;   ///< font-style descriptor (0=normal, 1=italic, 2=oblique)
  int fontStretch = 5;  ///< font-stretch descriptor (1-9, 5=normal, matching FontStretch enum)
};

}  // namespace donner::css
