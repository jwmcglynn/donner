#pragma once
/// @file

#include <ostream>
#include <string>
#include <string_view>

namespace donner {

/**
 * Error context for a failed parse, such as the error reason, line, and character offset.
 */
struct ParseError {
  /// Magic value for \ref offset to indicate the error occurred at the end of the string.
  static constexpr int kEndOfString = -1;

  /// Error message string.
  std::string reason;
  /// Line number of the error.
  int line = 0;
  /// Character offset of the error in the string.
  int offset = 0;

  /**
   * Return the actual offset of the error in the string, resolving \ref kEndOfString to a valid
   * offset inside the provided sourceString.
   *
   * @param sourceString Corresponding string containing the file source, used to resolve the string
   * \ref offset.
   *
   * @return int Resolved offset.
   */
  int resolveOffset(std::string_view sourceString) const {
    if (offset == kEndOfString) {
      return static_cast<int>(sourceString.size());
    } else {
      return offset;
    }
  }

  /// Print the error to an ostream.
  friend std::ostream& operator<<(std::ostream& os, const ParseError& error);
};

}  // namespace donner
