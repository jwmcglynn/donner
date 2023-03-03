#pragma once

#include <algorithm>
#include <cassert>
#include <string_view>
#include <vector>

#include "src/base/utils.h"

namespace donner::svg {

/**
 * Helper class for finding newlines in a string, so that error messages can convert string-relative
 * offsets into line numbers.
 *
 * This supports all newline styles, including `\r\n` and `\r`.
 */
class LineOffsets {
public:
  /**
   * Construct a LineOffsets object for the given input string.
   *
   * @param input Input string.
   */
  LineOffsets(std::string_view input) {
    // Compute the offsets for the start of each line. The line isn't considered started until
    // *after* the line break characters.
    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '\n') {
        offsets_.push_back(i + 1);
      } else if (input[i] == '\r') {
        // If there is a \r\n, it should only count as one line, skip over the \n if it exists.
        if (i + 1 < input.size() && input[i + 1] == '\n') {
          ++i;
        }

        offsets_.push_back(i + 1);
      }
    }
  }

  /**
   * Return the offsets of the start of each line.
   */
  const std::vector<size_t>& offsets() const { return offsets_; }

  /**
   * Return line numbers for the given offset.
   *
   * For example, given a string: "abc\n123", offsets 0-3 would be considered line 1, and offsets
   * after 4 (corresponding to the index of '1'), would be line 2. Values beyond the length of the
   * string return the last line number.
   *
   * @param offset Character index.
   * @return size_t Line number, 1-indexed.
   */
  size_t offsetToLine(size_t offset) const {
    auto it = std::upper_bound(offsets_.begin(), offsets_.end(), offset);
    if (it == offsets_.end()) {
      return offsets_.size() + 1;
    } else {
      return static_cast<size_t>(it - offsets_.begin()) + 1;
    }
  }

  /**
   * Returns the offset of a given 1-indexed line number.
   */
  size_t lineOffset(size_t line) const {
    if (line == 1) {
      return 0;
    } else {
      UTILS_RELEASE_ASSERT(line > 0 && line - 1 <= offsets_.size() && "Line out of range");
      return offsets_[line - 2];
    }
  }

private:
  std::vector<size_t> offsets_;
};

}  // namespace donner::svg
