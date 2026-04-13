#include "donner/editor/TextEditor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <regex>
#include <stack>
#include <string>

#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

namespace donner::editor {

namespace {

/**
 * Compare two ranges for equality using a binary predicate.
 */
template <typename InputIt1, typename InputIt2, typename BinaryPredicate>
bool rangesEqual(InputIt1 first1, InputIt1 last1, InputIt2 first2, InputIt2 last2,
                 BinaryPredicate pred) {
  for (; first1 != last1 && first2 != last2; ++first1, ++first2) {
    if (!pred(*first1, *first2)) {
      return false;
    }
  }
  return first1 == last1 && first2 == last2;
}

constexpr bool IsAscii(char c) {
  return static_cast<unsigned char>(c) <= 127;
}

}  // namespace

TextEditor::TextEditor()
    : text_(core_.buffer()),
      state_(core_.mutableState()),
      replaceIndex_(core_.replaceIndexRef()),
      foldBegin_(core_.foldBegin()),
      foldEnd_(core_.foldEnd()),
      foldSorted_(core_.foldSorted()),
      scrollbarMarkers_(core_.scrollbarMarkersRef()),
      changedLines_(core_.changedLines()),
      tabSize_(core_.tabSizeRef()),
      scrollToCursor_(core_.scrollToCursorRef()),
      scrollToTop_(core_.scrollToTopRef()),
      cursorPositionChanged_(core_.cursorPositionChangedRef()),
      textChanged_(core_.textChangedRef()),
      autoIndentOnPaste_(core_.autoIndentOnPasteRef()),
      selectionMode_(core_.selectionModeRef()),
      paletteBase_(core_.mutablePaletteBase()),
      palette_(core_.mutablePalette()),
      languageDefinition_(core_.getLanguageDefinition()),
      errorMarkers_(core_.mutableErrorMarkers()),
      interactiveStart_(core_.interactiveStart()),
      interactiveEnd_(core_.interactiveEnd()) {
  // Initialize with default dark color scheme
  setPalette(getDarkPalette());
  setLanguageDefinition(LanguageDefinition::SVG());

  // Set keyboard shortcuts
  shortcuts_ = getDefaultShortcuts();

  // Store creation time
  startTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

  // Wire the core's shell-hook callbacks so that moved editing paths
  // (e.g. `backspace`, `enterCharacter`) can still invoke the ImGui-side
  // helpers that live in this shell. When the shell is driven from a
  // headless test context (C3), these hooks remain set but the inner
  // methods early-return outside `withinRender_`.
  core_.ensureCursorVisibleHook = [this] { ensureCursorVisible(); };
  core_.requestAutocompleteHook = [this] {
    requestAutocomplete_ = true;
    readyForAutocomplete_ = false;
  };
  core_.functionTooltipHook = [this](char32_t ch, const Coordinates& pos) {
    handleFunctionTooltip(static_cast<ImWchar>(ch), pos);
  };
  core_.onContentUpdateInternal = [this] {
    if (onContentUpdate) {
      onContentUpdate(this);
    }
  };
}

TextEditor::~TextEditor() = default;

std::vector<Shortcut> TextEditor::getDefaultShortcuts() {
  std::vector<Shortcut> shortcuts;
  shortcuts.resize(static_cast<int>(ShortcutId::Count));

  // Basic navigation
  shortcuts[static_cast<int>(ShortcutId::MoveUp)] = Shortcut(ImGuiKey_UpArrow);
  shortcuts[static_cast<int>(ShortcutId::SelectUp)] =
      Shortcut(ImGuiKey_UpArrow, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveDown)] = Shortcut(ImGuiKey_DownArrow);
  shortcuts[static_cast<int>(ShortcutId::SelectDown)] =
      Shortcut(ImGuiKey_DownArrow, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveLeft)] = Shortcut(ImGuiKey_LeftArrow);
  shortcuts[static_cast<int>(ShortcutId::SelectLeft)] =
      Shortcut(ImGuiKey_LeftArrow, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveWordLeft)] =
      Shortcut(ImGuiKey_LeftArrow, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::SelectWordLeft)] =
      Shortcut(ImGuiKey_LeftArrow, ShortcutModifier::Ctrl | ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveRight)] = Shortcut(ImGuiKey_RightArrow);
  shortcuts[static_cast<int>(ShortcutId::SelectRight)] =
      Shortcut(ImGuiKey_RightArrow, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveWordRight)] =
      Shortcut(ImGuiKey_RightArrow, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::SelectWordRight)] =
      Shortcut(ImGuiKey_RightArrow, ShortcutModifier::Ctrl | ShortcutModifier::Shift);

  // Block navigation
  shortcuts[static_cast<int>(ShortcutId::MoveUpBlock)] = Shortcut(ImGuiKey_PageUp);
  shortcuts[static_cast<int>(ShortcutId::SelectUpBlock)] =
      Shortcut(ImGuiKey_PageUp, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveDownBlock)] = Shortcut(ImGuiKey_PageDown);
  shortcuts[static_cast<int>(ShortcutId::SelectDownBlock)] =
      Shortcut(ImGuiKey_PageDown, ShortcutModifier::Shift);

  // Document navigation
  shortcuts[static_cast<int>(ShortcutId::MoveTop)] =
      Shortcut(ImGuiKey_Home, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::SelectTop)] =
      Shortcut(ImGuiKey_Home, ShortcutModifier::Ctrl | ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveBottom)] =
      Shortcut(ImGuiKey_End, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::SelectBottom)] =
      Shortcut(ImGuiKey_End, ShortcutModifier::Ctrl | ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveStartLine)] = Shortcut(ImGuiKey_Home);
  shortcuts[static_cast<int>(ShortcutId::SelectStartLine)] =
      Shortcut(ImGuiKey_Home, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::MoveEndLine)] = Shortcut(ImGuiKey_End);
  shortcuts[static_cast<int>(ShortcutId::SelectEndLine)] =
      Shortcut(ImGuiKey_End, ShortcutModifier::Shift);

  // Editing operations
  shortcuts[static_cast<int>(ShortcutId::Undo)] = Shortcut(ImGuiKey_Z, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::Redo)] = Shortcut(ImGuiKey_Y, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::DeleteRight)] =
      Shortcut(ImGuiKey_Delete, ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::ForwardDelete)] = Shortcut(ImGuiKey_Delete);
  shortcuts[static_cast<int>(ShortcutId::ForwardDeleteWord)] =
      Shortcut(ImGuiKey_Delete, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::BackwardDelete)] = Shortcut(ImGuiKey_Backspace);
  shortcuts[static_cast<int>(ShortcutId::BackwardDeleteWord)] =
      Shortcut(ImGuiKey_Backspace, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::DeleteLeft)] =
      Shortcut(ImGuiKey_Backspace, ShortcutModifier::Shift);

  // Special operations
  shortcuts[static_cast<int>(ShortcutId::NewLine)] = Shortcut(ImGuiKey_Enter);
  shortcuts[static_cast<int>(ShortcutId::Copy)] = Shortcut(ImGuiKey_C, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::Paste)] = Shortcut(ImGuiKey_V, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::Cut)] = Shortcut(ImGuiKey_X, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::SelectAll)] = Shortcut(ImGuiKey_A, ShortcutModifier::Ctrl);

  // Code editing
  shortcuts[static_cast<int>(ShortcutId::Indent)] = Shortcut(ImGuiKey_Tab);
  shortcuts[static_cast<int>(ShortcutId::DuplicateLine)] =
      Shortcut(ImGuiKey_D, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::CommentLines)] =
      Shortcut(ImGuiKey_K, ShortcutModifier::Ctrl | ShortcutModifier::Shift);
  shortcuts[static_cast<int>(ShortcutId::UncommentLines)] =
      Shortcut(ImGuiKey_U, ShortcutModifier::Ctrl | ShortcutModifier::Shift);

  // Search and replace
  shortcuts[static_cast<int>(ShortcutId::Find)] = Shortcut(ImGuiKey_F, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::Replace)] = Shortcut(ImGuiKey_H, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::FindNext)] = Shortcut(ImGuiKey_G, ShortcutModifier::Ctrl);

  // Autocomplete
  shortcuts[static_cast<int>(ShortcutId::AutocompleteOpen)] =
      Shortcut(ImGuiKey_I, ShortcutModifier::Ctrl);
  shortcuts[static_cast<int>(ShortcutId::AutocompleteSelectActive)] = Shortcut(ImGuiKey_Enter);
  shortcuts[static_cast<int>(ShortcutId::AutocompleteUp)] = Shortcut(ImGuiKey_UpArrow);
  shortcuts[static_cast<int>(ShortcutId::AutocompleteDown)] = Shortcut(ImGuiKey_DownArrow);

  return shortcuts;
}

// ---------------------------------------------------------------------------
// Forwarders to TextEditorCore. The bodies live in TextEditorCore.cc; the
// shell keeps the public API for source-level compatibility.
// ---------------------------------------------------------------------------

void TextEditor::setLanguageDefinition(const LanguageDefinition& langDef) {
  core_.setLanguageDefinition(langDef);
}

void TextEditor::setPalette(const Palette& value) {
  core_.setPalette(value);
}

std::string TextEditor::getText(const Coordinates& start, const Coordinates& end) const {
  return core_.getText(start, end);
}

Coordinates TextEditor::getActualCursorCoordinates() const {
  return core_.getActualCursorCoordinates();
}

Coordinates TextEditor::sanitizeCoordinates(const Coordinates& value) const {
  return core_.sanitizeCoordinates(value);
}

void TextEditor::advance(Coordinates& coords) const {
  core_.advance(coords);
}

void TextEditor::deleteRange(const Coordinates& start, const Coordinates& end) {
  core_.deleteRange(start, end);
}

int TextEditor::insertTextAt(Coordinates& where, std::string_view text, bool indent) {
  return core_.insertTextAt(where, text, indent);
}

void TextEditor::addUndo(UndoRecord& record) {
  core_.addUndo(record);
}

namespace {

/**
 * Handle folded lines adjustment
 */
int adjustLineForFolds(int lineNo, const std::vector<Coordinates>& foldBegin,
                       const std::vector<Coordinates>& foldEnd, const std::vector<bool>& fold,
                       const std::vector<int>& foldConnection, int maxLine) {
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
std::pair<int, int> calculateColumnPosition(const Line& line, float localX, float textStart,
                                            float charAdvanceX, int tabSize) {
  int columnIndex = 0;
  int columnCoord = 0;
  float columnX = 0.0f;

  while (static_cast<size_t>(columnIndex) < line.size()) {
    float columnWidth = 0.0f;

    if (line[columnIndex].character == '\t') {
      float spaceSize =
          ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
      float oldX = columnX;
      float newColumnX =
          (1.0f + std::floor((1.0f + columnX) / (static_cast<float>(tabSize) * spaceSize))) *
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
      columnWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf).x;
      if (textStart + columnX + columnWidth * 0.5f > localX) {
        break;
      }
      columnX += columnWidth;
      columnCoord++;
    }
  }

  return {columnCoord, columnIndex};
}

}  // namespace

Coordinates TextEditor::screenPosToCoordinates(const ImVec2& position) const {
  // Local coordinates relative to editor window
  const ImVec2 local{position.x - uiCursorPos_.x, position.y - uiCursorPos_.y};

  // Calculate initial line number from Y position
  const int initialLineNo = std::max(0, static_cast<int>(std::floor(local.y / charAdvance_.y)));

  // Handle folded regions if enabled
  const int lineNo = foldEnabled_ ? adjustLineForFolds(initialLineNo, foldBegin_, foldEnd_, fold_,
                                                       foldConnection_, text_.getTotalLines() - 1)
                                  : initialLineNo;

  // Calculate column position if line is valid
  if (lineNo >= 0 && lineNo < text_.getTotalLines()) {
    const Line& line = text_.getLineGlyphs(lineNo);
    const auto [columnCoord, _] =
        calculateColumnPosition(line, local.x, textStart_, charAdvance_.x, tabSize_);
    return sanitizeCoordinates(Coordinates(lineNo, columnCoord));
  }

  // Return coordinates at start of line if line number invalid
  return sanitizeCoordinates(Coordinates(lineNo, 0));
}

Coordinates TextEditor::mousePosToCoordinates(const ImVec2& position) const {
  // Local coordinates relative to editor window
  const ImVec2 local{position.x - uiCursorPos_.x, position.y - uiCursorPos_.y};

  // Calculate initial line number from Y position
  const int initialLineNo = std::max(0, static_cast<int>(std::floor(local.y / charAdvance_.y)));

  // Handle folded regions if enabled
  const int lineNo = foldEnabled_ ? adjustLineForFolds(initialLineNo, foldBegin_, foldEnd_, fold_,
                                                       foldConnection_, text_.getTotalLines() - 1)
                                  : initialLineNo;

  // Calculate column position if line is valid
  if (lineNo >= 0 && lineNo < text_.getTotalLines()) {
    const Line& line = text_.getLineGlyphs(lineNo);
    const auto [columnCoord, columnIndex] =
        calculateColumnPosition(line, local.x, textStart_, charAdvance_.x, tabSize_);

    // Calculate tab character adjustments
    int tabModifier = 0;
    for (int i = 0; i < columnIndex; i++) {
      if (line[i].character == '\t') {
        tabModifier += 3;
      }
    }

    return sanitizeCoordinates(Coordinates(lineNo, columnCoord - tabModifier));
  }

  // Return coordinates at start of line if line number invalid
  return sanitizeCoordinates(Coordinates(lineNo, 0));
}

Coordinates TextEditor::findWordStart(const Coordinates& from) const {
  return core_.findWordStart(from);
}

Coordinates TextEditor::findWordEnd(const Coordinates& from) const {
  return core_.findWordEnd(from);
}

Coordinates TextEditor::findNextWord(const Coordinates& from) const {
  return core_.findNextWord(from);
}

bool TextEditor::isOnWordBoundary(const Coordinates& at) const {
  return core_.isOnWordBoundary(at);
}

void TextEditor::removeLine(int start, int end) {
  core_.removeLine(start, end);
}

void TextEditor::removeLine(int index) {
  core_.removeLine(index);
}

Line& TextEditor::insertLine(int index, int column) {
  return core_.insertLine(index, column);
}

RcString TextEditor::getWordUnderCursor() const {
  return core_.getWordUnderCursor();
}

RcString TextEditor::getWordAt(const Coordinates& coords) const {
  return core_.getWordAt(coords);
}

ImU32 TextEditor::getGlyphColor(const Glyph& glyph) const {
  return core_.getGlyphColor(glyph);
}

Coordinates TextEditor::findFirst(std::string_view searchText, const Coordinates& start) const {
  return core_.findFirst(searchText, start);
}

void TextEditor::handleCharacterInput(const ImGuiIO& io, bool shift, bool& keepAutocompleteOpen,
                                      bool& hasWrittenLetter) {
  // Process regular characters from InputQueueCharacters
  for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
    const auto c = static_cast<unsigned char>(io.InputQueueCharacters[i]);
    if (c == 0 || !(c == '\n' || c >= 32)) {
      continue;
    }

    // Insert character
    enterCharacter(c, shift);

    if (c == '.') {
      buildMemberSuggestions(&keepAutocompleteOpen);
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      hasWrittenLetter = true;
    }

    if (hasSnippets()) {
      handleSnippetInsertion(c);
    }
  }
}

void TextEditor::autocompleteSelect() {
  if (autocompleteIndex_ >= static_cast<int>(autocompleteSuggestions_.size())) {
    return;
  }

  UndoRecord undo;
  undo.before = state_;

  if (hasSelection()) {
    undo.removed = getSelectedText();
    undo.removedStart = state_.selectionStart;
    undo.removedEnd = state_.selectionEnd;
    deleteSelection();
  }

  const auto& entry = autocompleteSuggestions_[autocompleteIndex_];
  undo.added = entry.second;
  undo.addedStart = getActualCursorCoordinates();
  insertText(entry.second, true);
  undo.addedEnd = getActualCursorCoordinates();
  undo.after = state_;
  addUndo(undo);

  autocompleteOpened_ = false;
  autocompleteObject_.clear();
}

void TextEditor::autocompleteNavigate(bool up) {
  if (up) {
    autocompleteIndex_ = std::max(0, autocompleteIndex_ - 1);
  } else {
    autocompleteIndex_ =
        std::min(static_cast<int>(autocompleteSuggestions_.size()) - 1, autocompleteIndex_ + 1);
  }
  autocompleteSwitched_ = true;
}

bool TextEditor::hasSnippets() const {
  return !snippetTagStart_.empty();
}

void TextEditor::handleSnippetInsertion(ImWchar character) {
  snippetTagLength_++;
  snippetTagEnd_[snippetTagSelected_].column =
      snippetTagStart_[snippetTagSelected_].column + snippetTagLength_;

  const auto curCursor = getCursorPosition();
  setSelection(snippetTagStart_[snippetTagSelected_], snippetTagEnd_[snippetTagSelected_]);

  const auto word = getSelectedText();
  std::unordered_map<int, int> modifications;
  modifications[curCursor.line] = 0;

  for (size_t i = 0; i < snippetTagStart_.size(); i++) {
    if (i != snippetTagSelected_) {
      snippetTagStart_[i].column += modifications[snippetTagStart_[i].line];
      snippetTagEnd_[i].column += modifications[snippetTagStart_[i].line];
    }

    if (snippetTagId_[i] == snippetTagId_[snippetTagSelected_]) {
      modifications[snippetTagStart_[i].line] += snippetTagLength_ - snippetTagPreviousLength_;

      if (i != snippetTagSelected_) {
        setSelection(snippetTagStart_[i], snippetTagEnd_[i]);
        backspace();
        insertText(word);
        snippetTagEnd_[i].column = snippetTagStart_[i].column + snippetTagLength_;
      }
    }
  }

  setSelection(curCursor, curCursor);
  setCursorPosition(curCursor);
  ensureCursorVisible();
  snippetTagPreviousLength_ = snippetTagLength_;
}

void TextEditor::executeAction(TextEditor::ShortcutId actionId, bool& keepAutocompleteOpen,
                               bool shift, bool ctrl) {
  switch (actionId) {
    case ShortcutId::Indent:
      if (hasSelection()) {
        // TODO: Indent
      } else {
        enterCharacter('\t', false);
      }
      break;
    case TextEditor::ShortcutId::Undo: undo(); break;
    case TextEditor::ShortcutId::Redo: redo(); break;
    case TextEditor::ShortcutId::MoveUp: moveUp(1, /*select*/ false); break;
    case TextEditor::ShortcutId::MoveDown: moveDown(1, /*select*/ false); break;
    case TextEditor::ShortcutId::MoveLeft: moveLeft(1, /*select*/ false, /*wordMode*/ false); break;
    case TextEditor::ShortcutId::MoveRight:
      moveRight(1, /*select*/ false, /*wordMode*/ false);
      break;
    case TextEditor::ShortcutId::SelectUp: moveUp(1, /*select*/ true); break;
    case TextEditor::ShortcutId::SelectDown: moveDown(1, /*select*/ true); break;
    case TextEditor::ShortcutId::DeleteRight:
    case TextEditor::ShortcutId::ForwardDelete: delete_(); break;
    case TextEditor::ShortcutId::DeleteLeft:
    case TextEditor::ShortcutId::BackwardDelete: backspace(); break;
    case TextEditor::ShortcutId::Copy: copy(); break;
    case TextEditor::ShortcutId::Paste: paste(); break;
    case TextEditor::ShortcutId::Cut: cut(); break;
    case TextEditor::ShortcutId::SelectAll: selectAll(); break;
    case TextEditor::ShortcutId::AutocompleteSelect:
    case TextEditor::ShortcutId::AutocompleteSelectActive:
      autocompleteSelect();
      keepAutocompleteOpen = false;
      break;
    case TextEditor::ShortcutId::AutocompleteUp:
      autocompleteNavigate(true);
      keepAutocompleteOpen = true;
      break;
    case TextEditor::ShortcutId::AutocompleteDown:
      autocompleteNavigate(false);
      keepAutocompleteOpen = true;
      break;
    case TextEditor::ShortcutId::NewLine: enterCharacter('\n', false); break;
    default: break;
  }
}

void TextEditor::handleKeyboardInputs() {
  ImGuiIO& io = ImGui::GetIO();
  const bool shift = io.KeyShift;
  const bool ctrl = io.KeyCtrl;

  if (!ImGui::IsWindowFocused()) {
    io.WantCaptureKeyboard = false;
    io.WantTextInput = false;
    return;
  }

  const bool editorFocused = !findFocused_ && !replaceFocused_;

  if (ImGui::IsWindowHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  }

  // Handle shortcuts
  ShortcutId actionId = ShortcutId::Count;
  for (int i = 0; i < shortcuts_.size(); i++) {
    const auto& shortcut = shortcuts_[i];
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
        case ShortcutId::SelectAll: additionalChecks = editorFocused; break;

        case ShortcutId::MoveUp:
        case ShortcutId::MoveDown:
        case ShortcutId::SelectUp:
        case ShortcutId::SelectDown:
          additionalChecks = !autocompleteOpened_ && editorFocused;
          break;

        case ShortcutId::AutocompleteUp:
        case ShortcutId::AutocompleteDown:
        case ShortcutId::AutocompleteSelect: additionalChecks = autocompleteOpened_; break;

        case ShortcutId::AutocompleteSelectActive:
          additionalChecks = autocompleteOpened_ && autocompleteSwitched_;
          break;

        case ShortcutId::NewLine:
        case ShortcutId::Indent:
        case ShortcutId::Unindent: additionalChecks = !autocompleteOpened_ && editorFocused; break;

        default: break;
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

  if ((autocompleteOpened_ && !keepAutocompleteOpen) || functionDeclarationTooltip_) {
    if (hasWrittenLetter) {
      if (functionTooltipState == functionDeclarationTooltip_) {
        functionDeclarationTooltip_ = false;
      }
      autocompleteOpened_ = false;
    }
  }
}

void TextEditor::handleMouseInputs() {
  ImGuiIO& io = ImGui::GetIO();
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
    const auto clickCoords = screenPosToCoordinates(ImGui::GetMousePos());
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ = clickCoords;
    selectionMode_ = SelectionMode::Line;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    lastClick_ = -1.0f;
    return;
  }

  // Double click - select word
  if (doubleClick && !ctrl) {
    const auto clickCoords = screenPosToCoordinates(ImGui::GetMousePos());
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ = clickCoords;
    selectionMode_ =
        (selectionMode_ == SelectionMode::Line) ? SelectionMode::Normal : SelectionMode::Word;
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

    const auto clickCoords = screenPosToCoordinates(mousePos);

    // Update cursor position immediately
    state_.cursorPosition = clickCoords;

    if (!shift) {
      interactiveStart_ = clickCoords;
    }
    interactiveEnd_ = clickCoords;

    selectionMode_ = ctrl && !shift ? SelectionMode::Word : SelectionMode::Normal;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    lastClick_ = currentTime;

    // Ensure cursor is visible after click
    ensureCursorVisible();
    return;
  }

  // Handle dragging (text selection)
  if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0)) {
    io.WantCaptureMouse = true;

    // Update selection end point to current mouse position
    auto mousePos = screenPosToCoordinates(ImGui::GetMousePos());
    state_.cursorPosition = interactiveEnd_ = mousePos;

    // Create selection from start to current point
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);

    // Handle autoscroll during selection
    const float mouseX = ImGui::GetMousePos().x;
    if (mouseX > findOrigin_.x + windowWidth_ - 50 && mouseX < findOrigin_.x + windowWidth_) {
      ImGui::SetScrollX(ImGui::GetScrollX() + 1.0f);
    } else if (mouseX > findOrigin_.x && mouseX < findOrigin_.x + textStart_ + 50) {
      ImGui::SetScrollX(ImGui::GetScrollX() - 1.0f);
    }

    // Handle vertical autoscroll
    const float mouseY = ImGui::GetMousePos().y;
    const float windowTop = ImGui::GetWindowPos().y;
    const float windowBottom = windowTop + ImGui::GetWindowHeight();

    if (mouseY < windowTop + 50) {
      ImGui::SetScrollY(ImGui::GetScrollY() - 1.0f);
    } else if (mouseY > windowBottom - 50) {
      ImGui::SetScrollY(ImGui::GetScrollY() + 1.0f);
    }
  }
}

void TextEditor::renderInternal(std::string_view title) {
  calculateCharacterAdvance();
  updatePaletteAlpha();

  UTILS_RELEASE_ASSERT(lineBuffer_.empty());
  focused_ = ImGui::IsWindowFocused() || findFocused_ || replaceFocused_;

  const auto contentSize = ImGui::GetWindowContentRegionMax();
  auto* drawList = ImGui::GetWindowDrawList();
  float longestLine = textStart_;

  if (scrollToTop_) {
    scrollToTop_ = false;
    ImGui::SetScrollY(0.0f);
  }

  const ImVec2 cursorScreenPos = uiCursorPos_ = ImGui::GetCursorScreenPos();
  const float scrollX = ImGui::GetScrollX();
  const float scrollY = lastScroll_ = ImGui::GetScrollY();

  // Calculate visible lines
  const int pageSize = static_cast<int>(std::floor((scrollY + contentSize.y) / charAdvance_.y));
  int lineNo = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const int totalLines = text_.getTotalLines();
  int visibleLineMax = std::max(0, std::min(totalLines - 1, lineNo + pageSize));

  updateTextStart();
  calculateFolds(lineNo, totalLines);

  while (lineNo <= visibleLineMax) {
    const ImVec2 lineStartPos{cursorScreenPos.x,
                              cursorScreenPos.y + (lineNo - foldedLines_) * charAdvance_.y};
    const ImVec2 textPos{lineStartPos.x + textStart_, lineStartPos.y};

    renderLine(lineNo, lineStartPos, textPos, contentSize, scrollX, drawList, longestLine);
    lineNo++;
  }

  renderExtraUI(drawList, cursorScreenPos, scrollX, scrollY, longestLine, contentSize);
  handleScrolling();
}
void TextEditor::handleScrolling() {
  // Only handle scrolling if we've explicitly requested to scroll to the
  // cursor.
  if (!scrollToCursor_) {
    return;
  }

  if (!withinRender_) {
    return;
  }

  const float scrollX = ImGui::GetScrollX();
  const float scrollY = ImGui::GetScrollY();
  const float height = ImGui::GetWindowHeight();
  const float width = windowWidth_;

  const auto pos = getActualCursorCoordinates();
  const auto len = getTextDistanceToLineStart(pos);

  const auto top = 1 + static_cast<int>(std::ceil(scrollY / charAdvance_.y));
  const auto bottom = static_cast<int>(std::ceil((scrollY + height) / charAdvance_.y));
  const auto left = static_cast<int>(std::ceil(scrollX / charAdvance_.x));
  const auto right = static_cast<int>(std::ceil((scrollX + width) / charAdvance_.x));

  if (pos.line < top) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line - 1) * charAdvance_.y));
  }
  if (pos.line > bottom - 4) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line + 4) * charAdvance_.y - height));
  }
  if (pos.column < left) {
    ImGui::SetScrollX(std::max(0.0f, len + textStart_ - 11 * charAdvance_.x));
  }
  if (len + textStart_ > (right - 4) * charAdvance_.x) {
    ImGui::SetScrollX(std::max(0.0f, len + textStart_ + 4 * charAdvance_.x - width));
  }

  scrollToCursor_ = false;
}

void TextEditor::calculateCharacterAdvance() {
  const float fontSize =
      ImGui::GetFont()
          ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr)
          .x;
  charAdvance_ = ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * lineSpacing_);
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
  snprintf(buf, sizeof(buf), " %3d ", text_.getTotalLines());
  textStart_ = (ImGui::GetFont()
                    ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr)
                    .x +
                leftMargin_) *
               sidebar_;
}

void TextEditor::calculateFolds(int currentLine, int totalLines) {
  const uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
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

  while (foldStart < text_.getTotalLines()) {
    for (int i = 0; i < static_cast<int>(foldBegin_.size()); i++) {
      if (foldBegin_[i].line == foldStart && i < static_cast<int>(fold_.size()) && fold_[i]) {
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

void TextEditor::renderLine(int lineNo, const ImVec2& lineStart, const ImVec2& textStart,
                            const ImVec2& contentSize, float scrollX, ImDrawList* drawList,
                            float& longestLine) {
  if (lineNo >= text_.getTotalLines()) {
    return;
  }

  const Line& line = text_.getLineGlyphs(lineNo);
  longestLine = std::max(
      textStart_ + getTextDistanceToLineStart(Coordinates(lineNo, text_.getLineMaxColumn(lineNo))),
      longestLine);

  // Current line highlight
  if (state_.cursorPosition.line == lineNo) {
    renderLineBackground(lineNo, lineStart, contentSize, drawList);
  }

  // Render line numbers if enabled
  if (showLineNumbers_) {
    renderLineNumbers(lineNo, lineStart, drawList);
  }

  // Render selection for this line
  if (hasSelection()) {
    renderSelection(lineNo, line, lineStart, contentSize, drawList);
  }

  // Render text
  renderText(line, textStart, drawList);

  // Render cursor if this is the cursor line
  if (state_.cursorPosition.line == lineNo) {
    renderCursor(lineStart, drawList);
  }
}

void TextEditor::renderErrorTooltip(int line, const std::string& message) {
  ImGui::BeginTooltip();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                           palette_[static_cast<int>(ColorIndex::ErrorMessage)]));
  ImGui::Text("Error at line %d:", line);
  ImGui::PopStyleColor();

  ImGui::Separator();

  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                           palette_[static_cast<int>(ColorIndex::ErrorMessage)]));
  ImGui::TextUnformatted(message.c_str());
  ImGui::PopStyleColor();

  ImGui::EndTooltip();
}

void TextEditor::renderLineBackground(int lineNo, const ImVec2& lineStart,
                                      const ImVec2& contentSize, ImDrawList* drawList) {
  const ImVec2 start{lineStart.x + ImGui::GetScrollX(), lineStart.y};

  // Error markers
  auto errorIt = errorMarkers_.find(lineNo + 1);
  if (errorIt != errorMarkers_.end()) {
    const ImVec2 end{lineStart.x + contentSize.x + 2.0f * ImGui::GetScrollX(),
                     lineStart.y + charAdvance_.y};

    drawList->AddRectFilled(start, end, palette_[static_cast<int>(ColorIndex::ErrorMarker)]);

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

      const auto fillColor =
          focused ? ColorIndex::CurrentLineFill : ColorIndex::CurrentLineFillInactive;

      drawList->AddRectFilled(start, end, palette_[static_cast<int>(fillColor)]);
      drawList->AddRect(start, end, palette_[static_cast<int>(ColorIndex::CurrentLineEdge)], 1.0f);
    }
  }

  // User-defined highlighted lines
  if (highlightLine_ && std::find(highlightedLines_.begin(), highlightedLines_.end(), lineNo) !=
                            highlightedLines_.end()) {
    const ImVec2 end{start.x + contentSize.x + ImGui::GetScrollX(), start.y + charAdvance_.y};
    drawList->AddRectFilled(start, end, palette_[static_cast<int>(ColorIndex::CurrentLineFill)]);
  }
}

void TextEditor::renderSelection(int lineNo, const Line& line, const ImVec2& lineStart,
                                 const ImVec2& contentSize, ImDrawList* drawList) {
  // If no selection, nothing to do
  if (!hasSelection()) return;

  const auto& start = state_.selectionStart;
  const auto& end = state_.selectionEnd;

  // Check if the current line is outside the selection range
  if (end.line < lineNo || start.line > lineNo) {
    return;
  }

  // Determine where selection starts on this line
  float selStartX = 0.0f;
  if (start.line == lineNo) {
    selStartX = getTextDistanceToLineStart(start);
  } else {
    // Selection started before this line, so it goes from line start
    selStartX = 0.0f;
  }

  // Determine where selection ends on this line
  float selEndX = 0.0f;
  if (end.line == lineNo) {
    selEndX = getTextDistanceToLineStart(end);
  } else {
    // Selection continues beyond this line
    selEndX = getTextDistanceToLineStart(Coordinates(lineNo, text_.getLineMaxColumn(lineNo)));
  }

  if (selEndX > selStartX) {
    ImVec2 selStartPos(lineStart.x + textStart_ + selStartX, lineStart.y);
    ImVec2 selEndPos(lineStart.x + textStart_ + selEndX, lineStart.y + charAdvance_.y);

    drawList->AddRectFilled(selStartPos, selEndPos,
                            palette_[static_cast<int>(ColorIndex::Selection)]);
  }
}

void TextEditor::setSelection(const Coordinates& start, const Coordinates& end,
                              SelectionMode mode) {
  core_.setSelection(start, end, mode);
}

namespace {

void renderBufferedText(const std::string& buffer, ImVec2& offset, const ImVec2& pos,
                        ImDrawList* drawList, ImU32 color) {
  if (buffer.empty()) return;

  const ImVec2 textPos(pos.x + offset.x, pos.y + offset.y);
  drawList->AddText(textPos, color, buffer.c_str());

  auto textSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f,
                                                  buffer.c_str(), nullptr, nullptr);
  offset.x += textSize.x;
}

}  // namespace

void TextEditor::renderText(const Line& line, const ImVec2& pos, ImDrawList* drawList) {
  auto prevColor =
      line.empty() ? palette_[static_cast<int>(ColorIndex::Default)] : getGlyphColor(line[0]);

  ImVec2 offset;
  lineBuffer_.clear();

  // We'll reuse space size calculations frequently
  const float spaceSize =
      ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;

  for (size_t i = 0; i < line.size();) {
    auto& glyph = line[i];
    auto color = getGlyphColor(glyph);

    // If color or whitespace changed, flush buffered text
    if ((color != prevColor || glyph.character == '\t' || glyph.character == ' ') &&
        !lineBuffer_.empty()) {
      renderBufferedText(lineBuffer_, offset, pos, drawList, prevColor);
      lineBuffer_.clear();
    }
    prevColor = color;

    if (glyph.character == '\t') {
      // Advance offset according to tab size
      float oldX = offset.x;
      offset.x =
          (1.0f + std::floor((1.0f + offset.x) / (static_cast<float>(tabSize_) * spaceSize))) *
          (static_cast<float>(tabSize_) * spaceSize);

      // Draw arrow for the tab in the center of the tab area
      float arrowX = pos.x + oldX + (offset.x - oldX) * 0.5f;
      float arrowY = pos.y + offset.y + ImGui::GetFontSize() * 0.2f;
      // Using a gray color for the arrow
      ImU32 arrowColor = 0x99906060;

      drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(arrowX, arrowY), arrowColor,
                        "→");
      i++;
    } else if (glyph.character == ' ') {
      // Draw a small dot for space
      float centerX = pos.x + offset.x + spaceSize * 0.5f;
      float centerY = pos.y + offset.y + ImGui::GetFontSize() * 0.5f;
      // Using a gray color for the dot
      ImU32 dotColor = 0x99805050;

      drawList->AddCircleFilled(ImVec2(centerX, centerY), 1.0f, dotColor, 4);
      offset.x += spaceSize;
      i++;
    } else {
      // Regular character
      char buf[7];
      auto d = Utf8::SequenceLength(glyph.character);
      int charCount = 0;
      while (charCount < d && i < line.size()) {
        buf[charCount++] = line[i++].character;
      }
      buf[charCount] = '\0';
      lineBuffer_ += buf;  // Accumulate non-whitespace text
    }
  }

  // Render any remaining buffered text
  if (!lineBuffer_.empty()) {
    renderBufferedText(lineBuffer_, offset, pos, drawList, prevColor);
    lineBuffer_.clear();
  }
}

void TextEditor::renderErrorMarkers(int lineNo, const ImVec2& start, const ImVec2& end,
                                    ImDrawList* drawList) {
  auto errorIt = errorMarkers_.find(lineNo + 1);
  if (errorIt == errorMarkers_.end()) {
    return;
  }

  drawList->AddRectFilled(start, end, palette_[static_cast<int>(ColorIndex::ErrorMarker)]);

  if (ImGui::IsMouseHoveringRect(start, end)) {
    renderErrorTooltip(errorIt->first, errorIt->second);
  }
}

void TextEditor::renderLineNumbers(int lineNo, const ImVec2& pos, ImDrawList* drawList) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%3d  ", lineNo + 1);

  const float lineNoWidth =
      ImGui::GetFont()
          ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr)
          .x;

  drawList->AddText(ImVec2(pos.x + textStart_ - lineNoWidth, pos.y),
                    palette_[static_cast<int>(ColorIndex::LineNumber)], buf);
}

namespace {

struct FoldButtonMetrics {
  float btnSize;
  float startX;
  float startY;
  ImVec2 min;
  ImVec2 max;
};

FoldButtonMetrics calculateFoldMetrics(const ImVec2& lineStart, float scrollX, float textStart,
                                       float spaceSize, float fontSize) {
  FoldButtonMetrics metrics;
  metrics.btnSize = spaceSize;
  metrics.startX = lineStart.x + scrollX + textStart - spaceSize * 2.0f + 4;
  metrics.startY = lineStart.y + (fontSize - metrics.btnSize) / 2.0f;
  metrics.min = ImVec2(metrics.startX, metrics.startY + 2);
  metrics.max = ImVec2(metrics.min.x + metrics.btnSize, metrics.min.y + metrics.btnSize);
  return metrics;
}

}  // namespace

void TextEditor::renderFoldMarkers(int lineNo, const ImVec2& lineStartPos, ImDrawList* drawList) {
  const float spaceSize =
      ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
  const float fontSize = ImGui::GetFontSize();

  int foldId = -1;
  int foldWeight = 0;
  bool hasFold = false;
  bool hasFoldEnd = false;
  bool isFolded = false;

  // Find fold state for current line
  for (int i = 0; i < static_cast<int>(foldBegin_.size()); i++) {
    if (foldBegin_[i].line == lineNo) {
      hasFold = true;
      foldId = i;
      isFolded = i < static_cast<int>(fold_.size()) && fold_[i];
      break;
    } else if (foldBegin_[i].line < lineNo) {
      foldWeight++;
    }
  }

  for (int i = 0; i < static_cast<int>(foldEnd_.size()); i++) {
    if (foldEnd_[i].line == lineNo) {
      hasFoldEnd = true;
      break;
    } else if (foldEnd_[i].line < lineNo) {
      foldWeight--;
    }
  }

  const auto metrics =
      calculateFoldMetrics(lineStartPos, ImGui::GetScrollX(), textStart_, spaceSize, fontSize);

  // Vertical line for non-fold lines within a fold
  if (foldWeight > 0 && !hasFold) {
    const ImVec2 vertLineStart(metrics.startX + metrics.btnSize / 2, lineStartPos.y);
    const ImVec2 vertLineEnd(vertLineStart.x, vertLineStart.y + charAdvance_.y + 1.0f);
    drawList->AddLine(vertLineStart, vertLineEnd, palette_[static_cast<int>(ColorIndex::Default)]);
  }

  // Render fold button and connecting lines
  if (hasFold) {
    // Upper connecting line
    if (foldWeight > 0) {
      const ImVec2 upperLineStart(metrics.startX + metrics.btnSize / 2, lineStartPos.y);
      const ImVec2 upperLineEnd(upperLineStart.x, metrics.min.y);
      drawList->AddLine(upperLineStart, upperLineEnd,
                        palette_[static_cast<int>(ColorIndex::Default)]);
    }

    // Fold button
    drawList->AddRect(metrics.min, metrics.max, palette_[static_cast<int>(ColorIndex::Default)]);

    // Check mouse interaction with fold button
    const ImVec2 mousePos = ImGui::GetMousePos();
    if (mousePos.x >= metrics.min.x && mousePos.x <= metrics.max.x && mousePos.y >= metrics.min.y &&
        mousePos.y <= metrics.max.y) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && foldId < static_cast<int>(fold_.size())) {
        fold_[foldId] = !isFolded;
      }
    }

    // Horizontal line (minus sign)
    drawList->AddLine(ImVec2(metrics.min.x + 3, (metrics.min.y + metrics.max.y) / 2.0f),
                      ImVec2(metrics.max.x - 4, (metrics.min.y + metrics.max.y) / 2.0f),
                      palette_[static_cast<int>(ColorIndex::Default)]);

    // Vertical line (plus sign when folded)
    if (isFolded) {
      drawList->AddLine(ImVec2((metrics.min.x + metrics.max.x) / 2.0f, metrics.min.y + 3),
                        ImVec2((metrics.min.x + metrics.max.x) / 2.0f, metrics.max.y - 4),
                        palette_[static_cast<int>(ColorIndex::Default)]);
    }

    // Lower connecting line
    if (!isFolded || foldWeight > 1) {
      const float lineLength = (lineStartPos.y + charAdvance_.y) - metrics.max.y;
      const ImVec2 lowerLineStart(metrics.startX + metrics.btnSize / 2, metrics.max.y);
      const ImVec2 lowerLineEnd(lowerLineStart.x, metrics.max.y + lineLength);
      drawList->AddLine(lowerLineStart, lowerLineEnd,
                        palette_[static_cast<int>(ColorIndex::Default)]);
    }
  }
  // Horizontal connecting line for fold end
  else if (hasFoldEnd) {
    foldWeight--;
    const ImVec2 endLineStart(metrics.startX + metrics.btnSize / 2,
                              lineStartPos.y + charAdvance_.y - 1.0f);
    const ImVec2 endLineEnd(endLineStart.x + charAdvance_.x / 2.0f, endLineStart.y);
    drawList->AddLine(endLineStart, endLineEnd, palette_[static_cast<int>(ColorIndex::Default)]);
  }
}
void TextEditor::renderCursor(const ImVec2& pos, ImDrawList* drawList) {
  // Only blink cursor when editor has focus and we're not dragging
  const bool isFocused = ImGui::IsWindowFocused();
  const bool isDragging = ImGui::IsMouseDragging(0);

  // Get current time in milliseconds
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  // Reset blink timer on keyboard input or mouse clicks
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
      ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
      ImGui::IsMouseClicked(0) || textChanged_) {
    startTime_ = now;
  }

  // Show cursor if:
  // 1. We're dragging (always show)
  // 2. Editor has focus and we're in the visible part of blink cycle
  const bool showCursor = isDragging || (isFocused && ((now - startTime_) % 800 < 400));

  if (!showCursor) {
    return;
  }

  // Calculate cursor dimensions
  const float cursorWidth = 1.0f;
  const float x = pos.x + getTextDistanceToLineStart(state_.cursorPosition);

  // Draw cursor rectangle
  const ImVec2 cursorStart(x + textStart_,  // Account for margin
                           pos.y            // Line top
  );
  const ImVec2 cursorEnd(cursorStart.x + cursorWidth,
                         pos.y + charAdvance_.y  // Line bottom
  );

  drawList->AddRectFilled(cursorStart, cursorEnd, palette_[static_cast<int>(ColorIndex::Cursor)]);
}

namespace {

void renderFunctionTooltip(const std::string& declaration, const ImVec2& position, float uiScale,
                           ImDrawList* drawList) {
  const float tooltipWidth = 350.0f * uiScale;
  const float tooltipHeight = 50.0f * uiScale;

  drawList->AddRectFilled(position, ImVec2(position.x + tooltipWidth, position.y + tooltipHeight),
                          ImGui::GetColorU32(ImGuiCol_FrameBg));

  ImFont* font = ImGui::GetFont();
  ImGui::PopFont();

  ImGui::SetNextWindowPos(position, ImGuiCond_Always);
  ImGui::BeginChild("##texteditor_functooltip", ImVec2(tooltipWidth, tooltipHeight), true);
  ImGui::TextWrapped("%s", declaration.c_str());
  ImGui::EndChild();

  ImGui::PushFont(font);
}

void renderAutocomplete(const std::vector<std::pair<RcString, RcString>>& suggestions,
                        int selectedIndex, const ImVec2& position, float uiScale) {
  const float popupWidth = 150.0f * uiScale;
  const float popupHeight = 100.0f * uiScale;

  ImGui::SetNextWindowPos(position, ImGuiCond_Always);
  ImGui::BeginChild("##texteditor_autocompl", ImVec2(popupWidth, popupHeight), true);

  for (int i = 0; i < suggestions.size(); i++) {
    const bool isSelected = (i == selectedIndex);
    // TODO: Make a better way to have a null-terminated RcString
    const std::string suggestionStr = suggestions[i].first.str();
    if (ImGui::Selectable(suggestionStr.c_str(), isSelected) && isSelected) {
      ImGui::SetScrollHereY();
    }
  }

  ImGui::EndChild();
}

}  // namespace

void TextEditor::renderExtraUI(ImDrawList* drawList, const ImVec2& basePos, float scrollX,
                               float scrollY, float longest, const ImVec2& contentSize) {
  // Function declaration tooltip
  if (functionDeclarationTooltip_) {
    const ImVec2 tooltipPos = coordinatesToScreenPos(functionDeclarationCoord_);
    const ImVec2 adjustedPos(tooltipPos.x + ImGui::GetScrollX(), tooltipPos.y + charAdvance_.y);

    renderFunctionTooltip(functionDeclaration_, adjustedPos, uiScale_, drawList);

    ImGui::SetWindowFocus();
    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
      functionDeclarationTooltip_ = false;
    }
  }

  // Autocomplete window
  if (autocompleteOpened_) {
    const auto acCoord = findWordStart(autocompletePosition_);
    ImVec2 acPos = coordinatesToScreenPos(acCoord);
    acPos.y += charAdvance_.y;
    acPos.x += ImGui::GetScrollX();

    drawList->AddRectFilled(acPos, ImVec2(acPos.x + 150.0f * uiScale_, acPos.y + 100.0f * uiScale_),
                            ImGui::GetColorU32(ImGuiCol_FrameBg));

    ImFont* font = ImGui::GetFont();
    ImGui::PopFont();

    renderAutocomplete(autocompleteSuggestions_, autocompleteIndex_, acPos, uiScale_);

    ImGui::PushFont(font);

    ImGui::SetWindowFocus();
    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
      autocompleteOpened_ = false;
      autocompleteObject_.clear();
    }
  }

  // Add padding at the bottom of the editor
  ImGui::Dummy(
      ImVec2(longest + 100.0f * uiScale_, (text_.getTotalLines() - foldedLines_) * charAdvance_.y));

  // Handle cursor visibility
  if (scrollToCursor_) {
    ensureCursorVisible();
    scrollToCursor_ = false;
  }

  // Find/Replace dialog background
  if (findOpened_) {
    const ImVec2 findPos(basePos.x + scrollX + windowWidth_ - 250.0f * uiScale_,
                         basePos.y + ImGui::GetScrollY());

    const float dialogHeight = replaceOpened_ ? 90.0f : 40.0f;

    drawList->AddRectFilled(
        findPos, ImVec2(findPos.x + 220.0f * uiScale_, findPos.y + dialogHeight * uiScale_),
        ImGui::GetColorU32(ImGuiCol_WindowBg));
  }
}

void TextEditor::openFunctionDeclarationTooltip(std::string_view declaration,
                                                const Coordinates& coords) {
  functionDeclaration_ = declaration;
  functionDeclarationCoord_ = coords;
  functionDeclarationTooltip_ = true;
}

void TextEditor::removeFolds(const Coordinates& start, const Coordinates& end) {
  core_.removeFolds(start, end);
}

void TextEditor::removeFolds(std::vector<Coordinates>& folds, const Coordinates& start,
                             const Coordinates& end) {
  core_.removeFolds(folds, start, end);
}

namespace {

struct TagInfo {
  int id;
  size_t location;
  size_t length;
};

struct SnippetState {
  bool parsingTag = false;
  bool parsingTagPlaceholder = false;
  int tagId = -1;
  const char* tagStart = nullptr;
  const char* tagPlaceholderStart = nullptr;
};

}  // namespace

RcString TextEditor::autocompleteParseSnippet(std::string_view text, const Coordinates& start) {
  const char* buffer = text.data();
  SnippetState state;
  Coordinates cursor = start;
  Coordinates tagStartCoord, tagEndCoord;

  std::vector<TagInfo> tags;
  std::unordered_map<int, RcString> tagPlaceholders;

  snippetTagStart_.clear();
  snippetTagEnd_.clear();
  snippetTagId_.clear();
  snippetTagHighlight_.clear();

  int columnModifier = 0;

  while (*buffer != '\0') {
    if (*buffer == '{' && *(buffer + 1) == '$') {
      state.parsingTagPlaceholder = false;
      state.parsingTag = true;
      state.tagId = -1;
      state.tagStart = buffer;
      tagStartCoord = cursor;

      const char* skipBuffer = buffer;
      char** endLoc = const_cast<char**>(&buffer);  // Preserve existing behavior
      state.tagId = strtol(buffer + 2, endLoc, 10);
      cursor.column += *endLoc - skipBuffer;

      if (*buffer == ':') {
        state.tagPlaceholderStart = buffer + 1;
        state.parsingTagPlaceholder = true;
      }
    }

    if (*buffer == '}' && state.parsingTag) {
      RcString placeholder;
      if (state.parsingTagPlaceholder) {
        placeholder = RcString(state.tagPlaceholderStart, buffer - state.tagPlaceholderStart);
      }

      tags.push_back({state.tagId, static_cast<size_t>(state.tagStart - text.data()),
                      static_cast<size_t>(buffer - state.tagStart + 1)});

      if (!placeholder.empty() || tagPlaceholders.count(state.tagId) == 0) {
        if (placeholder.empty()) {
          placeholder = " ";
        }

        tagStartCoord.column = std::max(0, tagStartCoord.column - columnModifier);
        tagEndCoord = tagStartCoord;
        tagEndCoord.column += static_cast<int>(placeholder.size());

        snippetTagStart_.push_back(tagStartCoord);
        snippetTagEnd_.push_back(tagEndCoord);
        snippetTagId_.push_back(state.tagId);
        snippetTagHighlight_.push_back(true);

        tagPlaceholders[state.tagId] = placeholder;
      } else {
        tagStartCoord.column = std::max(0, tagStartCoord.column - columnModifier);
        tagEndCoord = tagStartCoord;
        tagEndCoord.column += static_cast<int>(tagPlaceholders[state.tagId].size());

        snippetTagStart_.push_back(tagStartCoord);
        snippetTagEnd_.push_back(tagEndCoord);
        snippetTagId_.push_back(state.tagId);
        snippetTagHighlight_.push_back(false);
      }

      columnModifier += static_cast<int>(tags.back().length - tagPlaceholders[state.tagId].size());

      state = SnippetState{};
    }

    if (*buffer == '\n') {
      cursor.line++;
      cursor.column = 0;
      columnModifier = 0;
    } else {
      cursor.column++;
    }

    buffer++;
  }

  isSnippet_ = !tags.empty();
  std::string result{text};

  for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
    result.erase(it->location, it->length);
    result.insert(it->location, tagPlaceholders[it->id]);
  }

  return RcString(result);
}

// void TextEditor::autocompleteSelect() {
//   UndoRecord undo;
//   undo.before = state_;

//   auto cursorPos = getCursorPosition();
//   cursorPos.column = std::max(cursorPos.column - 1, 0);

//   auto completionStart = findWordStart(cursorPos);
//   auto completionEnd = findWordEnd(cursorPos);

//   // TODO: autocompleteObject_ is never assigned
//   if (!autocompleteObject_.empty()) {
//     completionStart = autocompletePosition_;
//   }

//   undo.addedStart = completionStart;
//   int undoPopCount =
//       std::max(0, completionEnd.column - completionStart.column) + 1;

//   if (!autocompleteObject_.empty() && autocompleteWord_.empty()) {
//     undoPopCount = 0;
//   }

//   const auto &selectedEntry = autocompleteSuggestions_[autocompleteIndex_];
//   RcString entryText =
//       autocompleteParseSnippet(selectedEntry.second, completionStart);

//   if (completionStart.column != completionEnd.column) {
//     setSelection(completionStart, completionEnd);
//     backspace();
//   }
//   insertText(entryText, true);

//   undo.added = entryText;
//   undo.addedEnd = getActualCursorCoordinates();

//   if (isSnippet_ && !snippetTagStart_.empty()) {
//     setSelection(snippetTagStart_[0], snippetTagEnd_[0]);
//     setCursorPosition(snippetTagEnd_[0]);
//     snippetTagSelected_ = 0;
//     snippetTagLength_ = 0;
//     snippetTagPreviousLength_ = snippetTagEnd_[snippetTagSelected_].column -
//                                 snippetTagStart_[snippetTagSelected_].column;
//   }

//   requestAutocomplete_ = false;
//   autocompleteOpened_ = false;
//   autocompleteObject_.clear();

//   undo.after = state_;

//   while (undoPopCount-- != 0) {
//     undoIndex_--;
//     undoBuffer_.pop_back();
//   }
//   addUndo(undo);
// }

namespace {

struct AutocompleteEntry {
  RcString displayString;
  RcString value;
  int location;

  AutocompleteEntry(std::string_view display, std::string_view val, int loc)
      : displayString(display), value(val), location(loc) {}
};

bool isValidAutocompleteWord(std::string_view word) {
  return std::any_of(word.begin(), word.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

std::string toLowercase(std::string_view str) {
  std::string result(str);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

}  // namespace

void TextEditor::buildMemberSuggestions(bool* keepAutocompleteOpen) {
  autocompleteSuggestions_.clear();

  auto curPos = getCorrectCursorPosition();
  RcString object = getWordAt(curPos);

  if (!autocompleteSuggestions_.empty()) {
    autocompleteOpened_ = true;
    autocompleteWord_.clear();

    if (keepAutocompleteOpen != nullptr) {
      *keepAutocompleteOpen = true;
    }

    Coordinates curCursor = getCursorPosition();
    autocompletePosition_ = findWordStart(curCursor);
  }
}

void TextEditor::buildSuggestions(bool* keepAutocompleteOpen) {
  autocompleteWord_ = getWordUnderCursor().str();

  if (!isValidAutocompleteWord(autocompleteWord_)) {
    return;
  }

  autocompleteSuggestions_.clear();
  autocompleteIndex_ = 0;
  autocompleteSwitched_ = false;

  const std::string searchWord = toLowercase(autocompleteWord_);
  std::vector<AutocompleteEntry> matches;

  // Build matches for non-member completions
  if (autocompleteObject_.empty()) {
    // Match against stored entries
    for (size_t i = 0; i < autocompleteSearchTerms_.size(); i++) {
      const std::string lowerEntry = toLowercase(autocompleteSearchTerms_[i]);
      if (size_t loc = lowerEntry.find(searchWord); loc != std::string::npos) {
        matches.emplace_back(autocompleteEntries_[i].first, autocompleteEntries_[i].second,
                             static_cast<int>(loc));
      }
    }

    // Match against language keywords
    for (const auto& keyword : languageDefinition_.keywords) {
      const std::string lowerKeyword = toLowercase(keyword);
      if (size_t loc = lowerKeyword.find(searchWord); loc != std::string::npos) {
        matches.emplace_back(keyword, keyword, static_cast<int>(loc));
      }
    }

    // Match against language identifiers
    for (const auto& [identifier, _] : languageDefinition_.identifiers) {
      const std::string lowerIdentifier = toLowercase(identifier);
      if (size_t loc = lowerIdentifier.find(searchWord); loc != std::string::npos) {
        matches.emplace_back(identifier, identifier, static_cast<int>(loc));
      }
    }
  }

  // Sort exact matches first
  for (const auto& entry : matches) {
    if (entry.location == 0) {
      autocompleteSuggestions_.emplace_back(entry.displayString, entry.value);
    }
  }
  for (const auto& entry : matches) {
    if (entry.location != 0) {
      autocompleteSuggestions_.emplace_back(entry.displayString, entry.value);
    }
  }

  if (!autocompleteSuggestions_.empty()) {
    autocompleteOpened_ = true;

    if (keepAutocompleteOpen != nullptr) {
      *keepAutocompleteOpen = true;
    }

    Coordinates curCursor = getCursorPosition();
    curCursor.column--;
    autocompletePosition_ = findWordStart(curCursor);
  }
}

ImVec2 TextEditor::coordinatesToScreenPos(const Coordinates& position) const {
  const ImVec2 origin = uiCursorPos_;

  return ImVec2(origin.x + (textStart_ + position.column) * charAdvance_.x - ImGui::GetScrollX(),
                origin.y + position.line * charAdvance_.y);
}

namespace {

void renderScrollbarCurrentLine(ImDrawList* drawList, const ImRect& scrollBarRect, int currentLine,
                                size_t totalLines, const TextEditor::Palette& palette) {
  if (currentLine == 0) {
    return;
  }

  const float lineY = std::round(scrollBarRect.Min.y +
                                 (currentLine - 0.5f) / totalLines * scrollBarRect.GetHeight());

  drawList->AddLine(ImVec2(scrollBarRect.Min.x, lineY), ImVec2(scrollBarRect.Max.x, lineY),
                    (palette[static_cast<int>(ColorIndex::Default)] & 0x00FFFFFFu) | 0x83000000u,
                    3.0f);
}

void renderScrollbarChanges(ImDrawList* drawList, const ImRect& scrollBarRect,
                            const std::vector<int>& changedLines, size_t totalLines) {
  for (int line : changedLines) {
    const float lineStartY =
        std::round(scrollBarRect.Min.y +
                   (static_cast<float>(line) - 0.5f) / totalLines * scrollBarRect.GetHeight());
    const float lineEndY =
        std::round(scrollBarRect.Min.y +
                   (static_cast<float>(line + 1) - 0.5f) / totalLines * scrollBarRect.GetHeight());

    drawList->AddRectFilled(
        ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth() * 0.6f, lineStartY),
        ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth(), lineEndY), 0xFF8CE6F0);
  }
}

void renderScrollbarErrors(ImDrawList* drawList, const ImRect& scrollBarRect,
                           const TextEditor::ErrorMarkers& errors, size_t totalLines,
                           const TextEditor::Palette& palette) {
  for (const auto& [line, _] : errors) {
    const float lineY =
        std::round(scrollBarRect.Min.y +
                   (static_cast<float>(line) - 0.5f) / totalLines * scrollBarRect.GetHeight());

    drawList->AddRectFilled(
        ImVec2(scrollBarRect.Min.x, lineY),
        ImVec2(scrollBarRect.Min.x + scrollBarRect.GetWidth() * 0.4f, lineY + 6.0f),
        palette[static_cast<int>(ColorIndex::ErrorMarker)]);
  }
}

}  // namespace

void TextEditor::processFind(const std::string& findWord, bool findNext) {
  auto curPos = getCursorPosition();
  size_t charIndex = 0;

  for (size_t ln = 0; ln < curPos.line; ln++) {
    charIndex += text_.getLineCharacterCount(ln) + 1;
  }
  charIndex += curPos.column;

  std::string wordLower = findWord;
  std::string textSrc = getText();
  std::transform(wordLower.begin(), wordLower.end(), wordLower.begin(), ::tolower);
  std::transform(textSrc.begin(), textSrc.end(), textSrc.begin(), ::tolower);

  size_t textLoc = textSrc.find(wordLower, charIndex);
  if (textLoc == std::string::npos) {
    textLoc = textSrc.find(wordLower, 0);
  }

  if (textLoc != std::string::npos) {
    Coordinates newPos{0, 0};
    size_t currentIndex = 0;

    for (size_t ln = 0; ln < text_.getTotalLines(); ln++) {
      int charCount = text_.getLineCharacterCount(ln) + 1;
      if (currentIndex + charCount > textLoc) {
        newPos.line = ln;
        newPos.column = textLoc - currentIndex;
        break;
      }
      currentIndex += charCount;
    }

    auto selEnd = newPos;
    selEnd.column += wordLower.size();
    setSelection(newPos, selEnd);
    setCursorPosition(selEnd);
    scrollToCursor_ = true;

    if (!findNext) {
      ImGui::SetKeyboardFocusHere(-1);
    }
  }
}

void TextEditor::processReplace(const std::string& findWord, const std::string& replaceWord,
                                bool replaceAll) {
  if (findWord.empty()) {
    return;
  }

  Coordinates curPos{0, 0};
  std::string textSrc = getText();
  size_t textLoc = textSrc.find(findWord, replaceIndex_);

  do {
    if (textLoc == std::string::npos) {
      if (!replaceAll) {
        replaceIndex_ = 0;
        textLoc = textSrc.find(findWord, 0);
      }
      if (textLoc == std::string::npos) {
        break;
      }
    }

    int totalCount = 0;
    for (size_t ln = 0; ln < text_.getTotalLines(); ln++) {
      int lineCharCount = text_.getLineCharacterCount(ln) + 1;
      if (textLoc >= totalCount && textLoc < totalCount + lineCharCount) {
        curPos.line = ln;
        curPos.column = textLoc - totalCount;
        break;
      }
      totalCount += lineCharCount;
    }

    auto selEnd = curPos;
    selEnd.column += findWord.size();
    setSelection(curPos, selEnd);
    deleteSelection();
    insertText(replaceWord);
    setCursorPosition(selEnd);
    scrollToCursor_ = true;

    if (!replaceAll) {
      replaceIndex_ = textLoc + replaceWord.size();
      break;
    }

    textSrc = getText();
    textLoc = textSrc.find(findWord, textLoc + replaceWord.size());

  } while (replaceAll && textLoc != std::string::npos);
}

void TextEditor::render(std::string_view title, const ImVec2& size, bool showBorder) {
  // TODO: std::string operator+ with string_view is not supported.
  // Avoid this alloc.
  const std::string titleStr{title};

  withinRender_ = true;
  cursorPositionChanged_ = false;

  findOrigin_ = ImGui::GetCursorScreenPos();
  windowWidth_ = ImGui::GetWindowWidth();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(
                                              palette_[static_cast<int>(ColorIndex::Background)]));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

  if (!ignoreImGuiChild_) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    if (horizontalScroll_) {
      flags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
    }
    ImGui::BeginChild(title.data(), size, showBorder, flags);
  }

  if (handleKeyboardInputs_) {
    handleKeyboardInputs();
    ImGui::PushAllowKeyboardFocus(true);
  }

  if (handleMouseInputs_) {
    handleMouseInputs();
  }

  colorizeInternal();
  readyForAutocomplete_ = true;
  renderInternal(title);

  // Scrollbar markers
  if (scrollbarMarkers_) {
    ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if (window->ScrollbarY) {
      ImDrawList* drawList = ImGui::GetWindowDrawList();
      const ImRect scrollBarRect = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);

      ImGui::PushClipRect(scrollBarRect.Min, scrollBarRect.Max, false);

      renderScrollbarCurrentLine(drawList, scrollBarRect, state_.cursorPosition.line,
                                 text_.getTotalLines(), palette_);
      renderScrollbarChanges(drawList, scrollBarRect, changedLines_, text_.getTotalLines());
      renderScrollbarErrors(drawList, scrollBarRect, errorMarkers_, text_.getTotalLines(),
                            palette_);

      ImGui::PopClipRect();
    }
  }

  // Context menu
  if (ImGui::IsMouseClicked(1)) {
    rightClickPos_ = ImGui::GetMousePos();
    if (ImGui::IsWindowHovered()) {
      setCursorPosition(screenPosToCoordinates(rightClickPos_));
    }
  }

  if (ImGui::BeginPopupContextItem((std::string("##edcontext") + titleStr).c_str())) {
    if (rightClickPos_.x - uiCursorPos_.x > ImGui::GetStyle().WindowPadding.x) {
      if (ImGui::Selectable("Cut")) cut();
      if (ImGui::Selectable("Copy")) copy();
      if (ImGui::Selectable("Paste")) paste();
    }
    ImGui::EndPopup();
  }

  // Find/Replace window
  if (findOpened_) {
    ImFont* font = ImGui::GetFont();
    ImGui::PopFont();

    ImGui::SetNextWindowPos(
        ImVec2(findOrigin_.x + windowWidth_ - calculateUISize(250), findOrigin_.y),
        ImGuiCond_Always);

    const float height = calculateUISize(replaceOpened_ ? 90 : 40);
    ImGui::BeginChild((std::string("##ted_findwnd") + titleStr).c_str(),
                      ImVec2(calculateUISize(220), height), true, ImGuiWindowFlags_NoScrollbar);

    // Process find next shortcut
    findNext_ = shortcuts_[static_cast<int>(ShortcutId::FindNext)].matches(ImGui::GetIO());

    if (findJustOpened_) {
      findWord_ = getSelectedText();
    }

    ImGui::PushItemWidth(calculateUISize(-45));

    const bool findEnterPressed =
        ImGui::InputText((std::string("##ted_findtextbox") + titleStr).c_str(), &findWord_,
                         ImGuiInputTextFlags_EnterReturnsTrue);

    if (findEnterPressed || findNext_) {
      processFind(findWord_, findNext_);
      findNext_ = false;
    }

    findFocused_ = ImGui::IsItemActive();

    if (findJustOpened_) {
      ImGui::SetKeyboardFocusHere(-1);
      findJustOpened_ = false;
    }

    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::ArrowButton((std::string("##expandFind") + titleStr).c_str(),
                           replaceOpened_ ? ImGuiDir_Up : ImGuiDir_Down)) {
      replaceOpened_ = !replaceOpened_;
    }

    ImGui::SameLine();
    if (ImGui::Button((std::string("X##") + titleStr).c_str())) {
      findOpened_ = false;
    }

    // Replace functionality
    if (replaceOpened_) {
      ImGui::PushItemWidth(calculateUISize(-45));
      ImGui::NewLine();

      bool shouldReplace = ImGui::InputText((std::string("##ted_replacetb") + titleStr).c_str(),
                                            &replaceWord_, ImGuiInputTextFlags_EnterReturnsTrue);

      replaceFocused_ = ImGui::IsItemActive();
      ImGui::PopItemWidth();

      ImGui::SameLine();
      if (ImGui::Button((std::string(">##replaceOne") + titleStr).c_str()) || shouldReplace) {
        processReplace(findWord_, replaceWord_);
      }

      ImGui::SameLine();
      if (ImGui::Button((std::string(">>##replaceAll") + titleStr).c_str())) {
        processReplace(findWord_, replaceWord_, true);
      }
    }

    ImGui::EndChild();
    ImGui::PushFont(font);

    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
      findOpened_ = false;
    }
  }

  if (handleKeyboardInputs_) {
    ImGui::PopAllowKeyboardFocus();
  }

  if (!ignoreImGuiChild_) {
    ImGui::EndChild();
  }

  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  withinRender_ = false;
}

void TextEditor::setText(std::string_view text) {
  core_.setText(text);
}

void TextEditor::enterCharacter(ImWchar character, bool shift) {
  core_.enterCharacter(static_cast<char32_t>(character), shift);
}

void TextEditor::updateChangeTracking() {
  core_.updateChangeTracking();
}

void TextEditor::handleFunctionTooltip(ImWchar character, const Coordinates& curPos) {
  if (!functionDeclarationTooltipEnabled_) {
    return;
  }

  if (character == '(') {
    auto wordPos = getCorrectCursorPosition();
    RcString functionName = getWordAt(wordPos);
    openFunctionDeclarationTooltip(functionName, wordPos);
  } else if (character == ',') {
    auto wordPos = getCorrectCursorPosition();
    wordPos.column--;

    const Line& line = text_.getLineGlyphs(wordPos.line);
    int parenCount = 0;

    for (; wordPos.column > 0; wordPos.column--) {
      if (line[wordPos.column].character == '(') {
        if (parenCount == 0) {
          RcString functionName = getWordAt(wordPos);
          if (!functionName.empty()) {
            openFunctionDeclarationTooltip(functionName, wordPos);
          }
          break;
        }
        parenCount--;
      }
      if (line[wordPos.column].character == ')') {
        parenCount++;
      }
    }
  }
}

void TextEditor::setColorizerEnabled(bool enabled) {
  core_.setColorizerEnabled(enabled);
}

void TextEditor::handleEndOfLineDelete(Coordinates pos, UndoRecord& undo) {
  core_.handleEndOfLineDelete(pos, undo);
}

void TextEditor::handleMidLineDelete(Coordinates pos, UndoRecord& undo) {
  core_.handleMidLineDelete(pos, undo);
}

Coordinates TextEditor::getCorrectCursorPosition() {
  Coordinates curPos = getCursorPosition();

  if (curPos.line >= 0 && curPos.line <= getCursorPosition().line) {
    const Line& line = text_.getLineGlyphs(curPos.line);
    const int lineLength = std::min<int>(curPos.line, static_cast<int>(line.size()));

    for (int c = 0; c < lineLength; c++) {
      if (line[c].character == '\t') {
        curPos.column -= (tabSize_ - 1);
      }
    }
  }

  return curPos;
}

void TextEditor::setCursorPosition(const Coordinates& position) {
  core_.setCursorPosition(position);
}

void TextEditor::setSelectionStart(const Coordinates& position) {
  core_.setSelectionStart(position);
}

void TextEditor::setSelectionEnd(const Coordinates& position) {
  core_.setSelectionEnd(position);
}

void TextEditor::insertText(std::string_view text, bool indent) {
  core_.insertText(text, indent);
}

void TextEditor::deleteSelection() {
  core_.deleteSelection();
}

void TextEditor::moveUp(int amount, bool select) {
  core_.moveUp(amount, select);
}

void TextEditor::moveDown(int amount, bool select) {
  core_.moveDown(amount, select);
}

void TextEditor::moveLeft(int amount, bool select, bool wordMode) {
  core_.moveLeft(amount, select, wordMode);
}

void TextEditor::moveRight(int amount, bool select, bool wordMode) {
  core_.moveRight(amount, select, wordMode);
}

void TextEditor::moveTop(bool select) {
  core_.moveTop(select);
}

void TextEditor::moveBottom(bool select) {
  core_.moveBottom(select);
}

void TextEditor::moveHome(bool select) {
  core_.moveHome(select);
}

void TextEditor::moveEnd(bool select) {
  core_.moveEnd(select);
}

void TextEditor::delete_() {
  core_.delete_();
}

void TextEditor::backspace() {
  core_.backspace();
}

void TextEditor::selectWordUnderCursor() {
  core_.selectWordUnderCursor();
}

void TextEditor::selectAll() {
  core_.selectAll();
}

bool TextEditor::hasSelection() const {
  return core_.hasSelection();
}

void TextEditor::setShortcut(ShortcutId id, Shortcut shortcut) {
  shortcuts_[static_cast<int>(id)] = shortcut;
}

void TextEditor::copy() {
  if (hasSelection()) {
    const std::string selectedText = getSelectedText();
    ImGui::SetClipboardText(selectedText.c_str());
    return;
  }

  if (text_.getTotalLines() > 0) {
    const auto& line = text_.getLineGlyphs(getActualCursorCoordinates().line);
    std::string lineText;
    lineText.reserve(line.size());
    for (const auto& glyph : line) {
      lineText.push_back(glyph.character);
    }
    ImGui::SetClipboardText(lineText.c_str());
  }
}

void TextEditor::cut() {
  if (!hasSelection()) {
    return;
  }

  UndoRecord undo;
  undo.before = state_;
  undo.removed = getSelectedText();
  undo.removedStart = state_.selectionStart;
  undo.removedEnd = state_.selectionEnd;

  copy();
  deleteSelection();

  undo.after = state_;
  addUndo(undo);
}

void TextEditor::paste() {
  const char* clipText = ImGui::GetClipboardText();
  if (clipText == nullptr || clipText[0] == '\0') {
    return;
  }

  UndoRecord undo;
  undo.before = state_;

  if (hasSelection()) {
    undo.removed = getSelectedText();
    undo.removedStart = state_.selectionStart;
    undo.removedEnd = state_.selectionEnd;
    deleteSelection();
  }

  undo.added = clipText;
  undo.addedStart = getActualCursorCoordinates();

  insertText(clipText, autoIndentOnPaste_);

  undo.addedEnd = getActualCursorCoordinates();
  undo.after = state_;
  addUndo(undo);
}

bool TextEditor::canUndo() const {
  return core_.canUndo();
}

bool TextEditor::canRedo() const {
  return core_.canRedo();
}

void TextEditor::undo(int steps) {
  core_.undo(steps);
}

void TextEditor::redo(int steps) {
  core_.redo(steps);
}

const TextEditor::Palette& TextEditor::getDarkPalette() {
  static constexpr Palette kPalette = {{
      0xff7f7f7f,  // Default
      0xffd69c56,  // Keyword
      0xff00ff00,  // Number
      0xff7070e0,  // String
      0xff70a0e0,  // Char literal
      0xffffffff,  // Punctuation
      0xffaaaaaa,  // Identifier
      0xff9bc64d,  // Known identifier
      0xff206020,  // Comment (single line)
      0xff406020,  // Comment (multi line)
      0xff101010,  // Background
      0xffe0e0e0,  // Cursor
      0x80a06020,  // Selection
      0x800020ff,  // ErrorMarker
      0xff0000ff,  // Breakpoint
      0xffffffff,  // Breakpoint outline
      0xFF1DD8FF,  // Current line indicator
      0xFF696969,  // Current line indicator outline
      0xff707000,  // Line number
      0x40000000,  // Current line fill
      0x40808080,  // Current line fill (inactive)
      0x40a0a0a0,  // Current line edge
      0xff33ffff,  // Error message
      0xffffffff,  // BreakpointDisabled
      0xffaaaaaa,  // UserFunction
      0xffb0c94e,  // UserType
      0xffaaaaaa,  // UniformType
      0xffaaaaaa,  // GlobalVariable
      0xffaaaaaa,  // LocalVariable
      0xff888888   // FunctionArgument
  }};

  return kPalette;
}

std::string TextEditor::getText() const {
  return core_.getText();
}

std::string TextEditor::getSelectedText() const {
  return core_.getSelectedText();
}

void TextEditor::colorize(int fromLine, int lines) {
  core_.colorize(fromLine, lines);
}

void TextEditor::colorizeRange(int fromLine, int toLine) {
  core_.colorizeRange(fromLine, toLine);
}

void TextEditor::colorizeInternal() {
  core_.colorizeInternal();
}

float TextEditor::getTextDistanceToLineStart(const Coordinates& pos) const {
  const Line& line = text_.getLineGlyphs(pos.line);
  float distance = 0.0f;
  const float spaceSize =
      ImGui::GetFont()
          ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr)
          .x;

  int charIndex = text_.getCharacterIndex(pos);
  for (size_t i = 0; i < line.size() && i < static_cast<size_t>(charIndex);) {
    if (line[i].character == '\t') {
      distance =
          (1.0f + std::floor((1.0f + distance) / (static_cast<float>(tabSize_) * spaceSize))) *
          (static_cast<float>(tabSize_) * spaceSize);
      ++i;
    } else {
      std::array<char, 7> tempBuffer;
      auto seqLen = Utf8::SequenceLength(line[i].character);
      int bufPos = 0;

      while (bufPos < 6 && seqLen-- > 0 && i < line.size()) {
        tempBuffer[bufPos++] = line[i++].character;
      }
      tempBuffer[bufPos] = '\0';

      distance += ImGui::GetFont()
                      ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, tempBuffer.data(),
                                      nullptr, nullptr)
                      .x;
    }
  }

  return distance;
}
void TextEditor::ensureCursorVisible() {
  // If not currently rendering, do nothing.
  if (!withinRender_) {
    return;
  }

  const float scrollX = ImGui::GetScrollX();
  const float scrollY = ImGui::GetScrollY();
  const float height = ImGui::GetWindowHeight();
  const float width = windowWidth_;

  const auto pos = getActualCursorCoordinates();
  const auto len = getTextDistanceToLineStart(pos);

  const auto top = 1 + static_cast<int>(std::ceil(scrollY / charAdvance_.y));
  const auto bottom = static_cast<int>(std::ceil((scrollY + height) / charAdvance_.y));
  const auto left = static_cast<int>(std::ceil(scrollX / charAdvance_.x));
  const auto right = static_cast<int>(std::ceil((scrollX + width) / charAdvance_.x));

  if (pos.line < top) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line - 1) * charAdvance_.y));
  }
  if (pos.line > bottom - 4) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line + 4) * charAdvance_.y - height));
  }
  if (pos.column < left) {
    ImGui::SetScrollX(std::max(0.0f, len + textStart_ - 11 * charAdvance_.x));
  }
  if (len + textStart_ > (right - 4) * charAdvance_.x) {
    ImGui::SetScrollX(std::max(0.0f, len + textStart_ + 4 * charAdvance_.x - width));
  }
}

int TextEditor::getPageSize() const {
  const float height = ImGui::GetWindowHeight() - 20.0f;
  return static_cast<int>(std::floor(height / charAdvance_.y));
}

// `UndoRecord::UndoRecord`, `UndoRecord::undo`, and `UndoRecord::redo`
// moved to TextEditorCore.cc as part of the C1 extraction. They now
// operate on a `TextEditorCore*` rather than a `TextEditor*`.

// `LanguageDefinition::SVG()` moved to TextEditorCore.cc.
#if 0
const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::SVG() {
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
        "id",    "class",   "style",  "viewBox", "xmlns",
        "href",  "transform", "fill",   "stroke",  "opacity",
        "d",     "cx",      "cy",     "r",       "rx",
        "ry",    "x",       "y",      "width",   "height",
        "x1",    "y1",      "x2",     "y2",      "preserveAspectRatio",
        "offset", "stop-color", "stop-opacity",
        "font-family", "font-size", "font-weight", "font-style",
        "text-anchor", "dominant-baseline", "stroke-width",
        "stroke-linecap", "stroke-linejoin", "stroke-dasharray",
        "fill-opacity", "stroke-opacity", "fill-rule", "clip-path",
        "clip-rule", "mask", "filter", "display", "visibility",
        "color", "pointer-events",
    };
    for (const auto& attr : knownAttrs) {
      def.identifiers.emplace(attr, TextEditor::Identifier(attr));
    }

    // Custom XML-aware tokenizer. Runs per-line; multi-line comments
    // (<!-- -->) are handled by the comment-detection pass below.
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

      // Quoted strings (attribute values). Emit the opening/closing
      // quote chars as Punctuation and just the inner value as String,
      // so double-click on the value text selects the contents without
      // the quote delimiters (double-click boundaries track color
      // changes, per findWordStart/findWordEnd).
      if (c == '"' || c == '\'') {
        // First call: emit the opening quote as Punctuation (1 char)
        // and let the next tokenizer invocation handle the inner
        // value. Since the tokenizer is called in a loop until
        // end-of-line, the subsequent iterations will colorize the
        // inner value char by char via the identifier/number/etc.
        // paths below. The closing quote is emitted similarly.
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
        while (p < inEnd && *p != ';' && *p != '<' && *p != '>' && *p != ' ' &&
               *p != '\t' && *p != '\n') {
          ++p;
        }
        if (p < inEnd && *p == ';') ++p;
        outBegin = inBegin;
        outEnd = p;
        outColor = ColorIndex::Number;
        return true;
      }

      // Numbers (attribute values like x="10.5")
      if ((c >= '0' && c <= '9') || (c == '-' && p + 1 < inEnd && p[1] >= '0' && p[1] <= '9') ||
          (c == '.' && p + 1 < inEnd && p[1] >= '0' && p[1] <= '9')) {
        if (c == '-') ++p;
        while (p < inEnd && ((*p >= '0' && *p <= '9') || *p == '.')) {
          ++p;
        }
        // Handle scientific notation (e.g., 1e-5)
        if (p < inEnd && (*p == 'e' || *p == 'E')) {
          ++p;
          if (p < inEnd && (*p == '+' || *p == '-')) ++p;
          while (p < inEnd && *p >= '0' && *p <= '9') ++p;
        }
        // Handle unit suffixes (px, em, %)
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

      // Identifiers: element names, attribute names
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
          static_cast<unsigned char>(c) >= 0x80) {
        ++p;
        while (p < inEnd &&
               ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                *p == '_' || *p == '-' || *p == '.' || *p == ':' ||
                static_cast<unsigned char>(*p) >= 0x80)) {
          ++p;
        }
        outBegin = inBegin;
        outEnd = p;
        outColor = ColorIndex::Identifier;
        // The caller (colorizeRange) will promote Identifier → Keyword if
        // the token text matches def.keywords, or → KnownIdentifier if it
        // matches def.identifiers.
        return true;
      }

      // Equals sign (structural, not colored)
      if (c == '=') {
        outBegin = p;
        outEnd = p + 1;
        outColor = ColorIndex::Punctuation;
        return true;
      }

      // Fall through for whitespace and anything else — the caller advances
      // past it character-by-character.
      return false;
    };

    // XML comment delimiters for the multi-line comment detection pass.
    def.commentStart = "<!--";
    def.commentEnd = "-->";
    def.singleLineComment = "";  // XML has no single-line comment syntax.

    // Settings
    def.caseSensitive = true;
    def.autoIndentation = true;
    def.name = "SVG";

    return def;
  }();

  return langDef;
}
#endif

void TextEditor::detectIndentationStyle() {
  core_.detectIndentationStyle();
}

}  // namespace donner::editor
