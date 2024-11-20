#pragma once
/// @file

#include "third_party/ImTextEdit/ImTextEdit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <regex>
#include <stack>
#include <string>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"

namespace donner::editor {

namespace {

/**
 * Compare two ranges for equality using a binary predicate.
 */
template <typename InputIt1, typename InputIt2, typename BinaryPredicate>
bool rangesEqual(InputIt1 first1, InputIt1 last1, InputIt2 first2,
                 InputIt2 last2, BinaryPredicate pred) {
  for (; first1 != last1 && first2 != last2; ++first1, ++first2) {
    if (!pred(*first1, *first2)) {
      return false;
    }
  }
  return first1 == last1 && first2 == last2;
}

/**
 * Check if a character is a bracket and what type.
 *
 * @return 0 if not a bracket, 1 if opening bracket, 2 if closing bracket
 */
int getBracketType(char c) {
  switch (c) {
  case '(':
  case '[':
  case '{':
  case '<':
    return 1;
  case ')':
  case ']':
  case '}':
  case '>':
    return 2;
  default:
    return 0;
  }
}

/**
 * Check if two brackets form a matching closing pair.
 */
bool isMatchingClosingBracket(char open, char close) {
  return (open == '{' && close == '}') || (open == '[' && close == ']') ||
         (open == '(' && close == ')') || (open == '<' && close == '>');
}

/**
 * Check if two brackets form a matching opening pair.
 */
bool isMatchingOpeningBracket(char close, char open) {
  return (close == '}' && open == '{') || (close == ']' && open == '[') ||
         (close == ')' && open == '(') || (close == '>' && open == '<');
}

} // namespace

TextEditor::TextEditor() {
  // Initialize with default dark color scheme
  setPalette(getDarkPalette());
  setLanguageDefinition(LanguageDefinition::SVG());

  // Start with empty document
  lines_.push_back(Line());

  // Set keyboard shortcuts
  shortcuts_ = getDefaultShortcuts();

  // Store creation time
  startTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
}

TextEditor::~TextEditor() = default;
std::vector<TextEditor::Shortcut> TextEditor::getDefaultShortcuts() {
  std::vector<Shortcut> shortcuts;
  shortcuts.resize(static_cast<int>(ShortcutId::Count));

  // Basic navigation
  shortcuts[static_cast<int>(ShortcutId::MoveUp)] =
      Shortcut(ImGuiKey_UpArrow); // Up Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectUp)] = Shortcut(
      ImGuiKey_UpArrow, ImGuiKey_None, false, false, true); // Shift + Up Arrow
  shortcuts[static_cast<int>(ShortcutId::MoveDown)] =
      Shortcut(ImGuiKey_DownArrow); // Down Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectDown)] =
      Shortcut(ImGuiKey_DownArrow, ImGuiKey_None, false, false,
               true); // Shift + Down Arrow
  shortcuts[static_cast<int>(ShortcutId::MoveLeft)] =
      Shortcut(ImGuiKey_LeftArrow); // Left Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectLeft)] =
      Shortcut(ImGuiKey_LeftArrow, ImGuiKey_None, false, false,
               true); // Shift + Left Arrow
  shortcuts[static_cast<int>(ShortcutId::MoveWordLeft)] = Shortcut(
      ImGuiKey_LeftArrow, ImGuiKey_None, false, true); // Ctrl + Left Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectWordLeft)] =
      Shortcut(ImGuiKey_LeftArrow, ImGuiKey_None, false, true,
               true); // Ctrl + Shift + Left Arrow
  shortcuts[static_cast<int>(ShortcutId::MoveRight)] =
      Shortcut(ImGuiKey_RightArrow); // Right Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectRight)] =
      Shortcut(ImGuiKey_RightArrow, ImGuiKey_None, false, false,
               true); // Shift + Right Arrow
  shortcuts[static_cast<int>(ShortcutId::MoveWordRight)] = Shortcut(
      ImGuiKey_RightArrow, ImGuiKey_None, false, true); // Ctrl + Right Arrow
  shortcuts[static_cast<int>(ShortcutId::SelectWordRight)] =
      Shortcut(ImGuiKey_RightArrow, ImGuiKey_None, false, true,
               true); // Ctrl + Shift + Right Arrow

  // Block navigation
  shortcuts[static_cast<int>(ShortcutId::MoveUpBlock)] =
      Shortcut(ImGuiKey_PageUp); // Page Up
  shortcuts[static_cast<int>(ShortcutId::SelectUpBlock)] = Shortcut(
      ImGuiKey_PageUp, ImGuiKey_None, false, false, true); // Shift + Page Up
  shortcuts[static_cast<int>(ShortcutId::MoveDownBlock)] =
      Shortcut(ImGuiKey_PageDown); // Page Down
  shortcuts[static_cast<int>(ShortcutId::SelectDownBlock)] =
      Shortcut(ImGuiKey_PageDown, ImGuiKey_None, false, false,
               true); // Shift + Page Down

  // Document navigation
  shortcuts[static_cast<int>(ShortcutId::MoveTop)] =
      Shortcut(ImGuiKey_Home, ImGuiKey_None, false, true); // Ctrl + Home
  shortcuts[static_cast<int>(ShortcutId::SelectTop)] = Shortcut(
      ImGuiKey_Home, ImGuiKey_None, false, true, true); // Ctrl + Shift + Home
  shortcuts[static_cast<int>(ShortcutId::MoveBottom)] =
      Shortcut(ImGuiKey_End, ImGuiKey_None, false, true); // Ctrl + End
  shortcuts[static_cast<int>(ShortcutId::SelectBottom)] = Shortcut(
      ImGuiKey_End, ImGuiKey_None, false, true, true); // Ctrl + Shift + End
  shortcuts[static_cast<int>(ShortcutId::MoveStartLine)] =
      Shortcut(ImGuiKey_Home); // Home
  shortcuts[static_cast<int>(ShortcutId::SelectStartLine)] = Shortcut(
      ImGuiKey_Home, ImGuiKey_None, false, false, true); // Shift + Home
  shortcuts[static_cast<int>(ShortcutId::MoveEndLine)] =
      Shortcut(ImGuiKey_End); // End
  shortcuts[static_cast<int>(ShortcutId::SelectEndLine)] =
      Shortcut(ImGuiKey_End, ImGuiKey_None, false, false, true); // Shift + End

  // Editing operations
  shortcuts[static_cast<int>(ShortcutId::Undo)] =
      Shortcut(ImGuiKey_Z, ImGuiKey_None, false, true); // Ctrl + Z
  shortcuts[static_cast<int>(ShortcutId::Redo)] =
      Shortcut(ImGuiKey_Y, ImGuiKey_None, false, true); // Ctrl + Y
  shortcuts[static_cast<int>(ShortcutId::DeleteRight)] = Shortcut(
      ImGuiKey_Delete, ImGuiKey_None, false, false, true); // Shift + Delete
  shortcuts[static_cast<int>(ShortcutId::ForwardDelete)] =
      Shortcut(ImGuiKey_Delete); // Delete
  shortcuts[static_cast<int>(ShortcutId::ForwardDeleteWord)] =
      Shortcut(ImGuiKey_Delete, ImGuiKey_None, false, true); // Ctrl + Delete
  shortcuts[static_cast<int>(ShortcutId::BackwardDelete)] =
      Shortcut(ImGuiKey_Backspace); // Backspace
  shortcuts[static_cast<int>(ShortcutId::BackwardDeleteWord)] = Shortcut(
      ImGuiKey_Backspace, ImGuiKey_None, false, true); // Ctrl + Backspace
  shortcuts[static_cast<int>(ShortcutId::DeleteLeft)] =
      Shortcut(ImGuiKey_Backspace, ImGuiKey_None, false, false,
               true); // Shift + Backspace

  // Special operations
  shortcuts[static_cast<int>(ShortcutId::NewLine)] =
      Shortcut(ImGuiKey_Enter); // Enter
  shortcuts[static_cast<int>(ShortcutId::Copy)] =
      Shortcut(ImGuiKey_C, ImGuiKey_None, false, true); // Ctrl + C
  shortcuts[static_cast<int>(ShortcutId::Paste)] =
      Shortcut(ImGuiKey_V, ImGuiKey_None, false, true); // Ctrl + V
  shortcuts[static_cast<int>(ShortcutId::Cut)] =
      Shortcut(ImGuiKey_X, ImGuiKey_None, false, true); // Ctrl + X
  shortcuts[static_cast<int>(ShortcutId::SelectAll)] =
      Shortcut(ImGuiKey_A, ImGuiKey_None, false, true); // Ctrl + A

  // Code editing
  shortcuts[static_cast<int>(ShortcutId::Indent)] =
      Shortcut(ImGuiKey_Tab); // Tab
  shortcuts[static_cast<int>(ShortcutId::Unindent)] =
      Shortcut(ImGuiKey_Tab, ImGuiKey_None, false, false, true); // Shift + Tab
  shortcuts[static_cast<int>(ShortcutId::DuplicateLine)] =
      Shortcut(ImGuiKey_D, ImGuiKey_None, false, true); // Ctrl + D
  shortcuts[static_cast<int>(ShortcutId::CommentLines)] = Shortcut(
      ImGuiKey_K, ImGuiKey_None, false, true, true); // Ctrl + Shift + K
  shortcuts[static_cast<int>(ShortcutId::UncommentLines)] = Shortcut(
      ImGuiKey_U, ImGuiKey_None, false, true, true); // Ctrl + Shift + U

  // Search and replace
  shortcuts[static_cast<int>(ShortcutId::Find)] =
      Shortcut(ImGuiKey_F, ImGuiKey_None, false, true); // Ctrl + F
  shortcuts[static_cast<int>(ShortcutId::Replace)] =
      Shortcut(ImGuiKey_H, ImGuiKey_None, false, true); // Ctrl + H
  shortcuts[static_cast<int>(ShortcutId::FindNext)] =
      Shortcut(ImGuiKey_G, ImGuiKey_None, false, true); // Ctrl + G

  // Autocomplete
  shortcuts[static_cast<int>(ShortcutId::AutocompleteOpen)] =
      Shortcut(ImGuiKey_I, ImGuiKey_None, false, true); // Ctrl + I
  shortcuts[static_cast<int>(ShortcutId::AutocompleteSelect)] =
      Shortcut(ImGuiKey_Tab); // Tab
  shortcuts[static_cast<int>(ShortcutId::AutocompleteSelectActive)] =
      Shortcut(ImGuiKey_Enter); // Enter
  shortcuts[static_cast<int>(ShortcutId::AutocompleteUp)] =
      Shortcut(ImGuiKey_UpArrow); // Up Arrow
  shortcuts[static_cast<int>(ShortcutId::AutocompleteDown)] =
      Shortcut(ImGuiKey_DownArrow); // Down Arrow

  return shortcuts;
}
void TextEditor::setLanguageDefinition(const LanguageDefinition &langDef) {
  languageDefinition_ = langDef;
  regexList_.clear();

  // Build optimized regex list for syntax highlighting
  for (const auto &regex : languageDefinition_.tokenRegexStrings) {
    regexList_.emplace_back(
        std::regex(regex.first, std::regex_constants::optimize), regex.second);
  }

  colorize();
}

void TextEditor::setPalette(const Palette &value) { paletteBase_ = value; }

RcString TextEditor::getText(const Coordinates &start,
                             const Coordinates &end) const {
  if (lines_.empty()) {
    return RcString();
  }

  // Calculate start and end positions
  const int startLine = start.line;
  const int endLine = end.line;
  int startIndex = getCharacterIndex(start);
  const int endIndex = getCharacterIndex(end);

  // Estimate initial string size
  size_t totalSize = 0;
  for (int i = startLine; i < endLine; i++) {
    totalSize += lines_[i].size();
  }
  totalSize += totalSize / 8; // Extra space for newlines

  RcString result;
  result.reserve(totalSize);

  // Build result string
  while (startIndex < endIndex || startLine < endLine) {
    if (startLine >= static_cast<int>(lines_.size())) {
      break;
    }

    const Line &line = lines_[startLine];
    if (startIndex < static_cast<int>(line.size())) {
      result += line[startIndex].character;
      startIndex++;
    } else {
      startIndex = 0;
      // Add newline unless this is the last line and we're at the end
      if (!(startLine == endLine - 1 && endIndex == -1)) {
        result += '\n';
      }
      ++startLine;
    }
  }

  return result;
}

Coordinates TextEditor::getActualCursorCoordinates() const {
  return sanitizeCoordinates(state_.cursorPosition);
}

Coordinates TextEditor::sanitizeCoordinates(const Coordinates &value) const {
  int line = value.line;
  int column = value.column;

  // Handle out of bounds line numbers
  if (line >= static_cast<int>(lines_.size())) {
    if (lines_.empty()) {
      line = 0;
      column = 0;
    } else {
      line = static_cast<int>(lines_.size()) - 1;
      column = getLineMaxColumn(line);
    }
    return Coordinates(line, column);
  }

  // Clamp column to valid range
  column = std::max(
      0, lines_.empty() ? 0 : std::min(column, getLineMaxColumn(line)));

  return Coordinates(line, column);
}

void TextEditor::advance(Coordinates &coords) const {
  if (coords.line >= static_cast<int>(lines_.size())) {
    return;
  }

  const auto &line = lines_[coords.line];
  const auto charIndex = getCharacterIndex(coords);

  if (charIndex + 1 < static_cast<int>(line.size())) {
    // Get length of current UTF-8 sequence at position
    int delta = Utf8::SequenceLength(line[charIndex].character);
    const int newIndex =
        std::min(charIndex + delta, static_cast<int>(line.size()) - 1);
    coords.column = getCharacterColumn(coords.line, newIndex);
  } else {
    // Move to start of next line
    ++coords.line;
    coords.column = 0;
  }
}

void TextEditor::deleteRange(const Coordinates &start, const Coordinates &end) {
  UTILS_RELEASE_ASSERT(end >= start);

  if (end == start) {
    return;
  }

  auto startIndex = getCharacterIndex(start);
  auto endIndex = getCharacterIndex(end);

  if (start.line == end.line) {
    // Delete within single line
    auto &line = lines_[start.line];
    auto maxColumn = getLineMaxColumn(start.line);

    if (end.column >= maxColumn) {
      line.erase(line.begin() + startIndex, line.end());
    } else {
      line.erase(line.begin() + startIndex, line.begin() + endIndex);
    }

    removeFolds(start, end);
  } else {
    // Delete across multiple lines
    auto &firstLine = lines_[start.line];
    auto &lastLine = lines_[end.line];

    removeFolds(start, end);

    firstLine.erase(firstLine.begin() + startIndex, firstLine.end());
    lastLine.erase(lastLine.begin(), lastLine.begin() + endIndex);

    if (start.line < end.line) {
      firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());
      removeLine(start.line + 1, end.line + 1);
    }
  }

  // Update scrollbar markers if enabled
  if (scrollbarMarkers_) {
    for (int i = 0; i < changedLines_.size(); i++) {
      if (changedLines_[i] > end.line) {
        changedLines_[i] -= (end.line - start.line);
      } else if (changedLines_[i] > start.line && changedLines_[i] < end.line) {
        changedLines_.erase(changedLines_.begin() + i);
        i--;
      }
    }
  }

  textChanged_ = true;
  if (onContentUpdate) {
    onContentUpdate(this);
  }
}

namespace {

// Counts leading whitespace (in character positions, tabs counted as tabSize)
int countLeadingWhitespace(const TextEditor::Line &line, int tabSize) {
  int indent = 0;
  for (const auto &glyph : line) {
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

// Finds the first non-whitespace character in a string view
const char *findFirstNonSpace(const char *text) {
  while (*text != '\0' && std::isspace(*text) && *text != '\n') {
    text++;
  }
  return text;
}

} // namespace

int TextEditor::insertTextAt(Coordinates & /* inout */ where, const char *text,
                             bool indent) {
  UTILS_RELEASE_ASSERT(!lines_.empty());

  // Calculate initial indentation
  int autoIndentStart =
      indent ? countLeadingWhitespace(lines_[where.line], tabSize_) : 0;

  int charIndex = getCharacterIndex(where);
  int totalLines = 0;
  int autoIndent = autoIndentStart;

  while (*text != '\0') {
    UTILS_RELEASE_ASSERT(!lines_.empty());

    if (*text == '\r') {
      ++text;
      continue;
    }

    if (*text == '\n') {
      if (charIndex < static_cast<int>(lines_[where.line].size()) &&
          charIndex >= 0) {
        // Split line
        auto &newLine = insertLine(where.line + 1, where.column);
        auto &line = lines_[where.line];
        newLine.insert(newLine.begin(), line.begin() + charIndex, line.end());
        line.erase(line.begin() + charIndex, line.end());

        // Update fold positions
        for (auto &fold : foldBegin_) {
          if (fold.line == where.line + 1 && fold.column >= where.column) {
            fold.column = std::max<int>(0, fold.column - where.column);
          }
        }
        for (auto &fold : foldEnd_) {
          if (fold.line == where.line + 1 && fold.column >= where.column) {
            fold.column = std::max<int>(0, fold.column - where.column);
          }
        }
      } else {
        insertLine(where.line + 1, where.column);
      }

      ++where.line;
      charIndex = 0;
      where.column = 0;
      ++totalLines;
      ++text;

      if (indent) {
        bool lineIsAlreadyIndented = (*text != '\0' && std::isspace(*text) &&
                                      *text != '\n' && *text != '\r');

        // Check for unindent on closing brace
        const char *nextNonSpace = findFirstNonSpace(text);
        if (*nextNonSpace == '}') {
          autoIndent = std::max(0, autoIndent - tabSize_);
        }

        int actualAutoIndent = autoIndent;
        if (lineIsAlreadyIndented) {
          actualAutoIndent = autoIndentStart;

          const char *textCopy = text;
          while (*textCopy != '\0' && std::isspace(*textCopy) &&
                 *textCopy != '\n' && *textCopy != '\r') {
            actualAutoIndent = std::max(0, actualAutoIndent - tabSize_);
            textCopy++;
          }
        }

        // Calculate indentation characters
        int tabCount = actualAutoIndent / tabSize_;
        int spaceCount = actualAutoIndent - tabCount * tabSize_;
        if (insertSpaces_) {
          tabCount = 0;
          spaceCount = actualAutoIndent;
        }

        charIndex = tabCount + spaceCount;
        where.column = actualAutoIndent;

        // Update fold positions for indentation
        for (auto &fold : foldBegin_) {
          if (fold.line == where.line && fold.column >= where.column) {
            fold.column += spaceCount + tabCount * tabSize_;
          }
        }
        for (auto &fold : foldEnd_) {
          if (fold.line == where.line && fold.column >= where.column) {
            fold.column += spaceCount + tabCount * tabSize_;
          }
        }

        // Insert indentation characters
        while (spaceCount-- > 0) {
          lines_[where.line].insert(lines_[where.line].begin(),
                                    Glyph(' ', ColorIndex::Default));
          for (auto &tag : snippetTagStart_) {
            if (tag.line == where.line) {
              tag.column++;
              snippetTagEnd_[&tag - &snippetTagStart_[0]].column++;
            }
          }
        }
        while (tabCount-- > 0) {
          lines_[where.line].insert(lines_[where.line].begin(),
                                    Glyph('\t', ColorIndex::Default));
          for (auto &tag : snippetTagStart_) {
            if (tag.line == where.line) {
              tag.column += tabSize_;
              snippetTagEnd_[&tag - &snippetTagStart_[0]].column += tabSize_;
            }
          }
        }
      }
    } else {
      char currentChar = *text;
      bool isTab = (currentChar == '\t');
      auto &line = lines_[where.line];
      auto charLen = Utf8::SequenceLength(currentChar);
      int foldOffset = 0;

      while (charLen-- > 0 && *text != '\0') {
        foldOffset += (*text == '\t') ? tabSize_ : 1;
        line.insert(line.begin() + charIndex++,
                    Glyph(*text++, ColorIndex::Default));
      }

      // Update fold positions for character insertion
      for (auto &fold : foldBegin_) {
        if (fold.line == where.line && fold.column >= where.column) {
          fold.column += foldOffset;
        }
      }
      for (auto &fold : foldEnd_) {
        if (fold.line == where.line && fold.column >= where.column) {
          fold.column += foldOffset;
        }
      }

      // Handle fold markers
      if (currentChar == '{') {
        autoIndent += tabSize_;
        foldBegin_.push_back(where);
        foldSorted_ = false;
      } else if (currentChar == '}') {
        autoIndent = std::max(0, autoIndent - tabSize_);
        foldEnd_.push_back(where);
        foldSorted_ = false;
      }

      where.column += (isTab ? tabSize_ : 1);
    }
  }

  // Track changes
  if (scrollbarMarkers_) {
    if (std::find(changedLines_.begin(), changedLines_.end(), where.line) ==
        changedLines_.end()) {
      changedLines_.push_back(where.line);
    }
  }

  textChanged_ = true;
  if (onContentUpdate) {
    onContentUpdate(this);
  }

  return totalLines;
}

void TextEditor::addUndo(UndoRecord &record) {
  undoBuffer_.resize(static_cast<size_t>(undoIndex_ + 1));
  undoBuffer_.back() = record;
  ++undoIndex_;
}

namespace {

/**
 * Handle folded lines adjustment
 */
int adjustLineForFolds(int lineNo, const std::vector<Coordinates> &foldBegin,
                       const std::vector<Coordinates> &foldEnd,
                       const std::vector<bool> &fold,
                       const std::vector<int> &foldConnection, int maxLine) {
  int foldLineStart = 0;
  int foldLineEnd = std::min(maxLine, lineNo);

  while (foldLineStart < foldLineEnd) {
    for (int i = 0; i < foldBegin.size(); i++) {
      if (foldBegin[i].line == foldLineStart && i < fold.size() && fold[i]) {
        int foldCon = foldConnection[i];
        if (foldCon != -1 && foldCon < foldEnd.size()) {
          int diff = foldEnd[foldCon].line - foldBegin[i].line;
          lineNo += diff;
          foldLineEnd = std::min(maxLine, foldLineEnd + diff);
        }
        break;
      }
    }
    foldLineStart++;
  }
  return lineNo;
}

/**
 * Calculate column position from pixel position within a line
 */
std::pair<int, int> calculateColumnPosition(const TextEditor::Line &line,
                                            float localX, float textStart,
                                            float charAdvanceX, int tabSize) {
  int columnIndex = 0;
  int columnCoord = 0;
  float columnX = 0.0f;

  while (static_cast<size_t>(columnIndex) < line.size()) {
    float columnWidth = 0.0f;

    if (line[columnIndex].character == '\t') {
      float spaceSize =
          ImGui::GetFont()
              ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ")
              .x;
      float oldX = columnX;
      float newColumnX =
          (1.0f + std::floor((1.0f + columnX) /
                             (static_cast<float>(tabSize) * spaceSize))) *
          (static_cast<float>(tabSize) * spaceSize);
      columnWidth = newColumnX - oldX;

      if (textStart + columnX + columnWidth * 0.5f > localX) {
        break;
      }
      columnX = newColumnX;
      columnCoord = (columnCoord / tabSize) * tabSize + tabSize;
      columnIndex++;
    } else {
      char buf[7];
      auto d = Utf8::SequenceLength(line[columnIndex].character);
      int i = 0;
      while (i < 6 && d-- > 0) {
        buf[i++] = line[columnIndex++].character;
      }
      buf[i] = '\0';
      columnWidth =
          ImGui::GetFont()
              ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf)
              .x;
      if (textStart + columnX + columnWidth * 0.5f > localX) {
        break;
      }
      columnX += columnWidth;
      columnCoord++;
    }
  }

  return {columnCoord, columnIndex};
}

} // namespace

Coordinates TextEditor::screenPosToCoordinates(const ImVec2 &position) const {
  ImVec2 local(position.x - uiCursorPos_.x, position.y - uiCursorPos_.y);

  int lineNo =
      std::max(0, static_cast<int>(std::floor(local.y / charAdvance_.y)));

  if (foldEnabled_) {
    lineNo =
        adjustLineForFolds(lineNo, foldBegin_, foldEnd_, fold_, foldConnection_,
                           static_cast<int>(lines_.size()) - 1);
  }

  if (lineNo >= 0 && lineNo < static_cast<int>(lines_.size())) {
    auto [columnCoord, _] = calculateColumnPosition(
        lines_[lineNo], local.x, textStart_, charAdvance_.x, tabSize_);
    return sanitizeCoordinates(Coordinates(lineNo, columnCoord));
  }

  return sanitizeCoordinates(Coordinates(lineNo, 0));
}

Coordinates TextEditor::mousePosToCoordinates(const ImVec2 &position) const {
  ImVec2 local(position.x - uiCursorPos_.x, position.y - uiCursorPos_.y);

  int lineNo =
      std::max(0, static_cast<int>(std::floor(local.y / charAdvance_.y)));
  int modifier = 0;

  if (foldEnabled_) {
    lineNo =
        adjustLineForFolds(lineNo, foldBegin_, foldEnd_, fold_, foldConnection_,
                           static_cast<int>(lines_.size()) - 1);
  }

  if (lineNo >= 0 && lineNo < static_cast<int>(lines_.size())) {
    auto [columnCoord, columnIndex] = calculateColumnPosition(
        lines_[lineNo], local.x, textStart_, charAdvance_.x, tabSize_);
    // Count tab modifiers
    for (int i = 0; i < columnIndex; i++) {
      if (lines_[lineNo][i].character == '\t') {
        modifier += 3;
      }
    }
    return sanitizeCoordinates(Coordinates(lineNo, columnCoord - modifier));
  }

  return sanitizeCoordinates(Coordinates(lineNo, 0));
}

Coordinates TextEditor::findWordStart(const Coordinates &from) const {
  if (from.line >= static_cast<int>(lines_.size())) {
    return from;
  }

  const auto &line = lines_[from.line];
  int charIndex = getCharacterIndex(from);

  if (charIndex >= static_cast<int>(line.size())) {
    return from;
  }

  // Skip trailing spaces
  while (charIndex > 0 && std::isspace(line[charIndex].character)) {
    --charIndex;
  }

  // Find word boundary
  auto startColor = static_cast<ColorIndex>(line[charIndex].colorIndex);
  while (charIndex > 0) {
    char c = line[charIndex].character;
    if ((c & 0xC0) != 0x80) { // Not UTF-8 continuation byte
      if (c <= 32 && std::isspace(c)) {
        charIndex++;
        break;
      }
      if (startColor !=
          static_cast<ColorIndex>(line[charIndex - 1].colorIndex)) {
        break;
      }
    }
    --charIndex;
  }

  return Coordinates(from.line, getCharacterColumn(from.line, charIndex));
}

Coordinates TextEditor::findWordEnd(const Coordinates &from) const {
  if (from.line >= static_cast<int>(lines_.size())) {
    return from;
  }

  const auto &line = lines_[from.line];
  int charIndex = getCharacterIndex(from);

  if (charIndex >= static_cast<int>(line.size())) {
    return from;
  }

  bool prevSpace = std::isspace(line[charIndex].character);
  auto startColor = static_cast<ColorIndex>(line[charIndex].colorIndex);

  while (charIndex < static_cast<int>(line.size())) {
    char c = line[charIndex].character;
    int charLen = Utf8::SequenceLength(c);

    if (startColor != static_cast<ColorIndex>(line[charIndex].colorIndex)) {
      break;
    }

    bool currSpace = std::isspace(c);
    if (prevSpace != currSpace) {
      if (currSpace) {
        while (charIndex < static_cast<int>(line.size()) &&
               std::isspace(line[charIndex].character)) {
          ++charIndex;
        }
      }
      break;
    }
    charIndex += charLen;
  }

  return Coordinates(from.line, getCharacterColumn(from.line, charIndex));
}
Coordinates TextEditor::findNextWord(const Coordinates &from) const {
  if (from.line >= static_cast<int>(lines_.size())) {
    return from;
  }

  Coordinates at = from;
  int charIndex = getCharacterIndex(from);
  bool isWord = false;
  bool skip = false;

  if (charIndex < static_cast<int>(lines_[at.line].size())) {
    const auto &line = lines_[at.line];
    isWord = std::isalnum(line[charIndex].character);
    skip = isWord;
  }

  while (!isWord || skip) {
    if (at.line >= static_cast<int>(lines_.size())) {
      int lastLine = std::max(0, static_cast<int>(lines_.size()) - 1);
      return Coordinates(lastLine, getLineMaxColumn(lastLine));
    }

    const auto &line = lines_[at.line];
    if (charIndex < static_cast<int>(line.size())) {
      isWord = std::isalnum(line[charIndex].character);

      if (isWord && !skip) {
        return Coordinates(at.line, getCharacterColumn(at.line, charIndex));
      }

      if (!isWord) {
        skip = false;
      }

      charIndex++;
    } else {
      charIndex = 0;
      ++at.line;
      skip = false;
      isWord = false;
    }
  }

  return at;
}

int TextEditor::getCharacterIndex(const Coordinates &coords) const {
  if (coords.line >= static_cast<int>(lines_.size())) {
    return -1;
  }

  const auto &line = lines_[coords.line];
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

int TextEditor::getCharacterColumn(int line, int index) const {
  if (line >= static_cast<int>(lines_.size())) {
    return 0;
  }

  const auto &lineContent = lines_[line];
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

int TextEditor::getLineCharacterCount(int line) const {
  if (line >= static_cast<int>(lines_.size())) {
    return 0;
  }

  const auto &lineContent = lines_[line];
  int count = 0;
  for (unsigned i = 0; i < lineContent.size(); count++) {
    i += Utf8::SequenceLength(lineContent[i].character);
  }
  return count;
}

int TextEditor::getLineMaxColumn(int line) const {
  if (line >= static_cast<int>(lines_.size())) {
    return 0;
  }

  const auto &lineContent = lines_[line];
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

bool TextEditor::isOnWordBoundary(const Coordinates &at) const {
  if (at.line >= static_cast<int>(lines_.size()) || at.column == 0) {
    return true;
  }

  const auto &line = lines_[at.line];
  int charIndex = getCharacterIndex(at);
  if (charIndex >= static_cast<int>(line.size())) {
    return true;
  }

  if (colorizerEnabled_) {
    return line[charIndex].colorIndex != line[charIndex - 1].colorIndex;
  }

  return std::isspace(line[charIndex].character) !=
         std::isspace(line[charIndex - 1].character);
}
void TextEditor::removeLine(int start, int end) {
  UTILS_RELEASE_ASSERT(end >= start);
  UTILS_RELEASE_ASSERT(lines_.size() > static_cast<size_t>(end - start));

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto &marker : errorMarkers_) {
    int line = marker.first >= start ? marker.first - 1 : marker.first;
    if (line >= start && line <= end) {
      continue;
    }
    updatedMarkers.insert({line, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  lines_.erase(lines_.begin() + start, lines_.begin() + end);
  UTILS_RELEASE_ASSERT(!lines_.empty());

  // Update scrollbar markers
  if (scrollbarMarkers_) {
    for (int i = 0; i < changedLines_.size(); i++) {
      if (changedLines_[i] > end) {
        changedLines_[i] -= (end - start);
      } else if (changedLines_[i] >= start && changedLines_[i] <= end) {
        changedLines_.erase(changedLines_.begin() + i);
        i--;
      }
    }
  }

  textChanged_ = true;
  if (onContentUpdate) {
    onContentUpdate(this);
  }
}

void TextEditor::removeLine(int index) {
  UTILS_RELEASE_ASSERT(lines_.size() > 1);

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto &marker : errorMarkers_) {
    int line = marker.first > index ? marker.first - 1 : marker.first;
    if (line - 1 == index) {
      continue;
    }
    updatedMarkers.insert({line, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  lines_.erase(lines_.begin() + index);
  UTILS_RELEASE_ASSERT(!lines_.empty());

  // Remove folds
  removeFolds(Coordinates(index, 0), Coordinates(index, 100000));

  // Update scrollbar markers
  if (scrollbarMarkers_) {
    for (int i = 0; i < changedLines_.size(); i++) {
      if (changedLines_[i] > index) {
        changedLines_[i]--;
      } else if (changedLines_[i] == index) {
        changedLines_.erase(changedLines_.begin() + i);
        i--;
      }
    }
  }

  textChanged_ = true;
  if (onContentUpdate) {
    onContentUpdate(this);
  }
}

Line &TextEditor::insertLine(int index, int column) {
  auto &result = *lines_.insert(lines_.begin() + index, Line());

  // Update fold positions
  for (auto &fold : foldBegin_) {
    if (fold.line > index - 1 ||
        (fold.line == index - 1 && fold.column >= column)) {
      fold.line++;
    }
  }

  for (auto &fold : foldEnd_) {
    if (fold.line > index - 1 ||
        (fold.line == index - 1 && fold.column >= column)) {
      fold.line++;
    }
  }

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto &marker : errorMarkers_) {
    updatedMarkers.insert(
        {marker.first >= index ? marker.first + 1 : marker.first,
         marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  return result;
}
RcString TextEditor::getWordUnderCursor() const {
  auto coords = getCursorPosition();
  coords.column = std::max(coords.column - 1, 0);
  return getWordAt(coords);
}

RcString TextEditor::getWordAt(const Coordinates &coords) const {
  auto start = findWordStart(coords);
  auto end = findWordEnd(coords);

  RcString result;

  auto startIndex = getCharacterIndex(start);
  auto endIndex = getCharacterIndex(end);

  for (auto i = startIndex; i < endIndex; ++i) {
    result.push_back(lines_[coords.line][i].character);
  }

  return result;
}

namespace {

ImU32 blendColors(ImU32 c1, ImU32 c2) const {
  return ImU32(((c1 & 0xff) + (c2 & 0xff)) / 2 |
               ((((c1 >> 8) & 0xff) + ((c2 >> 8) & 0xff)) / 2) << 8 |
               ((((c1 >> 16) & 0xff) + ((c2 >> 16) & 0xff)) / 2) << 16 |
               ((((c1 >> 24) & 0xff) + ((c2 >> 24) & 0xff)) / 2) << 24);
}

} // namespace

ImU32 TextEditor::getGlyphColor(const Glyph &glyph) const {
  if (!colorizerEnabled_) {
    return palette_[static_cast<int>(ColorIndex::Default)];
  }

  if (glyph.isComment) {
    return palette_[static_cast<int>(ColorIndex::Comment)];
  }

  if (glyph.isMultiLineComment) {
    return palette_[static_cast<int>(ColorIndex::MultiLineComment)];
  }

  auto color = palette_[static_cast<int>(glyph.colorIndex)];
  if (glyph.isPreprocessor) {
    const auto ppColor = palette_[static_cast<int>(ColorIndex::Preprocessor)];
    return blendColors(color, ppColor);
  }

  return color;
}

Coordinates TextEditor::findFirst(std::string_view searchText,
                                  const Coordinates &start) const {
  if (start.line < 0 || start.line >= static_cast<int>(lines_.size())) {
    return Coordinates(static_cast<int>(lines_.size()), 0);
  }

  RcString text =
      getText(start, Coordinates(static_cast<int>(lines_.size()), 0));

  size_t index = 0;
  size_t found = text.find(searchText);
  Coordinates current = start;

  while (found != std::string::npos) {
    // Advance to found position
    while (index < found) {
      if (text[index] == '\n') {
        current.column = 0;
        current.line++;
      } else {
        current.column++;
      }
      index++;
    }

    // Convert character index to column position
    current.column = getCharacterColumn(current.line, current.column);

    // Check if this is a word boundary match
    if (getWordAt(current) == searchText) {
      return current;
    }

    found = text.find(searchText, found + 1);
  }

  return Coordinates(static_cast<int>(lines_.size()), 0);
}

void TextEditor::handleKeyboardInputs() {
  const ImGuiIO &io = ImGui::GetIO();
  const bool shift = io.KeyShift;
  const bool ctrl = io.KeyCtrl;

  if (!ImGui::IsWindowFocused()) {
    return;
  }

  const bool editorFocused = !findFocused_ && !replaceFocused_;

  if (ImGui::IsWindowHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  }

  io.WantCaptureKeyboard = true;
  io.WantTextInput = true;

  // Handle shortcuts
  ShortcutId actionId = ShortcutId::Count;
  for (int i = 0; i < shortcuts_.size(); i++) {
    const auto &shortcut = shortcuts_[i];
    ShortcutId currentAction = ShortcutId::Count;
    bool additionalChecks = true;

    if (shortcut.matches(io)) {
      currentAction = static_cast<ShortcutId>(i);

      switch (currentAction) {
      case ShortcutId::Paste:
      case ShortcutId::Cut:
      case ShortcutId::Redo:
      case ShortcutId::Undo:
      case ShortcutId::ForwardDelete:
      case ShortcutId::BackwardDelete:
      case ShortcutId::DeleteLeft:
      case ShortcutId::DeleteRight:
      case ShortcutId::ForwardDeleteWord:
      case ShortcutId::BackwardDeleteWord:
      case ShortcutId::SelectAll:
        additionalChecks = editorFocused;
        break;

      case ShortcutId::MoveUp:
      case ShortcutId::MoveDown:
      case ShortcutId::SelectUp:
      case ShortcutId::SelectDown:
        additionalChecks = !autocompleteOpened_ && editorFocused;
        break;

      case ShortcutId::AutocompleteUp:
      case ShortcutId::AutocompleteDown:
      case ShortcutId::AutocompleteSelect:
        additionalChecks = autocompleteOpened_;
        break;

      case ShortcutId::AutocompleteSelectActive:
        additionalChecks = autocompleteOpened_ && autocompleteSwitched_;
        break;

      case ShortcutId::NewLine:
      case ShortcutId::Indent:
      case ShortcutId::Unindent:
        additionalChecks = !autocompleteOpened_ && editorFocused;
        break;

      default:
        break;
      }
    }

    if (additionalChecks && currentAction != ShortcutId::Count) {
      actionId = currentAction;
    }
  }

  bool keepAutocompleteOpen = false;
  const bool functionTooltipState = functionDeclarationTooltip_;
  bool hasWrittenLetter = false;

  if (actionId != ShortcutId::Count) {
    if (actionId != ShortcutId::Indent) {
      isSnippet_ = false;
    }

    executeAction(actionId, keepAutocompleteOpen, shift, ctrl);
  } else if (editorFocused) {
    handleCharacterInput(io, shift, keepAutocompleteOpen, hasWrittenLetter);
  }

  // Handle autocomplete
  if (requestAutocomplete_ && readyForAutocomplete_ && !isSnippet_) {
    buildSuggestions(&keepAutocompleteOpen);
    requestAutocomplete_ = false;
    readyForAutocomplete_ = false;
  }

  if ((autocompleteOpened_ && !keepAutocompleteOpen) ||
      functionDeclarationTooltip_) {
    if (hasWrittenLetter) {
      if (functionTooltipState == functionDeclarationTooltip_) {
        functionDeclarationTooltip_ = false;
      }
      autocompleteOpened_ = false;
    }
  }
}
void TextEditor::handleMouseInputs() {
  const ImGuiIO &io = ImGui::GetIO();
  const bool shift = io.KeyShift;
  const bool ctrl = io.KeyCtrl;
  const bool alt = io.KeyAlt;

  if (!ImGui::IsWindowHovered()) {
    return;
  }

  const bool click = ImGui::IsMouseClicked(0);
  if (alt || (shift && !click)) {
    return;
  }

  const bool doubleClick = ImGui::IsMouseDoubleClicked(0);
  const float currentTime = ImGui::GetTime();
  const bool tripleClick = click && !doubleClick && lastClick_ != -1.0f &&
                           (currentTime - lastClick_) < io.MouseDoubleClickTime;

  if (click || doubleClick || tripleClick) {
    isSnippet_ = false;
    functionDeclarationTooltip_ = false;
  }

  // Triple click - select line
  if (tripleClick && !ctrl) {
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ =
        screenPosToCoordinates(ImGui::GetMousePos());
    selectionMode_ = SelectionMode::Line;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    lastClick_ = -1.0f;
    return;
  }

  // Double click - select word
  if (doubleClick && !ctrl) {
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ =
        screenPosToCoordinates(ImGui::GetMousePos());
    selectionMode_ = (selectionMode_ == SelectionMode::Line)
                         ? SelectionMode::Normal
                         : SelectionMode::Word;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    state_.cursorPosition = state_.selectionEnd;
    lastClick_ = currentTime;
    return;
  }

  // Single click
  if (click) {
    const ImVec2 mousePos = ImGui::GetMousePos();

    // Handle click in line number margin
    if (mousePos.x - uiCursorPos_.x < ImGui::GetStyle().WindowPadding.x) {
      Coordinates lineInfo = screenPosToCoordinates(mousePos);
      lineInfo.line += 1;
      return;
    }

    // Normal click in text area
    autocompleteOpened_ = false;
    autocompleteObject_.clear();

    auto clickCoords = screenPosToCoordinates(mousePos);
    if (!shift) {
      interactiveStart_ = clickCoords;
    }
    state_.cursorPosition = interactiveEnd_ = clickCoords;

    selectionMode_ =
        ctrl && !shift ? SelectionMode::Word : SelectionMode::Normal;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    lastClick_ = currentTime;
    return;
  }

  // Mouse dragging
  if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0)) {
    io.WantCaptureMouse = true;
    state_.cursorPosition = interactiveEnd_ =
        screenPosToCoordinates(ImGui::GetMousePos());
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);

    // Handle horizontal scrolling during selection
    const float mouseX = ImGui::GetMousePos().x;
    if (mouseX > findOrigin_.x + windowWidth_ - 50 &&
        mouseX < findOrigin_.x + windowWidth_) {
      ImGui::SetScrollX(ImGui::GetScrollX() + 1.0f);
    } else if (mouseX > findOrigin_.x &&
               mouseX < findOrigin_.x + textStart_ + 50) {
      ImGui::SetScrollX(ImGui::GetScrollX() - 1.0f);
    }
  }
}

void TextEditor::renderInternal(std::string_view title) {
  calculateCharacterAdvance();
  updatePaletteAlpha();

  UTILS_RELEASE_ASSERT(lineBuffer_.empty());
  focused_ = ImGui::IsWindowFocused() || findFocused_ || replaceFocused_;

  const auto contentSize = ImGui::GetWindowContentRegionMax();
  auto *drawList = ImGui::GetWindowDrawList();
  float longestLine = textStart_;

  handleScrollToTop();

  const ImVec2 cursorScreenPos = uiCursorPos_ = ImGui::GetCursorScreenPos();
  const float scrollX = ImGui::GetScrollX();
  const float scrollY = lastScroll_ = ImGui::GetScrollY();

  // Calculate visible lines
  const int pageSize =
      static_cast<int>(std::floor((scrollY + contentSize.y) / charAdvance_.y));
  int lineNo = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const int totalLines = static_cast<int>(lines_.size());
  int visibleLineMax = std::max(0, std::min(totalLines - 1, lineNo + pageSize));

  updateTextStart();
  calculateFolds(lineNo, totalLines);

  while (lineNo <= visibleLineMax) {
    const ImVec2 lineStartPos{cursorScreenPos.x,
                              cursorScreenPos.y +
                                  (lineNo - foldedLines_) * charAdvance_.y};
    const ImVec2 textPos{lineStartPos.x + textStart_, lineStartPos.y};

    renderLine(lineNo, lineStartPos, textPos, contentSize, scrollX, drawList,
               longestLine);
    lineNo++;
  }

  renderExtraUI(drawList, cursorScreenPos, scrollX, scrollY, longestLine,
                contentSize);
  handleScrolling();
}

void TextEditor::calculateCharacterAdvance() {
  const float fontSize = ImGui::GetFont()
                             ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX,
                                             -1.0f, "#", nullptr, nullptr)
                             .x;
  charAdvance_ =
      ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * lineSpacing_);
}

void TextEditor::updatePaletteAlpha() {
  const float alpha = ImGui::GetStyle().Alpha;
  for (int i = 0; i < static_cast<int>(ColorIndex::Max); ++i) {
    auto color = ImGui::ColorConvertU32ToFloat4(paletteBase_[i]);
    color.w *= alpha;
    palette_[i] = ImGui::ColorConvertFloat4ToU32(color);
  }
}

void TextEditor::updateTextStart() {
  char buf[16];
  snprintf(buf, sizeof(buf), " %3d ", static_cast<int>(lines_.size()));
  textStart_ = (ImGui::GetFont()
                    ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf,
                                    nullptr, nullptr)
                    .x +
                leftMargin_) *
               sidebar_;
}

void TextEditor::calculateFolds(int currentLine, int totalLines) {
  const uint64_t currentTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (currentTime - foldLastIteration_ > 3000) {
    if (!foldSorted_) {
      std::sort(foldBegin_.begin(), foldBegin_.end());
      std::sort(foldEnd_.begin(), foldEnd_.end());
      foldSorted_ = true;
    }

    if (fold_.size() != foldBegin_.size()) {
      fold_.resize(foldBegin_.size(), false);
      foldConnection_.resize(foldBegin_.size(), -1);
    }

    std::vector<bool> foldUsed(foldEnd_.size(), false);
    for (int i = static_cast<int>(foldBegin_.size()) - 1; i >= 0; i--) {
      int j = static_cast<int>(foldEnd_.size()) - 1;
      int lastUnused = j;

      for (; j >= 0; j--) {
        if (foldEnd_[j] < foldBegin_[i]) {
          break;
        }
        if (!foldUsed[j]) {
          lastUnused = j;
        }
      }

      if (lastUnused < static_cast<int>(foldEnd_.size())) {
        foldUsed[lastUnused] = true;
        foldConnection_[i] = lastUnused;
      }
    }

    foldLastIteration_ = currentTime;
  }

  updateFoldedLines(currentLine, totalLines);
}

void TextEditor::updateFoldedLines(int currentLine, int totalLines) {
  foldedLines_ = 0;
  int foldStart = 0;
  int foldEnd = std::min(totalLines - 1, currentLine);

  while (foldStart < static_cast<int>(lines_.size())) {
    for (int i = 0; i < static_cast<int>(foldBegin_.size()); i++) {
      if (foldBegin_[i].line == foldStart &&
          i < static_cast<int>(fold_.size()) && fold_[i]) {
        int foldConn = foldConnection_[i];
        if (foldConn != -1 && foldConn < static_cast<int>(foldEnd_.size())) {
          int diff = foldEnd_[foldConn].line - foldBegin_[i].line;
          if (foldStart < foldEnd) {
            foldedLines_ += diff;
            foldEnd = std::min(totalLines - 1, foldEnd + diff);
          }
        }
        break;
      }
    }
    foldStart++;
  }
}

namespace {

void renderTabGlyph(const ImVec2 &textStart, const ImVec2 &offset, float oldX,
                    ImDrawList *drawList) {
  const float size = ImGui::GetFontSize();
  const float x1 = textStart.x + oldX + 1.0f;
  const float x2 = textStart.x + offset.x - 1.0f;
  const float y = textStart.y + offset.y + size * 0.5f;

  drawList->AddLine(ImVec2(x1, y), ImVec2(x2, y), 0x90909090);
  drawList->AddLine(ImVec2(x2, y), ImVec2(x2 - size * 0.2f, y - size * 0.2f),
                    0x90909090);
  drawList->AddLine(ImVec2(x2, y), ImVec2(x2 - size * 0.2f, y + size * 0.2f),
                    0x90909090);
}

void renderSpaceGlyph(const ImVec2 &textStart, const ImVec2 &offset,
                      ImDrawList *drawList) {
  const float size = ImGui::GetFontSize();
  const float spaceSize =
      ImGui::GetFont()
          ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ")
          .x;
  const float x = textStart.x + offset.x + spaceSize * 0.5f;
  const float y = textStart.y + offset.y + size * 0.5f;

  drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
}

} // namespace

void TextEditor::renderLine(int lineNo, const ImVec2 &lineStart,
                            const ImVec2 &textStart, const ImVec2 &contentSize,
                            float scrollX, ImDrawList *drawList,
                            float &longestLine) {
  if (lineNo >= static_cast<int>(lines_.size())) {
    return;
  }

  auto &line = lines_[lineNo];
  longestLine = std::max(textStart_ + getTextDistanceToLineStart(Coordinates(
                                          lineNo, getLineMaxColumn(lineNo))),
                         longestLine);

  // Render selection
  const Coordinates lineStartCoord(lineNo, 0);
  const Coordinates lineEndCoord(lineNo, getLineMaxColumn(lineNo));

  if (state_.selectionStart <= lineEndCoord) {
    float selStart = state_.selectionStart > lineStartCoord
                         ? getTextDistanceToLineStart(state_.selectionStart)
                         : 0.0f;
    float selEnd = -1.0f;

    if (state_.selectionEnd > lineStartCoord) {
      selEnd = getTextDistanceToLineStart(state_.selectionEnd < lineEndCoord
                                              ? state_.selectionEnd
                                              : lineEndCoord);
    }

    if (state_.selectionEnd.line > lineNo) {
      selEnd = getTextDistanceToLineStart(lineEndCoord);
    }

    if (selStart != -1.0f && selEnd != -1.0f && selStart < selEnd) {
      const ImVec2 selStartPos(lineStart.x + textStart_ + selStart,
                               lineStart.y);
      const ImVec2 selEndPos(lineStart.x + textStart_ + selEnd,
                             lineStart.y + charAdvance_.y);
      drawList->AddRectFilled(
          selStartPos, selEndPos,
          palette_[static_cast<int>(ColorIndex::Selection)]);
    }
  }

  // Render text
  auto prevColor = line.empty()
                       ? palette_[static_cast<int>(ColorIndex::Default)]
                       : getGlyphColor(line[0]);

  ImVec2 bufferOffset;
  for (size_t i = 0; i < line.size();) {
    auto &glyph = line[i];
    auto color = getGlyphColor(glyph);

    if ((color != prevColor || glyph.character == '\t' ||
         glyph.character == ' ') &&
        !lineBuffer_.empty()) {
      const ImVec2 textPos(textStart.x + bufferOffset.x,
                           textStart.y + bufferOffset.y);
      drawList->AddText(textPos, prevColor, lineBuffer_.c_str());

      auto textSize = ImGui::GetFont()->CalcTextSizeA(
          ImGui::GetFontSize(), FLT_MAX, -1.0f, lineBuffer_.c_str(), nullptr,
          nullptr);
      bufferOffset.x += textSize.x;
      lineBuffer_.clear();
    }
    prevColor = color;

    if (glyph.character == '\t') {
      const float spaceSize =
          ImGui::GetFont()
              ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ")
              .x;
      const float oldX = bufferOffset.x;
      bufferOffset.x =
          (1.0f + std::floor((1.0f + bufferOffset.x) /
                             (static_cast<float>(tabSize_) * spaceSize))) *
          (static_cast<float>(tabSize_) * spaceSize);

      if (showWhitespaces_) {
        renderTabGlyph(textStart, bufferOffset, oldX, drawList);
      }
      i++;
    } else if (glyph.character == ' ') {
      if (showWhitespaces_) {
        renderSpaceGlyph(textStart, bufferOffset, drawList);
      }
      const float spaceSize =
          ImGui::GetFont()
              ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ")
              .x;
      bufferOffset.x += spaceSize;
      i++;
    } else {
      auto charLen = Utf8::SequenceLength(glyph.character);
      while (charLen-- > 0 && i < line.size()) {
        lineBuffer_.push_back(line[i++].character);
      }
    }
  }

  if (!lineBuffer_.empty()) {
    const ImVec2 textPos(textStart.x + bufferOffset.x,
                         textStart.y + bufferOffset.y);
    drawList->AddText(textPos, prevColor, lineBuffer_.c_str());
    lineBuffer_.clear();
  }
}

namespace {

void renderErrorTooltip(int line, const std::string &message) {
  ImGui::BeginTooltip();
  ImGui::PushStyleColor(
      ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                         palette_[static_cast<int>(ColorIndex::ErrorMessage)]));
  ImGui::Text("Error at line %d:", line);
  ImGui::PopStyleColor();

  ImGui::Separator();

  ImGui::PushStyleColor(
      ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                         palette_[static_cast<int>(ColorIndex::ErrorMessage)]));
  ImGui::TextUnformatted(message.c_str());
  ImGui::PopStyleColor();

  ImGui::EndTooltip();
}

} // namespace

void TextEditor::renderLineBackground(int lineNo, const ImVec2 &lineStart,
                                      const ImVec2 &contentSize,
                                      ImDrawList *drawList) {
  const ImVec2 start{lineStart.x + ImGui::GetScrollX(), lineStart.y};

  // Error markers
  auto errorIt = errorMarkers_.find(lineNo + 1);
  if (errorIt != errorMarkers_.end()) {
    const ImVec2 end{lineStart.x + contentSize.x + 2.0f * ImGui::GetScrollX(),
                     lineStart.y + charAdvance_.y};

    drawList->AddRectFilled(
        start, end, palette_[static_cast<int>(ColorIndex::ErrorMarker)]);

    if (ImGui::IsMouseHoveringRect(lineStart, end)) {
      renderErrorTooltip(errorIt->first, errorIt->second);
    }
  }

  // Current line highlight
  if (state_.cursorPosition.line == lineNo) {
    const bool focused = ImGui::IsWindowFocused();

    if (highlightLine_ && !hasSelection()) {
      const ImVec2 end{start.x + contentSize.x + ImGui::GetScrollX(),
                       start.y + charAdvance_.y + 2.0f};

      const auto fillColor = focused ? ColorIndex::CurrentLineFill
                                     : ColorIndex::CurrentLineFillInactive;

      drawList->AddRectFilled(start, end,
                              palette_[static_cast<int>(fillColor)]);
      drawList->AddRect(start, end,
                        palette_[static_cast<int>(ColorIndex::CurrentLineEdge)],
                        1.0f);
    }
  }

  // User-defined highlighted lines
  if (highlightLine_ &&
      std::find(highlightedLines_.begin(), highlightedLines_.end(), lineNo) !=
          highlightedLines_.end()) {
    const ImVec2 end{start.x + contentSize.x + ImGui::GetScrollX(),
                     start.y + charAdvance_.y};
    drawList->AddRectFilled(
        start, end, palette_[static_cast<int>(ColorIndex::CurrentLineFill)]);
  }
}

namespace {

float calculateSelectionStart(
    const TextEditor::Coordinates &selectionStart,
    const TextEditor::Coordinates &lineStart,
    const TextEditor::Coordinates &lineEnd,
    std::function<float(const TextEditor::Coordinates &)> getDistance) {
  if (selectionStart <= lineEnd) {
    return selectionStart > lineStart ? getDistance(selectionStart) : 0.0f;
  }
  return -1.0f;
}

float calculateSelectionEnd(
    const TextEditor::Coordinates &selectionEnd,
    const TextEditor::Coordinates &lineStart,
    const TextEditor::Coordinates &lineEnd,
    std::function<float(const TextEditor::Coordinates &)> getDistance) {
  if (selectionEnd > lineStart) {
    return getDistance(selectionEnd < lineEnd ? selectionEnd : lineEnd);
  }
  return -1.0f;
}

} // namespace

void TextEditor::renderSelection(const Line &line, const ImVec2 &start,
                                 const ImVec2 &contentSize,
                                 ImDrawList *drawList) {
  const Coordinates lineStart(state_.cursorPosition.line, 0);
  const Coordinates lineEnd(state_.cursorPosition.line,
                            getLineMaxColumn(state_.cursorPosition.line));

  auto getDistance = [this](const Coordinates &coord) {
    return getTextDistanceToLineStart(coord);
  };

  const float selStart = calculateSelectionStart(
      state_.selectionStart, lineStart, lineEnd, getDistance);
  const float selEnd = calculateSelectionEnd(state_.selectionEnd, lineStart,
                                             lineEnd, getDistance);

  if (state_.selectionEnd.line > state_.cursorPosition.line) {
    selEnd += charAdvance_.x;
  }

  if (selStart != -1.0f && selEnd != -1.0f && selStart < selEnd) {
    const ImVec2 selStartPos(start.x + textStart_ + selStart, start.y);
    const ImVec2 selEndPos(start.x + textStart_ + selEnd,
                           start.y + charAdvance_.y);
    drawList->AddRectFilled(selStartPos, selEndPos,
                            palette_[static_cast<int>(ColorIndex::Selection)]);
  }
}

namespace {

void renderBufferedText(const std::string &buffer, ImVec2 &offset,
                        const ImVec2 &pos, ImDrawList *drawList, ImU32 color) {
  if (buffer.empty())
    return;

  const ImVec2 textPos(pos.x + offset.x, pos.y + offset.y);
  drawList->AddText(textPos, color, buffer.c_str());

  auto textSize = ImGui::GetFont()->CalcTextSizeA(
      ImGui::GetFontSize(), FLT_MAX, -1.0f, buffer.c_str(), nullptr, nullptr);
  offset.x += textSize.x;
}

void handleWhitespace(char c, ImVec2 &offset, const ImVec2 &pos,
                      ImDrawList *drawList, bool showWhitespaces, int tabSize) {
  const float spaceSize =
      ImGui::GetFont()
          ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ")
          .x;

  if (c == '\t') {
    const float oldX = offset.x;
    offset.x = (1.0f + std::floor((1.0f + offset.x) /
                                  (static_cast<float>(tabSize) * spaceSize))) *
               (static_cast<float>(tabSize) * spaceSize);

    if (showWhitespaces) {
      const float size = ImGui::GetFontSize();
      const float x1 = pos.x + oldX + 1.0f;
      const float x2 = pos.x + offset.x - 1.0f;
      const float y = pos.y + offset.y + size * 0.5f;

      drawList->AddLine(ImVec2(x1, y), ImVec2(x2, y), 0x90909090);
      drawList->AddLine(ImVec2(x2, y),
                        ImVec2(x2 - size * 0.2f, y - size * 0.2f), 0x90909090);
      drawList->AddLine(ImVec2(x2, y),
                        ImVec2(x2 - size * 0.2f, y + size * 0.2f), 0x90909090);
    }
  } else if (c == ' ') {
    if (showWhitespaces) {
      const float size = ImGui::GetFontSize();
      const float x = pos.x + offset.x + spaceSize * 0.5f;
      const float y = pos.y + offset.y + size * 0.5f;
      drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
    }
    offset.x += spaceSize;
  }
}

} // namespace

void TextEditor::renderText(const Line &line, const ImVec2 &pos,
                            ImDrawList *drawList) {
  auto prevColor = line.empty()
                       ? palette_[static_cast<int>(ColorIndex::Default)]
                       : getGlyphColor(line[0]);

  ImVec2 offset;
  lineBuffer_.clear();

  for (size_t i = 0; i < line.size();) {
    auto &glyph = line[i];
    auto color = getGlyphColor(glyph);

    if ((color != prevColor || glyph.character == '\t' ||
         glyph.character == ' ') &&
        !lineBuffer_.empty()) {
      renderBufferedText(lineBuffer_, offset, pos, drawList, prevColor);
      lineBuffer_.clear();
    }
    prevColor = color;

    if (glyph.character == '\t' || glyph.character == ' ') {
      handleWhitespace(glyph.character, offset, pos, drawList, showWhitespaces_,
                       tabSize_);
      i++;
    } else {
      auto charLen = Utf8::SequenceLength(glyph.character);
      while (charLen-- > 0 && i < line.size()) {
        lineBuffer_.push_back(line[i++].character);
      }
    }
  }

  if (!lineBuffer_.empty()) {
    renderBufferedText(lineBuffer_, offset, pos, drawList, prevColor);
    lineBuffer_.clear();
  }
}

void ImTextEdit::mOpenFunctionDeclarationTooltip(
    const std::string &obj, ImTextEdit::Coordinates coord) {}

void ImTextEdit::mRemoveFolds(const Coordinates &aStart,
                              const Coordinates &aEnd) {
  mRemoveFolds(mFoldBegin, aStart, aEnd);
  mRemoveFolds(mFoldEnd, aStart, aEnd);
}
void ImTextEdit::mRemoveFolds(std::vector<Coordinates> &folds,
                              const Coordinates &aStart,
                              const Coordinates &aEnd) {
  bool deleteFullyLastLine = false;
  if (aEnd.mLine >= mLines.size() || aEnd.mColumn >= 100000)
    deleteFullyLastLine = true;

  for (int i = 0; i < folds.size(); i++) {
    if (folds[i].mLine >= aStart.mLine && folds[i].mLine <= aEnd.mLine) {
      if (folds[i].mLine == aStart.mLine && aStart.mLine != aEnd.mLine) {
        if (folds[i].mColumn >= aStart.mColumn) {
          folds.erase(folds.begin() + i);
          mFoldSorted = false;
          i--;
        }
      } else if (folds[i].mLine == aEnd.mLine) {
        if (folds[i].mColumn < aEnd.mColumn) {
          if (aEnd.mLine != aStart.mLine ||
              folds[i].mColumn >= aStart.mColumn) {
            folds.erase(folds.begin() + i);
            mFoldSorted = false;
            i--;
          }
        } else {
          if (aEnd.mLine == aStart.mLine)
            folds[i].mColumn = std::max<int>(
                0, folds[i].mColumn - (aEnd.mColumn - aStart.mColumn));
          else {
            // calculate new
            if (aStart.mLine < mLines.size()) {
              auto *line = &mLines[aStart.mLine];
              int colOffset = 0;
              int chi = 0;
              bool skipped = false;
              int bracketEndChIndex = GetCharacterIndex(mFoldEnd[i]);
              while (chi < (int)line->size() &&
                     (!skipped || (skipped && chi < bracketEndChIndex))) {
                auto c = (*line)[chi].mChar;
                chi += UTF8CharLength(c);
                if (c == '\t')
                  colOffset = (colOffset / mTabSize) * mTabSize + mTabSize;
                else
                  colOffset++;

                // go to the last line
                if (chi == line->size() && aEnd.mLine < mLines.size() &&
                    !skipped) {
                  chi = GetCharacterIndex(aEnd);
                  line = &mLines[aEnd.mLine];
                  skipped = true;
                }
              }
              folds[i].mColumn = colOffset;
            }

            folds[i].mLine -= (aEnd.mLine - aStart.mLine);
          }
        }
      } else {
        folds.erase(folds.begin() + i);
        mFoldSorted = false;
        i--;
      }
    } else if (folds[i].mLine > aEnd.mLine)
      folds[i].mLine -= (aEnd.mLine - aStart.mLine) + deleteFullyLastLine;
  }
}

std::string ImTextEdit::mAutcompleteParse(const std::string &str,
                                          const Coordinates &start) {
  const char *buffer = str.c_str();
  const char *tagPlaceholderStart = buffer;
  const char *tagStart = buffer;

  bool parsingTag = false;
  bool parsingTagPlaceholder = false;

  std::vector<int> tagIds, tagLocations, tagLengths;
  std::unordered_map<int, std::string> tagPlaceholders;

  mSnippetTagStart.clear();
  mSnippetTagEnd.clear();
  mSnippetTagID.clear();
  mSnippetTagHighlight.clear();

  Coordinates cursor = start, tagStartCoord, tagEndCoord;

  int tagId = -1;

  int modif = 0;
  while (*buffer != '\0') {
    if (*buffer == '{' && *(buffer + 1) == '$') {
      parsingTagPlaceholder = false;
      parsingTag = true;
      tagId = -1;
      tagStart = buffer;

      tagStartCoord = cursor;

      const char *skipBuffer = buffer;
      char **endLoc = const_cast<char **>(&buffer); // oops
      tagId = strtol(buffer + 2, endLoc, 10);

      cursor.mColumn += *endLoc - skipBuffer;

      if (*buffer == ':') {
        tagPlaceholderStart = buffer + 1;
        parsingTagPlaceholder = true;
      }
    }

    if (*buffer == '}' && parsingTag) {
      std::string tagPlaceholder = "";
      if (parsingTagPlaceholder)
        tagPlaceholder =
            std::string(tagPlaceholderStart, buffer - tagPlaceholderStart);

      tagIds.push_back(tagId);
      tagLocations.push_back(tagStart - str.c_str());
      tagLengths.push_back(buffer - tagStart + 1);
      if (!tagPlaceholder.empty() || tagPlaceholders.count(tagId) == 0) {
        if (tagPlaceholder.empty())
          tagPlaceholder = " ";

        tagStartCoord.mColumn = std::max<int>(0, tagStartCoord.mColumn - modif);
        tagEndCoord = tagStartCoord;
        tagEndCoord.mColumn += tagPlaceholder.size();

        mSnippetTagStart.push_back(tagStartCoord);
        mSnippetTagEnd.push_back(tagEndCoord);
        mSnippetTagID.push_back(tagId);
        mSnippetTagHighlight.push_back(true);

        tagPlaceholders[tagId] = tagPlaceholder;
      } else {
        tagStartCoord.mColumn = std::max<int>(0, tagStartCoord.mColumn - modif);
        tagEndCoord = tagStartCoord;
        tagEndCoord.mColumn += tagPlaceholders[tagId].size();

        mSnippetTagStart.push_back(tagStartCoord);
        mSnippetTagEnd.push_back(tagEndCoord);
        mSnippetTagID.push_back(tagId);
        mSnippetTagHighlight.push_back(false);
      }
      modif += (tagLengths.back() - tagPlaceholders[tagId].size());

      parsingTagPlaceholder = false;
      parsingTag = false;
      tagId = -1;
    }

    if (*buffer == '\n') {
      cursor.mLine++;
      cursor.mColumn = 0;
      modif = 0;
    } else
      cursor.mColumn++;

    buffer++;
  }

  mIsSnippet = !tagIds.empty();

  std::string ret = str;
  for (int i = tagLocations.size() - 1; i >= 0; i--) {
    ret.erase(tagLocations[i], tagLengths[i]);
    ret.insert(tagLocations[i], tagPlaceholders[tagIds[i]]);
  }

  return ret;
}
void ImTextEdit::mAutocompleteSelect() {
  UndoRecord undo;
  undo.mBefore = mState;

  auto curCoord = GetCursorPosition();
  curCoord.mColumn = std::max<int>(curCoord.mColumn - 1, 0);

  auto acStart = FindWordStart(curCoord);
  auto acEnd = FindWordEnd(curCoord);

  if (!mACObject.empty())
    acStart = mACPosition;

  undo.mAddedStart = acStart;
  int undoPopCount = std::max(0, acEnd.mColumn - acStart.mColumn) + 1;

  if (!mACObject.empty() && mACWord.empty())
    undoPopCount = 0;

  const auto &acEntry = mACSuggestions[mACIndex];

  std::string entryText = mAutcompleteParse(acEntry.second, acStart);

  if (acStart.mColumn != acEnd.mColumn) {
    SetSelection(acStart, acEnd);
    Backspace();
  }
  InsertText(entryText, true);

  undo.mAdded = entryText;
  undo.mAddedEnd = GetActualCursorCoordinates();

  if (mIsSnippet && mSnippetTagStart.size() > 0) {
    SetSelection(mSnippetTagStart[0], mSnippetTagEnd[0]);
    SetCursorPosition(mSnippetTagEnd[0]);
    mSnippetTagSelected = 0;
    mSnippetTagLength = 0;
    mSnippetTagPreviousLength = mSnippetTagEnd[mSnippetTagSelected].mColumn -
                                mSnippetTagStart[mSnippetTagSelected].mColumn;
  }

  m_requestAutocomplete = false;
  mACOpened = false;
  mACObject = "";

  undo.mAfter = mState;

  while (undoPopCount-- != 0) {
    mUndoIndex--;
    mUndoBuffer.pop_back();
  }
  AddUndo(undo);
}

void ImTextEdit::m_buildMemberSuggestions(bool *keepACOpened) {
  mACSuggestions.clear();

  auto curPos = GetCorrectCursorPosition();
  std::string obj = GetWordAt(curPos);

  if (mACSuggestions.size() > 0) {
    mACOpened = true;
    mACWord = "";

    if (keepACOpened != nullptr)
      *keepACOpened = true;

    Coordinates curCursor = GetCursorPosition();

    mACPosition = FindWordStart(curCursor);
  }
}
void ImTextEdit::m_buildSuggestions(bool *keepACOpened) {
  mACWord = GetWordUnderCursor();

  bool isValid = false;
  for (int i = 0; i < mACWord.size(); i++)
    if ((mACWord[i] >= 'a' && mACWord[i] <= 'z') ||
        (mACWord[i] >= 'A' && mACWord[i] <= 'Z')) {
      isValid = true;
      break;
    }

  if (isValid) {
    mACSuggestions.clear();
    mACIndex = 0;
    mACSwitched = false;

    std::string acWord = mACWord;
    std::transform(acWord.begin(), acWord.end(), acWord.begin(), tolower);

    struct ACEntry {
      ACEntry(const std::string &str, const std::string &val, int loc) {
        DisplayString = str;
        Value = val;
        Location = loc;
      }

      std::string DisplayString;
      std::string Value;
      int Location;
    };
    std::vector<ACEntry> weights;

    if (mACObject.empty()) {
      // get the words
      for (int i = 0; i < mACEntrySearch.size(); i++) {
        std::string lwrStr = mACEntrySearch[i];
        std::transform(lwrStr.begin(), lwrStr.end(), lwrStr.begin(), tolower);

        size_t loc = lwrStr.find(acWord);
        if (loc != std::string::npos)
          weights.push_back(
              ACEntry(mACEntries[i].first, mACEntries[i].second, loc));
      }
      for (auto &str : mLanguageDefinition.mKeywords) {
        std::string lwrStr = str;
        std::transform(lwrStr.begin(), lwrStr.end(), lwrStr.begin(), tolower);

        size_t loc = lwrStr.find(acWord);
        if (loc != std::string::npos)
          weights.push_back(ACEntry(str, str, loc));
      }
      for (auto &str : mLanguageDefinition.mIdentifiers) {
        std::string lwrStr = str.first;
        std::transform(lwrStr.begin(), lwrStr.end(), lwrStr.begin(), tolower);

        size_t loc = lwrStr.find(acWord);
        if (loc != std::string::npos) {
          std::string val = str.first;
          if (mCompleteBraces)
            val += "()";
          weights.push_back(ACEntry(str.first, val, loc));
        }
      }
    }

    // build the actual list
    for (const auto &entry : weights)
      if (entry.Location == 0)
        mACSuggestions.push_back(
            std::make_pair(entry.DisplayString, entry.Value));
    for (const auto &entry : weights)
      if (entry.Location != 0)
        mACSuggestions.push_back(
            std::make_pair(entry.DisplayString, entry.Value));

    if (mACSuggestions.size() > 0) {
      mACOpened = true;

      if (keepACOpened != nullptr)
        *keepACOpened = true;

      Coordinates curCursor = GetCursorPosition();
      curCursor.mColumn--;

      mACPosition = FindWordStart(curCursor);
    }
  }
}

ImVec2 ImTextEdit::CoordinatesToScreenPos(
    const ImTextEdit::Coordinates &aPosition) const {
  ImVec2 origin = mUICursorPos;
  int dist = aPosition.mColumn;

  int retY = origin.y + aPosition.mLine * mCharAdvance.y;
  int retX = origin.x + GetTextStart() * mCharAdvance.x +
             dist * mCharAdvance.x - ImGui::GetScrollX();

  return ImVec2(retX, retY);
}

void ImTextEdit::Render(const char *aTitle, const ImVec2 &aSize, bool aBorder) {
  mWithinRender = true;
  mCursorPositionChanged = false;

  mFindOrigin = ImGui::GetCursorScreenPos();
  float windowWidth = mWindowWidth = ImGui::GetWindowWidth();

  ImGui::PushStyleColor(
      ImGuiCol_ChildBg,
      ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  if (!mIgnoreImGuiChild)
    ImGui::BeginChild(
        aTitle, aSize, aBorder,
        (ImGuiWindowFlags_AlwaysHorizontalScrollbar * mHorizontalScroll) |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);

  if (mHandleKeyboardInputs) {
    HandleKeyboardInputs();
    ImGui::PushAllowKeyboardFocus(true);
  }

  if (mHandleMouseInputs)
    HandleMouseInputs();

  ColorizeInternal();
  m_readyForAutocomplete = true;
  RenderInternal(aTitle);

  // markers
  if (mScrollbarMarkers) {
    ImGuiWindow *window = ImGui::GetCurrentWindowRead();
    if (window->ScrollbarY) {
      ImDrawList *drawList = ImGui::GetWindowDrawList();
      ImRect scrollBarRect = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);
      ImGui::PushClipRect(scrollBarRect.Min, scrollBarRect.Max, false);
      int mSelectedLine = mState.mCursorPosition.mLine;

      // current line marker
      if (mSelectedLine != 0) {
        float lineStartY = std::round(scrollBarRect.Min.y +
                                      (mSelectedLine - 0.5f) / mLines.size() *
                                          scrollBarRect.GetHeight());
        drawList->AddLine(ImVec2(scrollBarRect.Min.x, lineStartY),
                          ImVec2(scrollBarRect.Max.x, lineStartY),
                          (mPalette[(int)PaletteIndex::Default] & 0x00FFFFFFu) |
                              0x83000000u,
                          3);
      }

      // changed lines marker
      for (int line : mChangedLines) {
        float lineStartY = std::round(scrollBarRect.Min.y +
                                      (float(line) - 0.5f) / mLines.size() *
                                          scrollBarRect.GetHeight());
        float lineEndY = std::round(scrollBarRect.Min.y +
                                    (float(line + 1) - 0.5f) / mLines.size() *
                                        scrollBarRect.GetHeight());
        drawList->AddRectFilled(
            ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth() * 0.6f,
                   lineStartY),
            ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth(), lineEndY),
            0xFF8CE6F0);
      }

      // error markers
      for (auto &error : mErrorMarkers) {
        float lineStartY = std::round(
            scrollBarRect.Min.y + (float(error.first) - 0.5f) / mLines.size() *
                                      scrollBarRect.GetHeight());
        drawList->AddRectFilled(
            ImVec2(scrollBarRect.Min.x, lineStartY),
            ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth() * 0.4f,
                   lineStartY + 6.0f),
            mPalette[(int)PaletteIndex::ErrorMarker]);
      }
      ImGui::PopClipRect();
    }
  }

  if (ImGui::IsMouseClicked(1)) {
    mRightClickPos = ImGui::GetMousePos();

    if (ImGui::IsWindowHovered())
      SetCursorPosition(ScreenPosToCoordinates(mRightClickPos));
  }

  if (ImGui::BeginPopupContextItem(
          ("##edcontext" + std::string(aTitle)).c_str())) {
    if (mRightClickPos.x - mUICursorPos.x > ImGui::GetStyle().WindowPadding.x) {
      if (ImGui::Selectable("Cut")) {
        Cut();
      }
      if (ImGui::Selectable("Copy")) {
        Copy();
      }
      if (ImGui::Selectable("Paste")) {
        Paste();
      }
    }
    ImGui::EndPopup();
  }

  /* FIND TEXT WINDOW */
  if (mFindOpened) {
    ImFont *font = ImGui::GetFont();
    ImGui::PopFont();

    ImGui::SetNextWindowPos(
        ImVec2(mFindOrigin.x + windowWidth - mUICalculateSize(250),
               mFindOrigin.y),
        ImGuiCond_Always);
    ImGui::BeginChild(("##ted_findwnd" + std::string(aTitle)).c_str(),
                      ImVec2(mUICalculateSize(220),
                             mUICalculateSize(mReplaceOpened ? 90 : 40)),
                      true, ImGuiWindowFlags_NoScrollbar);

    // check for findnext shortcut here...
    ImGuiIO &io = ImGui::GetIO();
    mFindNext = m_shortcuts[(int)ImTextEdit::ShortcutID::FindNext].matches(io);

    if (mFindJustOpened) {
      mFindWord = GetSelectedText();
    }

    ImGui::PushItemWidth(mUICalculateSize(-45));

    const bool findEnterPressed =
        ImGui::InputText(("##ted_findtextbox" + std::string(aTitle)).c_str(),
                         &mFindWord, ImGuiInputTextFlags_EnterReturnsTrue);
    if (findEnterPressed || mFindNext) {
      auto curPos = mState.mCursorPosition;
      size_t cindex = 0;
      for (size_t ln = 0; ln < curPos.mLine; ln++)
        cindex += GetLineCharacterCount(ln) + 1;
      cindex += curPos.mColumn;

      std::string wordLower = mFindWord;
      std::transform(wordLower.begin(), wordLower.end(), wordLower.begin(),
                     ::tolower);

      std::string textSrc = GetText();
      std::transform(textSrc.begin(), textSrc.end(), textSrc.begin(),
                     ::tolower);

      size_t textLoc = textSrc.find(wordLower, cindex);
      if (textLoc == std::string::npos)
        textLoc = textSrc.find(wordLower, 0);

      if (textLoc != std::string::npos) {
        curPos.mLine = curPos.mColumn = 0;
        cindex = 0;
        for (size_t ln = 0; ln < mLines.size(); ln++) {
          int charCount = GetLineCharacterCount(ln) + 1;
          if (cindex + charCount > textLoc) {
            curPos.mLine = ln;
            curPos.mColumn = textLoc - cindex;

            auto &line = mLines[curPos.mLine];
            for (int i = 0; i < line.size(); i++)
              if (line[i].mChar == '\t')
                curPos.mColumn += (mTabSize - 1);

            break;
          } else { // just keep adding
            cindex += charCount;
          }
        }

        auto selEnd = curPos;
        selEnd.mColumn += mFindWord.size();
        SetSelection(curPos, selEnd);
        SetCursorPosition(selEnd);
        mScrollToCursor = true;

        if (!mFindNext) {
          ImGui::SetKeyboardFocusHere(-1);
        }
      }

      mFindNext = false;
    }

    mFindFocused = ImGui::IsItemActive();

    if (mFindJustOpened) {
      ImGui::SetKeyboardFocusHere(-1);
      mFindJustOpened = false;
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::ArrowButton(("##expandFind" + std::string(aTitle)).c_str(),
                           mReplaceOpened ? ImGuiDir_Up : ImGuiDir_Down)) {
      mReplaceOpened = !mReplaceOpened;
    }

    ImGui::SameLine();
    if (ImGui::Button(("X##" + std::string(aTitle)).c_str()))
      mFindOpened = false;

    if (mReplaceOpened) {
      ImGui::PushItemWidth(mUICalculateSize(-45));
      ImGui::NewLine();
      bool shouldReplace = false;
      if (ImGui::InputText(("##ted_replacetb" + std::string(aTitle)).c_str(),
                           &mReplaceWord, ImGuiInputTextFlags_EnterReturnsTrue))
        shouldReplace = true;
      if (ImGui::IsItemActive())
        mReplaceFocused = true;
      else
        mReplaceFocused = false;
      ImGui::PopItemWidth();

      ImGui::SameLine();
      if (ImGui::Button((">##replaceOne" + std::string(aTitle)).c_str()) ||
          shouldReplace) {
        if (!mFindWord.empty()) {
          auto curPos = mState.mCursorPosition;

          std::string textSrc = GetText();
          if (mReplaceIndex >= textSrc.size())
            mReplaceIndex = 0;
          size_t textLoc = textSrc.find(mFindWord, mReplaceIndex);
          if (textLoc == std::string::npos) {
            mReplaceIndex = 0;
            textLoc = textSrc.find(mFindWord, 0);
          }

          if (textLoc != std::string::npos) {
            curPos.mLine = curPos.mColumn = 0;
            int totalCount = 0;
            for (size_t ln = 0; ln < mLines.size(); ln++) {
              int lineCharCount = GetLineCharacterCount(ln) + 1;
              if (textLoc >= totalCount &&
                  textLoc < totalCount + lineCharCount) {
                curPos.mLine = ln;
                curPos.mColumn = textLoc - totalCount;

                auto &line = mLines[curPos.mLine];
                for (int i = 0; i < line.size(); i++)
                  if (line[i].mChar == '\t')
                    curPos.mColumn += (mTabSize - 1);

                break;
              }
              totalCount += lineCharCount;
            }

            auto selEnd = curPos;
            selEnd.mColumn += mFindWord.size();
            SetSelection(curPos, selEnd);
            DeleteSelection();
            InsertText(mReplaceWord);
            SetCursorPosition(selEnd);
            mScrollToCursor = true;

            mReplaceIndex = textLoc + mReplaceWord.size();
          }
        }
      }

      ImGui::SameLine();
      if (ImGui::Button((">>##replaceAll" + std::string(aTitle)).c_str())) {
        if (!mFindWord.empty()) {
          auto curPos = mState.mCursorPosition;

          std::string textSrc = GetText();
          size_t textLoc = textSrc.find(mFindWord, 0);

          do {
            if (textLoc != std::string::npos) {
              curPos.mLine = curPos.mColumn = 0;
              int totalCount = 0;
              for (size_t ln = 0; ln < mLines.size(); ln++) {
                int lineCharCount = GetLineCharacterCount(ln) + 1;
                if (textLoc >= totalCount &&
                    textLoc < totalCount + lineCharCount) {
                  curPos.mLine = ln;
                  curPos.mColumn = textLoc - totalCount;

                  auto &line = mLines[curPos.mLine];
                  for (int i = 0; i < line.size(); i++)
                    if (line[i].mChar == '\t')
                      curPos.mColumn += (mTabSize - 1);

                  break;
                }
                totalCount += lineCharCount;
              }

              auto selEnd = curPos;
              selEnd.mColumn += mFindWord.size();
              SetSelection(curPos, selEnd);
              DeleteSelection();
              InsertText(mReplaceWord);
              SetCursorPosition(selEnd);
              mScrollToCursor = true;

              // find next occurance
              textSrc = GetText();
              textLoc += mReplaceWord.size();
              textLoc = textSrc.find(mFindWord, textLoc);
            }
          } while (textLoc != std::string::npos);
        }
      }
    }

    ImGui::EndChild();

    ImGui::PushFont(font);

    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
      mFindOpened = false;
  }

  if (mHandleKeyboardInputs)
    ImGui::PopAllowKeyboardFocus();

  if (!mIgnoreImGuiChild)
    ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  mWithinRender = false;
}

void ImTextEdit::SetText(const std::string &aText) {
  mLines.clear();
  mFoldBegin.clear();
  mFoldEnd.clear();
  mFoldSorted = false;

  mLines.emplace_back(Line());
  for (auto chr : aText) {
    if (chr == '\r') {
      // ignore the carriage return character
    } else if (chr == '\n')
      mLines.emplace_back(Line());
    else {
      if (chr == '{')
        mFoldBegin.push_back(
            Coordinates(mLines.size() - 1, mLines.back().size()));
      else if (chr == '}')
        mFoldEnd.push_back(
            Coordinates(mLines.size() - 1, mLines.back().size()));

      mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
    }
  }

  mTextChanged = true;
  mScrollToTop = true;

  mUndoBuffer.clear();
  mUndoIndex = 0;

  Colorize();
}

void ImTextEdit::EnterCharacter(ImWchar aChar, bool aShift) {
  UndoRecord u;

  u.mBefore = mState;

  if (HasSelection()) {
    if (aChar == '\t' &&
        mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine) {
      auto start = mState.mSelectionStart;
      auto end = mState.mSelectionEnd;
      auto originalEnd = end;

      if (start > end)
        std::swap(start, end);
      start.mColumn = 0;
      //			end.mColumn = end.mLine < mLines.size() ?
      // mLines[end.mLine].size() : 0;
      if (end.mColumn == 0 && end.mLine > 0)
        --end.mLine;
      if (end.mLine >= (int)mLines.size())
        end.mLine = mLines.empty() ? 0 : (int)mLines.size() - 1;
      end.mColumn = GetLineMaxColumn(end.mLine);

      // if (end.mColumn >= GetLineMaxColumn(end.mLine))
      //	end.mColumn = GetLineMaxColumn(end.mLine) - 1;

      u.mRemovedStart = start;
      u.mRemovedEnd = end;
      u.mRemoved = GetText(start, end);

      bool modified = false;

      for (int i = start.mLine; i <= end.mLine; i++) {
        auto &line = mLines[i];
        if (aShift) {
          if (!line.empty()) {
            if (line.front().mChar == '\t') {
              line.erase(line.begin());
              modified = true;
            } else {
              for (int j = 0;
                   j < mTabSize && !line.empty() && line.front().mChar == ' ';
                   j++) {
                line.erase(line.begin());
                modified = true;
              }
            }
          }
        } else {
          if (mInsertSpaces) {
            for (int i = 0; i < mTabSize; i++)
              line.insert(line.begin(),
                          Glyph(' ', ImTextEdit::PaletteIndex::Background));
          } else
            line.insert(line.begin(),
                        Glyph('\t', ImTextEdit::PaletteIndex::Background));
          modified = true;
        }
      }

      if (modified) {
        start = Coordinates(start.mLine, GetCharacterColumn(start.mLine, 0));
        Coordinates rangeEnd;
        if (originalEnd.mColumn != 0) {
          end = Coordinates(end.mLine, GetLineMaxColumn(end.mLine));
          rangeEnd = end;
          u.mAdded = GetText(start, end);
        } else {
          end = Coordinates(originalEnd.mLine, 0);
          rangeEnd =
              Coordinates(end.mLine - 1, GetLineMaxColumn(end.mLine - 1));
          u.mAdded = GetText(start, rangeEnd);
        }

        u.mAddedStart = start;
        u.mAddedEnd = rangeEnd;
        u.mAfter = mState;

        mState.mSelectionStart = start;
        mState.mSelectionEnd = end;
        AddUndo(u);

        mTextChanged = true;
        if (OnContentUpdate != nullptr)
          OnContentUpdate(this);

        EnsureCursorVisible();
      }

      return;
    } else {
      u.mRemoved = GetSelectedText();
      u.mRemovedStart = mState.mSelectionStart;
      u.mRemovedEnd = mState.mSelectionEnd;
      DeleteSelection();
    }
  }

  auto coord = GetActualCursorCoordinates();
  u.mAddedStart = coord;

  if (mLines.empty())
    mLines.push_back(Line());

  if (aChar == '\n') {
    InsertLine(coord.mLine + 1, coord.mColumn);
    auto &line = mLines[coord.mLine];
    auto &newLine = mLines[coord.mLine + 1];
    auto cindex = GetCharacterIndex(coord);

    int foldOffset = 0;
    for (int i = 0; i < cindex; i++)
      foldOffset -= 1 + (line[i].mChar == '\t') * 3;

    if (mLanguageDefinition.mAutoIndentation && mSmartIndent)
      for (size_t it = 0; it < line.size() && isascii(line[it].mChar) &&
                          isblank(line[it].mChar);
           ++it) {
        newLine.push_back(line[it]);
        foldOffset += 1 + (line[it].mChar == '\t') * 3;
      }

    const size_t whitespaceSize = newLine.size();
    newLine.insert(newLine.end(), line.begin() + cindex, line.end());
    line.erase(line.begin() + cindex, line.begin() + line.size());
    SetCursorPosition(
        Coordinates(coord.mLine + 1,
                    GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)));
    u.mAdded = (char)aChar;

    // shift folds
    for (int b = 0; b < mFoldBegin.size(); b++)
      if (mFoldBegin[b].mLine == coord.mLine + 1)
        mFoldBegin[b].mColumn =
            std::max<int>(0, (mFoldBegin[b].mColumn + foldOffset) +
                                 (mFoldBegin[b].mColumn != coord.mColumn));
    for (int b = 0; b < mFoldEnd.size(); b++)
      if (mFoldEnd[b].mLine == coord.mLine + 1)
        mFoldEnd[b].mColumn =
            std::max<int>(0, (mFoldEnd[b].mColumn + foldOffset) +
                                 (mFoldEnd[b].mColumn != coord.mColumn));
  } else {
    char buf[7];
    int e = ImTextCharToUtf8(buf, 7, aChar);
    if (e > 0) {
      if (mInsertSpaces && e == 1 && buf[0] == '\t') {
        for (int i = 0; i < mTabSize; i++)
          buf[i] = ' ';
        e = mTabSize;
      }
      buf[e] = '\0';

      auto &line = mLines[coord.mLine];
      auto cindex = GetCharacterIndex(coord);

      // move the folds if necessary
      int foldOffset = 0;
      if (buf[0] == '\t')
        foldOffset =
            mTabSize - (coord.mColumn - (coord.mColumn / mTabSize) * mTabSize);
      else
        foldOffset = strlen(buf);

      int foldColumn = GetCharacterColumn(coord.mLine, cindex);
      for (int b = 0; b < mFoldBegin.size(); b++)
        if (mFoldBegin[b].mLine == coord.mLine &&
            mFoldBegin[b].mColumn >= foldColumn)
          mFoldBegin[b].mColumn += foldOffset;
      for (int b = 0; b < mFoldEnd.size(); b++)
        if (mFoldEnd[b].mLine == coord.mLine &&
            mFoldEnd[b].mColumn >= foldColumn)
          mFoldEnd[b].mColumn += foldOffset;

      // insert text
      for (auto p = buf; *p != '\0'; p++, ++cindex) {
        if (*p == '{') {
          mFoldBegin.push_back(Coordinates(coord.mLine, foldColumn));
          mFoldSorted = false;
        } else if (*p == '}') {
          mFoldEnd.push_back(Coordinates(coord.mLine, foldColumn));
          mFoldSorted = false;
        }

        line.insert(line.begin() + cindex, Glyph(*p, PaletteIndex::Default));
      }
      u.mAdded = buf;

      SetCursorPosition(
          Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)));
    } else
      return;
  }

  // active suggestions
  if (mActiveAutocomplete && aChar <= 127 && (isalpha(aChar) || aChar == '_')) {
    m_requestAutocomplete = true;
    m_readyForAutocomplete = false;
  }

  if (mScrollbarMarkers) {
    bool changeExists = false;
    for (int i = 0; i < mChangedLines.size(); i++) {
      if (mChangedLines[i] == mState.mCursorPosition.mLine) {
        changeExists = true;
        break;
      }
    }
    if (!changeExists)
      mChangedLines.push_back(mState.mCursorPosition.mLine);
  }

  mTextChanged = true;
  if (OnContentUpdate != nullptr)
    OnContentUpdate(this);

  u.mAddedEnd = GetActualCursorCoordinates();
  u.mAfter = mState;

  AddUndo(u);

  Colorize(coord.mLine - 1, 3);
  EnsureCursorVisible();

  // function tooltip
  if (mFunctionDeclarationTooltipEnabled) {
    if (aChar == '(') {
      auto curPos = GetCorrectCursorPosition();
      std::string obj = GetWordAt(curPos);
      mOpenFunctionDeclarationTooltip(obj, curPos);
    } else if (aChar == ',') {
      auto curPos = GetCorrectCursorPosition();
      curPos.mColumn--;

      const auto &line = mLines[curPos.mLine];
      std::string obj = "";
      int weight = 0;

      for (; curPos.mColumn > 0; curPos.mColumn--) {
        if (line[curPos.mColumn].mChar == '(') {
          if (weight == 0) {
            obj = GetWordAt(curPos);
            break;
          }

          weight--;
        }
        if (line[curPos.mColumn].mChar == ')')
          weight++;
      }

      if (!obj.empty())
        mOpenFunctionDeclarationTooltip(obj, curPos);
    }
  }

  // auto brace completion
  if (mCompleteBraces) {
    if (aChar == '{') {
      EnterCharacter('\n', false);
      EnterCharacter('}', false);
    } else if (aChar == '(')
      EnterCharacter(')', false);
    else if (aChar == '[')
      EnterCharacter(']', false);

    if (aChar == '{' || aChar == '(' || aChar == '[')
      mState.mCursorPosition.mColumn--;
  }
}

void ImTextEdit::SetColorizerEnable(bool aValue) { mColorizerEnabled = aValue; }

ImTextEdit::Coordinates ImTextEdit::GetCorrectCursorPosition() {
  auto curPos = GetCursorPosition();

  if (curPos.mLine >= 0 && curPos.mLine <= GetCursorPosition().mLine) {
    for (int c = 0;
         c < std::min<int>(curPos.mLine, mLines[curPos.mLine].size()); c++) {
      if (mLines[curPos.mLine][c].mChar == '\t')
        curPos.mColumn -= (GetTabSize() - 1);
    }
  }

  return curPos;
}

void ImTextEdit::SetCursorPosition(const Coordinates &aPosition) {
  if (mState.mCursorPosition != aPosition) {
    mState.mCursorPosition = aPosition;
    mCursorPositionChanged = true;
    EnsureCursorVisible();
  }
}

void ImTextEdit::SetSelectionStart(const Coordinates &aPosition) {
  mState.mSelectionStart = SanitizeCoordinates(aPosition);
  if (mState.mSelectionStart > mState.mSelectionEnd)
    std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void ImTextEdit::SetSelectionEnd(const Coordinates &aPosition) {
  mState.mSelectionEnd = SanitizeCoordinates(aPosition);
  if (mState.mSelectionStart > mState.mSelectionEnd)
    std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void ImTextEdit::SetSelection(const Coordinates &aStart,
                              const Coordinates &aEnd, SelectionMode aMode) {
  auto oldSelStart = mState.mSelectionStart;
  auto oldSelEnd = mState.mSelectionEnd;

  mState.mSelectionStart = SanitizeCoordinates(aStart);
  mState.mSelectionEnd = SanitizeCoordinates(aEnd);
  if (mState.mSelectionStart > mState.mSelectionEnd)
    std::swap(mState.mSelectionStart, mState.mSelectionEnd);

  switch (aMode) {
  case ImTextEdit::SelectionMode::Normal:
    break;
  case ImTextEdit::SelectionMode::Word: {
    mState.mSelectionStart = FindWordStart(mState.mSelectionStart);
    if (!IsOnWordBoundary(mState.mSelectionEnd))
      mState.mSelectionEnd = FindWordEnd(FindWordStart(mState.mSelectionEnd));
    break;
  }
  case ImTextEdit::SelectionMode::Line: {
    const auto lineNo = mState.mSelectionEnd.mLine;
    mState.mSelectionStart = Coordinates(mState.mSelectionStart.mLine, 0);
    mState.mSelectionEnd = Coordinates(lineNo, GetLineMaxColumn(lineNo));
    break;
  }
  default:
    break;
  }

  if (mState.mSelectionStart != oldSelStart ||
      mState.mSelectionEnd != oldSelEnd)
    mCursorPositionChanged = true;

  // update mReplaceIndex
  mReplaceIndex = 0;
  for (size_t ln = 0; ln < mState.mCursorPosition.mLine; ln++)
    mReplaceIndex += GetLineCharacterCount(ln) + 1;
  mReplaceIndex += mState.mCursorPosition.mColumn;
}

void ImTextEdit::InsertText(const std::string &aValue, bool indent) {
  InsertText(aValue.c_str(), indent);
}

void ImTextEdit::InsertText(const char *aValue, bool indent) {
  if (aValue == nullptr)
    return;

  auto pos = GetActualCursorCoordinates();
  auto start = std::min<Coordinates>(pos, mState.mSelectionStart);
  int totalLines = pos.mLine - start.mLine;

  totalLines += InsertTextAt(pos, aValue, indent);

  SetSelection(pos, pos);
  SetCursorPosition(pos);
  Colorize(start.mLine - 1, totalLines + 2);
}

void ImTextEdit::DeleteSelection() {
  assert(mState.mSelectionEnd >= mState.mSelectionStart);

  if (mState.mSelectionEnd == mState.mSelectionStart)
    return;

  DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);

  SetSelection(mState.mSelectionStart, mState.mSelectionStart);
  SetCursorPosition(mState.mSelectionStart);
  Colorize(mState.mSelectionStart.mLine, 1);
}

void ImTextEdit::MoveUp(int aAmount, bool aSelect) {
  auto oldPos = mState.mCursorPosition;
  mState.mCursorPosition.mLine =
      std::max<int>(0, mState.mCursorPosition.mLine - aAmount);
  if (oldPos != mState.mCursorPosition) {
    if (aSelect) {
      if (oldPos == mInteractiveStart)
        mInteractiveStart = mState.mCursorPosition;
      else if (oldPos == mInteractiveEnd)
        mInteractiveEnd = mState.mCursorPosition;
      else {
        mInteractiveStart = mState.mCursorPosition;
        mInteractiveEnd = oldPos;
      }
    } else
      mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd);

    EnsureCursorVisible();
  }
}

void ImTextEdit::MoveDown(int aAmount, bool aSelect) {
  assert(mState.mCursorPosition.mColumn >= 0);
  auto oldPos = mState.mCursorPosition;
  mState.mCursorPosition.mLine =
      std::max<int>(0, std::min<int>((int)mLines.size() - 1,
                                     mState.mCursorPosition.mLine + aAmount));

  if (mState.mCursorPosition != oldPos) {
    if (aSelect) {
      if (oldPos == mInteractiveEnd)
        mInteractiveEnd = mState.mCursorPosition;
      else if (oldPos == mInteractiveStart)
        mInteractiveStart = mState.mCursorPosition;
      else {
        mInteractiveStart = oldPos;
        mInteractiveEnd = mState.mCursorPosition;
      }
    } else
      mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd);

    EnsureCursorVisible();
  }
}

static bool IsUTFSequence(char c) { return (c & 0xC0) == 0x80; }

void ImTextEdit::MoveLeft(int aAmount, bool aSelect, bool aWordMode) {
  if (mLines.empty())
    return;

  auto oldPos = mState.mCursorPosition;
  mState.mCursorPosition = GetActualCursorCoordinates();
  auto line = mState.mCursorPosition.mLine;
  auto cindex = GetCharacterIndex(mState.mCursorPosition);

  while (aAmount-- > 0) {
    if (cindex == 0) {
      if (line > 0) {
        --line;
        if ((int)mLines.size() > line)
          cindex = (int)mLines[line].size();
        else
          cindex = 0;
      }
    } else {
      --cindex;
      if (cindex > 0) {
        if ((int)mLines.size() > line) {
          while (cindex > 0 && IsUTFSequence(mLines[line][cindex].mChar))
            --cindex;
        }
      }
    }

    mState.mCursorPosition =
        Coordinates(line, GetCharacterColumn(line, cindex));
    if (aWordMode) {
      mState.mCursorPosition = FindWordStart(mState.mCursorPosition);
      cindex = GetCharacterIndex(mState.mCursorPosition);
    }
  }

  mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));

  assert(mState.mCursorPosition.mColumn >= 0);
  if (aSelect) {
    mInteractiveStart = mState.mSelectionStart;
    mInteractiveEnd = mState.mSelectionEnd;

    if (oldPos == mInteractiveStart)
      mInteractiveStart = mState.mCursorPosition;
    else if (oldPos == mInteractiveEnd)
      mInteractiveEnd = mState.mCursorPosition;
    else {
      mInteractiveStart = mState.mCursorPosition;
      mInteractiveEnd = oldPos;
    }
  } else
    mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
  SetSelection(mInteractiveStart, mInteractiveEnd, SelectionMode::Normal);

  EnsureCursorVisible();
}

void ImTextEdit::MoveRight(int aAmount, bool aSelect, bool aWordMode) {
  auto oldPos = mState.mCursorPosition;

  if (mLines.empty() || oldPos.mLine >= mLines.size())
    return;

  mState.mCursorPosition = GetActualCursorCoordinates();
  auto cindex = GetCharacterIndex(mState.mCursorPosition);

  while (aAmount-- > 0) {
    auto lindex = mState.mCursorPosition.mLine;
    auto &line = mLines[lindex];

    if (cindex >= line.size()) {
      if (mState.mCursorPosition.mLine < mLines.size() - 1) {
        mState.mCursorPosition.mLine =
            std::max(0, std::min((int)mLines.size() - 1,
                                 mState.mCursorPosition.mLine + 1));
        mState.mCursorPosition.mColumn = 0;
      } else
        return;
    } else {
      cindex += UTF8CharLength(line[cindex].mChar);
      mState.mCursorPosition =
          Coordinates(lindex, GetCharacterColumn(lindex, cindex));
      if (aWordMode)
        mState.mCursorPosition = FindWordEnd(mState.mCursorPosition);
    }
  }

  if (aSelect) {
    mInteractiveStart = mState.mSelectionStart;
    mInteractiveEnd = mState.mSelectionEnd;

    if (oldPos == mInteractiveEnd)
      mInteractiveEnd = mState.mCursorPosition;
    else if (oldPos == mInteractiveStart)
      mInteractiveStart = mState.mCursorPosition;
    else {
      mInteractiveStart = oldPos;
      mInteractiveEnd = mState.mCursorPosition;
    }
  } else
    mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
  SetSelection(mInteractiveStart, mInteractiveEnd, SelectionMode::Normal);

  EnsureCursorVisible();
}

void ImTextEdit::MoveTop(bool aSelect) {
  auto oldPos = mState.mCursorPosition;
  SetCursorPosition(Coordinates(0, 0));

  if (mState.mCursorPosition != oldPos) {
    if (aSelect) {
      mInteractiveEnd = oldPos;
      mInteractiveStart = mState.mCursorPosition;
    } else
      mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd);
  }
}

void ImTextEdit::MoveBottom(bool aSelect) {
  auto oldPos = GetCursorPosition();
  auto newPos = Coordinates((int)mLines.size() - 1, 0);
  SetCursorPosition(newPos);
  if (aSelect) {
    mInteractiveStart = oldPos;
    mInteractiveEnd = newPos;
  } else
    mInteractiveStart = mInteractiveEnd = newPos;
  SetSelection(mInteractiveStart, mInteractiveEnd);
}

void ImTextEdit::MoveHome(bool aSelect) {
  auto oldPos = mState.mCursorPosition;
  SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, 0));

  if (mState.mCursorPosition != oldPos) {
    if (aSelect) {
      if (oldPos == mInteractiveStart)
        mInteractiveStart = mState.mCursorPosition;
      else if (oldPos == mInteractiveEnd)
        mInteractiveEnd = mState.mCursorPosition;
      else {
        mInteractiveStart = mState.mCursorPosition;
        mInteractiveEnd = oldPos;
      }
    } else
      mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd);
  }
}

void ImTextEdit::MoveEnd(bool aSelect) {
  auto oldPos = mState.mCursorPosition;
  SetCursorPosition(Coordinates(mState.mCursorPosition.mLine,
                                GetLineMaxColumn(oldPos.mLine)));

  if (mState.mCursorPosition != oldPos) {
    if (aSelect) {
      if (oldPos == mInteractiveEnd)
        mInteractiveEnd = mState.mCursorPosition;
      else if (oldPos == mInteractiveStart)
        mInteractiveStart = mState.mCursorPosition;
      else {
        mInteractiveStart = oldPos;
        mInteractiveEnd = mState.mCursorPosition;
      }
    } else
      mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd);
  }
}

void ImTextEdit::Delete() {
  if (mLines.empty())
    return;

  UndoRecord u;
  u.mBefore = mState;

  if (HasSelection()) {
    u.mRemoved = GetSelectedText();
    u.mRemovedStart = mState.mSelectionStart;
    u.mRemovedEnd = mState.mSelectionEnd;

    DeleteSelection();
  } else {
    auto pos = GetActualCursorCoordinates();
    SetCursorPosition(pos);
    auto &line = mLines[pos.mLine];

    if (pos.mColumn == GetLineMaxColumn(pos.mLine)) {
      if (pos.mLine == (int)mLines.size() - 1)
        return;

      u.mRemoved = '\n';
      u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
      Advance(u.mRemovedEnd);

      // move folds
      for (int i = 0; i < mFoldBegin.size(); i++)
        if (mFoldBegin[i].mLine == pos.mLine + 1) {
          mFoldBegin[i].mLine = std::max<int>(0, mFoldBegin[i].mLine - 1);
          mFoldBegin[i].mColumn += GetCharacterColumn(pos.mLine, line.size());
        }
      for (int i = 0; i < mFoldEnd.size(); i++)
        if (mFoldEnd[i].mLine == pos.mLine + 1) {
          mFoldEnd[i].mLine = std::max<int>(0, mFoldEnd[i].mLine - 1);
          mFoldEnd[i].mColumn += GetCharacterColumn(pos.mLine, line.size());
        }

      auto &nextLine = mLines[pos.mLine + 1];
      line.insert(line.end(), nextLine.begin(), nextLine.end());

      RemoveLine(pos.mLine + 1);
    } else {
      auto cindex = GetCharacterIndex(pos);
      u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
      u.mRemovedEnd.mColumn++;
      u.mRemoved = GetText(u.mRemovedStart, u.mRemovedEnd);

      mRemoveFolds(u.mRemovedStart, u.mRemovedEnd);

      auto d = UTF8CharLength(line[cindex].mChar);
      while (d-- > 0 && cindex < (int)line.size())
        line.erase(line.begin() + cindex);
    }

    if (mScrollbarMarkers) {
      bool changeExists = false;
      for (int i = 0; i < mChangedLines.size(); i++) {
        if (mChangedLines[i] == mState.mCursorPosition.mLine) {
          changeExists = true;
          break;
        }
      }
      if (!changeExists)
        mChangedLines.push_back(mState.mCursorPosition.mLine);
    }

    mTextChanged = true;
    if (OnContentUpdate != nullptr)
      OnContentUpdate(this);

    Colorize(pos.mLine, 1);
  }

  u.mAfter = mState;
  AddUndo(u);
}

void ImTextEdit::Backspace() {
  if (mLines.empty())
    return;

  UndoRecord u;
  u.mBefore = mState;

  if (HasSelection()) {
    u.mRemoved = GetSelectedText();
    u.mRemovedStart = mState.mSelectionStart;
    u.mRemovedEnd = mState.mSelectionEnd;

    DeleteSelection();
  } else {
    auto pos = GetActualCursorCoordinates();
    SetCursorPosition(pos);

    if (mState.mCursorPosition.mColumn == 0) {
      if (mState.mCursorPosition.mLine == 0)
        return;

      u.mRemoved = '\n';
      u.mRemovedStart = u.mRemovedEnd =
          Coordinates(pos.mLine - 1, GetLineMaxColumn(pos.mLine - 1));
      Advance(u.mRemovedEnd);

      auto &line = mLines[mState.mCursorPosition.mLine];
      auto &prevLine = mLines[mState.mCursorPosition.mLine - 1];
      auto prevSize = GetLineMaxColumn(mState.mCursorPosition.mLine - 1);
      prevLine.insert(prevLine.end(), line.begin(), line.end());

      // error markers
      ErrorMarkers etmp;
      for (auto &i : mErrorMarkers)
        etmp.insert(ErrorMarkers::value_type(
            i.first - 1 == mState.mCursorPosition.mLine ? i.first - 1 : i.first,
            i.second));
      mErrorMarkers = std::move(etmp);

      // shift folds
      for (int b = 0; b < mFoldBegin.size(); b++)
        if (mFoldBegin[b].mLine == mState.mCursorPosition.mLine) {
          mFoldBegin[b].mLine = std::max<int>(0, mFoldBegin[b].mLine - 1);
          mFoldBegin[b].mColumn = mFoldBegin[b].mColumn + prevSize;
        }
      for (int b = 0; b < mFoldEnd.size(); b++)
        if (mFoldEnd[b].mLine == mState.mCursorPosition.mLine) {
          mFoldEnd[b].mLine = std::max<int>(0, mFoldEnd[b].mLine - 1);
          mFoldEnd[b].mColumn = mFoldEnd[b].mColumn + prevSize;
        }

      RemoveLine(mState.mCursorPosition.mLine);
      --mState.mCursorPosition.mLine;
      mState.mCursorPosition.mColumn = prevSize;
    } else {
      auto &line = mLines[mState.mCursorPosition.mLine];
      auto cindex = GetCharacterIndex(pos) - 1;
      auto cend = cindex + 1;
      while (cindex > 0 && IsUTFSequence(line[cindex].mChar))
        --cindex;

      // if (cindex > 0 && UTF8CharLength(line[cindex].mChar) > 1)
      //	--cindex;

      int actualLoc = pos.mColumn;
      for (int i = 0; i < line.size(); i++) {
        if (line[i].mChar == '\t')
          actualLoc -= GetTabSize() - 1;
      }

      if (mCompleteBraces && actualLoc > 0 && actualLoc < line.size()) {
        if ((line[actualLoc - 1].mChar == '(' &&
             line[actualLoc].mChar == ')') ||
            (line[actualLoc - 1].mChar == '{' &&
             line[actualLoc].mChar == '}') ||
            (line[actualLoc - 1].mChar == '[' && line[actualLoc].mChar == ']'))
          Delete();
      }

      u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();

      while (cindex < line.size() && cend-- > cindex) {
        int remColumn = 0;
        for (int i = 0; i < cindex && i < line.size(); i++) {
          if (line[i].mChar == '\t') {
            int tabSize = remColumn - (remColumn / mTabSize) * mTabSize;
            remColumn += mTabSize - tabSize;
          } else
            remColumn++;
        }
        int remSize = mState.mCursorPosition.mColumn - remColumn;

        u.mRemoved += line[cindex].mChar;
        u.mRemovedStart.mColumn -= remSize;

        line.erase(line.begin() + cindex);

        mState.mCursorPosition.mColumn -= remSize;
      }

      mRemoveFolds(u.mRemovedStart, u.mRemovedEnd);
    }

    if (mScrollbarMarkers) {
      bool changeExists = false;
      for (int i = 0; i < mChangedLines.size(); i++) {
        if (mChangedLines[i] == mState.mCursorPosition.mLine) {
          changeExists = true;
          break;
        }
      }
      if (!changeExists)
        mChangedLines.push_back(mState.mCursorPosition.mLine);
    }

    mTextChanged = true;
    if (OnContentUpdate != nullptr)
      OnContentUpdate(this);

    EnsureCursorVisible();
    Colorize(mState.mCursorPosition.mLine, 1);
  }

  u.mAfter = mState;
  AddUndo(u);

  // autocomplete
  if (mActiveAutocomplete && mACOpened) {
    m_requestAutocomplete = true;
    m_readyForAutocomplete = false;
  }
}

void ImTextEdit::SelectWordUnderCursor() {
  auto c = GetCursorPosition();
  SetSelection(FindWordStart(c), FindWordEnd(c));
}

void ImTextEdit::SelectAll() {
  SetSelection(Coordinates(0, 0), Coordinates((int)mLines.size(), 0));
}

bool ImTextEdit::HasSelection() const {
  return mState.mSelectionEnd > mState.mSelectionStart;
}

void ImTextEdit::SetShortcut(ImTextEdit::ShortcutID id, Shortcut s) {
  m_shortcuts[(int)id] = s;
}

void ImTextEdit::Copy() {
  if (HasSelection()) {
    ImGui::SetClipboardText(GetSelectedText().c_str());
  } else {
    if (!mLines.empty()) {
      std::string str;
      auto &line = mLines[GetActualCursorCoordinates().mLine];
      for (auto &g : line)
        str.push_back(g.mChar);
      ImGui::SetClipboardText(str.c_str());
    }
  }
}

void ImTextEdit::Cut() {
  if (HasSelection()) {
    UndoRecord u;
    u.mBefore = mState;
    u.mRemoved = GetSelectedText();
    u.mRemovedStart = mState.mSelectionStart;
    u.mRemovedEnd = mState.mSelectionEnd;

    Copy();
    DeleteSelection();

    u.mAfter = mState;
    AddUndo(u);
  }
}

void ImTextEdit::Paste() {
  auto clipText = ImGui::GetClipboardText();
  if (clipText != nullptr && strlen(clipText) > 0) {
    UndoRecord u;
    u.mBefore = mState;

    if (HasSelection()) {
      u.mRemoved = GetSelectedText();
      u.mRemovedStart = mState.mSelectionStart;
      u.mRemovedEnd = mState.mSelectionEnd;
      DeleteSelection();
    }

    u.mAdded = clipText;
    u.mAddedStart = GetActualCursorCoordinates();

    InsertText(clipText, mAutoindentOnPaste);

    u.mAddedEnd = GetActualCursorCoordinates();
    u.mAfter = mState;
    AddUndo(u);
  }
}

bool ImTextEdit::CanUndo() { return mUndoIndex > 0; }

bool ImTextEdit::CanRedo() { return mUndoIndex < (int)mUndoBuffer.size(); }

void ImTextEdit::Undo(int aSteps) {
  while (CanUndo() && aSteps-- > 0)
    mUndoBuffer[--mUndoIndex].Undo(this);
}

void ImTextEdit::Redo(int aSteps) {
  while (CanRedo() && aSteps-- > 0)
    mUndoBuffer[mUndoIndex++].Redo(this);
}

std::vector<std::string> ImTextEdit::GetRelevantExpressions(int line) {
  std::vector<std::string> ret;
  line--;

  if (line < 0 || line >= mLines.size() ||
      (mLanguageDefinition.mName != "HLSL" &&
       mLanguageDefinition.mName != "GLSL"))
    return ret;

  std::string expr = "";
  for (int i = 0; i < mLines[line].size(); i++)
    expr += mLines[line][i].mChar;

  enum class TokenType {
    Identifier,
    Operator,
    Number,
    Parenthesis,
    Comma,
    Semicolon
  };
  struct Token {
    TokenType Type;
    std::string Content;
  };

  char buffer[512] = {0};
  int bufferLoc = 0;
  std::vector<Token> tokens;

  // convert expression into list of tokens
  const char *e = expr.c_str();
  while (*e != 0) {
    if (*e == '*' || *e == '/' || *e == '+' || *e == '-' || *e == '%' ||
        *e == '&' || *e == '|' || *e == '=' || *e == '(' || *e == ')' ||
        *e == ',' || *e == ';' || *e == '<' || *e == '>') {
      if (bufferLoc != 0)
        tokens.push_back({TokenType::Identifier, std::string(buffer)});

      memset(buffer, 0, 512);
      bufferLoc = 0;

      if (*e == '(' || *e == ')')
        tokens.push_back({TokenType::Parenthesis, std::string(e, 1)});
      else if (*e == ',')
        tokens.push_back({TokenType::Comma, ","});
      else if (*e == ';')
        tokens.push_back({TokenType::Semicolon, ";"});
      else
        tokens.push_back({TokenType::Operator, std::string(e, 1)});
    } else if (*e == '{' || *e == '}')
      break;
    else if (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t') {
      // empty the buffer if needed
      if (bufferLoc != 0) {
        tokens.push_back({TokenType::Identifier, std::string(buffer)});

        memset(buffer, 0, 512);
        bufferLoc = 0;
      }
    } else {
      buffer[bufferLoc] = *e;
      bufferLoc++;
    }
    e++;
  }

  // empty the buffer
  if (bufferLoc != 0)
    tokens.push_back({TokenType::Identifier, std::string(buffer)});

  // some "post processing"
  int multilineComment = 0;
  for (int i = 0; i < tokens.size(); i++) {
    if (tokens[i].Type == TokenType::Identifier) {
      if (tokens[i].Content.size() > 0) {
        if (tokens[i].Content[0] == '.' || isdigit(tokens[i].Content[0]))
          tokens[i].Type = TokenType::Number;
        else if (tokens[i].Content == "true" || tokens[i].Content == "false")
          tokens[i].Type = TokenType::Number;
      }
    } else if (i != 0 && tokens[i].Type == TokenType::Operator) {
      if (tokens[i - 1].Type == TokenType::Operator) {
        // comment
        if (tokens[i].Content == "/" && tokens[i - 1].Content == "/") {
          tokens.erase(tokens.begin() + i - 1, tokens.end());
          break;
        } else if (tokens[i - 1].Content == "/" && tokens[i].Content == "*")
          multilineComment = i - 1;
        else if (tokens[i - 1].Content == "*" && tokens[i].Content == "/") {
          tokens.erase(tokens.begin() + multilineComment,
                       tokens.begin() + i + 1);
          i -= (i - multilineComment) - 1;
          multilineComment = 0;
          continue;
        } else {
          // &&, <=, ...
          tokens[i - 1].Content += tokens[i].Content;
          tokens.erase(tokens.begin() + i);
          i--;
          continue;
        }
      }
    }
  }

  // 1. get all the identifiers (highest priority)
  for (int i = 0; i < tokens.size(); i++) {
    if (tokens[i].Type == TokenType::Identifier) {
      if (i == tokens.size() - 1 || tokens[i + 1].Content != "(")
        if (std::count(ret.begin(), ret.end(), tokens[i].Content) == 0)
          ret.push_back(tokens[i].Content);
    }
  }

  // 2. get all the function calls, their arguments and expressions
  std::stack<std::string> funcStack;
  std::stack<std::string> argStack;
  std::string exprBuffer = "";
  int exprParenthesis = 0;
  int forSection = -1;
  for (int i = 0; i < tokens.size(); i++) {
    if ((forSection == -1 || forSection == 1) &&
        tokens[i].Type == TokenType::Identifier && i + 1 < tokens.size() &&
        tokens[i + 1].Content == "(") {
      if (tokens[i].Content == "if" || tokens[i].Content == "for" ||
          tokens[i].Content == "while") {
        if (tokens[i].Content == "for")
          forSection = 0;
        else
          i++; // skip '('
        continue;
      }

      funcStack.push(tokens[i].Content + "(");
      argStack.push("");
      i++;
      continue;
    } else if ((forSection == -1 || forSection == 1) &&
               (tokens[i].Type == TokenType::Comma ||
                tokens[i].Content == ")") &&
               !argStack.empty() && !funcStack.empty()) {
      funcStack.top() += argStack.top().substr(0, argStack.top().size() - 1) +
                         tokens[i].Content;

      if (tokens[i].Content == ")") {
        std::string topFunc = funcStack.top();
        funcStack.pop();

        if (!argStack.top().empty())
          ret.push_back(argStack.top().substr(0, argStack.top().size() - 1));
        argStack.pop();
        if (!argStack.empty())
          argStack.top() += topFunc + " ";

        ret.push_back(topFunc);

        if (funcStack.empty())
          exprBuffer += topFunc + " ";
      } else if (tokens[i].Type == TokenType::Comma) {
        funcStack.top() += " ";
        ret.push_back(argStack.top().substr(0, argStack.top().size() - 1));
        argStack.top() = "";
      }
    } else if (tokens[i].Type == TokenType::Semicolon) {
      if (forSection != -1) {
        if (forSection == 1 && !exprBuffer.empty()) {
          ret.push_back(exprBuffer);
          exprBuffer.clear();
          exprParenthesis = 0;
        }
        forSection++;
      }
    } else if (forSection == -1 || forSection == 1) {
      if (tokens[i].Content == "(")
        exprParenthesis++;
      else if (tokens[i].Content == ")")
        exprParenthesis--;

      if (!argStack.empty())
        argStack.top() += tokens[i].Content + " ";
      else if (exprParenthesis < 0) {
        if (!exprBuffer.empty())
          ret.push_back(exprBuffer.substr(0, exprBuffer.size() - 1));
        exprBuffer.clear();
        exprParenthesis = 0;
      } else if (tokens[i].Type == TokenType::Operator &&
                 (tokens[i].Content.size() >= 2 || tokens[i].Content == "=")) {
        if (!exprBuffer.empty())
          ret.push_back(exprBuffer.substr(0, exprBuffer.size() - 1));
        exprBuffer.clear();
        exprParenthesis = 0;
      } else {
        bool isKeyword = false;
        for (const auto &kwd : mLanguageDefinition.mKeywords) {
          if (kwd == tokens[i].Content) {
            isKeyword = true;
            break;
          }
        }
        if (!isKeyword)
          exprBuffer += tokens[i].Content + " ";
      }
    }
  }

  if (!exprBuffer.empty())
    ret.push_back(exprBuffer.substr(0, exprBuffer.size() - 1));

  // remove duplicates, numbers & keywords
  for (int i = 0; i < ret.size(); i++) {
    std::string r = ret[i];
    bool eraseR = false;

    // empty or duplicate
    if (ret.empty() || std::count(ret.begin(), ret.begin() + i, r) > 0)
      eraseR = true;

    // boolean
    if (r == "true" || r == "false")
      eraseR = true;

    // number
    bool isNumber = true;
    for (int i = 0; i < r.size(); i++)
      if (!isdigit(r[i]) && r[i] != '.' && r[i] != 'f') {
        isNumber = false;
        break;
      }
    if (isNumber)
      eraseR = true;

    // keyword
    if (!eraseR) {
      for (const auto &ident : mLanguageDefinition.mIdentifiers)
        if (ident.first == r) {
          eraseR = true;
          break;
        }

      for (const auto &kwd : mLanguageDefinition.mKeywords)
        if (kwd == r) {
          eraseR = true;
          break;
        }
    }

    // delete it from the array
    if (eraseR) {
      ret.erase(ret.begin() + i);
      i--;
      continue;
    }
  }

  return ret;
}

const ImTextEdit::Palette &ImTextEdit::GetDarkPalette() {
  const static Palette p = {{
      0xff7f7f7f, // Default
      0xffd69c56, // Keyword
      0xff00ff00, // Number
      0xff7070e0, // String
      0xff70a0e0, // Char literal
      0xffffffff, // Punctuation
      0xff408080, // Preprocessor
      0xffaaaaaa, // Identifier
      0xff9bc64d, // Known identifier
      0xffc040a0, // Preproc identifier
      0xff206020, // Comment (single line)
      0xff406020, // Comment (multi line)
      0xff101010, // Background
      0xffe0e0e0, // Cursor
      0x80a06020, // Selection
      0x800020ff, // ErrorMarker
      0xff0000ff, // Breakpoint
      0xffffffff, // Breakpoint outline
      0xFF1DD8FF, // Current line indicator
      0xFF696969, // Current line indicator outline
      0xff707000, // Line number
      0x40000000, // Current line fill
      0x40808080, // Current line fill (inactive)
      0x40a0a0a0, // Current line edge
      0xff33ffff, // Error message
      0xffffffff, // BreakpointDisabled
      0xffaaaaaa, // UserFunction
      0xffb0c94e, // UserType
      0xffaaaaaa, // UniformType
      0xffaaaaaa, // GlobalVariable
      0xffaaaaaa, // LocalVariable
      0xff888888  // FunctionArgument
  }};
  return p;
}

const ImTextEdit::Palette &ImTextEdit::GetLightPalette() {
  const static Palette p = {{
      0xff7f7f7f, // None
      0xffff0c06, // Keyword
      0xff008000, // Number
      0xff2020a0, // String
      0xff304070, // Char literal
      0xff000000, // Punctuation
      0xff406060, // Preprocessor
      0xff404040, // Identifier
      0xff606010, // Known identifier
      0xffc040a0, // Preproc identifier
      0xff205020, // Comment (single line)
      0xff405020, // Comment (multi line)
      0xffffffff, // Background
      0xff000000, // Cursor
      0x80DFBF80, // Selection
      0xa00010ff, // ErrorMarker
      0xff0000ff, // Breakpoint
      0xff000000, // Breakpoint outline
      0xFF1DD8FF, // Current line indicator
      0xFF696969, // Current line indicator outline
      0xff505000, // Line number
      0x20000000, // Current line fill
      0x20808080, // Current line fill (inactive)
      0x30000000, // Current line edge
      0xff3333ff, // Error message
      0xffffffff, // BreakpointDisabled
      0xff404040, // UserFunction
      0xffb0912b, // UserType
      0xff404040, // UniformType
      0xff404040, // GlobalVariable
      0xff404040, // LocalVariable
      0xff606060  // FunctionArgument
  }};
  return p;
}

const ImTextEdit::Palette &ImTextEdit::GetRetroBluePalette() {
  const static Palette p = {{
      0xff00ffff, // None
      0xffffff00, // Keyword
      0xff00ff00, // Number
      0xff808000, // String
      0xff808000, // Char literal
      0xffffffff, // Punctuation
      0xff008000, // Preprocessor
      0xff00ffff, // Identifier
      0xffffffff, // Known identifier
      0xffff00ff, // Preproc identifier
      0xff808080, // Comment (single line)
      0xff404040, // Comment (multi line)
      0xff800000, // Background
      0xff0080ff, // Cursor
      0x80ffff00, // Selection
      0xa00000ff, // ErrorMarker
      0xff0000ff, // Breakpoint
      0xffffffff, // Breakpoint outline
      0xFF1DD8FF, // Current line indicator
      0xff808000, // Line number
      0x40000000, // Current line fill
      0x40808080, // Current line fill (inactive)
      0x40000000, // Current line edge
      0xffffff00, // Error message
      0xffffffff, // BreakpointDisabled
      0xff00ffff, // UserFunction
      0xff00ffff, // UserType
      0xff00ffff, // UniformType
      0xff00ffff, // GlobalVariable
      0xff00ffff, // LocalVariable
      0xff00ffff  // FunctionArgument
  }};
  return p;
}

std::string ImTextEdit::GetText() const {
  return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::string ImTextEdit::GetSelectedText() const {
  return GetText(mState.mSelectionStart, mState.mSelectionEnd);
}

std::string ImTextEdit::GetCurrentLineText() const {
  auto lineLength = GetLineMaxColumn(mState.mCursorPosition.mLine);
  return GetText(Coordinates(mState.mCursorPosition.mLine, 0),
                 Coordinates(mState.mCursorPosition.mLine, lineLength));
}

void ImTextEdit::ProcessInputs() {}

void ImTextEdit::Colorize(int aFromLine, int aLines) {
  int toLine = aLines == -1
                   ? (int)mLines.size()
                   : std::min<int>((int)mLines.size(), aFromLine + aLines);
  mColorRangeMin = std::min<int>(mColorRangeMin, aFromLine);
  mColorRangeMax = std::max<int>(mColorRangeMax, toLine);
  mColorRangeMin = std::max<int>(0, mColorRangeMin);
  mColorRangeMax = std::max<int>(mColorRangeMin, mColorRangeMax);
  mCheckComments = true;
}

void ImTextEdit::ColorizeRange(int aFromLine, int aToLine) {
  if (mLines.empty() || !mColorizerEnabled)
    return;

  std::string buffer;
  std::cmatch results;
  std::string id;

  int endLine = std::max(0, std::min((int)mLines.size(), aToLine));
  for (int i = aFromLine; i < endLine; ++i) {
    auto &line = mLines[i];

    if (line.empty())
      continue;

    buffer.resize(line.size());
    for (size_t j = 0; j < line.size(); ++j) {
      auto &col = line[j];
      buffer[j] = col.mChar;
      col.mColorIndex = PaletteIndex::Default;
    }

    const char *bufferBegin = &buffer.front();
    const char *bufferEnd = bufferBegin + buffer.size();

    auto last = bufferEnd;

    for (auto first = bufferBegin; first != last;) {
      const char *token_begin = nullptr;
      const char *token_end = nullptr;
      PaletteIndex token_color = PaletteIndex::Default;

      bool hasTokenizeResult = false;

      if (mLanguageDefinition.mTokenize != nullptr) {
        if (mLanguageDefinition.mTokenize(first, last, token_begin, token_end,
                                          token_color))
          hasTokenizeResult = true;
      }

      if (hasTokenizeResult == false) {
        // todo : remove
        // printf("using regex for %.*s\n", first + 10 < last ? 10 : int(last -
        // first), first);

        for (auto &p : mRegexList) {
          if (std::regex_search(first, last, results, p.first,
                                std::regex_constants::match_continuous)) {
            hasTokenizeResult = true;

            auto &v = *results.begin();
            token_begin = v.first;
            token_end = v.second;
            token_color = p.second;
            break;
          }
        }
      }

      if (hasTokenizeResult == false) {
        first++;
      } else {
        const size_t token_length = token_end - token_begin;

        if (token_color == PaletteIndex::Identifier) {
          id.assign(token_begin, token_end);

          // todo : allmost all language definitions use lower case to specify
          // keywords, so shouldn't this use ::tolower ?
          if (!mLanguageDefinition.mCaseSensitive)
            std::transform(id.begin(), id.end(), id.begin(), ::toupper);

          if (!line[first - bufferBegin].mPreprocessor) {
            if (mLanguageDefinition.mKeywords.count(id) != 0)
              token_color = PaletteIndex::Keyword;
            else if (mLanguageDefinition.mIdentifiers.count(id) != 0)
              token_color = PaletteIndex::KnownIdentifier;
            else if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
              token_color = PaletteIndex::PreprocIdentifier;
          } else {
            if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
              token_color = PaletteIndex::PreprocIdentifier;
          }
        }

        for (size_t j = 0; j < token_length; ++j)
          line[(token_begin - bufferBegin) + j].mColorIndex = token_color;

        first = token_end;
      }
    }
  }
}

void ImTextEdit::ColorizeInternal() {
  if (mLines.empty() || !mColorizerEnabled)
    return;

  if (mCheckComments) {
    auto endLine = mLines.size();
    auto endIndex = 0;
    auto commentStartLine = endLine;
    auto commentStartIndex = endIndex;
    auto withinString = false;
    auto withinSingleLineComment = false;
    auto withinPreproc = false;
    auto firstChar =
        true; // there is no other non-whitespace characters in the line before
    auto concatenate = false; // '\' on the very end of the line
    auto currentLine = 0;
    auto currentIndex = 0;
    while (currentLine < endLine || currentIndex < endIndex) {
      auto &line = mLines[currentLine];

      if (currentIndex == 0 && !concatenate) {
        withinSingleLineComment = false;
        withinPreproc = false;
        firstChar = true;
      }

      concatenate = false;

      if (!line.empty()) {
        auto &g = line[currentIndex];
        auto c = g.mChar;

        if (c != mLanguageDefinition.mPreprocChar && !isspace(c))
          firstChar = false;

        if (currentIndex == (int)line.size() - 1 &&
            line[line.size() - 1].mChar == '\\')
          concatenate = true;

        bool inComment = (commentStartLine < currentLine ||
                          (commentStartLine == currentLine &&
                           commentStartIndex <= currentIndex));

        if (withinString) {
          line[currentIndex].mMultiLineComment = inComment;

          if (c == '\"') {
            if (currentIndex + 1 < (int)line.size() &&
                line[currentIndex + 1].mChar == '\"') {
              currentIndex += 1;
              if (currentIndex < (int)line.size())
                line[currentIndex].mMultiLineComment = inComment;
            } else
              withinString = false;
          } else if (c == '\\') {
            currentIndex += 1;
            if (currentIndex < (int)line.size())
              line[currentIndex].mMultiLineComment = inComment;
          }
        } else {
          if (firstChar && c == mLanguageDefinition.mPreprocChar)
            withinPreproc = true;

          if (c == '\"') {
            withinString = true;
            line[currentIndex].mMultiLineComment = inComment;
          } else {
            auto pred = [](const char &a, const Glyph &b) {
              return a == b.mChar;
            };
            auto from = line.begin() + currentIndex;
            auto &startStr = mLanguageDefinition.mCommentStart;
            auto &singleStartStr = mLanguageDefinition.mSingleLineComment;

            if (singleStartStr.size() > 0 &&
                currentIndex + singleStartStr.size() <= line.size() &&
                equals(singleStartStr.begin(), singleStartStr.end(), from,
                       from + singleStartStr.size(), pred)) {
              withinSingleLineComment = true;
            } else if (!withinSingleLineComment &&
                       currentIndex + startStr.size() <= line.size() &&
                       equals(startStr.begin(), startStr.end(), from,
                              from + startStr.size(), pred)) {
              commentStartLine = currentLine;
              commentStartIndex = currentIndex;
            }

            inComment = inComment = (commentStartLine < currentLine ||
                                     (commentStartLine == currentLine &&
                                      commentStartIndex <= currentIndex));

            line[currentIndex].mMultiLineComment = inComment;
            line[currentIndex].mComment = withinSingleLineComment;

            auto &endStr = mLanguageDefinition.mCommentEnd;
            if (currentIndex + 1 >= (int)endStr.size() &&
                equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(),
                       from + 1, pred)) {
              commentStartIndex = endIndex;
              commentStartLine = endLine;
            }
          }
        }
        line[currentIndex].mPreprocessor = withinPreproc;
        currentIndex += UTF8CharLength(c);
        if (currentIndex >= (int)line.size()) {
          currentIndex = 0;
          ++currentLine;
        }
      } else {
        currentIndex = 0;
        ++currentLine;
      }
    }
    mCheckComments = false;
  }

  if (mColorRangeMin < mColorRangeMax) {
    const int increment =
        (mLanguageDefinition.mTokenize == nullptr) ? 10 : 10000;
    const int to = std::min<int>(mColorRangeMin + increment, mColorRangeMax);
    ColorizeRange(mColorRangeMin, to);
    mColorRangeMin = to;

    if (mColorRangeMax == mColorRangeMin) {
      mColorRangeMin = std::numeric_limits<int>::max();
      mColorRangeMax = 0;
    }
    return;
  }
}

float ImTextEdit::TextDistanceToLineStart(const Coordinates &aFrom) const {
  auto &line = mLines[aFrom.mLine];
  float distance = 0.0f;
  float spaceSize = ImGui::GetFont()
                        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f,
                                        " ", nullptr, nullptr)
                        .x;
  int colIndex = GetCharacterIndex(aFrom);
  for (size_t it = 0u; it < line.size() && it < colIndex;) {
    if (line[it].mChar == '\t') {
      distance = (1.0f + std::floor((1.0f + distance) /
                                    (float(mTabSize) * spaceSize))) *
                 (float(mTabSize) * spaceSize);
      ++it;
    } else {
      auto d = UTF8CharLength(line[it].mChar);
      char tempCString[7];
      int i = 0;
      for (; i < 6 && d-- > 0 && it < (int)line.size(); i++, it++)
        tempCString[i] = line[it].mChar;

      tempCString[i] = '\0';
      distance += ImGui::GetFont()
                      ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f,
                                      tempCString, nullptr, nullptr)
                      .x;
    }
  }

  return distance;
}

void ImTextEdit::EnsureCursorVisible() {
  if (!mWithinRender) {
    mScrollToCursor = true;
    return;
  }

  float scrollX = ImGui::GetScrollX();
  float scrollY = ImGui::GetScrollY();

  auto height = ImGui::GetWindowHeight();
  auto width = mWindowWidth;

  auto top = 1 + (int)ceil(scrollY / mCharAdvance.y);
  auto bottom = (int)ceil((scrollY + height) / mCharAdvance.y);

  auto left = (int)ceil(scrollX / mCharAdvance.x);
  auto right = (int)ceil((scrollX + width) / mCharAdvance.x);

  auto pos = GetActualCursorCoordinates();
  auto len = TextDistanceToLineStart(pos);

  if (pos.mLine < top)
    ImGui::SetScrollY(std::max(0.0f, (pos.mLine - 1) * mCharAdvance.y));
  if (pos.mLine > bottom - 4)
    ImGui::SetScrollY(
        std::max(0.0f, (pos.mLine + 4) * mCharAdvance.y - height));
  if (pos.mColumn < left)
    ImGui::SetScrollX(std::max(0.0f, len + mTextStart - 11 * mCharAdvance.x));
  if (len + mTextStart > (right - 4) * mCharAdvance.x)
    ImGui::SetScrollX(
        std::max(0.0f, len + mTextStart + 4 * mCharAdvance.x - width));
}
int ImTextEdit::GetPageSize() const {
  auto height = ImGui::GetWindowHeight() - 20.0f;
  return (int)floor(height / mCharAdvance.y);
}

ImTextEdit::UndoRecord::UndoRecord(const std::string &aAdded,
                                   const ImTextEdit::Coordinates aAddedStart,
                                   const ImTextEdit::Coordinates aAddedEnd,
                                   const std::string &aRemoved,
                                   const ImTextEdit::Coordinates aRemovedStart,
                                   const ImTextEdit::Coordinates aRemovedEnd,
                                   ImTextEdit::EditorState &aBefore,
                                   ImTextEdit::EditorState &aAfter)
    : mAdded(aAdded), mAddedStart(aAddedStart), mAddedEnd(aAddedEnd),
      mRemoved(aRemoved), mRemovedStart(aRemovedStart),
      mRemovedEnd(aRemovedEnd), mBefore(aBefore), mAfter(aAfter) {
  assert(mAddedStart <= mAddedEnd);
  assert(mRemovedStart <= mRemovedEnd);
}

void ImTextEdit::UndoRecord::Undo(ImTextEdit *aEditor) {
  if (!mAdded.empty()) {
    aEditor->DeleteRange(mAddedStart, mAddedEnd);
    aEditor->Colorize(mAddedStart.mLine - 1,
                      mAddedEnd.mLine - mAddedStart.mLine + 2);
  }

  if (!mRemoved.empty()) {
    auto start = mRemovedStart;
    aEditor->InsertTextAt(start, mRemoved.c_str());
    aEditor->Colorize(mRemovedStart.mLine - 1,
                      mRemovedEnd.mLine - mRemovedStart.mLine + 2);
  }

  aEditor->mState = mBefore;
  aEditor->EnsureCursorVisible();
}

void ImTextEdit::UndoRecord::Redo(ImTextEdit *aEditor) {
  if (!mRemoved.empty()) {
    aEditor->DeleteRange(mRemovedStart, mRemovedEnd);
    aEditor->Colorize(mRemovedStart.mLine - 1,
                      mRemovedEnd.mLine - mRemovedStart.mLine + 1);
  }

  if (!mAdded.empty()) {
    auto start = mAddedStart;
    aEditor->InsertTextAt(start, mAdded.c_str());
    aEditor->Colorize(mAddedStart.mLine - 1,
                      mAddedEnd.mLine - mAddedStart.mLine + 1);
  }

  aEditor->mState = mAfter;
  aEditor->EnsureCursorVisible();
}

const ImTextEdit::LanguageDefinition &ImTextEdit::LanguageDefinition::SVG() {
  static bool inited = false;
  static LanguageDefinition langDef;
  if (!inited) {
    static const char *const keywords[] = {
        "circle",  "clipPath", "defs",     "ellipse",        "feGaussianBlur",
        "filter",  "g",        "image",    "line",           "linearGradient",
        "marker",  "mask",     "stop",     "radialGradient", "path",
        "pattern", "polygon",  "polyline", "rect",           "style",
        "svg",     "use"};
    for (auto &k : keywords) {
      langDef.mKeywords.insert(k);
    }

    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("[ \\t]*#[ \\t]*[a-zA-Z_]+",
                                                  PaletteIndex::Preprocessor));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"",
                                                  PaletteIndex::String));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("\\'\\\\?[^\\']\\'",
                                                  PaletteIndex::CharLiteral));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>(
            "[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?",
            PaletteIndex::Number));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?",
                                                  PaletteIndex::Number));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?",
                                                  PaletteIndex::Number));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>(
            "0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*",
                                                  PaletteIndex::Identifier));
    langDef.mTokenRegexStrings.push_back(
        std::make_pair<std::string, PaletteIndex>(
            "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/"
            "\\;\\,\\.]",
            PaletteIndex::Punctuation));

    langDef.mCommentStart = "/*";
    langDef.mCommentEnd = "*/";
    langDef.mSingleLineComment = "//";

    langDef.mCaseSensitive = true;
    langDef.mAutoIndentation = true;

    langDef.mName = "HLSL";

    inited = true;
  }
  return langDef;
}

} // namespace donner::editor
