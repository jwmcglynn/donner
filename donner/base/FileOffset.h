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
   * Represents line information within a file, including the line number and the character offset
   * on that line.
   */
  struct LineInfo {
    size_t line;          ///< Line number in the file, 1-based.
    size_t offsetOnLine;  ///< Character offset on the line (i.e. column index), 0-based.

    /// Ostream operator for printing the line information, prints `line:offset`.
    friend std::ostream& operator<<(std::ostream& os, const LineInfo& value) {
      return os << value.line << ":" << value.offsetOnLine;
    }

    /// Equality operator.
    bool operator<=>(const LineInfo&) const = default;
  };

  /**
   * Create a FileOffset for a single-line string.
   *
   * @param offset Character offset of the error in the string.
   */
  static FileOffset Offset(size_t offset) { return FileOffset{offset, std::nullopt}; }

  /**
   * Create a FileOffset for a multi-line string.
   *
   * @param line Line number of the error.
   * @param offset Character offset of the error in the string.
   */
  static FileOffset OffsetWithLineInfo(size_t offset, LineInfo lineInfo) {
    return FileOffset{offset, lineInfo};
  }

  /**
   * Indicates an error occurred at the end of the input string.
   */
  static FileOffset EndOfString() { return FileOffset{std::nullopt, std::nullopt}; }

  /// Character offset of the error in the string, if known.
  std::optional<size_t> offset;

  /// Line information for multi-line strings, if known.
  std::optional<LineInfo> lineInfo;

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
      return FileOffset{sourceString.size(), std::nullopt};
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
    // TODO: Change this to assert offset has a value, and update callers to call resolveOffset
    // first.
    const size_t selfOffset = offset.value_or(0);

    std::optional<LineInfo> newLineInfo;
    if (parentOffset.lineInfo.has_value()) {
      if (lineInfo.has_value()) {
        if (lineInfo->line == 1) {
          newLineInfo = LineInfo{parentOffset.lineInfo->line,
                                 parentOffset.lineInfo->offsetOnLine + lineInfo->offsetOnLine};
        } else {
          newLineInfo =
              LineInfo{parentOffset.lineInfo->line + lineInfo->line - 1, lineInfo->offsetOnLine};
        }
      } else {
        // Parent has line info, but child does not.
        newLineInfo =
            LineInfo{parentOffset.lineInfo->line, parentOffset.lineInfo->offsetOnLine + selfOffset};
      }
    }

    return FileOffset{parentOffset.offset.value() + selfOffset, newLineInfo};
  }

  /// Equality operator.
  bool operator==(const FileOffset& other) const {
    return offset == other.offset && lineInfo == other.lineInfo;
  }

  /// Print the offset to an ostream.
  friend std::ostream& operator<<(std::ostream& os, const FileOffset& value) {
    os << "FileOffset[";
    if (value.lineInfo) {
      os << "line " << value.lineInfo.value();
    }

    if (value.offset) {
      if (value.lineInfo) {
        os << " ";
      }
      os << "offset ";
      os << value.offset.value();
    } else {
      os << "<eos>";
    }

    return os << "]";
  }

private:
  /**
   * Internal constructor to create a FileOffset.
   *
   * @param offset Character offset of the error in the string, or std::nullopt to indicate the
   * error is at the end of the line.
   * @param line Line-number-relative error location, or std::nullopt if that information is not
   * available.
   */
  FileOffset(std::optional<size_t> offset, std::optional<LineInfo> lineInfo)
      : offset(offset), lineInfo(lineInfo) {}
};

/**
 * Holds a selection range for a region in the source text.
 */
struct FileOffsetRange {
  base::parser::FileOffset start;  ///< Start offset.
  base::parser::FileOffset end;    ///< End offset.
};

}  // namespace donner::base::parser

// Re-export in svg and css namespaces for convenience.
namespace donner::svg::parser {
using donner::base::parser::FileOffset;
}  // namespace donner::svg::parser

namespace donner::css::parser {
using donner::base::parser::FileOffset;
}  // namespace donner::css::parser
