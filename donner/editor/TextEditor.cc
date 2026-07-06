#include "donner/editor/TextEditor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <regex>
#include <span>
#include <stack>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Utf8.h"
#include "donner/base/Utils.h"
#include "donner/editor/ImGuiInternalIncludes.h"
#include "donner/editor/TextChipIconSet.h"
#include "misc/cpp/imgui_stdlib.h"

namespace donner::editor {

namespace {

constexpr float kFocusReferenceRopeWakeSeconds = 1.0f / 60.0f;
constexpr int kMaxAnimatedFocusReferenceRopes = 64;
constexpr double kFocusReferenceRopeCullMarginPx = 48.0;

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

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
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
  core_.sourceEditIntentHook = [this](const SourceEditIntent& intent) {
    remapFocusMetadataForSourceEdit(intent);
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
  if (wordWrapEnabled_ || focusPartitionActive_) {
    return visualScreenPosToCoordinates(position);
  }

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
  if (wordWrapEnabled_ || focusPartitionActive_) {
    return visualScreenPosToCoordinates(position);
  }

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

  const auto& entry = autocompleteSuggestions_[autocompleteIndex_];
  if (autocompleteReplacementActive_) {
    const Coordinates start = text_.getCoordinatesAtByteOffset(autocompleteReplacementStartOffset_);
    const Coordinates end = text_.getCoordinatesAtByteOffset(autocompleteReplacementEndOffset_);
    setSelection(start, end);
    autocompleteReplacementActive_ = false;
  }

  insertText(entry.second, true);

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
        case ShortcutId::Unindent:
          additionalChecks = !autocompleteOpened_ && editorFocused && !io.KeySuper;
          break;

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

  if (click && tryNavigateToFocusReferenceRopeAt(ImGui::GetMousePos())) {
    autocompleteOpened_ = false;
    autocompleteObject_.clear();
    lastClick_ = -1.0f;
    return;
  }

  if (click && tryExpandFocusHiddenPlaceholderAt(ImGui::GetMousePos())) {
    lastClick_ = -1.0f;
    return;
  }

  // Triple click - select line
  if (tripleClick && !ctrl) {
    const auto clickCoords = screenPosToCoordinates(ImGui::GetMousePos());
    const bool cursorChanged = state_.cursorPosition != clickCoords;
    cursorPositionChanged_ = cursorPositionChanged_ || cursorChanged;
    cursorPositionChangedByMouse_ = cursorPositionChangedByMouse_ || cursorChanged;
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ = clickCoords;
    selectionMode_ = SelectionMode::Line;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    lastClick_ = -1.0f;
    return;
  }

  // Double click - select word
  if (doubleClick && !ctrl) {
    const auto clickCoords = screenPosToCoordinates(ImGui::GetMousePos());
    const bool cursorChanged = state_.cursorPosition != clickCoords;
    cursorPositionChanged_ = cursorPositionChanged_ || cursorChanged;
    cursorPositionChangedByMouse_ = cursorPositionChangedByMouse_ || cursorChanged;
    state_.cursorPosition = interactiveStart_ = interactiveEnd_ = clickCoords;
    selectionMode_ =
        (selectionMode_ == SelectionMode::Line) ? SelectionMode::Normal : SelectionMode::Word;
    setSelection(interactiveStart_, interactiveEnd_, selectionMode_);
    const bool selectionEndChanged = state_.cursorPosition != state_.selectionEnd;
    cursorPositionChanged_ = cursorPositionChanged_ || selectionEndChanged;
    cursorPositionChangedByMouse_ = cursorPositionChangedByMouse_ || selectionEndChanged;
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
    const bool cursorChanged = state_.cursorPosition != clickCoords;
    cursorPositionChanged_ = cursorPositionChanged_ || cursorChanged;
    cursorPositionChangedByMouse_ = cursorPositionChangedByMouse_ || cursorChanged;
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
    const bool cursorChanged = state_.cursorPosition != mousePos;
    cursorPositionChanged_ = cursorPositionChanged_ || cursorChanged;
    cursorPositionChangedByMouse_ = cursorPositionChangedByMouse_ || cursorChanged;
    state_.cursorPosition = interactiveEnd_ = mousePos;

    // Create selection from start to current point
    core_.setInteractiveSelection(interactiveStart_, interactiveEnd_, selectionMode_);

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

namespace {

bool LineRangeContains(const std::vector<LineRange>& ranges, int lineNo) {
  return std::ranges::any_of(ranges, [lineNo](const LineRange& range) {
    return lineNo >= range.startLine && lineNo < range.endLine;
  });
}

bool FocusPartitionsEqual(const FocusPartition& lhs, const FocusPartition& rhs) {
  return lhs.fullColor == rhs.fullColor && lhs.referenceColor == rhs.referenceColor &&
         lhs.dimmed == rhs.dimmed && lhs.hidden == rhs.hidden &&
         lhs.referenceLinks == rhs.referenceLinks;
}

std::string LineToString(const Line& line) {
  std::string result;
  result.reserve(line.size());
  for (const Glyph& glyph : line) {
    result.push_back(glyph.character);
  }
  return result;
}

}  // namespace

bool TextEditor::isLineHiddenByFocus(int lineNo) const {
  return focusPartitionActive_ && LineRangeContains(focusPartition_.hidden, lineNo) &&
         !isLineExpandedHiddenByFocus(lineNo);
}

bool TextEditor::isLineReferenceColoredByFocus(int lineNo) const {
  return focusPartitionActive_ && LineRangeContains(focusPartition_.referenceColor, lineNo) &&
         !LineRangeContains(focusPartition_.fullColor, lineNo);
}

bool TextEditor::isLineDimmedByFocus(int lineNo) const {
  return focusPartitionActive_ &&
         (LineRangeContains(focusPartition_.dimmed, lineNo) ||
          isLineExpandedHiddenByFocus(lineNo)) &&
         !LineRangeContains(focusPartition_.fullColor, lineNo) &&
         !LineRangeContains(focusPartition_.referenceColor, lineNo);
}

bool TextEditor::isLineExpandedHiddenByFocus(int lineNo) const {
  return focusPartitionActive_ && LineRangeContains(expandedFocusHiddenRanges_, lineNo);
}

std::optional<LineRange> TextEditor::focusHiddenRangeForLine(int lineNo) const {
  if (!focusPartitionActive_) {
    return std::nullopt;
  }

  const auto it = std::ranges::find_if(focusPartition_.hidden, [lineNo](const LineRange& range) {
    return lineNo >= range.startLine && lineNo < range.endLine;
  });
  if (it == focusPartition_.hidden.end()) {
    return std::nullopt;
  }

  return *it;
}

bool TextEditor::isFocusHiddenRangeExpanded(LineRange range) const {
  return std::ranges::find(expandedFocusHiddenRanges_, range) != expandedFocusHiddenRanges_.end();
}

void TextEditor::expandFocusHiddenRange(LineRange range) {
  if (range.endLine <= range.startLine || isFocusHiddenRangeExpanded(range)) {
    return;
  }

  expandedFocusHiddenRanges_.push_back(range);
  visualLayoutMaxColumns_ = 0;
}

bool TextEditor::tryExpandFocusHiddenPlaceholderAt(const ImVec2& position) {
  if (!focusPartitionActive_ || visualLines_.empty()) {
    return false;
  }

  const ImVec2 local{position.x - uiCursorPos_.x, position.y - uiCursorPos_.y};
  const int visualIndex = static_cast<int>(std::floor(local.y / charAdvance_.y));
  if (visualIndex < 0 || visualIndex >= static_cast<int>(visualLines_.size())) {
    return false;
  }

  const VisualLine& visualLine = visualLines_[visualIndex];
  if (!visualLine.focusHiddenPlaceholder) {
    return false;
  }

  expandFocusHiddenRange(visualLine.hiddenRange);
  return true;
}

void TextEditor::updateHoveredTextPosition() {
  hoveredTextPosition_.reset();
  if (!ImGui::IsWindowHovered() || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
      ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    return;
  }

  const ImVec2 mousePos = ImGui::GetMousePos();
  const ImVec2 local{mousePos.x - uiCursorPos_.x, mousePos.y - uiCursorPos_.y};
  if (local.x < textStart_ || local.y < 0.0f || charAdvance_.x <= 0.0f || charAdvance_.y <= 0.0f) {
    return;
  }

  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    const int visualIndex = static_cast<int>(std::floor(local.y / charAdvance_.y));
    if (visualIndex < 0 || visualIndex >= static_cast<int>(visualLines_.size()) ||
        visualLines_[visualIndex].focusHiddenPlaceholder) {
      return;
    }
  } else {
    const int lineNo = static_cast<int>(std::floor(local.y / charAdvance_.y));
    if (lineNo < 0 || lineNo >= text_.getTotalLines()) {
      return;
    }
  }

  hoveredTextPosition_ = screenPosToCoordinates(mousePos);
}

void TextEditor::rebuildVisualLines(const ImVec2& contentSize) {
  const int maxColumns =
      std::max(12, static_cast<int>((contentSize.x - textStart_ - 12.0f) / charAdvance_.x));
  if (visualLayoutMaxColumns_ == maxColumns && !visualLines_.empty()) {
    return;
  }

  visualLayoutMaxColumns_ = maxColumns;
  visualLines_.clear();
  for (int lineNo = 0; lineNo < text_.getTotalLines(); ++lineNo) {
    if (std::optional<LineRange> hiddenRange = focusHiddenRangeForLine(lineNo)) {
      if (!isFocusHiddenRangeExpanded(*hiddenRange)) {
        visualLines_.push_back(VisualLine{
            .lineNo = hiddenRange->startLine,
            .focusHiddenPlaceholder = true,
            .hiddenRange = *hiddenRange,
        });
        lineNo = hiddenRange->endLine - 1;
        continue;
      }
    }

    if (isLineHiddenByFocus(lineNo)) {
      continue;
    }

    const Line& line = text_.getLineGlyphs(lineNo);
    const std::string lineText = LineToString(line);
    std::vector<SoftWrapSegment> segments = wordWrapEnabled_
                                                ? ComputeSoftWrapSegments(lineText, maxColumns)
                                                : std::vector<SoftWrapSegment>{SoftWrapSegment{
                                                      .startColumn = 0,
                                                      .endColumn = text_.getLineMaxColumn(lineNo),
                                                  }};
    for (const SoftWrapSegment& segment : segments) {
      visualLines_.push_back(VisualLine{
          .lineNo = lineNo,
          .startColumn = segment.startColumn,
          .endColumn = segment.endColumn,
          .indentColumns = segment.indentColumns,
          .continuation = segment.continuation,
      });
    }
  }

  if (visualLines_.empty()) {
    visualLines_.push_back(VisualLine{});
  }
}

int TextEditor::visualLineIndexForCoordinates(const Coordinates& position) const {
  if (visualLines_.empty()) {
    return std::max(0, position.line);
  }

  int fallback = 0;
  for (int i = 0; i < static_cast<int>(visualLines_.size()); ++i) {
    const VisualLine& visualLine = visualLines_[i];
    if (visualLine.lineNo < position.line) {
      fallback = i;
      continue;
    }
    if (visualLine.lineNo > position.line) {
      return fallback;
    }
    if (position.column >= visualLine.startColumn && position.column <= visualLine.endColumn) {
      return i;
    }
    fallback = i;
  }
  return fallback;
}

Coordinates TextEditor::visualScreenPosToCoordinates(const ImVec2& position) const {
  if (visualLines_.empty()) {
    return sanitizeCoordinates(Coordinates(0, 0));
  }

  const ImVec2 local{position.x - uiCursorPos_.x, position.y - uiCursorPos_.y};
  const int visualIndex = std::clamp(static_cast<int>(std::floor(local.y / charAdvance_.y)), 0,
                                     static_cast<int>(visualLines_.size()) - 1);
  const VisualLine& visualLine = visualLines_[visualIndex];
  const float visualTextStart =
      textStart_ + static_cast<float>(visualLine.indentColumns) * charAdvance_.x;
  const float localX = local.x - visualTextStart;
  const int columnOffset = std::max(0, static_cast<int>(std::round(localX / charAdvance_.x)));
  const int column = std::clamp(visualLine.startColumn + columnOffset, visualLine.startColumn,
                                std::max(visualLine.startColumn, visualLine.endColumn));
  return sanitizeCoordinates(Coordinates(visualLine.lineNo, column));
}

Coordinates TextEditor::visibleSelectionEndCoordinates() const {
  Coordinates end = state_.selectionEnd;
  if (!hasSelection()) {
    return end;
  }

  if (end.column > 0) {
    return Coordinates(end.line, end.column - 1);
  }

  if (end.line > state_.selectionStart.line) {
    const int previousLine = end.line - 1;
    return Coordinates(previousLine, text_.getLineMaxColumn(previousLine));
  }

  return end;
}

float TextEditor::visibleTextRegionHeight() const {
  if (scrollViewportHeight_ > 0.0f) {
    return scrollViewportHeight_;
  }
  return ImGui::GetWindowHeight();
}

void TextEditor::scrollCoordinatesRangeIntoView(const Coordinates& start, const Coordinates& end) {
  if (!withinRender_) {
    return;
  }

  const float scrollY = ImGui::GetScrollY();
  const float height = visibleTextRegionHeight();

  int firstLine = std::min(start.line, end.line);
  int lastLine = std::max(start.line, end.line);
  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    firstLine = std::min(visualLineIndexForCoordinates(start), visualLineIndexForCoordinates(end));
    lastLine = std::max(visualLineIndexForCoordinates(start), visualLineIndexForCoordinates(end));
  }

  const int firstVisible = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const int visibleLineCount = std::max(1, static_cast<int>(std::floor(height / charAdvance_.y)));
  const int lastVisible = firstVisible + visibleLineCount - 1;
  const int selectedLineCount = lastLine - firstLine + 1;

  if (selectedLineCount > visibleLineCount || firstLine < firstVisible) {
    ImGui::SetScrollY(std::max(0.0f, static_cast<float>(firstLine) * charAdvance_.y));
    return;
  }

  if (lastLine > lastVisible) {
    const int targetFirstLine = lastLine - visibleLineCount + 1;
    ImGui::SetScrollY(std::max(0.0f, static_cast<float>(targetFirstLine) * charAdvance_.y));
  }
}

void TextEditor::renderInternal(std::string_view title) {
  calculateCharacterAdvance();
  updatePaletteAlpha();

  UTILS_RELEASE_ASSERT(lineBuffer_.empty());
  focused_ = ImGui::IsWindowFocused() || findFocused_ || replaceFocused_;

  const ImVec2 contentRegionMin = ImGui::GetWindowContentRegionMin();
  const auto contentSize = ImGui::GetWindowContentRegionMax();
  contentRegionMax_ = contentSize;
  scrollViewportHeight_ = std::max(0.0f, contentSize.y - contentRegionMin.y);
  auto* drawList = ImGui::GetWindowDrawList();
  float longestLine = textStart_;
  tickSourceFlashes();

  if (scrollToTop_) {
    scrollToTop_ = false;
    ImGui::SetScrollY(0.0f);
  }

  const ImVec2 cursorScreenPos = uiCursorPos_ = ImGui::GetCursorScreenPos();
  if (!horizontalScroll_) {
    ImGui::SetScrollX(0.0f);
  }
  const float scrollX = lastScrollX_ = horizontalScroll_ ? ImGui::GetScrollX() : 0.0f;
  const float scrollY = lastScroll_ = ImGui::GetScrollY();

  // Calculate visible lines
  const int pageSize = static_cast<int>(std::floor((scrollY + contentSize.y) / charAdvance_.y));
  int lineNo = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const int totalLines = text_.getTotalLines();
  int visibleLineMax = std::max(0, std::min(totalLines - 1, lineNo + pageSize));

  updateTextStart();
  visualLayoutMaxColumns_ = 0;
  rebuildVisualLines(contentSize);
  calculateFolds(lineNo, totalLines);

  if (wordWrapEnabled_ || focusPartitionActive_) {
    const int firstVisualLine = std::clamp(static_cast<int>(std::floor(scrollY / charAdvance_.y)),
                                           0, static_cast<int>(visualLines_.size()) - 1);
    const int lastVisualLine =
        std::clamp(firstVisualLine + pageSize + 1, 0, static_cast<int>(visualLines_.size()) - 1);
    for (int visualIndex = firstVisualLine; visualIndex <= lastVisualLine; ++visualIndex) {
      const ImVec2 lineStartPos{cursorScreenPos.x,
                                cursorScreenPos.y + visualIndex * charAdvance_.y};
      const ImVec2 textPos{
          lineStartPos.x + textStart_ + visualLines_[visualIndex].indentColumns * charAdvance_.x,
          lineStartPos.y};
      renderVisualLine(visualLines_[visualIndex], visualIndex, lineStartPos, textPos, contentSize,
                       drawList, longestLine);
    }
  } else {
    while (lineNo <= visibleLineMax) {
      const ImVec2 lineStartPos{cursorScreenPos.x,
                                cursorScreenPos.y + (lineNo - foldedLines_) * charAdvance_.y};
      const ImVec2 textPos{lineStartPos.x + textStart_, lineStartPos.y};

      renderLine(lineNo, lineStartPos, textPos, contentSize, scrollX, drawList, longestLine);
      lineNo++;
    }
  }

  renderFocusReferenceLinks(drawList);
  renderSourceStyleDecorationChips(drawList);
  hitTestSourceStyleDecorationChips();
  renderSourceStyleDecorationTooltip();
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

  if (scrollSelectionIntoView_ && hasSelection()) {
    scrollCoordinatesRangeIntoView(state_.selectionStart, visibleSelectionEndCoordinates());
    scrollSelectionIntoView_ = false;
    scrollToCursor_ = false;
    return;
  }

  const float scrollY = ImGui::GetScrollY();
  const float height = visibleTextRegionHeight();

  const auto pos = getActualCursorCoordinates();

  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    const int visualIndex = visualLineIndexForCoordinates(pos);
    const int firstVisible = static_cast<int>(std::floor(scrollY / charAdvance_.y));
    const int lastVisible =
        static_cast<int>(std::floor((scrollY + height - charAdvance_.y) / charAdvance_.y));
    if (visualIndex < firstVisible) {
      ImGui::SetScrollY(std::max(0.0f, visualIndex * charAdvance_.y));
    } else if (visualIndex > lastVisible) {
      ImGui::SetScrollY(std::max(0.0f, (visualIndex + 1) * charAdvance_.y - height));
    }
    scrollToCursor_ = false;
    return;
  }

  const auto top = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const auto bottom =
      static_cast<int>(std::floor((scrollY + height - charAdvance_.y) / charAdvance_.y));

  if (pos.line < top) {
    ImGui::SetScrollY(std::max(0.0f, pos.line * charAdvance_.y));
  }
  if (pos.line > bottom) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line + 1) * charAdvance_.y - height));
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
  renderSourceStyleDecorationStrikethroughs(lineNo, 0, text_.getLineMaxColumn(lineNo), drawList);

  // Render cursor if this is the cursor line
  if (state_.cursorPosition.line == lineNo) {
    renderCursor(lineStart, drawList);
  }
}

void TextEditor::renderVisualLine(const VisualLine& visualLine, int visualLineIndex,
                                  const ImVec2& lineStart, const ImVec2& textStart,
                                  const ImVec2& contentSize, ImDrawList* drawList,
                                  float& longestLine) {
  if (visualLine.focusHiddenPlaceholder) {
    renderFocusHiddenPlaceholder(visualLine, lineStart, contentSize, drawList, longestLine);
    return;
  }

  if (visualLine.lineNo >= text_.getTotalLines()) {
    return;
  }

  const Line& line = text_.getLineGlyphs(visualLine.lineNo);
  const int visualColumns =
      visualLine.indentColumns + std::max(0, visualLine.endColumn - visualLine.startColumn);
  longestLine = std::max(textStart_ + visualColumns * charAdvance_.x, longestLine);

  renderLineBackground(visualLine, lineStart, contentSize, drawList);

  if (showLineNumbers_ && !visualLine.continuation) {
    renderLineNumbers(visualLine.lineNo, lineStart, drawList);
  }

  if (hasSelection()) {
    renderSelection(visualLine, line, lineStart, drawList);
  }

  renderText(visualLine, line, textStart, drawList);
  renderSourceStyleDecorationStrikethroughs(visualLine.lineNo, visualLine.startColumn,
                                            visualLine.endColumn, drawList);

  if (state_.cursorPosition.line == visualLine.lineNo &&
      visualLineIndex == visualLineIndexForCoordinates(state_.cursorPosition)) {
    renderCursor(lineStart, drawList);
  }
}

void TextEditor::renderFocusHiddenPlaceholder(const VisualLine& visualLine, const ImVec2& lineStart,
                                              const ImVec2& contentSize, ImDrawList* drawList,
                                              float& longestLine) {
  const ImVec2 rowStart{lineStart.x + ImGui::GetScrollX(), lineStart.y};
  const ImVec2 rowEnd{lineStart.x + contentSize.x + 2.0f * ImGui::GetScrollX(),
                      lineStart.y + charAdvance_.y};
  const bool hovered = ImGui::IsMouseHoveringRect(rowStart, rowEnd);
  if (hovered) {
    drawList->AddRectFilled(rowStart, rowEnd,
                            palette_[static_cast<int>(ColorIndex::CurrentLineFillInactive)]);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
  }

  if (showLineNumbers_) {
    const ImVec2 markerPos{lineStart.x + textStart_ - 3.0f * charAdvance_.x, lineStart.y};
    drawList->AddText(markerPos, palette_[static_cast<int>(ColorIndex::LineNumber)], "...");
  }

  const ImU32 color = palette_[static_cast<int>(ColorIndex::Comment)];
  const ImVec2 textPos{lineStart.x + textStart_, lineStart.y};
  drawList->AddText(textPos, color, "...");
  longestLine = std::max(longestLine, textStart_ + 3.0f * charAdvance_.x);

  (void)visualLine;
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

void TextEditor::renderLineBackground(const VisualLine& visualLine, const ImVec2& lineStart,
                                      const ImVec2& contentSize, ImDrawList* drawList) {
  renderLineBackground(visualLine.lineNo, lineStart, contentSize, drawList);

  const float textStartX =
      lineStart.x + textStart_ + static_cast<float>(visualLine.indentColumns) * charAdvance_.x;

  const auto drawSourceRange = [&](SourceByteRange byteRange, ImU32 color) {
    const Coordinates rangeStart = getCoordinatesAtByteOffset(byteRange.start);
    const Coordinates rangeEnd = getCoordinatesAtByteOffset(byteRange.end);
    if (rangeEnd.line < visualLine.lineNo || rangeStart.line > visualLine.lineNo) {
      return;
    }

    int startColumn = visualLine.startColumn;
    int endColumn = visualLine.endColumn;
    if (rangeStart.line == visualLine.lineNo) {
      startColumn = std::max(startColumn, rangeStart.column);
    }
    if (rangeEnd.line == visualLine.lineNo) {
      endColumn = std::min(endColumn, rangeEnd.column);
    }
    if (endColumn <= startColumn) {
      return;
    }

    const ImVec2 start{textStartX + (startColumn - visualLine.startColumn) * charAdvance_.x,
                       lineStart.y};
    const ImVec2 end{textStartX + (endColumn - visualLine.startColumn) * charAdvance_.x,
                     lineStart.y + charAdvance_.y};
    drawList->AddRectFilled(start, end, color, 2.0f * uiScale_);
  };

  const ImU32 hoverColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.62f, 1.0f, 0.13f));
  for (const SourceByteRange& range : hoverSourceRanges_) {
    drawSourceRange(range, hoverColor);
  }

  const std::vector<ActiveFlash> flashes =
      flashDecorations_.activeBackgrounds(FlashDecorations::Clock::now());
  for (const ActiveFlash& flash : flashes) {
    const float alpha = std::clamp(0.18f + 0.32f * flash.intensity, 0.0f, 0.5f);
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.78f, 0.16f, alpha));
    drawSourceRange(flash.byteRange, color);
  }
}

namespace {

ImVec2 AddVec(const ImVec2& lhs, const ImVec2& rhs) {
  return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y);
}

ImVec2 SubVec(const ImVec2& lhs, const ImVec2& rhs) {
  return ImVec2(lhs.x - rhs.x, lhs.y - rhs.y);
}

ImVec2 MulVec(const ImVec2& value, float scale) {
  return ImVec2(value.x * scale, value.y * scale);
}

Vector2d ToVector2d(const ImVec2& value) {
  return Vector2d(value.x, value.y);
}

ImVec2 ToImVec2(const Vector2d& value) {
  return ImVec2(static_cast<float>(value.x), static_cast<float>(value.y));
}

Box2d BoxFromCorners(const ImVec2& lhs, const ImVec2& rhs) {
  const Vector2d topLeft(std::min(lhs.x, rhs.x), std::min(lhs.y, rhs.y));
  const Vector2d bottomRight(std::max(lhs.x, rhs.x), std::max(lhs.y, rhs.y));
  return Box2d(topLeft, bottomRight);
}

void AddPointToBox(Box2d* box, const ImVec2& point) {
  box->addPoint(Vector2d(point.x, point.y));
}

Box2d InflatedBox(Box2d box, double margin) {
  box.topLeft -= Vector2d(margin, margin);
  box.bottomRight += Vector2d(margin, margin);
  return box;
}

bool BoxesIntersect(const Box2d& lhs, const Box2d& rhs) {
  return lhs.bottomRight.x >= rhs.topLeft.x && lhs.topLeft.x <= rhs.bottomRight.x &&
         lhs.bottomRight.y >= rhs.topLeft.y && lhs.topLeft.y <= rhs.bottomRight.y;
}

float VecLength(const ImVec2& value) {
  return std::sqrt(value.x * value.x + value.y * value.y);
}

ImVec2 NormalizeVec(const ImVec2& value) {
  const float length = VecLength(value);
  return length > 0.001f ? MulVec(value, 1.0f / length) : ImVec2(0.0f, 0.0f);
}

bool IsReferenceSourceChar(char value) {
  const unsigned char ch = static_cast<unsigned char>(value);
  return std::isalnum(ch) != 0 || value == '_' || value == '-' || value == '#' || value == '.' ||
         value == ':' || value == '%';
}

enum class EditBoundaryBias {
  Before,
  After,
};

SourcePoint ToSourcePoint(const SourceEditPoint& point) {
  return SourcePoint{.line = point.line, .column = point.column};
}

int CompareSourcePoints(const SourcePoint& lhs, const SourcePoint& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line ? -1 : 1;
  }
  if (lhs.column != rhs.column) {
    return lhs.column < rhs.column ? -1 : 1;
  }
  return 0;
}

SourcePoint ShiftPointAfterEdit(const SourcePoint& point, const SourcePoint& oldEnd,
                                const SourcePoint& newEnd) {
  if (point.line == oldEnd.line) {
    return SourcePoint{
        .line = newEnd.line,
        .column = std::max(0, newEnd.column + point.column - oldEnd.column),
    };
  }

  return SourcePoint{
      .line = std::max(0, point.line + newEnd.line - oldEnd.line),
      .column = point.column,
  };
}

SourcePoint MapSourcePointForEdit(const SourcePoint& point, const SourceEditIntent& intent,
                                  EditBoundaryBias bias) {
  const SourcePoint start = ToSourcePoint(intent.start);
  const SourcePoint oldEnd = ToSourcePoint(intent.removedEnd);
  const SourcePoint newEnd = ToSourcePoint(intent.replacementEnd);
  const int startComparison = CompareSourcePoints(point, start);
  if (startComparison < 0) {
    return point;
  }

  if (CompareSourcePoints(oldEnd, start) == 0) {
    if (startComparison == 0) {
      return bias == EditBoundaryBias::After ? newEnd : start;
    }
    return ShiftPointAfterEdit(point, oldEnd, newEnd);
  }

  const int endComparison = CompareSourcePoints(point, oldEnd);
  if (endComparison > 0) {
    return ShiftPointAfterEdit(point, oldEnd, newEnd);
  }
  if (startComparison == 0 && bias == EditBoundaryBias::Before) {
    return start;
  }

  return bias == EditBoundaryBias::After ? newEnd : start;
}

std::size_t MapSourceOffsetForEdit(std::size_t point, const SourceEditIntent& intent,
                                   EditBoundaryBias bias) {
  const std::size_t editStart = intent.offset;
  const std::size_t removedLength = intent.removedLength;
  const std::size_t insertedLength = intent.replacement.size();
  const std::size_t editEnd = editStart + removedLength;
  if (point < editStart) {
    return point;
  }

  if (removedLength == 0u && point == editStart) {
    return bias == EditBoundaryBias::After ? editStart + insertedLength : editStart;
  }

  if (point > editEnd) {
    if (insertedLength >= removedLength) {
      return point + (insertedLength - removedLength);
    }
    return point - (removedLength - insertedLength);
  }

  if (point == editStart && bias == EditBoundaryBias::Before) {
    return editStart;
  }
  return editStart + insertedLength;
}

SourceByteRange MapSourceByteRangeForEdit(SourceByteRange range, const SourceEditIntent& intent,
                                          std::size_t newBufferSize) {
  range.start =
      std::min(MapSourceOffsetForEdit(range.start, intent, EditBoundaryBias::After), newBufferSize);
  range.end =
      std::min(MapSourceOffsetForEdit(range.end, intent, EditBoundaryBias::After), newBufferSize);
  if (range.end < range.start) {
    range.end = range.start;
  }
  return range;
}

LineRange MapLineRangeForEdit(LineRange range, const SourceEditIntent& intent, int totalLines) {
  const SourcePoint start = MapSourcePointForEdit(SourcePoint{.line = range.startLine, .column = 0},
                                                  intent, EditBoundaryBias::Before);
  const SourcePoint end = MapSourcePointForEdit(SourcePoint{.line = range.endLine, .column = 0},
                                                intent, EditBoundaryBias::After);
  range.startLine = std::clamp(start.line, 0, totalLines);
  range.endLine = std::clamp(end.line, 0, totalLines);
  if (range.endLine < range.startLine) {
    range.endLine = range.startLine;
  }
  return range;
}

void DrawArrowhead(ImDrawList* drawList, const ImVec2& tip, const ImVec2& direction, ImU32 color,
                   float length, float halfWidth) {
  const ImVec2 unitDirection = NormalizeVec(direction);
  if (VecLength(unitDirection) <= 0.001f) {
    return;
  }

  const ImVec2 normal(-unitDirection.y, unitDirection.x);
  const ImVec2 base = SubVec(tip, MulVec(unitDirection, length));
  drawList->AddTriangleFilled(tip, AddVec(base, MulVec(normal, halfWidth)),
                              SubVec(base, MulVec(normal, halfWidth)), color);
}

std::uint32_t MixReferenceSeed(const FocusReferenceLink& link, int linkIndex) {
  std::uint32_t seed = 0x9e3779b9u ^ static_cast<std::uint32_t>(linkIndex + 1);
  const auto mix = [&seed](int value) {
    seed ^= static_cast<std::uint32_t>(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
  };
  mix(link.from.line);
  mix(link.from.column);
  mix(link.to.line);
  mix(link.to.column);
  return seed;
}

float UnitFromSeed(std::uint32_t seed) {
  seed ^= seed >> 16u;
  seed *= 0x7feb352du;
  seed ^= seed >> 15u;
  seed *= 0x846ca68bu;
  seed ^= seed >> 16u;
  return static_cast<float>(seed & 0xffffu) / static_cast<float>(0xffffu);
}

ImU32 PastelReferenceColor(const FocusReferenceLink& link, int linkIndex, float alpha) {
  const std::uint32_t seed = MixReferenceSeed(link, linkIndex);
  const float hue = UnitFromSeed(seed);
  const float saturation = 0.30f + 0.10f * UnitFromSeed(seed ^ 0x68bc21ebu);
  const float value = 0.90f + 0.08f * UnitFromSeed(seed ^ 0x02e5be93u);

  float red = 0.0f;
  float green = 0.0f;
  float blue = 0.0f;
  ImGui::ColorConvertHSVtoRGB(hue, saturation, value, red, green, blue);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, std::clamp(alpha, 0.0f, 1.0f)));
}

ImU32 BrightenReferenceColor(ImU32 color) {
  ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
  value.x = std::min(1.0f, value.x * 1.16f + 0.06f);
  value.y = std::min(1.0f, value.y * 1.16f + 0.06f);
  value.z = std::min(1.0f, value.z * 1.16f + 0.06f);
  value.w = std::min(1.0f, value.w + 0.20f);
  return ImGui::ColorConvertFloat4ToU32(value);
}

void StrokeDonnerPath(ImDrawList* drawList, const Path& path, ImU32 color, float thickness) {
  if (path.empty()) {
    return;
  }

  drawList->PathClear();
  path.forEach([drawList](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo:
      case Path::Verb::LineTo: drawList->PathLineTo(ToImVec2(points[0])); break;
      case Path::Verb::QuadTo:
        drawList->PathBezierQuadraticCurveTo(ToImVec2(points[0]), ToImVec2(points[1]), 8);
        break;
      case Path::Verb::CurveTo:
        drawList->PathBezierCubicCurveTo(ToImVec2(points[0]), ToImVec2(points[1]),
                                         ToImVec2(points[2]), 8);
        break;
      case Path::Verb::ClosePath: break;
    }
  });
  drawList->PathStroke(color, ImDrawFlags_None, thickness);
}

float TextBaselineOffsetY() {
  const ImFont* font = ImGui::GetFont();
  if (font == nullptr || font->FontSize <= 0.0f) {
    return ImGui::GetTextLineHeight();
  }

  return font->Ascent * (ImGui::GetFontSize() / font->FontSize);
}

ImU32 SourceStyleChipFillColor() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(0.13f, 0.39f, 0.64f, 0.96f));
}

ImU32 SourceStyleChipBorderColor() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(0.58f, 0.78f, 0.95f, 0.76f));
}

ImU32 SourceStyleChipTextColor() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(0.96f, 0.99f, 1.0f, 1.0f));
}

ImU32 SourceStyleStrikethroughColor() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.92f));
}

constexpr const char* kStyleSourceChipIcon = "✦";

ImVec2 StyleSourceChipTextSize() {
  return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f,
                                         kStyleSourceChipIcon);
}

ImVec2 ChipConnectorPoint(const ImVec2& min, const ImVec2& max, const ImVec2& otherEndpoint,
                          float verticalTolerance) {
  const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  if (max.y + verticalTolerance < otherEndpoint.y) {
    return ImVec2(center.x, max.y);
  }

  const float leftDistance = std::abs(otherEndpoint.x - min.x);
  const float rightDistance = std::abs(otherEndpoint.x - max.x);
  const float sideX = leftDistance <= rightDistance ? min.x : max.x;
  return ImVec2(sideX, center.y);
}

}  // namespace

std::optional<TextEditor::FocusReferenceConnectorLayout> TextEditor::focusReferenceConnectorLayout(
    const FocusReferenceLink& link, int linkIndex) const {
  if (isLineHiddenByFocus(link.from.line) || isLineHiddenByFocus(link.to.line)) {
    return std::nullopt;
  }

  FocusReferenceConnectorLayout layout;
  const float baselineY = TextBaselineOffsetY();
  layout.start = coordinatesToScreenPos(Coordinates(link.from.line, link.from.column));
  layout.start.x += charAdvance_.x * 0.5f;
  layout.start.y += baselineY;
  if (std::optional<SourceStyleChipBounds> targetChip =
          sourceStyleChipBoundsForReferenceTarget(link.to)) {
    layout.tip = ChipConnectorPoint(targetChip->min, targetChip->max, layout.start, uiScale_);

    if (targetChip->kind == SourceStyleChipKind::SelectorMatchCount) {
      if (std::optional<SourceStyleChipBounds> sourceChip =
              focusReferenceStyleSourceChipBounds(link.from)) {
        layout.sourceStyleChip = *sourceChip;
        layout.hasSourceStyleChip = true;
        layout.start = ChipConnectorPoint(sourceChip->min, sourceChip->max, layout.tip, uiScale_);
      }
    }
  } else {
    layout.tip = coordinatesToScreenPos(Coordinates(link.to.line, link.to.column));
    layout.tip.x -= 2.0f * uiScale_;
    layout.tip.y += baselineY;
  }

  if (!layout.hasSourceStyleChip) {
    if (std::optional<FocusReferenceSourceUnderline> sourceUnderline =
            focusReferenceSourceUnderline(link.from)) {
      layout.sourceUnderline = *sourceUnderline;
      layout.hasSourceUnderline = true;
      layout.start = ImVec2((sourceUnderline->start.x + sourceUnderline->end.x) * 0.5f,
                            sourceUnderline->start.y);
    } else {
      layout.start = coordinatesToScreenPos(Coordinates(link.from.line, link.from.column));
      layout.start.x += charAdvance_.x * 0.5f;
      layout.start.y += baselineY;
    }
  }

  const float selectionAlpha =
      ImGui::ColorConvertU32ToFloat4(palette_[static_cast<int>(ColorIndex::Selection)]).w;
  layout.color = PastelReferenceColor(link, linkIndex, selectionAlpha);
  return layout;
}

std::optional<TextEditor::FocusReferenceSourceUnderline> TextEditor::focusReferenceSourceUnderline(
    const SourcePoint& source) const {
  const Coordinates position = sanitizeCoordinates(Coordinates(source.line, source.column));
  if (position.line < 0 || position.line >= text_.getTotalLines()) {
    return std::nullopt;
  }

  const Line& line = text_.getLineGlyphs(position.line);
  if (line.empty()) {
    return std::nullopt;
  }

  int focusIndex = text_.getCharacterIndex(position);
  if (focusIndex >= static_cast<int>(line.size())) {
    focusIndex = static_cast<int>(line.size()) - 1;
  }
  if (focusIndex < 0) {
    return std::nullopt;
  }

  if (!IsReferenceSourceChar(line[focusIndex].character) && focusIndex > 0 &&
      IsReferenceSourceChar(line[focusIndex - 1].character)) {
    --focusIndex;
  }

  int underlineStartIndex = focusIndex;
  int underlineEndIndex = std::min(focusIndex + 1, static_cast<int>(line.size()));
  if (IsReferenceSourceChar(line[focusIndex].character)) {
    while (underlineStartIndex > 0 &&
           IsReferenceSourceChar(line[underlineStartIndex - 1].character)) {
      --underlineStartIndex;
    }
    while (underlineEndIndex < static_cast<int>(line.size()) &&
           IsReferenceSourceChar(line[underlineEndIndex].character)) {
      ++underlineEndIndex;
    }
  }

  const int startColumn = text_.getCharacterColumn(position.line, underlineStartIndex);
  int endColumn = text_.getCharacterColumn(position.line, underlineEndIndex);
  if (endColumn <= startColumn) {
    endColumn = startColumn + 1;
  }

  const ImVec2 startPos = coordinatesToScreenPos(Coordinates(position.line, startColumn));
  const ImVec2 endPos = coordinatesToScreenPos(Coordinates(position.line, endColumn));
  const float underlineY = startPos.y + TextBaselineOffsetY() + std::max(1.5f * uiScale_, 1.5f);
  return FocusReferenceSourceUnderline{
      .start = ImVec2(startPos.x, underlineY),
      .end = ImVec2(endPos.x, underlineY),
  };
}

RopeSimulationOptions TextEditor::focusReferenceRopeOptions() const {
  RopeSimulationOptions options;
  options.segmentCount = 28;
  options.constraintIterations = 10;
  options.gravityPxPerSec2 = 180.0 * uiScale_;
  options.damping = 0.985;
  options.scrollResponse = 0.008;
  options.maxScrollImpulsePx = 0.8 * uiScale_;
  options.idleSwayPxPerSec2 = 18.0 * uiScale_;
  options.idleSwayFrequencyHz = 0.45;
  options.idleSwayMaxSpeed = 6.0 * uiScale_;
  options.maxDeltaTime = 1.0 / 60.0;
  options.settleTimeSeconds = 5.0;
  options.settleMotionThresholdPx = 0.012 * uiScale_;
  options.settleStillnessSeconds = 0.20;
  options.settleRestDistanceThresholdPx = 0.45 * uiScale_;
  options.overdueDamping = 0.42;
  options.overdueDampingRampSeconds = 1.25;
  options.catenaryRestoringForcePerSec2 = 640.0;
  options.bezierTension = 1.0;
  options.catenarySlackRatio = 0.18;
  options.catenaryMinSlackPx = 30.0 * uiScale_;
  options.catenaryMaxSlackPx = 120.0 * uiScale_;
  options.initialImpulsePx = 0.2 * uiScale_;
  options.endpointFollow = 0.84;
  options.endpointImpulse = 0.0;
  options.maxEndpointImpulsePx = 0.0;
  options.endpointMotionVelocityRetention = 0.22;
  options.endpointCatenaryBlend = 0.18;
  return options;
}

bool TextEditor::isFocusReferenceRopeHit(const Path& path, const ImVec2& mousePos,
                                         float hitWidth) const {
  return !path.empty() && path.isOnPath(ToVector2d(mousePos), static_cast<double>(hitWidth));
}

void TextEditor::navigateToFocusReferenceLink(const FocusReferenceLink& link) {
  const Coordinates target(link.to.line, link.to.column);
  setSelection(target, target);
  setCursorPosition(target);
  cursorPositionChanged_ = true;
  cursorPositionChangedByMouse_ = true;
  scrollToCursor_ = true;
}

void TextEditor::remapFocusMetadataForSourceEdit(const SourceEditIntent& intent) {
  if (intent.removedLength == 0u && intent.replacement.empty()) {
    return;
  }

  const std::size_t bufferSize = getText().size();
  for (SourceByteRange& range : hoverSourceRanges_) {
    range = MapSourceByteRangeForEdit(range, intent, bufferSize);
  }
  std::erase_if(hoverSourceRanges_,
                [](const SourceByteRange& range) { return range.end <= range.start; });

  for (SourceStyleDecoration& decoration : sourceStyleDecorations_) {
    decoration.range = MapSourceByteRangeForEdit(decoration.range, intent, bufferSize);
    decoration.chipRange = MapSourceByteRangeForEdit(decoration.chipRange, intent, bufferSize);
  }
  sourceStyleChipHitRects_.clear();

  if (!focusPartitionActive_) {
    return;
  }

  const int totalLines = text_.getTotalLines();
  const auto remapRanges = [&intent, totalLines](std::vector<LineRange>* ranges) {
    for (LineRange& range : *ranges) {
      range = MapLineRangeForEdit(range, intent, totalLines);
    }
    std::erase_if(*ranges, [](const LineRange& range) { return range.endLine <= range.startLine; });
  };

  remapRanges(&focusPartition_.fullColor);
  remapRanges(&focusPartition_.referenceColor);
  remapRanges(&focusPartition_.dimmed);
  remapRanges(&focusPartition_.hidden);
  remapRanges(&expandedFocusHiddenRanges_);

  std::map<FocusReferenceLink, FocusReferenceRopeState, FocusReferenceLinkLess> remappedRopes;
  for (FocusReferenceLink& link : focusPartition_.referenceLinks) {
    const FocusReferenceLink oldLink = link;
    link.from = MapSourcePointForEdit(link.from, intent, EditBoundaryBias::After);
    link.to = MapSourcePointForEdit(link.to, intent, EditBoundaryBias::After);

    auto node = focusReferenceRopes_.extract(oldLink);
    if (!node.empty()) {
      node.key() = link;
      remappedRopes.insert(std::move(node));
    }
  }
  focusReferenceRopes_ = std::move(remappedRopes);
  focusPartitionActive_ = !focusPartition_.empty();
  visualLayoutMaxColumns_ = 0;
}

bool TextEditor::tryNavigateToFocusReferenceRopeAt(const ImVec2& mousePos) {
  if (!focusPartitionActive_ || focusReferenceRopes_.empty()) {
    return false;
  }

  const float hitWidth = std::max(7.0f * uiScale_, 5.0f);
  for (const auto& [link, state] : focusReferenceRopes_) {
    if (isFocusReferenceRopeHit(state.path, mousePos, hitWidth)) {
      navigateToFocusReferenceLink(link);
      return true;
    }
  }

  return false;
}

std::optional<TextEditor::SourceStyleChipBounds> TextEditor::sourceStyleChipBoundsForDecoration(
    const SourceStyleDecoration& decoration) const {
  if (!decoration.showChip) {
    return std::nullopt;
  }

  const SourceByteRange anchorRange = decoration.chipRange.end > decoration.chipRange.start
                                          ? decoration.chipRange
                                          : decoration.range;
  const Coordinates anchor = getCoordinatesAtByteOffset(anchorRange.end);
  if (isLineHiddenByFocus(anchor.line)) {
    return std::nullopt;
  }

  const float paddingX = std::max(3.0f * uiScale_, 3.0f);
  const float paddingY = std::max(1.0f * uiScale_, 1.0f);
  const float gap = std::max(4.0f * uiScale_, 4.0f);
  const std::string label = std::to_string(decoration.chipCount);
  const ImVec2 textSize =
      ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, label.c_str());
  const ImVec2 chipSize(textSize.x + paddingX * 2.0f, textSize.y + paddingY * 2.0f);
  ImVec2 min = coordinatesToScreenPos(anchor);
  min.x += gap;
  min.y += std::max(0.0f, (charAdvance_.y - chipSize.y) * 0.5f);
  return SourceStyleChipBounds{
      .min = min,
      .max = ImVec2(min.x + chipSize.x, min.y + chipSize.y),
      .kind = decoration.chipKind,
  };
}

std::optional<TextEditor::SourceStyleChipBounds>
TextEditor::sourceStyleChipBoundsForReferenceTarget(const SourcePoint& target) const {
  const Coordinates targetCoordinates(target.line, target.column);
  const std::size_t targetOffset = getByteOffsetAtCoordinates(targetCoordinates);
  for (const SourceStyleDecoration& decoration : sourceStyleDecorations_) {
    if (!decoration.showChip) {
      continue;
    }

    const SourceByteRange anchorRange = decoration.chipRange.end > decoration.chipRange.start
                                            ? decoration.chipRange
                                            : decoration.range;
    const Coordinates chipAnchor = getCoordinatesAtByteOffset(anchorRange.end);
    const bool exactAnchor = chipAnchor.line == target.line && chipAnchor.column == target.column;
    const bool inChipRange = targetOffset >= anchorRange.start && targetOffset <= anchorRange.end;
    if (!exactAnchor && !inChipRange) {
      continue;
    }

    return sourceStyleChipBoundsForDecoration(decoration);
  }

  return std::nullopt;
}

std::optional<TextEditor::SourceStyleChipBounds> TextEditor::focusReferenceStyleSourceChipBounds(
    const SourcePoint& source) const {
  const Coordinates position = sanitizeCoordinates(Coordinates(source.line, source.column));
  if (position.line < 0 || position.line >= text_.getTotalLines() ||
      isLineHiddenByFocus(position.line)) {
    return std::nullopt;
  }

  const Coordinates lineEnd(position.line, text_.getLineMaxColumn(position.line));
  const float paddingX = std::max(4.0f * uiScale_, 4.0f);
  const float paddingY = std::max(1.0f * uiScale_, 1.0f);
  const float gap = std::max(5.0f * uiScale_, 5.0f);
  const ImVec2 iconSize = StyleSourceChipTextSize();
  const ImVec2 chipSize(iconSize.x + paddingX * 2.0f, iconSize.y + paddingY * 2.0f);
  ImVec2 min = coordinatesToScreenPos(lineEnd);
  min.x += gap;
  min.y += std::max(0.0f, (charAdvance_.y - chipSize.y) * 0.5f);
  return SourceStyleChipBounds{
      .min = min,
      .max = ImVec2(min.x + chipSize.x, min.y + chipSize.y),
      .kind = SourceStyleChipKind::SelectorMatchCount,
  };
}

void TextEditor::renderFocusReferenceLinks(ImDrawList* drawList) {
  if (!focusPartitionActive_ || focusPartition_.referenceLinks.empty()) {
    focusReferenceRopes_.clear();
    lastFocusReferenceRopeScrollY_ = lastScroll_;
    lastSourceRopeCost_ = FrameCostBreakdown::SourceRopes{};
    return;
  }

  FrameCostBreakdown::SourceRopes ropeCost;
  ropeCost.candidateCount = static_cast<int>(focusPartition_.referenceLinks.size());

  const float thickness = 1.75f * uiScale_;
  const float hoverThickness = 2.45f * uiScale_;
  const float arrowLength = 8.0f * uiScale_;
  const float arrowHalfWidth = 4.5f * uiScale_;
  const float hitWidth = std::max(7.0f * uiScale_, hoverThickness + 4.0f * uiScale_);
  const RopeSimulationOptions ropeOptions = focusReferenceRopeOptions();
  const double deltaTime = static_cast<double>(ImGui::GetIO().DeltaTime);
  const bool hadRopes = !focusReferenceRopes_.empty();
  const bool sourceWindowHovered = ImGui::IsWindowHovered();
  const double scrollDeltaY =
      hadRopes && sourceWindowHovered
          ? static_cast<double>(lastScroll_ - lastFocusReferenceRopeScrollY_)
          : 0.0;
  lastFocusReferenceRopeScrollY_ = lastScroll_;

  ++focusReferenceRopeFrame_;

  int visibleLinkIndex = 0;
  int animatedLinkCount = 0;
  bool hoveredAny = false;
  const bool canHover = sourceWindowHovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        !ImGui::IsMouseDown(ImGuiMouseButton_Right);
  const ImVec2 mousePos = ImGui::GetMousePos();
  const ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
  const ImVec2 clipMin(uiCursorPos_.x, uiCursorPos_.y + lastScroll_);
  const ImVec2 clipMax(uiCursorPos_.x + contentMax.x,
                       uiCursorPos_.y + lastScroll_ + visibleTextRegionHeight());
  const Box2d clipRect = BoxFromCorners(clipMin, clipMax);
  const Box2d visibleTextBounds =
      InflatedBox(clipRect, kFocusReferenceRopeCullMarginPx * static_cast<double>(uiScale_));

  const auto layoutBounds = [](const FocusReferenceConnectorLayout& layout) {
    Box2d bounds = BoxFromCorners(layout.start, layout.tip);
    if (layout.hasSourceUnderline) {
      AddPointToBox(&bounds, layout.sourceUnderline.start);
      AddPointToBox(&bounds, layout.sourceUnderline.end);
    }
    if (layout.hasSourceStyleChip) {
      AddPointToBox(&bounds, layout.sourceStyleChip.min);
      AddPointToBox(&bounds, layout.sourceStyleChip.max);
    }
    return bounds;
  };
  const auto sourcePointIntersectsVisibleRows = [&](const SourcePoint& point) {
    if (isLineHiddenByFocus(point.line)) {
      return false;
    }

    int visualIndex = point.line;
    if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
      visualIndex = visualLineIndexForCoordinates(Coordinates(point.line, point.column));
      if (visualIndex < 0 || visualIndex >= static_cast<int>(visualLines_.size()) ||
          visualLines_[visualIndex].lineNo != point.line) {
        return false;
      }
    }

    const double rowTop = uiCursorPos_.y + static_cast<double>(visualIndex) * charAdvance_.y;
    const double rowBottom = rowTop + charAdvance_.y;
    return rowBottom >= visibleTextBounds.topLeft.y && rowTop <= visibleTextBounds.bottomRight.y;
  };

  drawList->PushClipRect(clipMin, clipMax, true);
  for (const FocusReferenceLink& link : focusPartition_.referenceLinks) {
    if (!sourcePointIntersectsVisibleRows(link.from)) {
      ++ropeCost.culledCount;
      continue;
    }

    const auto layoutStart = std::chrono::steady_clock::now();
    std::optional<FocusReferenceConnectorLayout> layout =
        focusReferenceConnectorLayout(link, visibleLinkIndex);
    ropeCost.layoutMs += MillisecondsSince(layoutStart);
    if (!layout.has_value()) {
      continue;
    }
    ++ropeCost.laidOutCount;
    if (!BoxesIntersect(layoutBounds(*layout), visibleTextBounds)) {
      ++ropeCost.culledCount;
      continue;
    }

    const auto drawStaticConnector = [&] {
      const auto drawStart = std::chrono::steady_clock::now();
      const ImU32 color = layout->color;
      if (layout->hasSourceUnderline) {
        drawList->AddLine(layout->sourceUnderline.start, layout->sourceUnderline.end, color,
                          std::max(1.0f, uiScale_));
      }
      if (layout->hasSourceStyleChip) {
        renderFocusReferenceStyleSourceChip(drawList, layout->sourceStyleChip, color);
      }
      drawList->AddLine(layout->start, layout->tip, color, thickness);
      DrawArrowhead(drawList, layout->tip, NormalizeVec(SubVec(layout->tip, layout->start)), color,
                    arrowLength, arrowHalfWidth);
      ropeCost.drawMs += MillisecondsSince(drawStart);
      ++ropeCost.drawnCount;
      ++ropeCost.staticDrawnCount;
      ++visibleLinkIndex;
    };

    if (animatedLinkCount >= kMaxAnimatedFocusReferenceRopes) {
      drawStaticConnector();
      continue;
    }

    const auto updateStart = std::chrono::steady_clock::now();
    const Vector2d start = ToVector2d(layout->start);
    const Vector2d tip = ToVector2d(layout->tip);
    const double chordLength = start.distance(tip);
    const std::uint32_t ropeSeed = MixReferenceSeed(link, visibleLinkIndex);
    FocusReferenceRopeState& ropeState = focusReferenceRopes_[link];
    const bool lengthChanged =
        ropeState.initialized && std::abs(ropeState.chordLength - chordLength) >
                                     static_cast<double>(std::max(28.0f * uiScale_, 8.0f));
    if (!ropeState.initialized || ropeState.rope.empty() || lengthChanged) {
      ropeState.rope.resetCatenary(start, tip, ropeOptions);
      const double side = UnitFromSeed(ropeSeed ^ 0x7c15f3a9u) < 0.5f ? -1.0 : 1.0;
      ropeState.rope.applyBottomImpulse(Vector2d(side * ropeOptions.initialImpulsePx, 0.0), 0.12);
      ropeState.path = ropeState.rope.toPath(ropeOptions);
      ropeState.initialized = true;
    }

    const bool hoveredBeforeUpdate = canHover && ropeState.initialized &&
                                     isFocusReferenceRopeHit(ropeState.path, mousePos, hitWidth);
    ropeState.rope.update(start, tip, deltaTime, scrollDeltaY, ImGui::GetTime(), ropeSeed,
                          hoveredBeforeUpdate, ropeOptions);
    ropeState.path = ropeState.rope.toPath(ropeOptions);
    ropeState.hovered = canHover && isFocusReferenceRopeHit(ropeState.path, mousePos, hitWidth);
    ropeState.chordLength = chordLength;
    ropeState.lastFrameSeen = focusReferenceRopeFrame_;
    ropeCost.updateMs += MillisecondsSince(updateStart);
    ++animatedLinkCount;

    const std::optional<Box2d> routeBounds = ropeState.path.bounds();
    if (routeBounds.has_value() && !BoxesIntersect(*routeBounds, visibleTextBounds)) {
      ++ropeCost.culledCount;
      ++visibleLinkIndex;
      continue;
    }

    const auto drawStart = std::chrono::steady_clock::now();
    const ImU32 color = ropeState.hovered ? BrightenReferenceColor(layout->color) : layout->color;
    if (layout->hasSourceUnderline) {
      drawList->AddLine(layout->sourceUnderline.start, layout->sourceUnderline.end, color,
                        std::max(1.0f, uiScale_));
    }
    if (layout->hasSourceStyleChip) {
      renderFocusReferenceStyleSourceChip(drawList, layout->sourceStyleChip, color);
    }
    StrokeDonnerPath(drawList, ropeState.path, color,
                     ropeState.hovered ? hoverThickness : thickness);
    ropeState.arrowDirection = ropeState.rope.endTangent();
    DrawArrowhead(drawList, layout->tip, ToImVec2(ropeState.arrowDirection), color, arrowLength,
                  arrowHalfWidth);
    ropeCost.drawMs += MillisecondsSince(drawStart);
    ++ropeCost.drawnCount;
    if (ropeState.hovered && !hoveredAny) {
      hoveredAny = true;
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ++visibleLinkIndex;
  }
  drawList->PopClipRect();

  for (auto it = focusReferenceRopes_.begin(); it != focusReferenceRopes_.end();) {
    if (it->second.lastFrameSeen != focusReferenceRopeFrame_) {
      it = focusReferenceRopes_.erase(it);
    } else {
      ++it;
    }
  }
  ropeCost.activeStateCount = static_cast<int>(focusReferenceRopes_.size());
  lastSourceRopeCost_ = ropeCost;
}

const TextEditor::SourceStyleDecoration* TextEditor::sourceStyleDecorationAtByteOffset(
    std::size_t byteOffset, bool ineffectiveOnly) const {
  for (const SourceStyleDecoration& decoration : sourceStyleDecorations_) {
    if (ineffectiveOnly && !decoration.ineffective) {
      continue;
    }
    if (byteOffset >= decoration.range.start && byteOffset < decoration.range.end) {
      return &decoration;
    }
  }

  return nullptr;
}

bool TextEditor::isByteOffsetInIneffectiveStyleDecoration(std::size_t byteOffset) const {
  return sourceStyleDecorationAtByteOffset(byteOffset, /*ineffectiveOnly=*/true) != nullptr;
}

void TextEditor::renderSourceStyleDecorationStrikethroughs(int lineNo, int startColumn,
                                                           int endColumn, ImDrawList* drawList) {
  if (sourceStyleDecorations_.empty() || drawList == nullptr || endColumn <= startColumn) {
    return;
  }

  const ImU32 color = SourceStyleStrikethroughColor();
  const float thickness = std::max(1.0f, uiScale_);
  constexpr float kStrikeHeightFraction = 0.5f;
  constexpr float kStrikeOffsetY = -2.0f;

  for (const SourceStyleDecoration& decoration : sourceStyleDecorations_) {
    if (!decoration.ineffective) {
      continue;
    }

    const Coordinates rangeStart = getCoordinatesAtByteOffset(decoration.range.start);
    const Coordinates rangeEnd = getCoordinatesAtByteOffset(decoration.range.end);
    if (rangeEnd.line < lineNo || rangeStart.line > lineNo) {
      continue;
    }

    int strikeStartColumn = startColumn;
    int strikeEndColumn = endColumn;
    if (rangeStart.line == lineNo) {
      strikeStartColumn = std::max(strikeStartColumn, rangeStart.column);
    }
    if (rangeEnd.line == lineNo) {
      strikeEndColumn = std::min(strikeEndColumn, rangeEnd.column);
    }
    if (strikeEndColumn <= strikeStartColumn) {
      continue;
    }

    const ImVec2 start = coordinatesToScreenPos(Coordinates(lineNo, strikeStartColumn));
    const ImVec2 end = coordinatesToScreenPos(Coordinates(lineNo, strikeEndColumn));
    const float y =
        std::floor(start.y + ImGui::GetFontSize() * kStrikeHeightFraction + kStrikeOffsetY) + 0.5f;
    drawList->AddLine(ImVec2(start.x, y), ImVec2(end.x, y), color, thickness);
  }
}

void TextEditor::renderFocusReferenceStyleSourceChip(ImDrawList* drawList,
                                                     const SourceStyleChipBounds& bounds,
                                                     ImU32 color) const {
  if (drawList == nullptr) {
    return;
  }

  const float paddingX = std::max(4.0f * uiScale_, 4.0f);
  const float paddingY = std::max(1.0f * uiScale_, 1.0f);
  const float rounding = 4.0f * uiScale_;
  const ImVec4 colorVec = ImGui::ColorConvertU32ToFloat4(color);
  const ImU32 fillColor =
      ImGui::ColorConvertFloat4ToU32(ImVec4(colorVec.x, colorVec.y, colorVec.z, 0.18f));
  const ImU32 borderColor =
      ImGui::ColorConvertFloat4ToU32(ImVec4(colorVec.x, colorVec.y, colorVec.z, 0.72f));

  drawList->AddRectFilled(bounds.min, bounds.max, fillColor, rounding);
  drawList->AddRect(bounds.min, bounds.max, borderColor, rounding, ImDrawFlags_None,
                    std::max(1.0f, uiScale_));

  // QA-F22: draw the chip mark as a Donner-rendered SVG icon tinted with the
  // rope color. The embedded fonts lack the "✦" glyph, so the text draw below
  // is only a headless-test fallback (no icon-texture provider available).
  const ImVec2 iconOrigin(bounds.min.x + paddingX, bounds.min.y + paddingY);
  const ImVec2 glyphSize = StyleSourceChipTextSize();
  const float iconSide = glyphSize.x < glyphSize.y ? glyphSize.x : glyphSize.y;
  const ImVec2 iconMax(iconOrigin.x + iconSide, iconOrigin.y + iconSide);
  if (!DrawTextChipIcon(drawList, TextChipIcon::StyleSource, iconOrigin, iconMax, borderColor,
                        chipIconTextureProvider_)) {
    drawList->AddText(iconOrigin, borderColor, kStyleSourceChipIcon);
  }
}

void TextEditor::renderSourceStyleDecorationChips(ImDrawList* drawList) {
  sourceStyleChipHitRects_.clear();
  if (sourceStyleDecorations_.empty()) {
    return;
  }

  const float paddingX = std::max(3.0f * uiScale_, 3.0f);
  const float paddingY = std::max(1.0f * uiScale_, 1.0f);
  const float rounding = 4.0f * uiScale_;
  const float markerGap = std::max(2.0f * uiScale_, 2.0f);
  const float markerFontSize = ImGui::GetFontSize() * 1.45f;
  const float markerHitPadding = std::max(2.0f * uiScale_, 2.0f);
  constexpr const char* kOverflowMarker = "✱";

  for (const SourceStyleDecoration& decoration : sourceStyleDecorations_) {
    if (!decoration.showChip) {
      continue;
    }

    std::optional<SourceStyleChipBounds> bounds = sourceStyleChipBoundsForDecoration(decoration);
    if (!bounds.has_value()) {
      continue;
    }

    const std::string label = std::to_string(decoration.chipCount);
    const ImVec2 min = bounds->min;
    const ImVec2 max = bounds->max;
    const ImVec2 chipSize(max.x - min.x, max.y - min.y);

    drawList->AddRectFilled(min, max, SourceStyleChipFillColor(), rounding);
    drawList->AddRect(min, max, SourceStyleChipBorderColor(), rounding, ImDrawFlags_None,
                      std::max(1.0f, uiScale_));
    drawList->AddText(ImVec2(min.x + paddingX, min.y + paddingY), SourceStyleChipTextColor(),
                      label.c_str());

    sourceStyleChipHitRects_.push_back(SourceStyleChipHitRect{
        .id = decoration.id,
        .min = min,
        .max = max,
        .tooltip = decoration.chipTooltip.empty() ? decoration.tooltip : decoration.chipTooltip,
    });

    if (decoration.showOverflowMarker) {
      const ImVec2 markerSize =
          ImGui::GetFont()->CalcTextSizeA(markerFontSize, FLT_MAX, -1.0f, kOverflowMarker);
      const ImVec2 markerMin(max.x + markerGap, min.y + (chipSize.y - markerSize.y) * 0.5f);
      const ImVec2 markerMax(markerMin.x + markerSize.x, markerMin.y + markerSize.y);
      // QA-F22: SVG icon for the overflow marker (the "✱" glyph is absent from
      // the embedded fonts); text draw is a headless-test fallback only.
      const float markerSide = markerSize.x < markerSize.y ? markerSize.x : markerSize.y;
      const ImVec2 markerIconMax(markerMin.x + markerSide, markerMin.y + markerSide);
      if (!DrawTextChipIcon(drawList, TextChipIcon::Overflow, markerMin, markerIconMax,
                            SourceStyleChipFillColor(), chipIconTextureProvider_)) {
        drawList->AddText(ImGui::GetFont(), markerFontSize, markerMin, SourceStyleChipFillColor(),
                          kOverflowMarker);
      }

      sourceStyleChipHitRects_.push_back(SourceStyleChipHitRect{
          .id = decoration.id,
          .min = ImVec2(markerMin.x - markerHitPadding, markerMin.y - markerHitPadding),
          .max = ImVec2(markerMax.x + markerHitPadding, markerMax.y + markerHitPadding),
          .tooltip = decoration.overflowTooltip,
          .clickEnabled = false,
      });
    }
  }
}

void TextEditor::hitTestSourceStyleDecorationChips() {
  if (sourceStyleChipHitRects_.empty()) {
    return;
  }

  for (const SourceStyleChipHitRect& hitRect : sourceStyleChipHitRects_) {
    if (!ImGui::IsMouseHoveringRect(hitRect.min, hitRect.max)) {
      continue;
    }

    if (!hitRect.tooltip.empty()) {
      ImGui::SetTooltip("%s", hitRect.tooltip.c_str());
    }
    if (hitRect.clickEnabled && ImGui::IsMouseClicked(0)) {
      clickedSourceStyleChipId_ = hitRect.id;
    }
    return;
  }
}

void TextEditor::renderSourceStyleDecorationTooltip() {
  if (!hoveredTextPosition_.has_value()) {
    return;
  }
  for (const SourceStyleChipHitRect& hitRect : sourceStyleChipHitRects_) {
    if (ImGui::IsMouseHoveringRect(hitRect.min, hitRect.max)) {
      return;
    }
  }

  const std::size_t byteOffset = getByteOffsetAtCoordinates(*hoveredTextPosition_);
  const SourceStyleDecoration* decoration =
      sourceStyleDecorationAtByteOffset(byteOffset, /*ineffectiveOnly=*/true);
  if (decoration != nullptr && !decoration->tooltip.empty()) {
    ImGui::SetTooltip("%s", decoration->tooltip.c_str());
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

void TextEditor::renderSelection(const VisualLine& visualLine, const Line& line,
                                 const ImVec2& lineStart, ImDrawList* drawList) {
  if (!hasSelection()) {
    return;
  }

  (void)line;
  const auto& start = state_.selectionStart;
  const auto& end = state_.selectionEnd;
  if (end.line < visualLine.lineNo || start.line > visualLine.lineNo) {
    return;
  }

  int selStartColumn = visualLine.startColumn;
  int selEndColumn = visualLine.endColumn;
  if (start.line == visualLine.lineNo) {
    selStartColumn = std::max(selStartColumn, start.column);
  }
  if (end.line == visualLine.lineNo) {
    selEndColumn = std::min(selEndColumn, end.column);
  }
  if (selEndColumn <= selStartColumn) {
    return;
  }

  const float textStartX =
      lineStart.x + textStart_ + static_cast<float>(visualLine.indentColumns) * charAdvance_.x;
  const ImVec2 selStartPos(textStartX + (selStartColumn - visualLine.startColumn) * charAdvance_.x,
                           lineStart.y);
  const ImVec2 selEndPos(textStartX + (selEndColumn - visualLine.startColumn) * charAdvance_.x,
                         lineStart.y + charAdvance_.y);
  drawList->AddRectFilled(selStartPos, selEndPos,
                          palette_[static_cast<int>(ColorIndex::Selection)]);
}

void TextEditor::setSelection(const Coordinates& start, const Coordinates& end,
                              SelectionMode mode) {
  core_.setSelection(start, end, mode);
}

void TextEditor::setFocusPartition(const FocusPartition& partition) {
  if (!FocusPartitionsEqual(focusPartition_, partition)) {
    expandedFocusHiddenRanges_.clear();
    focusReferenceRopes_.clear();
  }

  focusPartition_ = partition;
  focusPartitionActive_ = !partition.empty();
}

bool TextEditor::setHoverSourceRanges(std::vector<SourceByteRange> ranges) {
  const std::size_t bufferSize = getText().size();
  for (SourceByteRange& range : ranges) {
    range.start = std::min(range.start, bufferSize);
    range.end = std::min(range.end, bufferSize);
  }
  std::erase_if(ranges, [](const SourceByteRange& range) { return range.end <= range.start; });
  std::sort(ranges.begin(), ranges.end(),
            [](const SourceByteRange& lhs, const SourceByteRange& rhs) {
              return lhs.start != rhs.start ? lhs.start < rhs.start : lhs.end < rhs.end;
            });
  ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());

  if (hoverSourceRanges_ == ranges) {
    return false;
  }

  hoverSourceRanges_ = std::move(ranges);
  return true;
}

bool TextEditor::setSourceStyleDecorations(std::vector<SourceStyleDecoration> decorations) {
  const std::size_t bufferSize = getText().size();
  for (SourceStyleDecoration& decoration : decorations) {
    decoration.range.start = std::min(decoration.range.start, bufferSize);
    decoration.range.end = std::min(decoration.range.end, bufferSize);
    decoration.chipRange.start = std::min(decoration.chipRange.start, bufferSize);
    decoration.chipRange.end = std::min(decoration.chipRange.end, bufferSize);
    if (decoration.chipRange.end <= decoration.chipRange.start) {
      decoration.chipRange = decoration.range;
    }
    decoration.chipCount = std::max(0, decoration.chipCount);
  }

  std::erase_if(decorations, [](const SourceStyleDecoration& decoration) {
    return decoration.range.end <= decoration.range.start;
  });
  std::sort(decorations.begin(), decorations.end(),
            [](const SourceStyleDecoration& lhs, const SourceStyleDecoration& rhs) {
              if (lhs.range.start != rhs.range.start) {
                return lhs.range.start < rhs.range.start;
              }
              if (lhs.range.end != rhs.range.end) {
                return lhs.range.end < rhs.range.end;
              }
              return lhs.id < rhs.id;
            });
  decorations.erase(std::unique(decorations.begin(), decorations.end()), decorations.end());

  if (sourceStyleDecorations_ == decorations) {
    return false;
  }

  sourceStyleDecorations_ = std::move(decorations);
  sourceStyleChipHitRects_.clear();
  clickedSourceStyleChipId_.reset();
  return true;
}

std::optional<std::size_t> TextEditor::takeClickedSourceStyleChipId() {
  std::optional<std::size_t> clicked = clickedSourceStyleChipId_;
  clickedSourceStyleChipId_.reset();
  return clicked;
}

bool TextEditor::isPositionInsideFocusRange(const Coordinates& position) const {
  if (!focusPartitionActive_) {
    return false;
  }

  const int lineNo = sanitizeCoordinates(position).line;
  return LineRangeContains(focusPartition_.fullColor, lineNo) ||
         LineRangeContains(focusPartition_.referenceColor, lineNo) ||
         LineRangeContains(focusPartition_.dimmed, lineNo);
}

void TextEditor::clearFocusPartition() {
  focusPartition_ = FocusPartition{};
  focusPartitionActive_ = false;
  expandedFocusHiddenRanges_.clear();
  focusReferenceRopes_.clear();
}

void TextEditor::flashSourceRange(SourceByteRange byteRange) {
  flashDecorations_.flash(byteRange, FlashDecorations::Clock::now(), getText().size());
}

std::optional<float> TextEditor::nextFlashWakeSeconds() const {
  return flashDecorations_.nextWakeSeconds(FlashDecorations::Clock::now());
}

std::optional<float> TextEditor::nextRopeAnimationWakeSeconds() const {
  if (!focusPartitionActive_) {
    return std::nullopt;
  }

  for (const auto& ropeEntry : focusReferenceRopes_) {
    const FocusReferenceRopeState& state = ropeEntry.second;
    if (!state.hovered && state.rope.needsAnimation()) {
      return kFocusReferenceRopeWakeSeconds;
    }
  }

  return std::nullopt;
}

void TextEditor::tickSourceFlashes() {
  flashDecorations_.tick(FlashDecorations::Clock::now());
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

ImU32 dimmedTextColor(ImU32 color) {
  ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
  const float gray = (value.x + value.y + value.z) / 3.0f;
  value.x = gray * 0.75f;
  value.y = gray * 0.75f;
  value.z = gray * 0.75f;
  value.w *= 0.62f;
  return ImGui::ColorConvertFloat4ToU32(value);
}

ImU32 referenceTextColor(ImU32 color) {
  ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
  value.x *= 0.72f;
  value.y *= 0.72f;
  value.z *= 0.72f;
  value.w *= 0.82f;
  return ImGui::ColorConvertFloat4ToU32(value);
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

void TextEditor::renderText(const VisualLine& visualLine, const Line& line, const ImVec2& pos,
                            ImDrawList* drawList) {
  const int startIndex =
      text_.getCharacterIndex(Coordinates(visualLine.lineNo, visualLine.startColumn));
  const int endIndex =
      text_.getCharacterIndex(Coordinates(visualLine.lineNo, visualLine.endColumn));
  const bool referenceColored = isLineReferenceColoredByFocus(visualLine.lineNo);
  const bool dimmed = isLineDimmedByFocus(visualLine.lineNo);

  ImU32 prevColor = palette_[static_cast<int>(ColorIndex::Default)];
  ImVec2 offset;
  lineBuffer_.clear();

  const float spaceSize =
      ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;

  for (int glyphIndex = startIndex;
       glyphIndex < endIndex && glyphIndex < static_cast<int>(line.size());) {
    const Glyph& glyph = line[glyphIndex];
    ImU32 color = getGlyphColor(glyph);
    if (referenceColored) {
      color = referenceTextColor(color);
    }
    if (dimmed) {
      color = dimmedTextColor(color);
    }

    if ((color != prevColor || glyph.character == '\t' || glyph.character == ' ') &&
        !lineBuffer_.empty()) {
      renderBufferedText(lineBuffer_, offset, pos, drawList, prevColor);
      lineBuffer_.clear();
    }
    prevColor = color;

    if (glyph.character == '\t') {
      const float oldX = offset.x;
      offset.x =
          (1.0f + std::floor((1.0f + offset.x) / (static_cast<float>(tabSize_) * spaceSize))) *
          (static_cast<float>(tabSize_) * spaceSize);
      const float arrowX = pos.x + oldX + (offset.x - oldX) * 0.5f;
      const float arrowY = pos.y + offset.y + ImGui::GetFontSize() * 0.2f;
      ImU32 arrowColor = 0x99906060;
      if (referenceColored) {
        arrowColor = referenceTextColor(arrowColor);
      }
      if (dimmed) {
        arrowColor = dimmedTextColor(arrowColor);
      }
      drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(arrowX, arrowY), arrowColor,
                        "→");
      ++glyphIndex;
    } else if (glyph.character == ' ') {
      const float centerX = pos.x + offset.x + spaceSize * 0.5f;
      const float centerY = pos.y + offset.y + ImGui::GetFontSize() * 0.5f;
      ImU32 dotColor = 0x99805050;
      if (referenceColored) {
        dotColor = referenceTextColor(dotColor);
      }
      if (dimmed) {
        dotColor = dimmedTextColor(dotColor);
      }
      drawList->AddCircleFilled(ImVec2(centerX, centerY), 1.0f, dotColor, 4);
      offset.x += spaceSize;
      ++glyphIndex;
    } else {
      char buf[7];
      int sequenceLength = Utf8::SequenceLength(glyph.character);
      if (sequenceLength <= 0) {
        sequenceLength = 1;
      }
      int charCount = 0;
      while (charCount < sequenceLength && glyphIndex < endIndex &&
             glyphIndex < static_cast<int>(line.size())) {
        buf[charCount++] = line[glyphIndex++].character;
      }
      buf[charCount] = '\0';
      lineBuffer_ += buf;
    }
  }

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
  float x = pos.x + getTextDistanceToLineStart(state_.cursorPosition);
  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    const int visualIndex = visualLineIndexForCoordinates(state_.cursorPosition);
    const VisualLine& visualLine = visualLines_[visualIndex];
    x = pos.x + static_cast<float>(visualLine.indentColumns) * charAdvance_.x +
        std::max(0, state_.cursorPosition.column - visualLine.startColumn) * charAdvance_.x;
  }

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

std::string popupWindowName(std::string_view label) {
  return std::string("TextEditor") + std::string(label) + "##" +
         std::to_string(ImGui::GetID(label.data(), label.data() + label.size()));
}

ImFont* popPushedEditorFont() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (context == nullptr || context->FontStack.Size == 0) {
    return nullptr;
  }

  ImFont* font = ImGui::GetFont();
  ImGui::PopFont();
  return font;
}

void pushEditorFontIfNeeded(ImFont* font) {
  if (font != nullptr) {
    ImGui::PushFont(font);
  }
}

void renderFunctionTooltip(const std::string& declaration, const ImVec2& position, float uiScale) {
  const float tooltipWidth = 350.0f * uiScale;
  const float tooltipHeight = 50.0f * uiScale;

  ImFont* font = popPushedEditorFont();

  ImGui::SetNextWindowPos(position, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(tooltipWidth, tooltipHeight), ImGuiCond_Always);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoInputs;
  const std::string windowName = popupWindowName("FunctionTooltip");
  if (ImGui::Begin(windowName.c_str(), nullptr, flags)) {
    ImGui::TextWrapped("%s", declaration.c_str());
  }
  ImGui::End();

  pushEditorFontIfNeeded(font);
}

void renderAutocomplete(const std::vector<std::pair<RcString, RcString>>& suggestions,
                        int selectedIndex, const ImVec2& position, float uiScale) {
  const float popupWidth = 150.0f * uiScale;
  const float popupHeight = 100.0f * uiScale;

  ImGui::SetNextWindowPos(position, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(popupWidth, popupHeight), ImGuiCond_Always);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
  const std::string windowName = popupWindowName("Autocomplete");
  if (!ImGui::Begin(windowName.c_str(), nullptr, flags)) {
    ImGui::End();
    return;
  }

  for (int i = 0; i < suggestions.size(); i++) {
    const bool isSelected = (i == selectedIndex);
    // TODO: Make a better way to have a null-terminated RcString
    const std::string suggestionStr = suggestions[i].first.str();
    if (ImGui::Selectable(suggestionStr.c_str(), isSelected) && isSelected) {
      ImGui::SetScrollHereY();
    }
  }

  ImGui::End();
}

}  // namespace

void TextEditor::renderExtraUI(ImDrawList* drawList, const ImVec2& basePos, float scrollX,
                               float scrollY, float longest, const ImVec2& contentSize) {
  // Function declaration tooltip
  if (functionDeclarationTooltip_) {
    const ImVec2 tooltipPos = coordinatesToScreenPos(functionDeclarationCoord_);
    const ImVec2 adjustedPos(tooltipPos.x + ImGui::GetScrollX(), tooltipPos.y + charAdvance_.y);

    renderFunctionTooltip(functionDeclaration_, adjustedPos, uiScale_);

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

    ImFont* font = popPushedEditorFont();

    renderAutocomplete(autocompleteSuggestions_, autocompleteIndex_, acPos, uiScale_);

    pushEditorFontIfNeeded(font);

    ImGui::SetWindowFocus();
    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
      autocompleteOpened_ = false;
      autocompleteObject_.clear();
    }
  }

  // Add padding at the bottom of the editor
  const int visualLineCount = (wordWrapEnabled_ || focusPartitionActive_)
                                  ? static_cast<int>(visualLines_.size())
                                  : text_.getTotalLines() - foldedLines_;
  const float dummyWidth = horizontalScroll_ ? longest + 100.0f * uiScale_ : 0.0f;
  ImGui::Dummy(ImVec2(dummyWidth, visualLineCount * charAdvance_.y));

  // Handle cursor visibility
  if (scrollToCursor_) {
    if (scrollSelectionIntoView_ && hasSelection()) {
      scrollCoordinatesRangeIntoView(state_.selectionStart, visibleSelectionEndCoordinates());
      scrollSelectionIntoView_ = false;
    } else {
      ensureCursorVisible();
    }
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
  autocompleteReplacementActive_ = false;

  if (autocompleteProvider_) {
    const std::string source = getText();
    const std::size_t cursorOffset = text_.getByteOffset(getCursorPosition());
    std::optional<AutocompleteResponse> response =
        autocompleteProvider_(AutocompleteRequest{source, cursorOffset});
    if (response.has_value()) {
      autocompleteSuggestions_.clear();
      autocompleteIndex_ = 0;
      autocompleteSwitched_ = false;

      for (const AutocompleteSuggestion& suggestion : response->suggestions) {
        autocompleteSuggestions_.emplace_back(suggestion.displayText, suggestion.insertText);
      }

      if (!autocompleteSuggestions_.empty()) {
        autocompleteOpened_ = true;
        autocompleteReplacementActive_ = true;
        autocompleteReplacementStartOffset_ = std::min(response->replaceStartOffset, source.size());
        autocompleteReplacementEndOffset_ = std::min(response->replaceEndOffset, source.size());
        autocompletePosition_ =
            text_.getCoordinatesAtByteOffset(autocompleteReplacementStartOffset_);

        if (keepAutocompleteOpen != nullptr) {
          *keepAutocompleteOpen = true;
        }
      } else {
        autocompleteOpened_ = false;
      }

      return;
    }
  }

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
  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    const int visualIndex = visualLineIndexForCoordinates(position);
    const VisualLine& visualLine = visualLines_[visualIndex];
    const int column = std::max(visualLine.startColumn, position.column);
    return ImVec2(
        origin.x + textStart_ +
            (visualLine.indentColumns + column - visualLine.startColumn) * charAdvance_.x -
            lastScrollX_,
        origin.y + visualIndex * charAdvance_.y);
  }

  return ImVec2(origin.x + (textStart_ + position.column) * charAdvance_.x - lastScrollX_,
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
      // `SetKeyboardFocusHere()` dereferences the current window, which only exists inside an
      // active frame. In production processFind() runs from the find-dialog render path (always
      // mid-frame), but guard it so the find/select logic above is safe to invoke outside a frame
      // (tests, or any non-render caller) instead of dereferencing a null window and crashing.
      const ImGuiContext* g = ImGui::GetCurrentContext();
      if (g != nullptr && g->WithinFrameScope) {
        ImGui::SetKeyboardFocusHere(-1);
      }
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
    insertText(replaceWord);
    Coordinates replacementEnd = curPos;
    replacementEnd.column += static_cast<int>(replaceWord.size());
    setCursorPosition(replacementEnd);
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
  cursorPositionChangedByMouse_ = false;
  hoveredTextPosition_.reset();

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

  calculateCharacterAdvance();
  updateTextStart();
  uiCursorPos_ = ImGui::GetCursorScreenPos();
  visualLayoutMaxColumns_ = 0;
  rebuildVisualLines(ImGui::GetWindowContentRegionMax());
  updateHoveredTextPosition();

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
    if (sourceFocusModeContextMenuVisible_) {
      ImGui::Separator();
      if (ImGui::MenuItem("Source Focus Mode", nullptr, sourceFocusModeContextMenuChecked_)) {
        sourceFocusModeContextMenuToggleRequested_ = true;
      }
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

void TextEditor::setText(std::string_view text, bool preserveScroll) {
  core_.setText(text, preserveScroll);
}

void TextEditor::applyExternalSourceEdit(std::size_t offset, std::size_t removedLength,
                                         std::string_view replacement) {
  const std::size_t currentSize = getText().size();
  const std::size_t newSize = currentSize - removedLength + replacement.size();
  flashDecorations_.applySourceEdit(offset, removedLength, replacement.size(), newSize);
  core_.applyExternalSourceEdit(offset, removedLength, replacement);
  if (!replacement.empty()) {
    flashSourceRange(SourceByteRange{.start = offset, .end = offset + replacement.size()});
  }
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

void TextEditor::clearSelection() {
  // Collapse the selection to the caret so the cursor stays put. setSelection() with an empty
  // range is the same canonical path the editor already uses to drop a selection (e.g. after a
  // click), so the selection-dependent UI (highlight, copy/cut enablement) updates consistently.
  const Coordinates cursor = getCursorPosition();
  setSelection(cursor, cursor);
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

  copy();
  insertText("");
}

void TextEditor::paste() {
  const char* clipText = ImGui::GetClipboardText();
  if (clipText == nullptr || clipText[0] == '\0') {
    return;
  }

  insertText(clipText, autoIndentOnPaste_);
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

  const float scrollY = ImGui::GetScrollY();
  const float height = visibleTextRegionHeight();

  const auto pos = getActualCursorCoordinates();

  if ((wordWrapEnabled_ || focusPartitionActive_) && !visualLines_.empty()) {
    const int visualIndex = visualLineIndexForCoordinates(pos);
    const int firstVisible = static_cast<int>(std::floor(scrollY / charAdvance_.y));
    const int lastVisible =
        static_cast<int>(std::floor((scrollY + height - charAdvance_.y) / charAdvance_.y));
    if (visualIndex < firstVisible) {
      ImGui::SetScrollY(std::max(0.0f, visualIndex * charAdvance_.y));
    } else if (visualIndex > lastVisible) {
      ImGui::SetScrollY(std::max(0.0f, (visualIndex + 1) * charAdvance_.y - height));
    }
    return;
  }

  const auto top = static_cast<int>(std::floor(scrollY / charAdvance_.y));
  const auto bottom =
      static_cast<int>(std::floor((scrollY + height - charAdvance_.y) / charAdvance_.y));

  if (pos.line < top) {
    ImGui::SetScrollY(std::max(0.0f, pos.line * charAdvance_.y));
  }
  if (pos.line > bottom) {
    ImGui::SetScrollY(std::max(0.0f, (pos.line + 1) * charAdvance_.y - height));
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

    // SVG element names - used as keywords so tag names like <rect>, <circle>
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

    // Known SVG/CSS attribute names - highlighted as KnownIdentifier.
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

      // Fall through for whitespace and anything else - the caller advances
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
