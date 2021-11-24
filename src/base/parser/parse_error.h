#pragma once

#include <ostream>
#include <string>
#include <string_view>

namespace donner {

struct ParseError {
  static constexpr int kEndOfString = -1;

  std::string reason;
  int line = 0;
  int offset = 0;

  int resolveOffset(std::string_view sourceString) const {
    if (offset == kEndOfString) {
      return sourceString.size();
    } else {
      return offset;
    }
  }

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const ParseError& error);
};

}  // namespace donner
