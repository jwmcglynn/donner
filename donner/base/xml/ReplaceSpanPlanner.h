#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/SourceDocument.h"

namespace donner::xml {

/**
 * Orders span-based replacements, detects conflicts, and falls back to expanded replacements when
 * spans are missing or overlapping.
 */
class ReplaceSpanPlanner {
public:
  struct ReplaceSpan {
    SourceDocument::Replacement replacement;  ///< Primary replacement to attempt.
    std::optional<SourceDocument::Replacement> fallback;  ///< Optional fallback replacement.
  };

  struct PlanResult {
    std::vector<SourceDocument::Replacement> ordered;  ///< Sorted, non-overlapping replacements.
    bool usedFallback = false;                         ///< True if any fallback was chosen.
  };

  /**
   * Produce an ordered, non-overlapping replacement list. If a replacement lacks resolved offsets
   * or overlaps an earlier span, a fallback replacement will be chosen when provided and compatible.
   */
  ParseResult<PlanResult> plan(std::vector<ReplaceSpan> replacements) const;

private:
  static bool hasConcreteOffsets(const FileOffsetRange& range);
  static bool overlaps(const FileOffsetRange& lhs, const FileOffsetRange& rhs);
};

}  // namespace donner::xml

