#include "donner/base/xml/ReplaceSpanPlanner.h"

#include <algorithm>

#include "donner/base/ParseError.h"

namespace donner::xml {
namespace {

bool hasResolvedOffsets(const FileOffsetRange& range) {
  return range.start.offset.has_value() && range.end.offset.has_value();
}

size_t startOffset(const FileOffsetRange& range) {
  return range.start.offset.value();
}

size_t endOffset(const FileOffsetRange& range) {
  return range.end.offset.value();
}

}  // namespace

bool ReplaceSpanPlanner::hasConcreteOffsets(const FileOffsetRange& range) {
  return hasResolvedOffsets(range);
}

bool ReplaceSpanPlanner::overlaps(const FileOffsetRange& lhs, const FileOffsetRange& rhs) {
  return startOffset(lhs) < endOffset(rhs) && startOffset(rhs) < endOffset(lhs);
}

ParseResult<ReplaceSpanPlanner::PlanResult> ReplaceSpanPlanner::plan(
    std::vector<ReplaceSpan> replacements) const {
  PlanResult planResult;

  // Resolve missing offsets via fallback when possible.
  for (auto& entry : replacements) {
    if (!hasConcreteOffsets(entry.replacement.range)) {
      if (!entry.fallback.has_value() || !hasConcreteOffsets(entry.fallback->range)) {
        return ParseError{RcString("Replacement is missing resolved offsets"),
                          entry.replacement.range.start};
      }

      entry.replacement = entry.fallback.value();
      planResult.usedFallback = true;
      entry.fallback = std::nullopt;
    }
  }

  std::stable_sort(replacements.begin(), replacements.end(), [](const ReplaceSpan& lhs,
                                                                 const ReplaceSpan& rhs) {
    return startOffset(lhs.replacement.range) < startOffset(rhs.replacement.range);
  });

  for (const auto& entry : replacements) {
    if (planResult.ordered.empty()) {
      planResult.ordered.push_back(entry.replacement);
      continue;
    }

    auto& last = planResult.ordered.back();
    if (!overlaps(last.range, entry.replacement.range)) {
      planResult.ordered.push_back(entry.replacement);
      continue;
    }

    bool resolved = false;
    if (entry.fallback.has_value() && hasConcreteOffsets(entry.fallback->range)) {
      const auto& fallback = *entry.fallback;
      if (startOffset(fallback.range) <= startOffset(last.range) &&
          endOffset(fallback.range) >= endOffset(last.range) &&
          endOffset(fallback.range) >= endOffset(entry.replacement.range)) {
        if (planResult.ordered.size() == 1 ||
            endOffset(planResult.ordered[planResult.ordered.size() - 2].range) <=
                startOffset(fallback.range)) {
          last = fallback;
          planResult.usedFallback = true;
          resolved = true;
        }
      }
    }

    if (!resolved) {
      return ParseError{RcString("Overlapping replacements with no compatible fallback"),
                        entry.replacement.range.start};
    }
  }

  return planResult;
}

}  // namespace donner::xml

