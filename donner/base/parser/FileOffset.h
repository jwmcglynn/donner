#pragma once
/// @file

#include <cassert>
#include <optional>
#include <ostream>

namespace donner::base::parser {

/**
 * Error context for a failed parse, such as the error reason, line, and character offset.
 */
struct FileOffset {
  /**
   * Create a FileOffset for a single-line string.
   *
   * @param offset Character offset of the error in the string.
   */
  static FileOffset Offset(size_t offset) { return FileOffset{0, offset}; }

  /**
   * Create a FileOffset for a multi-line string.
   *
   * @param line Line number of the error.
   * @param offset Character offset of the error in the string.
   */
  static FileOffset LineAndOffset(size_t line, size_t offset) { return FileOffset{line, offset}; }

  /**
   * Indicates an error occurred at the end of the input string.
   */
  static FileOffset EndOfString() { return FileOffset{0, std::nullopt}; }

  /// Line number of the error.
  size_t line = 0;

  /// Character offset of the error in the string.
  std::optional<size_t> offset = 0;

  /**
   * Return the actual offset of the error in the string, resolving the character offset to a
   * location inside the source string.
   *
   * @param sourceString Corresponding string containing the file source, used to resolve the string
   * \ref offset.
   *
   * @return Resolved offset with EndOfString() converted to the length of the source string.
   */
  FileOffset resolveOffset(std::string_view sourceString) const {
    if (!offset.has_value()) {
      return FileOffset{line, sourceString.size()};
    } else {
      return *this;
    }
  }

  /**
   * Assuming this FileOffset is from a subparser that ran on a substring of the original string,
   * take the resulting FileOffset and convert it back to absolute coordinates.
   *
   * @param parentOffset Offset of the current string in the parent parser.
   */
  [[nodiscard]] FileOffset addParentOffset(FileOffset parentOffset) const {
    assert(parentOffset.offset.has_value() && "Parent offset must be resolved.");
    assert(offset.has_value() && "Child offset must be resolved.");

    return FileOffset{line + parentOffset.line, offset.value() + parentOffset.offset.value()};
  }

  /// Equality operator.
  bool operator==(const FileOffset& other) const {
    return line == other.line && offset == other.offset;
  }

  /// Print the offset to an ostream.
  friend std::ostream& operator<<(std::ostream& os, const FileOffset& value) {
    if (value.line != 0) {
      os << "line " << value.line << ", column ";
    }

    if (value.offset.has_value()) {
      os << value.offset.value();
    } else {
      os << "<eos>";
    }

    return os;
  }

private:
  /**
   * Internal constructor to create a FileOffset.
   *
   * @param line Line number of the error, starting at 0. For errors that have not yet been mapped
   * to lines, this may be 0 and \ref offset represents the byte offset in the source file.
   * @param offset Character offset of the error in the string, or std::nullopt to indicate the
   * error is at the end of the line.
   */
  FileOffset(size_t line, std::optional<size_t> offset) : line(line), offset(offset) {}
};

}  // namespace donner::base::parser

// Re-export in svg and css namespaces for convenience.
namespace donner::svg::parser {
using donner::base::parser::FileOffset;
}  // namespace donner::svg::parser

namespace donner::css::parser {
using donner::base::parser::FileOffset;
}  // namespace donner::css::parser
