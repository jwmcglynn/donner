#pragma once
/**
 * @file UnicodeBidi.h
 *
 * Defines the \ref donner::svg::UnicodeBidi enum for the `unicode-bidi` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `unicode-bidi` property.
 *
 * Controls how an element's text takes part in bidirectional reordering. On a `<text>` element,
 * which acts as the paragraph's block container, `bidi-override` and `isolate-override` give
 * every character the paragraph's `direction` instead of reordering it by its bidi type.
 *
 * @see https://www.w3.org/TR/css-writing-modes-3/#unicode-bidi
 * @see https://www.w3.org/TR/SVG2/text.html#UnicodeBidiProperty
 */
enum class UnicodeBidi : uint8_t {
  Normal,           ///< [DEFAULT] No additional embedding level.
  Embed,            ///< Opens an embedding level for the element's `direction`.
  Isolate,          ///< Isolates the element's content from its surroundings.
  BidiOverride,     ///< Overrides character bidi types with the element's `direction`.
  IsolateOverride,  ///< Combines `isolate` and `bidi-override`.
  Plaintext,        ///< Resolves direction from the content's first strong character.
};

/// Whether \p value gives every character of the element the element's `direction`.
constexpr bool OverridesBidi(UnicodeBidi value) {
  return value == UnicodeBidi::BidiOverride || value == UnicodeBidi::IsolateOverride;
}

/**
 * Ostream output operator for \ref UnicodeBidi enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, UnicodeBidi value) {
  switch (value) {
    case UnicodeBidi::Normal: return os << "normal";
    case UnicodeBidi::Embed: return os << "embed";
    case UnicodeBidi::Isolate: return os << "isolate";
    case UnicodeBidi::BidiOverride: return os << "bidi-override";
    case UnicodeBidi::IsolateOverride: return os << "isolate-override";
    case UnicodeBidi::Plaintext: return os << "plaintext";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
