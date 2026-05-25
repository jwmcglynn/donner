#include "donner/editor/SoftWrap.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace donner::editor {
namespace {

int LeadingWhitespaceColumns(std::string_view line) {
  int result = 0;
  for (char ch : line) {
    if (ch == ' ') {
      ++result;
    } else if (ch == '\t') {
      result += 2;
    } else {
      break;
    }
  }
  return result;
}

bool IsSpace(char ch) {
  return ch == ' ' || ch == '\t';
}

int FindBreakBefore(std::string_view line, int startColumn, int targetEnd) {
  const int end = std::min<int>(targetEnd, line.size());
  for (int column = end; column > startColumn; --column) {
    if (IsSpace(line[column - 1])) {
      return column - 1;
    }
  }
  return end;
}

}  // namespace

int ComputeXmlContinuationIndent(std::string_view line) {
  const int leading = LeadingWhitespaceColumns(line);
  std::size_t index = static_cast<std::size_t>(leading);
  if (index >= line.size() || line[index] != '<') {
    return leading;
  }

  ++index;
  if (index < line.size() && (line[index] == '/' || line[index] == '!' || line[index] == '?')) {
    return leading;
  }

  while (index < line.size() && !IsSpace(line[index]) && line[index] != '>' && line[index] != '/') {
    ++index;
  }
  while (index < line.size() && IsSpace(line[index])) {
    ++index;
  }

  if (index <= static_cast<std::size_t>(leading + 1)) {
    return leading;
  }
  return static_cast<int>(index);
}

std::vector<SoftWrapSegment> ComputeSoftWrapSegments(std::string_view line, int maxColumns) {
  if (line.empty()) {
    return {SoftWrapSegment{}};
  }

  maxColumns = std::max(maxColumns, 12);
  const int continuationIndent =
      std::min(ComputeXmlContinuationIndent(line), std::max(0, maxColumns - 8));

  std::vector<SoftWrapSegment> result;
  int start = 0;
  while (start < static_cast<int>(line.size())) {
    const bool continuation = !result.empty();
    const int indent = continuation ? continuationIndent : 0;
    const int availableColumns = std::max(8, maxColumns - indent);
    const int targetEnd = start + availableColumns;

    int end = std::min<int>(targetEnd, line.size());
    if (end < static_cast<int>(line.size())) {
      end = FindBreakBefore(line, start, targetEnd);
      if (end <= start) {
        end = std::min<int>(targetEnd, line.size());
      }
    }

    int nextStart = end;
    while (nextStart < static_cast<int>(line.size()) && IsSpace(line[nextStart])) {
      ++nextStart;
    }

    result.push_back(SoftWrapSegment{
        .startColumn = start,
        .endColumn = end,
        .indentColumns = indent,
        .continuation = continuation,
    });
    start = nextStart > start ? nextStart : end;
  }

  return result;
}

}  // namespace donner::editor
