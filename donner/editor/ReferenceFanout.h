#pragma once
/// @file

#include <cstddef>
#include <string_view>

namespace donner::editor {

/// Maximum number of reverse references that source focus expands directly.
inline constexpr std::size_t kMaxExpandedReverseReferences = 5;

/// Tooltip shown when a reverse-reference fanout is too large to expand.
inline constexpr std::string_view kReverseReferenceOverflowTooltip =
    "Too many reverse refs to draw lines";

/**
 * Return true when a reverse-reference fanout should stay summarized.
 *
 * @param count Number of reverse references or selector-matched elements.
 * @return True when expanding the fanout would be too noisy or expensive.
 */
[[nodiscard]] constexpr bool IsLargeReverseReferenceFanout(std::size_t count) {
  return count > kMaxExpandedReverseReferences;
}

}  // namespace donner::editor
