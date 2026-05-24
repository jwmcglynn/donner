#pragma once
/// @file

#include <string_view>
#include <vector>

namespace donner::editor {

/// One visual row produced from a logical source line.
struct SoftWrapSegment {
  int startColumn = 0;        ///< First source column included in this row.
  int endColumn = 0;          ///< First source column excluded from this row.
  int indentColumns = 0;      ///< Visual indentation inserted before this row.
  bool continuation = false;  ///< True when this row is a wrapped continuation.

  /// Equality operator.
  bool operator==(const SoftWrapSegment& other) const = default;
};

/**
 * Compute the visual continuation indent for XML-like source lines.
 *
 * @param line Logical line text.
 * @return Column used for wrapped continuations.
 */
[[nodiscard]] int ComputeXmlContinuationIndent(std::string_view line);

/**
 * Compute soft-wrap rows for a single logical line.
 *
 * @param line Logical line text.
 * @param maxColumns Maximum visual columns available for each row.
 * @return One or more visual rows. Empty input still returns one empty row.
 */
[[nodiscard]] std::vector<SoftWrapSegment> ComputeSoftWrapSegments(std::string_view line,
                                                                   int maxColumns);

}  // namespace donner::editor
