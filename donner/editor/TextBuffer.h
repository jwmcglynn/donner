#pragma once
/// @file

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"

namespace donner::editor {

/**
 * Coordinates representing a position in the text buffer, using a grid-based
 * system where tabs are expanded to spaces according to the tab size setting.
 */
struct Coordinates {
  int line = 0;    //!< Line number (0-based)
  int column = 0;  //!< Column number (0-based, in spaces)

  Coordinates() = default;

  /**
   * Create a new coordinate pair at a given line and column.
   *
   * @param line Line number (0-based)
   * @param column Column number (0-based, in spaces)
   */
  Coordinates(int line, int column) : line(line), column(column) {
    assert(line >= 0);
    assert(line >= 0);
  }

  /**
   * Get invalid coordinates for error cases.
   */
  static Coordinates Invalid() {
    static Coordinates invalid(-1, -1);
    return invalid;
  }

  /// Equality operator.
  bool operator==(const Coordinates& other) const = default;

  /// Spaceship comparison operator.
  auto operator<=>(const Coordinates& other) const = default;
};

/**
 * Color indices for different syntax elements. Used with the editor's color
 * palette to determine how different parts of the text should be colored.
 */
enum class ColorIndex {
  Default,
  Keyword,
  Number,
  String,
  CharLiteral,
  Punctuation,
  Identifier,
  KnownIdentifier,
  Comment,
  MultiLineComment,
  Background,
  Cursor,
  Selection,
  ErrorMarker,
  Breakpoint,
  BreakpointOutline,
  CurrentLineIndicator,
  CurrentLineIndicatorOutline,
  LineNumber,
  CurrentLineFill,
  CurrentLineFillInactive,
  CurrentLineEdge,
  ErrorMessage,
  BreakpointDisabled,
  UserFunction,
  UserType,
  UniformVariable,
  GlobalVariable,
  LocalVariable,
  FunctionArgument,
  Max
};

/**
 * A single character in the text buffer with associated syntax highlighting
 * information.
 */
struct Glyph {
  char character;                               //!< The actual character
  ColorIndex colorIndex = ColorIndex::Default;  //!< Color to use for this character
  bool isComment : 1;                           //!< Part of a single-line comment
  bool isMultiLineComment : 1;                  //!< Part of a multi-line comment

  Glyph(char c, ColorIndex color)
      : character(c), colorIndex(color), isComment(false), isMultiLineComment(false) {}
};

/**
 * A single line of text, stored as a vector of glyphs.
 * This inherits from std::vector<Glyph> purely for convenience.
 */
struct Line : public std::vector<Glyph> {
  using Base = std::vector<Glyph>;

  // Convenience method to insert a glyph at an arbitrary iterator position
  void emplace(iterator it, char ch, ColorIndex color) { Base::insert(it, Glyph(ch, color)); }
};

namespace details {

// Counts leading whitespace (in character positions, tabs counted as tabSize)
inline int CountLeadingWhitespace(const Line& line, int tabSize) {
  int indent = 0;
  for (const auto& glyph : line) {
    if (glyph.character == ' ') {
      indent++;
    } else if (glyph.character == '\t') {
      indent += tabSize;
    } else {
      break;
    }
  }
  return indent;
}

}  // namespace details

/**
 * Manages a collection of lines (the raw text) plus related text operations
 * like insert, delete, and substring extraction.
 */
class TextBuffer {
public:
  using Lines = std::vector<Line>;

  /**
   * Create an empty text buffer.
   */
  TextBuffer() {
    lines_.emplace_back();  // Always keep at least one line
  }

  /**
   * Replace the entire buffer with new text.
   *
   * @param text The new text to load into the buffer.
   */
  void setText(std::string_view text) {
    // Clear existing state

    lines_.clear();

    // Split on newline and create lines
    size_t start = 0;
    while (start < text.size()) {
      const auto newlinePos = text.find_first_of('\n', start);
      if (newlinePos == std::string_view::npos) {
        lines_.push_back(makeLine(text.substr(start)));
        break;
      } else {
        lines_.push_back(makeLine(text.substr(start, newlinePos - start)));
        start = newlinePos + 1;  // skip the '\n'
      }
    }

    if (lines_.empty()) {
      lines_.emplace_back();  // Always keep at least one line
    }
  }

  /**
   * Get the entire text buffer as a single string, joined by newlines.
   */
  std::string getText() const {
    std::string result;
    result.reserve(estimatedSize());
    for (size_t i = 0; i < lines_.size(); i++) {
      for (auto& g : lines_[i]) {
        result.push_back(g.character);
      }
      if (i + 1 < lines_.size()) {
        result.push_back('\n');
      }
    }
    return result;
  }

  /**
   * Get the text in [start, end), inclusive of start and exclusive of end,
   * or adapt as needed for your coordinate conventions.
   */
  std::string getText(const Coordinates& start, const Coordinates& end) const {
    if (!isValidCoord(start) || !isValidCoord(end, /*exclusiveEnd=*/true)) {
      return {};
    }
    if (start == end) {
      return {};
    }

    std::string result;
    // If start.line == end.line, just grab the substring
    if (start.line == end.line) {
      auto& line = lines_[start.line];
      const int beginCol = std::min(start.column, end.column);
      const int endCol = std::max(start.column, end.column);
      for (int c = beginCol; c < endCol && c < (int)line.size(); ++c) {
        result.push_back(line[c].character);
      }
      return result;
    }
    // Otherwise, grab partial from the first line
    {
      auto& line = lines_[start.line];
      for (int c = start.column; c < (int)line.size(); ++c) {
        result.push_back(line[c].character);
      }
    }
    // Middle lines
    for (int l = start.line + 1; l < end.line; ++l) {
      auto& line = lines_[l];
      result.push_back('\n');
      for (auto& g : line) {
        result.push_back(g.character);
      }
    }
    // Last line partial
    if (start.line < end.line && end.line < (int)lines_.size()) {
      result.push_back('\n');
      auto& line = lines_[end.line];
      for (int c = 0; c < end.column && c < (int)line.size(); ++c) {
        result.push_back(line[c].character);
      }
    }
    return result;
  }

  /**
   * Get the text in a given line.
   *
   * @param line The line number to get.
   */
  const Line& getLineGlyphs(int line) const {
    UTILS_RELEASE_ASSERT(line >= 0 && line < static_cast<int>(lines_.size()));

    return lines_[line];
  }

  /**
   * Get the text in a given line.
   *
   * @param line The line number to get.
   */
  Line& getLineGlyphsMutable(int line) {
    UTILS_RELEASE_ASSERT(line >= 0 && line < static_cast<int>(lines_.size()));

    return lines_[line];
  }

  /**
   * Insert text at a given position. Splits lines, handles newlines, etc.
   *
   * @param where Coordinates where the text should be inserted.
   * @param text The text to insert (can include newlines).
   * @param indent Whether to apply special indentation logic if desired.
   * @return The number of lines inserted (not counting the original partial line).
   */
  int insertTextAt(Coordinates& /* inout */ where, std::string_view text, bool indent = false) {
    UTILS_RELEASE_ASSERT(!lines_.empty());

    // Calculate initial indentation
    int autoIndentStart =
        indent ? details::CountLeadingWhitespace(lines_[where.line], tabSize_) : 0;
    int charIndex = getCharacterIndex(where);
    int totalLines = 0;
    int autoIndent = autoIndentStart;

    for (size_t i = 0; i < text.length(); ++i) {
      UTILS_RELEASE_ASSERT(!lines_.empty());

      if (text[i] == '\r') {
        continue;
      }

      if (text[i] == '\n') {
        if (charIndex < static_cast<int>(lines_[where.line].size()) && charIndex >= 0) {
          // Split line
          auto& line = lines_[where.line];
          auto& newLine = insertLine(where.line, where.column);

          // Insert everything after charIndex into the next line
          newLine.insert(newLine.begin(), line.begin() + charIndex, line.end());
          line.erase(line.begin() + charIndex, line.end());
        } else {
          insertLine(where.line, where.column);
        }

        ++where.line;
        charIndex = 0;
        where.column = 0;
        ++totalLines;

        if (indent) {
          // Check if the next line is already indented
          bool lineIsAlreadyIndented = (i + 1 < text.length() && std::isspace(text[i + 1]) &&
                                        text[i + 1] != '\n' && text[i + 1] != '\r');

          // Check for unindent on closing brace
          auto nextNonSpace = text.find_first_not_of(" \t", i + 1);
          if (nextNonSpace != std::string_view::npos && text[nextNonSpace] == '}') {
            autoIndent = std::max(0, autoIndent - tabSize_);
          }

          int actualAutoIndent = autoIndent;
          if (lineIsAlreadyIndented) {
            actualAutoIndent = autoIndentStart;

            size_t pos = i + 1;
            while (pos < text.length() && std::isspace(text[pos]) && text[pos] != '\n' &&
                   text[pos] != '\r') {
              actualAutoIndent = std::max(0, actualAutoIndent - tabSize_);
              pos++;
            }
          }

          // Calculate indentation characters
          int tabCount = actualAutoIndent / tabSize_;
          int spaceCount = actualAutoIndent - tabCount * tabSize_;
          if (expandTabsToSpaces_) {
            tabCount = 0;
            spaceCount = actualAutoIndent;
          }

          charIndex = tabCount + spaceCount;
          where.column = actualAutoIndent;

          // Insert indentation characters
          while (spaceCount-- > 0) {
            lines_[where.line].insert(lines_[where.line].begin(), Glyph(' ', ColorIndex::Default));
          }
          while (tabCount-- > 0) {
            lines_[where.line].insert(lines_[where.line].begin(), Glyph('\t', ColorIndex::Default));
          }
        }
      } else {
        char currentChar = text[i];
        bool isTab = (currentChar == '\t');
        auto& line = lines_[where.line];
        auto charLen = Utf8::SequenceLength(currentChar);

        if (charIndex > (int)line.size()) {
          charIndex = (int)line.size();
        }

        while (charLen-- > 0 && i < text.length()) {
          line.insert(line.begin() + charIndex++, Glyph(text[i++], ColorIndex::Default));
        }
        --i;  // Adjust for outer loop increment

        where.column += (isTab ? tabSize_ : 1);
      }
    }

    return totalLines;
  }

  /**
   * Remove text in [start, end). Merges lines if needed.
   *
   * Example:
   * ```
   * Line0
   * Line1
   * Line2
   * ```
   *
   * If you remove 'Line0' entirely, you'd pass start.column=0 and end.column=5.
   * If you want to remove multi-line, for example removing `0Lin`, pass
   * start (0,4) and end (1,3).
   *
   * @param start The start of the range to remove (inclusive).
   * @param end The end of the range to remove (exclusive).
   */
  void deleteRange(const Coordinates& start, const Coordinates& end) {
    UTILS_RELEASE_ASSERT(isValidCoord(start));
    UTILS_RELEASE_ASSERT(isValidCoord(end, /*exclusiveEnd=*/true));
    UTILS_RELEASE_ASSERT(end >= start);

    if (start == end) {
      return;
    }

    Coordinates actualEnd = end;
    if (end.column == getLineMaxColumn(end.line) + 1) {
      actualEnd.line++;
      actualEnd.column = 0;
    }

    const int startIndex = getCharacterIndex(start);
    const int endIndex = getCharacterIndex(actualEnd);

    // If single-line
    if (start.line == actualEnd.line) {
      Line& line = lines_[start.line];
      line.erase(line.begin() + startIndex, line.begin() + endIndex);

    } else {
      // Delete across multiple lines
      Line& firstLine = lines_[start.line];
      // Handle first partial line
      firstLine.erase(firstLine.begin() + startIndex, firstLine.end());

      if (actualEnd.line < lines_.size() && !lines_[actualEnd.line].empty()) {
        Line& lastLine = lines_[actualEnd.line];

        lastLine.erase(lastLine.begin(), lastLine.begin() + endIndex);
        // Merge first and last line
        firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());
      }

      // Remove everything between
      removeLine(start.line + 1, actualEnd.line + 1);
    }
  }

  /**
   * Insert an empty line at index, or optionally split at a column in an
   * existing line. Returns a reference to the newly inserted line.
   */
  Line& insertLine(int index, int column = 0) {
    if (index < 0) {
      index = 0;
    }
    if (index > (int)lines_.size()) {
      index = (int)lines_.size();
    }
    if (column > 0 && index < (int)lines_.size()) {
      // Split existing line at column
      auto& oldLine = lines_[index];
      if (column > (int)oldLine.size()) {
        column = (int)oldLine.size();
      }
      Line newLine;
      newLine.insert(newLine.end(), oldLine.begin() + column, oldLine.end());
      oldLine.erase(oldLine.begin() + column, oldLine.end());
      lines_.insert(lines_.begin() + index + 1, std::move(newLine));
      return lines_[index + 1];
    } else {
      // Insert a fresh blank line
      Line blankLine;
      lines_.insert(lines_.begin() + index, blankLine);
      return lines_[index];
    }
  }

  /**
   * Remove a single line at the given index if valid.
   */
  void removeLine(int index) {
    if (index < 0 || index >= (int)lines_.size()) {
      return;
    }
    lines_.erase(lines_.begin() + index);
    if (lines_.empty()) {
      lines_.emplace_back();
    }
  }

  /**
   * Remove lines in the range [start, end).
   */
  void removeLine(int start, int end) {
    if (start < 0) {
      start = 0;
    }
    if (end > (int)lines_.size()) {
      end = (int)lines_.size();
    }
    if (start >= end) {
      return;
    }
    lines_.erase(lines_.begin() + start, lines_.begin() + end);
    if (lines_.empty()) {
      lines_.emplace_back();
    }
  }

  /**
   * Query the max column index of a given line (i.e. length of that line).
   */
  int getLineMaxColumn(int line) const {
    if (line >= static_cast<int>(lines_.size())) {
      return 0;
    }

    const auto& lineContent = lines_[line];
    int col = 0;
    for (unsigned i = 0; i < lineContent.size();) {
      char c = lineContent[i].character;
      if (c == '\t') {
        col = (col / tabSize_) * tabSize_ + tabSize_;
      } else {
        col++;
      }
      i += Utf8::SequenceLength(c);
    }

    return col;
  }

  /**
   * Query how many characters (glyphs) are on a given line.
   */
  int getLineCharacterCount(int line) const {
    if (line >= static_cast<int>(lines_.size())) {
      return 0;
    }

    const auto& lineContent = lines_[line];
    int count = 0;
    for (unsigned i = 0; i < lineContent.size(); count++) {
      i += Utf8::SequenceLength(lineContent[i].character);
    }

    return count;
  }

  /**
   * Convert (line, column) to the internal character index within its line.
   */
  int getCharacterIndex(const Coordinates& coords) const {
    // TODO: Debug issues with coordinate conversion
    // UTILS_RELEASE_ASSERT(isValidCoord(coords, /*exclusiveEnd=*/true));

    const auto& line = lines_[coords.line];
    int col = 0;
    int index = 0;

    while (index < static_cast<int>(line.size()) && col < coords.column) {
      if (line[index].character == '\t') {
        col = (col / tabSize_) * tabSize_ + tabSize_;
      } else {
        ++col;
      }
      index += Utf8::SequenceLength(line[index].character);
    }

    return index;
  }

  /**
   * Given a line and character index within that line, return the 'visual' column.
   * If your editor expands tabs, you could calculate offset. Placeholder here.
   */
  int getCharacterColumn(int line, int index) const {
    if (line >= static_cast<int>(lines_.size())) {
      return 0;
    }

    const auto& lineContent = lines_[line];
    int col = 0;
    int i = 0;

    while (i < index && i < static_cast<int>(lineContent.size())) {
      char c = lineContent[i].character;
      i += Utf8::SequenceLength(c);
      if (c == '\t') {
        col = (col / tabSize_) * tabSize_ + tabSize_;
      } else {
        col++;
      }
    }

    return col;
  }

  /**
   * The total number of lines in the buffer.
   */
  int getTotalLines() const { return (int)lines_.size(); }

private:
  /// Text split into lines.
  Lines lines_;

  /// Number of spaces per tab character.
  int tabSize_ = 2;

  /// Whether or not tabs are converted to spaces on insert.
  // TODO: Make this settable from the public API.
  bool expandTabsToSpaces_ = true;

  /**
   * Helper: turn a string_view into a Line of Glyphs.
   */
  static Line makeLine(std::string_view text) {
    Line line;
    line.reserve(text.size());
    for (char c : text) {
      line.emplace_back(c, ColorIndex::Default);
    }
    return line;
  }

  /**
   * Helper: estimate final size of getText() so we can reserve().
   */
  size_t estimatedSize() const {
    size_t result = 0;
    for (auto& l : lines_) {
      result += l.size() + 1;  // plus a newline
    }
    return result;
  }

  /**
   * Check if a coordinate is valid.
   *
   * @param coord The coordinate to check.
   * @param exclusiveEnd If true, the coordinate is considered valid if it is one past the end, to
   * match iterator semantics where \c end() is not included.
   */
  bool isValidCoord(const Coordinates& coord, bool exclusiveEnd = false) const {
    if (coord.line >= 0 && coord.line < (int)lines_.size()) {
      if (coord.column >= 0 && coord.column <= (int)lines_[coord.line].size()) {
        return true;
      } else if (exclusiveEnd && coord.column == (int)lines_[coord.line].size() + 1) {
        return true;
      }
    }

    return false;
  }

  /**
   * Insert a substring (no newlines) directly into the current line at the
   * given coordinate, shifting glyphs to the right.
   */
  void insertSubstr(Coordinates& where, std::string_view text, bool /*indent*/) {
    auto& line = lines_[where.line];
    // Bound the column
    where.column = std::min(where.column, (int)line.size());
    // Insert each character
    for (char c : text) {
      line.emplace(line.begin() + where.column, c, ColorIndex::Default);
      where.column++;
    }
  }

  /**
   * Insert a newline at the given coordinate (splits the line).
   */
  void newLine(Coordinates& where) {
    auto& oldLine = lines_[where.line];
    // Everything after 'where.column' goes to the new line
    Line newLine;
    newLine.insert(newLine.end(), oldLine.begin() + where.column, oldLine.end());
    oldLine.erase(oldLine.begin() + where.column, oldLine.end());

    // Insert the new line after the current one
    lines_.insert(lines_.begin() + where.line + 1, std::move(newLine));
    where.line++;
    where.column = 0;
  }
};

}  // namespace donner::editor
