#include "donner/editor/TextEditorCore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <regex>
#include <string>
#include <unordered_map>

#include "donner/base/StringUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"

namespace donner::editor {

namespace {

constexpr bool IsAscii(char c) {
  return static_cast<unsigned char>(c) <= 127;
}

/// Check if a character is a UTF-8 continuation byte.
bool isUtfSequence(char c) {
  return (c & 0xC0) == 0x80;
}

/// Encode a UTF-32 character to UTF-8 in the given buffer. Mirrors ImGui's
/// `ImTextCharToUtf8` so the core can avoid depending on `imgui.h` while
/// preserving exact byte output for `handleRegularCharacter`.
int textCharToUtf8(char* buf, char32_t c) {
  if (c < 0x80) {
    buf[0] = static_cast<char>(c);
    buf[1] = '\0';
    return 1;
  }
  if (c < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (c >> 6));
    buf[1] = static_cast<char>(0x80 | (c & 0x3F));
    buf[2] = '\0';
    return 2;
  }
  if (c < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (c >> 12));
    buf[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (c & 0x3F));
    buf[3] = '\0';
    return 3;
  }
  if (c < 0x110000) {
    buf[0] = static_cast<char>(0xF0 | (c >> 18));
    buf[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (c & 0x3F));
    buf[4] = '\0';
    return 4;
  }
  // Invalid — write replacement character.
  buf[0] = '?';
  buf[1] = '\0';
  return 1;
}

bool matchesCommentStart(std::string_view line, size_t index, std::string_view commentStart) {
  if (index + commentStart.size() > line.size()) {
    return false;
  }
  return std::equal(commentStart.begin(), commentStart.end(), line.begin() + index);
}

bool matchesCommentEnd(std::string_view line, size_t index, std::string_view commentEnd) {
  if (index + 1 < commentEnd.size()) {
    return false;
  }
  return std::equal(commentEnd.begin(), commentEnd.end(),
                    line.begin() + index + 1 - commentEnd.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TextEditorCore::TextEditorCore() = default;
TextEditorCore::~TextEditorCore() = default;

// ---------------------------------------------------------------------------
// Language definition + palette
// ---------------------------------------------------------------------------

void TextEditorCore::setLanguageDefinition(const LanguageDefinition& langDef) {
  languageDefinition_ = langDef;
  regexList_.clear();

  // Build optimized regex list for syntax highlighting
  for (const auto& regex : languageDefinition_.tokenRegexStrings) {
    regexList_.emplace_back(std::regex(regex.first, std::regex_constants::optimize), regex.second);
  }

  colorize();
}

// ---------------------------------------------------------------------------
// Basic accessors
// ---------------------------------------------------------------------------

std::string TextEditorCore::getText() const {
  return text_.getText();
}

std::string TextEditorCore::getText(const Coordinates& start, const Coordinates& end) const {
  return text_.getText(start, end);
}

std::string TextEditorCore::getSelectedText() const {
  return getText(state_.selectionStart, state_.selectionEnd);
}

Coordinates TextEditorCore::getActualCursorCoordinates() const {
  return sanitizeCoordinates(state_.cursorPosition);
}

Coordinates TextEditorCore::sanitizeCoordinates(const Coordinates& value) const {
  int line = std::max(0, value.line);
  int column = value.column;

  // Handle out of bounds line numbers
  if (line >= text_.getTotalLines()) {
    line = text_.getTotalLines() - 1;
    column = text_.getLineMaxColumn(line);
    return Coordinates(line, column);
  }

  // Clamp column to valid range
  column = std::max(0, std::min(column, text_.getLineMaxColumn(line)));

  return Coordinates(line, column);
}

void TextEditorCore::advance(Coordinates& coords) const {
  if (coords.line >= text_.getTotalLines()) {
    return;
  }

  const Line& line = text_.getLineGlyphs(coords.line);
  const auto charIndex = text_.getCharacterIndex(coords);

  if (charIndex + 1 < static_cast<int>(line.size())) {
    // Get length of current UTF-8 sequence at position
    int delta = Utf8::SequenceLength(line[charIndex].character);
    const int newIndex = std::min(charIndex + delta, static_cast<int>(line.size()) - 1);
    coords.column = text_.getCharacterColumn(coords.line, newIndex);
  } else {
    // Move to start of next line
    ++coords.line;
    coords.column = 0;
  }
}

unsigned int TextEditorCore::getGlyphColor(const Glyph& glyph) const {
  if (!colorizerEnabled_) {
    return palette_[static_cast<int>(ColorIndex::Default)];
  }

  if (glyph.isComment) {
    return palette_[static_cast<int>(ColorIndex::Comment)];
  }

  if (glyph.isMultiLineComment) {
    return palette_[static_cast<int>(ColorIndex::MultiLineComment)];
  }

  return palette_[static_cast<int>(glyph.colorIndex)];
}

// ---------------------------------------------------------------------------
// Buffer mutation wrappers (update fold / error / changed-line bookkeeping
// owned by the core).
// ---------------------------------------------------------------------------

void TextEditorCore::deleteRange(const Coordinates& start, const Coordinates& end) {
  text_.deleteRange(start, end);

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
  fireContentUpdate();
}

int TextEditorCore::insertTextAt(Coordinates& /* inout */ where, std::string_view text,
                                 bool indent) {
  // Insert the text using TextBuffer, which handles basic insertion
  const int totalLines = text_.insertTextAt(where, text, indent);

  // Handle fold markers which TextBuffer doesn't manage
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] == '{') {
      foldBegin_.push_back(where);
      foldSorted_ = false;
    } else if (text[i] == '}') {
      foldEnd_.push_back(where);
      foldSorted_ = false;
    }
  }

  // Track changes for scrollbar markers
  if (scrollbarMarkers_) {
    if (std::find(changedLines_.begin(), changedLines_.end(), where.line) == changedLines_.end()) {
      changedLines_.push_back(where.line);
    }
  }

  // Set flags and trigger callback
  textChanged_ = true;
  fireContentUpdate();

  return totalLines;
}

void TextEditorCore::removeLine(int start, int end) {
  UTILS_RELEASE_ASSERT(end >= start);
  UTILS_RELEASE_ASSERT(text_.getTotalLines() > static_cast<size_t>(end - start));

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto& marker : errorMarkers_) {
    int line = marker.first >= start ? marker.first - 1 : marker.first;
    if (line >= start && line <= end) {
      continue;
    }
    updatedMarkers.insert({line, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  // Remove lines from buffer
  text_.removeLine(start, end + 1);

  // Update scrollbar markers
  if (scrollbarMarkers_) {
    for (int i = 0; i < static_cast<int>(changedLines_.size()); i++) {
      if (changedLines_[i] > end) {
        changedLines_[i] -= (end - start);
      } else if (changedLines_[i] >= start && changedLines_[i] <= end) {
        changedLines_.erase(changedLines_.begin() + i);
        i--;
      }
    }
  }

  textChanged_ = true;
  fireContentUpdate();
}

void TextEditorCore::removeLine(int index) {
  UTILS_RELEASE_ASSERT(text_.getTotalLines() > 1);

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto& marker : errorMarkers_) {
    int line = marker.first > index ? marker.first - 1 : marker.first;
    if (line - 1 == index) {
      continue;
    }
    updatedMarkers.insert({line, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  // Remove line from buffer
  text_.removeLine(index);

  // Remove folds
  removeFolds(Coordinates(index, 0), Coordinates(index, 100000));

  // Update scrollbar markers
  if (scrollbarMarkers_) {
    for (int i = 0; i < static_cast<int>(changedLines_.size()); i++) {
      if (changedLines_[i] > index) {
        changedLines_[i]--;
      } else if (changedLines_[i] == index) {
        changedLines_.erase(changedLines_.begin() + i);
        i--;
      }
    }
  }

  textChanged_ = true;
  fireContentUpdate();
}

Line& TextEditorCore::insertLine(int index, int column) {
  Line& result = text_.insertLine(index, column);

  // Update fold positions
  for (auto& fold : foldBegin_) {
    if (fold.line > index - 1 || (fold.line == index - 1 && fold.column >= column)) {
      fold.line++;
    }
  }

  for (auto& fold : foldEnd_) {
    if (fold.line > index - 1 || (fold.line == index - 1 && fold.column >= column)) {
      fold.line++;
    }
  }

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto& marker : errorMarkers_) {
    updatedMarkers.insert({marker.first >= index ? marker.first + 1 : marker.first, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  return result;
}

namespace {

// Local helper mirroring TextEditor::updateFoldColumn. It takes an
// explicit reference to the matching foldEnd marker because the original
// derived one from pointer arithmetic (`&fold - &foldBegin_[0]`) which
// implicitly assumed `folds` was `foldBegin_`. Making the dependency
// explicit avoids the undefined pointer arithmetic without changing the
// computed result.
void updateFoldColumnImpl(Coordinates& fold, const Coordinates& matchingEnd,
                          const Coordinates& start, const Coordinates& end, const TextBuffer& text,
                          int tabSize) {
  if (start.line >= text.getTotalLines()) {
    return;
  }

  const Line& startLine = text.getLineGlyphs(start.line);
  int colOffset = 0;
  int charIndex = 0;
  bool skipped = false;
  const int bracketEndCharIndex = text.getCharacterIndex(matchingEnd);

  while (charIndex < static_cast<int>(startLine.size()) &&
         (!skipped || (skipped && charIndex < bracketEndCharIndex))) {
    char c = startLine[charIndex].character;
    charIndex += Utf8::SequenceLength(c);

    if (c == '\t') {
      colOffset = (colOffset / tabSize) * tabSize + tabSize;
    } else {
      colOffset++;
    }

    if (charIndex == static_cast<int>(startLine.size()) && end.line < text.getTotalLines() &&
        !skipped) {
      charIndex = text.getCharacterIndex(end);
      skipped = true;
    }
  }

  fold.column = colOffset;
}

}  // namespace

void TextEditorCore::removeFolds(const Coordinates& start, const Coordinates& end) {
  removeFolds(foldBegin_, start, end);
  removeFolds(foldEnd_, start, end);
}

void TextEditorCore::removeFolds(std::vector<Coordinates>& folds, const Coordinates& start,
                                 const Coordinates& end) {
  const bool deleteFullyLastLine = end.line >= text_.getTotalLines() || end.column >= 100000;

  for (int i = 0; i < static_cast<int>(folds.size()); i++) {
    if (folds[i].line < start.line || folds[i].line > end.line) {
      if (folds[i].line > end.line) {
        folds[i].line -= (end.line - start.line) + deleteFullyLastLine;
      }
      continue;
    }

    // Handle fold on start line
    if (folds[i].line == start.line && start.line != end.line) {
      if (folds[i].column >= start.column) {
        folds.erase(folds.begin() + i);
        foldSorted_ = false;
        i--;
      }
      continue;
    }

    // Handle fold on end line
    if (folds[i].line == end.line) {
      if (folds[i].column < end.column) {
        if (end.line != start.line || folds[i].column >= start.column) {
          folds.erase(folds.begin() + i);
          foldSorted_ = false;
          i--;
        }
      } else {
        if (end.line == start.line) {
          folds[i].column = std::max(0, folds[i].column - (end.column - start.column));
        } else {
          // The original used `foldEnd_[&fold - &foldBegin_[0]]` to pick
          // the matching end marker — preserve that semantic when the
          // folds vector is `foldBegin_` and fall back to the current
          // fold itself otherwise.
          const Coordinates& matchingEnd =
              (&folds == &foldBegin_ && i < static_cast<int>(foldEnd_.size())) ? foldEnd_[i]
                                                                               : folds[i];
          updateFoldColumnImpl(folds[i], matchingEnd, start, end, text_, tabSize_);
          folds[i].line -= (end.line - start.line);
        }
      }
      continue;
    }

    // Handle folds in between
    folds.erase(folds.begin() + i);
    foldSorted_ = false;
    i--;
  }
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

void TextEditorCore::addUndo(UndoRecord& record) {
  undoBuffer_.resize(static_cast<size_t>(undoIndex_ + 1));
  undoBuffer_.back() = record;
  ++undoIndex_;
}

bool TextEditorCore::canUndo() const {
  return undoIndex_ > 0;
}

bool TextEditorCore::canRedo() const {
  return undoIndex_ < static_cast<int>(undoBuffer_.size());
}

void TextEditorCore::undo(int steps) {
  while (canUndo() && steps-- > 0) {
    undoBuffer_[--undoIndex_].undo(this);
  }
}

void TextEditorCore::redo(int steps) {
  while (canRedo() && steps-- > 0) {
    undoBuffer_[undoIndex_++].redo(this);
  }
}

// ---------------------------------------------------------------------------
// Word boundaries
// ---------------------------------------------------------------------------

namespace {

/// Word-boundary classification used by `findWordStart` / `findWordEnd`.
/// Characters in the same class belong to the same "word run" — so a
/// click on a letter selects the contiguous letters/digits, a click
/// on a punctuation character selects the contiguous punctuation
/// run, and a click on a space selects the contiguous whitespace
/// run. This is the standard double-click behavior in most editors
/// and is independent of syntax highlighting (which the previous
/// implementation relied on, with the result that text without a
/// language definition was treated as one giant word).
enum class WordClass : std::uint8_t {
  Alphanum,
  Whitespace,
  Punctuation,
};

WordClass ClassifyChar(char c) {
  // UTF-8 continuation bytes are treated as part of the current run
  // so multi-byte characters don't fragment word boundaries.
  if ((c & 0xC0) == 0x80) {
    return WordClass::Alphanum;
  }
  if (c <= 32 && std::isspace(static_cast<unsigned char>(c))) {
    return WordClass::Whitespace;
  }
  if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
    return WordClass::Alphanum;
  }
  return WordClass::Punctuation;
}

}  // namespace

Coordinates TextEditorCore::findWordStart(const Coordinates& from) const {
  if (from.line >= text_.getTotalLines()) {
    return from;
  }

  const Line& line = text_.getLineGlyphs(from.line);
  const int charIndex = text_.getCharacterIndex(from);

  if (charIndex >= static_cast<int>(line.size())) {
    return from;
  }

  // Walk backwards while the previous character is in the same
  // class as the start character. Terminates at index 0 or at the
  // first character of a different class.
  const WordClass startClass = ClassifyChar(line[charIndex].character);
  int currentIndex = charIndex;
  while (currentIndex > 0 && ClassifyChar(line[currentIndex - 1].character) == startClass) {
    --currentIndex;
  }

  return Coordinates(from.line, text_.getCharacterColumn(from.line, currentIndex));
}

Coordinates TextEditorCore::findWordEnd(const Coordinates& from) const {
  if (from.line >= text_.getTotalLines()) {
    return from;
  }

  const Line& line = text_.getLineGlyphs(from.line);
  const int charIndex = text_.getCharacterIndex(from);

  if (charIndex >= static_cast<int>(line.size())) {
    return from;
  }

  // Walk forward while subsequent characters are in the same class
  // as the start character. Stops at the first different-class
  // character (or end of line). Multi-byte characters are stepped by
  // their UTF-8 sequence length so we don't land mid-codepoint.
  const WordClass startClass = ClassifyChar(line[charIndex].character);
  int currentIndex = charIndex;
  while (currentIndex < static_cast<int>(line.size())) {
    if (ClassifyChar(line[currentIndex].character) != startClass) {
      break;
    }
    currentIndex += Utf8::SequenceLength(line[currentIndex].character);
  }

  return Coordinates(from.line, text_.getCharacterColumn(from.line, currentIndex));
}

Coordinates TextEditorCore::findNextWord(const Coordinates& from) const {
  if (from.line >= text_.getTotalLines()) {
    return from;
  }

  Coordinates current = from;
  int charIndex = text_.getCharacterIndex(from);
  bool isWord = false;
  bool skip = false;

  // Check initial character
  if (current.line < text_.getTotalLines()) {
    const Line& line = text_.getLineGlyphs(current.line);
    if (charIndex < static_cast<int>(line.size())) {
      isWord = std::isalnum(line[charIndex].character);
      skip = isWord;
    }
  }

  // Find next word start
  while (!isWord || skip) {
    if (current.line >= text_.getTotalLines()) {
      const int lastLine = std::max(0, text_.getTotalLines() - 1);
      return Coordinates(lastLine, text_.getLineMaxColumn(lastLine));
    }

    const Line& line = text_.getLineGlyphs(current.line);
    if (charIndex < static_cast<int>(line.size())) {
      isWord = std::isalnum(line[charIndex].character);

      if (isWord && !skip) {
        return Coordinates(current.line, text_.getCharacterColumn(current.line, charIndex));
      }

      if (!isWord) {
        skip = false;
      }

      charIndex++;
    } else {
      charIndex = 0;
      ++current.line;
      skip = false;
      isWord = false;
    }
  }

  return current;
}

bool TextEditorCore::isOnWordBoundary(const Coordinates& at) const {
  if (at.line >= text_.getTotalLines() || at.column == 0) {
    return true;
  }

  const Line& line = text_.getLineGlyphs(at.line);
  const int charIndex = text_.getCharacterIndex(at);

  if (charIndex >= static_cast<int>(line.size())) {
    return true;
  }

  if (colorizerEnabled_) {
    return line[charIndex].colorIndex != line[charIndex - 1].colorIndex;
  }

  return std::isspace(line[charIndex].character) != std::isspace(line[charIndex - 1].character);
}

RcString TextEditorCore::getWordUnderCursor() const {
  const Coordinates coords(getCursorPosition().line, std::max(getCursorPosition().column - 1, 0));
  return getWordAt(coords);
}

RcString TextEditorCore::getWordAt(const Coordinates& coords) const {
  const Coordinates start = findWordStart(coords);
  const Coordinates end = findWordEnd(coords);

  std::string result;
  const Line& line = text_.getLineGlyphs(coords.line);
  const int startIndex = text_.getCharacterIndex(start);
  const int endIndex = text_.getCharacterIndex(end);

  for (int i = startIndex; i < endIndex; ++i) {
    result.push_back(line[i].character);
  }

  return RcString(result);
}

Coordinates TextEditorCore::findFirst(std::string_view searchText, const Coordinates& start) const {
  const int totalLines = text_.getTotalLines();

  if (start.line < 0 || start.line >= totalLines) {
    return Coordinates(totalLines, 0);
  }

  const std::string text = getText(start, Coordinates(totalLines, 0));
  const std::string_view textView = text;

  size_t index = 0;
  size_t found = StringUtils::Find(textView, searchText);
  Coordinates current = start;

  while (found != std::string::npos) {
    // Advance to found position
    while (index < found) {
      if (textView[index] == '\n') {
        current.column = 0;
        current.line++;
      } else {
        current.column++;
      }
      index++;
    }

    // Convert character index to column position
    current.column = text_.getCharacterColumn(current.line, current.column);

    // Check if this is a word boundary match
    if (getWordAt(current) == searchText) {
      return current;
    }

    found = textView.find(searchText, found + 1);
  }

  return Coordinates(totalLines, 0);
}

// ---------------------------------------------------------------------------
// Selection + cursor
// ---------------------------------------------------------------------------

void TextEditorCore::applySelection(const Coordinates& start, const Coordinates& end,
                                    SelectionMode mode, bool updateInteractiveBounds) {
  const auto oldSelStart = state_.selectionStart;
  const auto oldSelEnd = state_.selectionEnd;

  state_.selectionStart = sanitizeCoordinates(start);
  state_.selectionEnd = sanitizeCoordinates(end);

  if (state_.selectionStart > state_.selectionEnd) {
    std::swap(state_.selectionStart, state_.selectionEnd);
  }

  switch (mode) {
    case SelectionMode::Normal: break;

    case SelectionMode::Word: {
      state_.selectionStart = findWordStart(state_.selectionStart);
      if (!isOnWordBoundary(state_.selectionEnd)) {
        state_.selectionEnd = findWordEnd(findWordStart(state_.selectionEnd));
      }
      break;
    }

    case SelectionMode::Line: {
      const auto lineNo = state_.selectionEnd.line;
      state_.selectionStart = Coordinates(state_.selectionStart.line, 0);
      state_.selectionEnd = Coordinates(lineNo, text_.getLineMaxColumn(lineNo));
      break;
    }
  }

  if (updateInteractiveBounds) {
    // Mirror the new bounds onto `interactiveStart_/End_` so subsequent
    // shifted arrow-key moves can extend or contract the selection
    // relative to the right anchor. Without this, callers that build a
    // selection via `setSelection()` (programmatic select, find/
    // replace, "select word" double-click, etc.) and then expect a
    // shifted arrow to grow or shrink it from the cursor side would
    // see the move logic treat the old selection as if it didn't
    // exist — see the `ShiftLeftContractsSelection` regression test.
    interactiveStart_ = state_.selectionStart;
    interactiveEnd_ = state_.selectionEnd;
  }

  if (state_.selectionStart != oldSelStart || state_.selectionEnd != oldSelEnd) {
    cursorPositionChanged_ = true;
  }

  // Update replace index for find/replace functionality
  replaceIndex_ = 0;
  for (size_t ln = 0; ln < state_.cursorPosition.line; ln++) {
    replaceIndex_ += text_.getLineCharacterCount(ln) + 1;
  }
  replaceIndex_ += state_.cursorPosition.column;
}

void TextEditorCore::setSelection(const Coordinates& start, const Coordinates& end,
                                  SelectionMode mode) {
  applySelection(start, end, mode, true);
}

void TextEditorCore::setInteractiveSelection(const Coordinates& start, const Coordinates& end,
                                             SelectionMode mode) {
  const Coordinates sanitizedStart = sanitizeCoordinates(start);
  const Coordinates sanitizedEnd = sanitizeCoordinates(end);

  applySelection(sanitizedStart, sanitizedEnd, mode, false);
  interactiveStart_ = sanitizedStart;
  interactiveEnd_ = sanitizedEnd;
}

void TextEditorCore::setCursorPosition(const Coordinates& position) {
  if (state_.cursorPosition != position) {
    state_.cursorPosition = position;
    cursorPositionChanged_ = true;
    scrollToCursor_ = true;
  }
}

void TextEditorCore::setSelectionStart(const Coordinates& position) {
  state_.selectionStart = sanitizeCoordinates(position);

  if (state_.selectionStart > state_.selectionEnd) {
    std::swap(state_.selectionStart, state_.selectionEnd);
  }
}

void TextEditorCore::setSelectionEnd(const Coordinates& position) {
  state_.selectionEnd = sanitizeCoordinates(position);

  if (state_.selectionStart > state_.selectionEnd) {
    std::swap(state_.selectionStart, state_.selectionEnd);
  }
}

bool TextEditorCore::hasSelection() const {
  return state_.selectionEnd > state_.selectionStart;
}

void TextEditorCore::selectWordUnderCursor() {
  const auto cursorPos = getCursorPosition();
  setSelection(findWordStart(cursorPos), findWordEnd(cursorPos));
}

void TextEditorCore::selectAll() {
  setSelection(Coordinates(0, 0), Coordinates(text_.getTotalLines(), 0));
}

// ---------------------------------------------------------------------------
// Editing (insertText / deleteSelection / enterCharacter / backspace / delete)
// ---------------------------------------------------------------------------

void TextEditorCore::insertText(std::string_view text, bool indent) {
  // Build a single undo record that covers both the selection
  // deletion (if any) and the new text insertion. An empty `text`
  // with a non-empty selection becomes a pure delete (the natural
  // "replace selection with nothing" form); an empty `text` with no
  // selection is a no-op and skips the undo entry entirely.
  //
  // Pre-refactor (before this fix), this function silently early-
  // returned on empty text and never deleted the existing selection
  // before inserting non-empty text either, so every `insertText`
  // call was effectively dropping selections on the floor and never
  // landing in the undo buffer. See the `TextEditorTests`
  // `InsertTextWithSelection` / `DeleteMultipleSelectionsSuccessively`
  // / `UndoMultipleOperations` regression tests.
  UndoRecord record;
  record.before = state_;

  if (hasSelection()) {
    record.removed = getSelectedText();
    record.removedStart = state_.selectionStart;
    record.removedEnd = state_.selectionEnd;
    deleteSelection();
  }

  if (!text.empty()) {
    auto pos = getActualCursorCoordinates();
    record.added = std::string(text);
    record.addedStart = pos;

    const int insertedLines = insertTextAt(pos, text, indent);

    setSelection(pos, pos);
    setCursorPosition(pos);

    record.addedEnd = pos;
    colorize(record.addedStart.line - 1, insertedLines + 2);
  }

  if (record.added.empty() && record.removed.empty()) {
    // Nothing actually happened — don't pollute the undo stack.
    return;
  }

  record.after = state_;
  addUndo(record);
  textChanged_ = true;
  fireContentUpdate();
}

void TextEditorCore::deleteSelection() {
  UTILS_RELEASE_ASSERT(state_.selectionEnd >= state_.selectionStart);

  if (state_.selectionEnd == state_.selectionStart) {
    return;
  }

  deleteRange(state_.selectionStart, state_.selectionEnd);

  setSelection(state_.selectionStart, state_.selectionStart);
  setCursorPosition(state_.selectionStart);
  colorize(state_.selectionStart.line, 1);
}

void TextEditorCore::handleNewLine(UndoState& state, const Coordinates& coord, bool smartIndent) {
  Line& line = text_.getLineGlyphsMutable(coord.line);
  Line& newLine = insertLine(coord.line, coord.column);

  // Auto indentation
  size_t whitespaceSize = 0;
  std::string added = "\n";
  if (languageDefinition_.autoIndentation && smartIndent) {
    for (size_t i = 0;
         i < line.size() && IsAscii(line[i].character) && std::isblank(line[i].character); ++i) {
      ++whitespaceSize;
      newLine.insert(newLine.begin(), line[i]);
      added.push_back(line[i].character);
    }
  }

  // Update cursor and state
  setCursorPosition(
      Coordinates(coord.line + 1, text_.getCharacterColumn(coord.line + 1, whitespaceSize)));

  state.record.added = added;
}

void TextEditorCore::handleRegularCharacter(UndoState& state, const Coordinates& coord,
                                            char32_t character) {
  // Determine where to insert the new character(s)
  int charIndex = text_.getCharacterIndex(coord);
  std::string added;

  Line& line = text_.getLineGlyphsMutable(coord.line);

  // Special handling for tab character if typed alone
  if (character == '\t') {
    if (insertSpaces_) {
      // Insert `tabSize_` spaces
      line.insert(line.begin() + charIndex, tabSize_, Glyph(' ', ColorIndex::Default));
      added.append(tabSize_, ' ');
    } else {
      // Insert a single '\t' character
      line.insert(line.begin() + charIndex, Glyph('\t', ColorIndex::Default));
      added.push_back('\t');
    }

    // Move cursor right by tab size if spaces, else by 1
    state_.cursorPosition.column += insertSpaces_ ? tabSize_ : 1;

    // Record undo state
    state.record.added = added;
    scrollToCursor_ = true;
    return;
  }

  // For other characters, convert to UTF-8
  char buf[5] = {};
  textCharToUtf8(buf, character);
  std::string_view chars(buf);

  // Insert the character(s) at the current cursor position
  for (char ch : chars) {
    if (ch == '\t' && insertSpaces_) {
      // If we get a tab and we're using spaces, insert tabSize_ spaces
      for (int i = 0; i < tabSize_; i++) {
        line.insert(line.begin() + charIndex++, Glyph(' ', ColorIndex::Default));
      }
      added.append(tabSize_, ' ');
    } else {
      line.insert(line.begin() + charIndex++, Glyph(ch, ColorIndex::Default));
      added.push_back(ch);
    }
  }

  // Update cursor position
  // For a tab, move cursor by tab size if using spaces, otherwise by 1
  const int advance = (chars[0] == '\t' && insertSpaces_) ? tabSize_ : (int)chars.size();
  state_.cursorPosition = Coordinates(coord.line, coord.column + advance);

  // Record what was added for undo
  state.record.added = added;

  // Ensure the cursor is visible
  scrollToCursor_ = true;
}

void TextEditorCore::enterCharacter(char32_t character, bool shift) {
  UndoState state;
  state.record.before = state_;

  // Handle selection
  if (hasSelection()) {
    if (character == '\t') {
      handleMultiLineTab(state, shift);
      return;
    } else {
      state.record.removed = getSelectedText();
      state.record.removedStart = state_.selectionStart;
      state.record.removedEnd = state_.selectionEnd;
      deleteSelection();
    }
  }

  state.insertPos = getActualCursorCoordinates();
  state.record.addedStart = state.insertPos;

  // Handle different character types
  if (character == '\n') {
    handleNewLine(state, state.insertPos, smartIndent_);
  } else {
    handleRegularCharacter(state, state.insertPos, character);
  }

  // Update autocomplete
  if (activeAutocomplete_ && character <= 127 && (std::isalpha(character) || character == '_')) {
    requestAutocomplete();
  }

  // Update change tracking
  updateChangeTracking();

  // Finalize changes
  textChanged_ = true;
  fireContentUpdate();

  state.record.addedEnd = getActualCursorCoordinates();
  state.record.after = state_;
  addUndo(state.record);

  colorize(state.insertPos.line - 1, 3);

  // Ensure cursor is visible after entering a character
  scrollToCursor_ = true;

  // Shell hook for function-declaration tooltips. Brace completion also
  // used to live in the shell as `handleBraceCompletion`; its logic is
  // self-contained so it now runs inline below.
  if (functionTooltipHook) {
    functionTooltipHook(character, state.insertPos);
  }

  // Brace completion (moved from `TextEditor::handleBraceCompletion`).
  if (completeBraces_) {
    char32_t closingChar = 0;
    switch (character) {
      case '{':
        enterCharacter('\n', false);
        closingChar = '}';
        break;
      case '(': closingChar = ')'; break;
      case '[': closingChar = ']'; break;
      default: break;
    }

    if (closingChar != 0) {
      enterCharacter(closingChar, false);

      auto cursorPos = state_.cursorPosition;
      cursorPos.column--;
      setCursorPosition(cursorPos);
    }
  }
}

void TextEditorCore::updateChangeTracking() {
  if (!scrollbarMarkers_) {
    return;
  }

  const int currentLine = state_.cursorPosition.line;
  if (std::find(changedLines_.begin(), changedLines_.end(), currentLine) == changedLines_.end()) {
    changedLines_.push_back(currentLine);
  }
}

void TextEditorCore::handleMultiLineTab(UndoState& state, bool shift) {
  auto start = state_.selectionStart;
  auto end = state_.selectionEnd;
  const auto originalEnd = end;

  if (start > end) {
    std::swap(start, end);
  }

  // Adjust selection to full lines
  start.column = 0;
  if (end.column == 0 && end.line > 0) {
    end.line--;
  }

  if (end.line >= text_.getTotalLines()) {
    end.line = text_.getTotalLines() == 0 ? 0 : text_.getTotalLines() - 1;
  }
  end.column = text_.getLineMaxColumn(end.line);

  // Store undo state
  state.record.removedStart = start;
  state.record.removedEnd = end;
  state.record.removed = getText(start, end);

  bool modified = false;
  for (int i = start.line; i <= end.line; i++) {
    Line& line = text_.getLineGlyphsMutable(i);
    if (shift) {
      if (!line.empty()) {
        if (line.front().character == '\t') {
          line.erase(line.begin());
          modified = true;
        } else {
          for (int j = 0; j < tabSize_ && !line.empty() && line.front().character == ' '; j++) {
            line.erase(line.begin());
            modified = true;
          }
        }
      }
    } else {
      if (insertSpaces_) {
        line.insert(line.begin(), tabSize_, Glyph(' ', ColorIndex::Background));
      } else {
        line.insert(line.begin(), Glyph('\t', ColorIndex::Background));
      }
      modified = true;
    }
  }

  if (modified) {
    state.record.addedStart = Coordinates(start.line, text_.getCharacterColumn(start.line, 0));

    // Update selection
    Coordinates rangeEnd;
    if (originalEnd.column != 0) {
      end = Coordinates(end.line, text_.getLineMaxColumn(end.line));
      rangeEnd = end;
      state.record.added = getText(start, end);
    } else {
      end = Coordinates(originalEnd.line, 0);
      rangeEnd = Coordinates(end.line - 1, text_.getLineMaxColumn(end.line - 1));
      state.record.added = getText(start, rangeEnd);
    }

    state.record.addedEnd = rangeEnd;
    state.record.after = state_;

    state_.selectionStart = start;
    state_.selectionEnd = end;
    requestEnsureCursorVisible();
  }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void TextEditorCore::moveUp(int amount, bool select) {
  const auto oldPos = state_.cursorPosition;
  state_.cursorPosition.line = std::max(0, state_.cursorPosition.line - amount);

  if (oldPos != state_.cursorPosition) {
    if (select) {
      if (oldPos == interactiveStart_) {
        interactiveStart_ = state_.cursorPosition;
      } else if (oldPos == interactiveEnd_) {
        interactiveEnd_ = state_.cursorPosition;
      } else {
        interactiveStart_ = state_.cursorPosition;
        interactiveEnd_ = oldPos;
      }
    } else {
      interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
    }

    setSelection(interactiveStart_, interactiveEnd_);
    scrollToCursor_ = true;
  }
}

void TextEditorCore::moveDown(int amount, bool select) {
  UTILS_RELEASE_ASSERT(state_.cursorPosition.column >= 0);

  const auto oldPos = state_.cursorPosition;
  state_.cursorPosition.line =
      std::max(0, std::min(text_.getTotalLines() - 1, state_.cursorPosition.line + amount));

  if (state_.cursorPosition != oldPos) {
    if (select) {
      if (oldPos == interactiveEnd_) {
        interactiveEnd_ = state_.cursorPosition;
      } else if (oldPos == interactiveStart_) {
        interactiveStart_ = state_.cursorPosition;
      } else {
        interactiveStart_ = oldPos;
        interactiveEnd_ = state_.cursorPosition;
      }
    } else {
      interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
    }

    setSelection(interactiveStart_, interactiveEnd_);
    scrollToCursor_ = true;
  }
}

void TextEditorCore::moveLeft(int amount, bool select, bool /*wordMode*/) {
  auto oldPos = state_.cursorPosition;

  while (amount-- > 0) {
    if (state_.cursorPosition.column > 0) {
      // Move left within line
      state_.cursorPosition.column--;
    } else {
      // Move to end of previous line if exists
      if (state_.cursorPosition.line > 0) {
        state_.cursorPosition.line--;
        state_.cursorPosition.column = text_.getLineMaxColumn(state_.cursorPosition.line);
      }
    }
  }

  if (select) {
    if (oldPos == interactiveStart_) {
      interactiveStart_ = state_.cursorPosition;
    } else if (oldPos == interactiveEnd_) {
      interactiveEnd_ = state_.cursorPosition;
    } else {
      interactiveStart_ = state_.cursorPosition;
      interactiveEnd_ = oldPos;
    }
    setSelection(interactiveStart_, interactiveEnd_);
  } else {
    // Non-select arrow: just move the cursor; leave the selection
    // alone. Callers that want to drop the selection must do so
    // explicitly via `setSelection`. This matches the
    // `SelectionPreservesAfterMove` regression test expectation.
    interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
  }

  scrollToCursor_ = true;
}

void TextEditorCore::moveRight(int amount, bool select, bool /*wordMode*/) {
  auto oldPos = state_.cursorPosition;

  while (amount-- > 0) {
    int maxCol = text_.getLineMaxColumn(state_.cursorPosition.line);
    if (state_.cursorPosition.column < maxCol) {
      // Move right within line
      state_.cursorPosition.column++;
    } else {
      // Move to start of next line if exists
      if (state_.cursorPosition.line < text_.getTotalLines() - 1) {
        state_.cursorPosition.line++;
        state_.cursorPosition.column = 0;
      }
    }
  }

  if (select) {
    if (oldPos == interactiveEnd_) {
      interactiveEnd_ = state_.cursorPosition;
    } else if (oldPos == interactiveStart_) {
      interactiveStart_ = state_.cursorPosition;
    } else {
      interactiveStart_ = oldPos;
      interactiveEnd_ = state_.cursorPosition;
    }
    setSelection(interactiveStart_, interactiveEnd_);
  } else {
    // Non-select arrow: just move the cursor; leave the selection
    // alone. See `moveLeft` for the rationale.
    interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
  }

  scrollToCursor_ = true;
}

void TextEditorCore::moveTop(bool select) {
  const auto oldPos = state_.cursorPosition;
  setCursorPosition(Coordinates(0, 0));

  if (state_.cursorPosition != oldPos) {
    if (select) {
      interactiveEnd_ = oldPos;
      interactiveStart_ = state_.cursorPosition;
    } else {
      interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
    }
    setSelection(interactiveStart_, interactiveEnd_);
  }
}

void TextEditorCore::moveBottom(bool select) {
  const auto oldPos = getCursorPosition();
  const auto newPos = Coordinates(text_.getTotalLines() - 1, 0);
  setCursorPosition(newPos);

  if (select) {
    interactiveStart_ = oldPos;
    interactiveEnd_ = newPos;
  } else {
    interactiveStart_ = interactiveEnd_ = newPos;
  }
  setSelection(interactiveStart_, interactiveEnd_);
}

void TextEditorCore::moveHome(bool select) {
  const auto oldPos = state_.cursorPosition;
  setCursorPosition(Coordinates(state_.cursorPosition.line, 0));

  if (state_.cursorPosition != oldPos) {
    if (select) {
      if (oldPos == interactiveStart_) {
        interactiveStart_ = state_.cursorPosition;
      } else if (oldPos == interactiveEnd_) {
        interactiveEnd_ = state_.cursorPosition;
      } else {
        interactiveStart_ = state_.cursorPosition;
        interactiveEnd_ = oldPos;
      }
    } else {
      interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
    }
    setSelection(interactiveStart_, interactiveEnd_);
  }
}

void TextEditorCore::moveEnd(bool select) {
  const auto oldPos = state_.cursorPosition;
  setCursorPosition(Coordinates(state_.cursorPosition.line, text_.getLineMaxColumn(oldPos.line)));

  if (state_.cursorPosition != oldPos) {
    if (select) {
      if (oldPos == interactiveEnd_) {
        interactiveEnd_ = state_.cursorPosition;
      } else if (oldPos == interactiveStart_) {
        interactiveStart_ = state_.cursorPosition;
      } else {
        interactiveStart_ = oldPos;
        interactiveEnd_ = state_.cursorPosition;
      }
    } else {
      interactiveStart_ = interactiveEnd_ = state_.cursorPosition;
    }
    setSelection(interactiveStart_, interactiveEnd_);
  }
}

// ---------------------------------------------------------------------------
// Deletion helpers + backspace / delete
// ---------------------------------------------------------------------------

void TextEditorCore::handleEndOfLineDelete(Coordinates pos, UndoRecord& undo) {
  if (pos.line == text_.getTotalLines() - 1) {
    return;
  }

  Line& line = text_.getLineGlyphsMutable(pos.line);
  undo.removed = "\n";
  undo.removedStart = undo.removedEnd = getActualCursorCoordinates();
  advance(undo.removedEnd);

  // Merge lines
  const Line& nextLine = text_.getLineGlyphs(pos.line + 1);
  line.insert(line.end(), nextLine.begin(), nextLine.end());
  removeLine(pos.line + 1);
}

void TextEditorCore::handleMidLineDelete(Coordinates pos, UndoRecord& undo) {
  Line& line = text_.getLineGlyphsMutable(pos.line);
  auto charIndex = text_.getCharacterIndex(pos);

  undo.removedStart = undo.removedEnd = getActualCursorCoordinates();
  undo.removedEnd.column++;
  undo.removed = getText(undo.removedStart, undo.removedEnd);

  removeFolds(undo.removedStart, undo.removedEnd);

  auto charLen = Utf8::SequenceLength(line[charIndex].character);
  while (charLen-- > 0 && charIndex < static_cast<int>(line.size())) {
    line.erase(line.begin() + charIndex);
  }
}

void TextEditorCore::delete_() {
  if (text_.getTotalLines() == 0) {
    return;
  }

  UndoRecord undo;
  undo.before = state_;

  if (hasSelection()) {
    undo.removed = getSelectedText();
    undo.removedStart = state_.selectionStart;
    undo.removedEnd = state_.selectionEnd;
    deleteSelection();
  } else {
    auto pos = getActualCursorCoordinates();
    setCursorPosition(pos);

    if (pos.column == text_.getLineMaxColumn(pos.line)) {
      handleEndOfLineDelete(pos, undo);
    } else {
      handleMidLineDelete(pos, undo);
    }

    updateChangeTracking();
    textChanged_ = true;
    fireContentUpdate();

    colorize(pos.line, 1);
  }

  undo.after = state_;
  addUndo(undo);
}

void TextEditorCore::handleStartOfLineDelete(const Coordinates& pos, UndoRecord& undo) {
  if (pos.line == 0) {
    return;
  }

  undo.removed = "\n";
  undo.removedStart = undo.removedEnd =
      Coordinates(pos.line - 1, text_.getLineMaxColumn(pos.line - 1));
  advance(undo.removedEnd);

  const Line& line = text_.getLineGlyphs(pos.line);
  Line& prevLine = text_.getLineGlyphsMutable(pos.line - 1);
  const int prevSize = text_.getLineMaxColumn(pos.line - 1);

  // Merge lines
  prevLine.insert(prevLine.end(), line.begin(), line.end());

  // Update error markers
  ErrorMarkers updatedMarkers;
  for (const auto& marker : errorMarkers_) {
    updatedMarkers.insert(
        {marker.first - 1 == pos.line ? marker.first - 1 : marker.first, marker.second});
  }
  errorMarkers_ = std::move(updatedMarkers);

  removeLine(pos.line);
  setCursorPosition(Coordinates(pos.line - 1, prevSize));
}

void TextEditorCore::handleMidLineBackspace(const Coordinates& pos, UndoRecord& undo) {
  // Before removing chars, detect if we're at an indentation boundary.
  if (pos.column > 0 && pos.column <= tabSize_) {
    Line& line = text_.getLineGlyphsMutable(pos.line);
    int charIndex = text_.getCharacterIndex(pos);
    if (insertSpaces_) {
      int startIndex = charIndex - 1;
      int countSpaces = 0;
      for (int i = 0; i < tabSize_ && startIndex - i >= 0; i++) {
        if (line[startIndex - i].character == ' ')
          countSpaces++;
        else
          break;
      }
      if (countSpaces == tabSize_) {
        undo.removedStart = Coordinates(pos.line, pos.column - tabSize_);
        undo.removedEnd = Coordinates(pos.line, pos.column);
        undo.removed = getText(undo.removedStart, undo.removedEnd);
        line.erase(line.begin() + (charIndex - tabSize_), line.begin() + charIndex);
        setCursorPosition(Coordinates(pos.line, pos.column - tabSize_));
        return;
      }
    } else {
      int prevIndex = charIndex - 1;
      if (prevIndex >= 0 && line[prevIndex].character == '\t') {
        undo.removedStart = Coordinates(pos.line, pos.column - 1);
        undo.removedEnd = Coordinates(pos.line, pos.column);
        undo.removed = "\t";
        line.erase(line.begin() + prevIndex);
        setCursorPosition(Coordinates(pos.line, pos.column - 1));
        return;
      }
    }
  }

  Line& line = text_.getLineGlyphsMutable(pos.line);

  // Find the character cluster to remove
  int charIndex = text_.getCharacterIndex(pos) - 1;
  while (charIndex > 0 && isUtfSequence(line[charIndex].character)) {
    --charIndex;
  }

  int charLen = Utf8::SequenceLength(line[charIndex].character);
  int endIndex = charIndex + charLen;

  // Calculate column positions before and after the character
  int preColumn = text_.getCharacterColumn(pos.line, charIndex);
  int postColumn = text_.getCharacterColumn(pos.line, endIndex);
  int remSize = postColumn - preColumn;

  // Set undo information
  undo.removedStart = Coordinates(pos.line, preColumn);
  undo.removedEnd = Coordinates(pos.line, postColumn);
  undo.removed = getText(undo.removedStart, undo.removedEnd);

  // Remove the character cluster
  line.erase(line.begin() + charIndex, line.begin() + endIndex);

  // Move cursor back by exactly remSize columns
  setCursorPosition(Coordinates(pos.line, pos.column - remSize));
}

void TextEditorCore::backspace() {
  if (text_.getTotalLines() == 0) {
    return;
  }

  UndoRecord undo;
  undo.before = state_;

  if (hasSelection()) {
    undo.removed = getSelectedText();
    undo.removedStart = state_.selectionStart;
    undo.removedEnd = state_.selectionEnd;
    deleteSelection();
  } else {
    auto pos = getActualCursorCoordinates();
    setCursorPosition(pos);

    if (state_.cursorPosition.column == 0) {
      handleStartOfLineDelete(pos, undo);
    } else {
      handleMidLineBackspace(pos, undo);
    }

    updateChangeTracking();
    textChanged_ = true;
    fireContentUpdate();

    requestEnsureCursorVisible();
    colorize(state_.cursorPosition.line, 1);
  }

  undo.after = state_;
  addUndo(undo);

  if (activeAutocomplete_) {
    requestAutocomplete();
  }
}

// ---------------------------------------------------------------------------
// setText
// ---------------------------------------------------------------------------

void TextEditorCore::setText(std::string_view text, bool preserveScroll) {
  // Clear existing state
  foldBegin_.clear();
  foldEnd_.clear();
  foldSorted_ = false;
  undoBuffer_.clear();
  undoIndex_ = 0;

  text_.setText(text);

  // Update editor state
  textChanged_ = true;
  // Only request scroll-to-top for "loaded a different document" calls.
  // In-place edits like the canvas→text writeback pass `preserveScroll`
  // so the user's view doesn't snap back to line 0 every time they
  // drag a shape.
  if (!preserveScroll) {
    scrollToTop_ = true;
  }

  detectIndentationStyle();
  colorize();
}

// ---------------------------------------------------------------------------
// Tab size
// ---------------------------------------------------------------------------

void TextEditorCore::setTabSize(int size) {
  tabSize_ = std::clamp(size, 0, 32);
}

// ---------------------------------------------------------------------------
// Indentation detection
// ---------------------------------------------------------------------------

void TextEditorCore::detectIndentationStyle() {
  int tabCount = 0;
  int spaceCount = 0;
  std::vector<int> indentLevels;
  int linesChecked = std::min<int>(text_.getTotalLines(), 50);

  for (int i = 0; i < linesChecked; ++i) {
    const Line& line = text_.getLineGlyphs(i);
    int leadingSpaces = 0;
    int leadingTabs = 0;
    for (auto& g : line) {
      if (g.character == ' ')
        leadingSpaces++;
      else if (g.character == '\t')
        leadingTabs++;
      else
        break;
    }

    // Count total spaces/tabs to guess mode
    spaceCount += leadingSpaces;
    tabCount += leadingTabs;

    // Collect indentation levels for guessing tab size if using spaces
    if (leadingTabs == 0 && leadingSpaces > 0) {
      indentLevels.push_back(leadingSpaces);
    }
  }

  // Decide indent mode
  if (tabCount > spaceCount) {
    indentMode_ = IndentMode::Tabs;
    insertSpaces_ = false;
  } else {
    indentMode_ = IndentMode::Spaces;
    insertSpaces_ = true;

    // If we have indentation levels, find gcd for tab size guess
    if (!indentLevels.empty()) {
      auto gcd = [](int a, int b) {
        while (b != 0) {
          int t = a % b;
          a = b;
          b = t;
        }
        return a;
      };

      int computedTabSize = indentLevels[0];
      for (size_t i = 1; i < indentLevels.size(); ++i) {
        computedTabSize = gcd(computedTabSize, indentLevels[i]);
        if (computedTabSize < 2) {
          computedTabSize = 4;
          break;
        }
      }
      tabSize_ = std::max(1, computedTabSize);
    } else {
      tabSize_ = 4;
    }
  }
}

// ---------------------------------------------------------------------------
// Syntax highlighting
// ---------------------------------------------------------------------------

void TextEditorCore::colorize(int fromLine, int lines) {
  const int toLine =
      lines == -1 ? text_.getTotalLines() : std::min<int>(text_.getTotalLines(), fromLine + lines);

  colorRangeMin_ = std::min(colorRangeMin_, fromLine);
  colorRangeMax_ = std::max(colorRangeMax_, toLine);
  colorRangeMin_ = std::max(0, colorRangeMin_);
  colorRangeMax_ = std::max(colorRangeMin_, colorRangeMax_);
  checkComments_ = true;
}

void TextEditorCore::colorizeRange(int fromLine, int toLine) {
  if (text_.getTotalLines() == 0 || !colorizerEnabled_) {
    return;
  }

  std::string buffer;
  std::cmatch results;
  std::string_view identifier;

  const int endLine = std::max(0, std::min(text_.getTotalLines(), toLine));

  for (int i = fromLine; i < endLine; ++i) {
    Line& line = text_.getLineGlyphsMutable(i);
    if (line.empty()) {
      continue;
    }

    // Build buffer from line
    buffer.resize(line.size());
    for (size_t j = 0; j < line.size(); ++j) {
      buffer[j] = line[j].character;
      line[j].colorIndex = ColorIndex::Default;
    }

    const char* current = buffer.data();
    const char* end = current + buffer.size();

    while (current != end) {
      const char* tokenBegin = nullptr;
      const char* tokenEnd = nullptr;
      ColorIndex tokenColor = ColorIndex::Default;
      bool hasToken = false;

      // Try custom tokenizer if available
      if (languageDefinition_.tokenize) {
        if (languageDefinition_.tokenize(current, end, tokenBegin, tokenEnd, tokenColor)) {
          hasToken = true;
        }
      }

      // Try regex matching if no custom token found
      if (!hasToken) {
        for (const auto& [regex, color] : regexList_) {
          if (std::regex_search(current, end, results, regex,
                                std::regex_constants::match_continuous)) {
            const auto& match = *results.begin();
            tokenBegin = match.first;
            tokenEnd = match.second;
            tokenColor = color;
            hasToken = true;
            break;
          }
        }
      }

      if (!hasToken) {
        ++current;
        continue;
      }

      // Process token
      const size_t tokenLength = tokenEnd - tokenBegin;
      if (tokenColor == ColorIndex::Identifier) {
        identifier = std::string_view(tokenBegin, tokenLength);

        // Handle case sensitivity
        std::string upperIdentifier;
        if (!languageDefinition_.caseSensitive) {
          upperIdentifier.resize(identifier.size());
          std::transform(identifier.begin(), identifier.end(), upperIdentifier.begin(), ::toupper);
          identifier = upperIdentifier;
        }

        if (languageDefinition_.keywords.count(std::string(identifier))) {
          tokenColor = ColorIndex::Keyword;
        } else if (languageDefinition_.identifiers.count(std::string(identifier))) {
          tokenColor = ColorIndex::KnownIdentifier;
        }
      }

      // Apply color to token
      const size_t offset = tokenBegin - buffer.data();
      for (size_t j = 0; j < tokenLength; ++j) {
        line[offset + j].colorIndex = tokenColor;
      }

      current = tokenEnd;
    }
  }
}

void TextEditorCore::colorizeInternal() {
  if (text_.getTotalLines() == 0 || !colorizerEnabled_) {
    return;
  }

  if (checkComments_) {
    const auto endLine = text_.getTotalLines();
    size_t commentStartLine = endLine;
    size_t commentStartIndex = 0;
    bool withinString = false;
    bool withinSingleLineComment = false;
    bool concatenate = false;  // '\' at end of line

    for (size_t currentLine = 0; currentLine < endLine;) {
      Line& line = text_.getLineGlyphsMutable(currentLine);
      if (line.empty()) {
        currentLine++;
        continue;
      }

      for (size_t currentIndex = 0; currentIndex < line.size();) {
        if (currentIndex == 0 && !concatenate) {
          withinSingleLineComment = false;
        }

        auto& glyph = line[currentIndex];
        const char c = glyph.character;

        concatenate = (currentIndex == line.size() - 1 && c == '\\');

        const bool inComment =
            (commentStartLine < currentLine ||
             (commentStartLine == currentLine && commentStartIndex <= currentIndex));

        if (withinString) {
          glyph.isMultiLineComment = inComment;

          if (c == '\"') {
            if (currentIndex + 1 < line.size() && line[currentIndex + 1].character == '\"') {
              currentIndex++;
              if (currentIndex < line.size()) {
                line[currentIndex].isMultiLineComment = inComment;
              }
            } else {
              withinString = false;
            }
          } else if (c == '\\') {
            currentIndex++;
            if (currentIndex < line.size()) {
              line[currentIndex].isMultiLineComment = inComment;
            }
          }
        } else {
          if (c == '\"') {
            withinString = true;
            glyph.isMultiLineComment = inComment;
          } else {
            const std::string_view lineView(reinterpret_cast<const char*>(&line[0].character),
                                            line.size());

            if (!withinSingleLineComment &&
                matchesCommentStart(lineView, currentIndex, languageDefinition_.commentStart)) {
              commentStartLine = currentLine;
              commentStartIndex = currentIndex;
            } else if (!languageDefinition_.singleLineComment.empty() &&
                       matchesCommentStart(lineView, currentIndex,
                                           languageDefinition_.singleLineComment)) {
              withinSingleLineComment = true;
            }

            glyph.isMultiLineComment = inComment;
            glyph.isComment = withinSingleLineComment;

            if (matchesCommentEnd(lineView, currentIndex, languageDefinition_.commentEnd)) {
              commentStartLine = endLine;
              commentStartIndex = 0;
            }
          }
        }

        currentIndex += Utf8::SequenceLength(c);
      }
      currentLine++;
    }
    checkComments_ = false;
  }

  if (colorRangeMin_ < colorRangeMax_) {
    const int increment = (languageDefinition_.tokenize == nullptr) ? 10 : 10000;
    const int to = std::min(colorRangeMin_ + increment, colorRangeMax_);
    colorizeRange(colorRangeMin_, to);
    colorRangeMin_ = to;

    if (colorRangeMax_ == colorRangeMin_) {
      colorRangeMin_ = std::numeric_limits<int>::max();
      colorRangeMax_ = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Hook helpers
// ---------------------------------------------------------------------------

void TextEditorCore::requestEnsureCursorVisible() {
  if (ensureCursorVisibleHook) {
    ensureCursorVisibleHook();
  }
}

void TextEditorCore::requestAutocomplete() {
  if (requestAutocompleteHook) {
    requestAutocompleteHook();
  }
}

void TextEditorCore::fireContentUpdate() {
  if (onContentUpdateInternal) {
    onContentUpdateInternal();
  }
}

// ---------------------------------------------------------------------------
// UndoRecord
// ---------------------------------------------------------------------------

UndoRecord::UndoRecord(std::string_view added, const Coordinates& addedStart,
                       const Coordinates& addedEnd, std::string_view removed,
                       const Coordinates& removedStart, const Coordinates& removedEnd,
                       const EditorState& before, const EditorState& after)
    : added(added),
      addedStart(addedStart),
      addedEnd(addedEnd),
      removed(removed),
      removedStart(removedStart),
      removedEnd(removedEnd),
      before(before),
      after(after) {
  UTILS_RELEASE_ASSERT(addedStart <= addedEnd);
  UTILS_RELEASE_ASSERT(removedStart <= removedEnd);
}

void UndoRecord::undo(TextEditorCore* core) {
  if (!added.empty()) {
    core->deleteRange(addedStart, addedEnd);
    core->colorize(addedStart.line - 1, addedEnd.line - addedStart.line + 2);
  }

  if (!removed.empty()) {
    auto start = removedStart;
    core->insertTextAt(start, removed);
    core->colorize(removedStart.line - 1, removedEnd.line - removedStart.line + 2);
  }

  core->mutableState() = before;
}

void UndoRecord::redo(TextEditorCore* core) {
  if (!removed.empty()) {
    core->deleteRange(removedStart, removedEnd);
    core->colorize(removedStart.line - 1, removedEnd.line - removedStart.line + 1);
  }

  if (!added.empty()) {
    auto start = addedStart;
    core->insertTextAt(start, added);
    core->colorize(addedStart.line - 1, addedEnd.line - addedStart.line + 1);
  }

  core->mutableState() = after;
}

// ---------------------------------------------------------------------------
// SVG language definition
// ---------------------------------------------------------------------------

const LanguageDefinition& LanguageDefinition::SVG() {
  static const LanguageDefinition langDef = [] {
    LanguageDefinition def;

    // SVG element names — used as keywords so tag names like <rect>, <circle>
    // highlight distinctly from unknown elements. This list is hardcoded
    // rather than pulled from kSVGElementNames because the text_editor
    // target must not depend on //donner/svg. Callers that want the full
    // registry-derived list can add more via `def.keywords.insert(...)`.
    static constexpr std::array keywords{
        "a",
        "animate",
        "animateMotion",
        "animateTransform",
        "circle",
        "clipPath",
        "defs",
        "desc",
        "ellipse",
        "feBlend",
        "feColorMatrix",
        "feComponentTransfer",
        "feComposite",
        "feConvolveMatrix",
        "feDiffuseLighting",
        "feDisplacementMap",
        "feDistantLight",
        "feDropShadow",
        "feFlood",
        "feFuncA",
        "feFuncB",
        "feFuncG",
        "feFuncR",
        "feGaussianBlur",
        "feImage",
        "feMerge",
        "feMergeNode",
        "feMorphology",
        "feOffset",
        "fePointLight",
        "feSpecularLighting",
        "feSpotLight",
        "feTile",
        "feTurbulence",
        "filter",
        "g",
        "image",
        "line",
        "linearGradient",
        "marker",
        "mask",
        "mpath",
        "path",
        "pattern",
        "polygon",
        "polyline",
        "radialGradient",
        "rect",
        "set",
        "stop",
        "style",
        "svg",
        "switch",
        "symbol",
        "text",
        "textPath",
        "title",
        "tspan",
        "use",
    };
    def.keywords.insert(keywords.begin(), keywords.end());

    // Known SVG/CSS attribute names — highlighted as KnownIdentifier.
    static constexpr std::array knownAttrs{
        "id",
        "class",
        "style",
        "viewBox",
        "xmlns",
        "href",
        "transform",
        "fill",
        "stroke",
        "opacity",
        "d",
        "cx",
        "cy",
        "r",
        "rx",
        "ry",
        "x",
        "y",
        "width",
        "height",
        "x1",
        "y1",
        "x2",
        "y2",
        "preserveAspectRatio",
        "offset",
        "stop-color",
        "stop-opacity",
        "font-family",
        "font-size",
        "font-weight",
        "font-style",
        "text-anchor",
        "dominant-baseline",
        "stroke-width",
        "stroke-linecap",
        "stroke-linejoin",
        "stroke-dasharray",
        "fill-opacity",
        "stroke-opacity",
        "fill-rule",
        "clip-path",
        "clip-rule",
        "mask",
        "filter",
        "display",
        "visibility",
        "color",
        "pointer-events",
    };
    for (const auto& attr : knownAttrs) {
      def.identifiers.emplace(attr, Identifier(attr));
    }

    // Custom XML-aware tokenizer.
    def.tokenize = [](const char* inBegin, const char* inEnd, const char*& outBegin,
                      const char*& outEnd, ColorIndex& outColor) -> bool {
      if (inBegin >= inEnd) return false;

      const char* p = inBegin;
      const char c = *p;

      // Tag delimiters: <, </, />, >
      if (c == '<') {
        if (p + 1 < inEnd && p[1] == '/') {
          outBegin = p;
          outEnd = p + 2;
          outColor = ColorIndex::Punctuation;
          return true;
        }
        outBegin = p;
        outEnd = p + 1;
        outColor = ColorIndex::Punctuation;
        return true;
      }
      if (c == '>') {
        outBegin = p;
        outEnd = p + 1;
        outColor = ColorIndex::Punctuation;
        return true;
      }
      if (c == '/' && p + 1 < inEnd && p[1] == '>') {
        outBegin = p;
        outEnd = p + 2;
        outColor = ColorIndex::Punctuation;
        return true;
      }

      // Quoted strings (attribute values).
      if (c == '"' || c == '\'') {
        outBegin = p;
        outEnd = p + 1;
        outColor = ColorIndex::Punctuation;
        return true;
      }

      // Entity references: &name; or &#digits; or &#xhex;
      if (c == '&') {
        ++p;
        if (p < inEnd && *p == '#') {
          ++p;
          if (p < inEnd && (*p == 'x' || *p == 'X')) ++p;
        }
        while (p < inEnd && *p != ';' && *p != '<' && *p != '>' && *p != ' ' && *p != '\t' &&
               *p != '\n') {
          ++p;
        }
        if (p < inEnd && *p == ';') ++p;
        outBegin = inBegin;
        outEnd = p;
        outColor = ColorIndex::Number;
        return true;
      }

      // Numbers
      if ((c >= '0' && c <= '9') || (c == '-' && p + 1 < inEnd && p[1] >= '0' && p[1] <= '9') ||
          (c == '.' && p + 1 < inEnd && p[1] >= '0' && p[1] <= '9')) {
        if (c == '-') ++p;
        while (p < inEnd && ((*p >= '0' && *p <= '9') || *p == '.')) {
          ++p;
        }
        if (p < inEnd && (*p == 'e' || *p == 'E')) {
          ++p;
          if (p < inEnd && (*p == '+' || *p == '-')) ++p;
          while (p < inEnd && *p >= '0' && *p <= '9') ++p;
        }
        if (p < inEnd && *p == '%') {
          ++p;
        } else {
          const char* unitStart = p;
          while (p < inEnd && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            ++p;
          }
          (void)unitStart;
        }
        outBegin = inBegin;
        outEnd = p;
        outColor = ColorIndex::Number;
        return true;
      }

      // Identifiers
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
          static_cast<unsigned char>(c) >= 0x80) {
        ++p;
        while (p < inEnd && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                             (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.' ||
                             *p == ':' || static_cast<unsigned char>(*p) >= 0x80)) {
          ++p;
        }
        outBegin = inBegin;
        outEnd = p;
        outColor = ColorIndex::Identifier;
        return true;
      }

      // Equals sign (structural, not colored)
      if (c == '=') {
        outBegin = p;
        outEnd = p + 1;
        outColor = ColorIndex::Punctuation;
        return true;
      }

      return false;
    };

    def.commentStart = "<!--";
    def.commentEnd = "-->";
    def.singleLineComment = "";

    def.caseSensitive = true;
    def.autoIndentation = true;
    def.name = "SVG";

    return def;
  }();

  return langDef;
}

}  // namespace donner::editor
