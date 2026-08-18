#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"

namespace donner::css {

/// A single entry listed in `src:`-either a local face, a URL, or inline data.
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

  /// True only for application-controlled font bytes, never parsed document sources.
  bool trusted = false;
};

/**
 * In-memory representation of a single `@font-face` rule.
 */
struct FontFace {
  RcString familyName;                  ///< font-family descriptor
  std::vector<FontFaceSource> sources;  ///< ordered src list
  int fontWeight = 400;                 ///< font-weight descriptor (100-900, 400=normal, 700=bold)
  int fontStyle = 0;                    ///< font-style descriptor (0=normal, 1=italic, 2=oblique)
  int fontStretch = 5;  ///< font-stretch descriptor (1-9, 5=normal, matching FontStretch enum)
};

/**
 * Build a string that identifies an `@font-face` declaration.
 *
 * Two declarations produce the same key exactly when they would select the same face and resolve
 * to the same bytes: the matching descriptors (family, compared case-insensitively, plus weight,
 * style, and stretch) together with the ordered source list, including each source's kind, trust,
 * format and technology hints, and payload identity. Inline data payloads are immutable and shared
 * by pointer, so the pointer value identifies the bytes as long as something holds a reference to
 * them; the key is only meaningful while its holder keeps that reference alive.
 *
 * Every variable-length field is length-prefixed, so a separator character appearing inside a
 * family name, hint, or URL cannot make two different declarations produce one key.
 *
 * The intended use is deduplication: callers that re-announce a document's `@font-face` set (a
 * style recompute does this on every pass) key on this so an unchanged declaration keeps whatever
 * identity and cached state it already had.
 *
 * @param face The declaration to identify.
 */
std::string FontFaceIdentityKey(const FontFace& face);

}  // namespace donner::css
