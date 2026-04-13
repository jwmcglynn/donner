#pragma once
/// @file

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditorCore.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

namespace donner::editor {

// Forward declaration.
class TextEditor;

// `EditorState`, `UndoRecord`, `UndoState`, `SelectionMode`, `Identifier`,
// `Identifiers`, `Keywords`, `ErrorMarkers`, `Palette`, and
// `LanguageDefinition` now live in `donner/editor/TextEditorCore.h` as part
// of the headless-core refactor. The `using` aliases inside the `TextEditor`
// class body below preserve every existing qualified name
// (`TextEditor::Palette`, `TextEditor::LanguageDefinition`, etc.) so
// callers don't change.

/// Enum class of modifier keys that can be combined using bitwise OR.
/// These modifiers can be applied to shortcuts for actions within the editor.
enum class ShortcutModifier : uint8_t {
  None = 0,       ///< No modifier
  Alt = 1 << 0,   ///< Alt key modifier
  Ctrl = 1 << 1,  ///< Ctrl key modifier
  Shift = 1 << 2  ///< Shift key modifier
};

/// Combine two ShortcutModifier flags using bitwise OR.
inline ShortcutModifier operator|(ShortcutModifier lhs, ShortcutModifier rhs) {
  return static_cast<ShortcutModifier>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

/// Check if a given ShortcutModifier set includes a specific modifier flag.
inline bool HasModifier(ShortcutModifier mods, ShortcutModifier flag) {
  return (static_cast<uint8_t>(mods) & static_cast<uint8_t>(flag)) != 0;
}

/**
 * Represents a keyboard shortcut for editor actions.
 * A shortcut consists of up to two keys and optional modifier keys.
 */
struct Shortcut {
  ImGuiKey key1 = ImGuiKey_None;                        ///< Primary key (required)
  ImGuiKey key2 = ImGuiKey_None;                        ///< Secondary key (optional)
  ShortcutModifier modifiers = ShortcutModifier::None;  ///< Combined modifiers

  /// Default constructor
  Shortcut() = default;

  /**
   * Constructs a shortcut with a single key and optional modifiers.
   * @param k Primary key.
   * @param m Modifier flags combined using bitwise OR.
   */
  Shortcut(ImGuiKey k, ShortcutModifier m = ShortcutModifier::None)
      : key1(k), key2(ImGuiKey_None), modifiers(m) {}

  /**
   * Constructs a shortcut with two keys and optional modifiers.
   * @param k1 Primary key.
   * @param k2 Secondary key.
   * @param m Modifier flags combined using bitwise OR.
   */
  Shortcut(ImGuiKey k1, ImGuiKey k2, ShortcutModifier m = ShortcutModifier::None)
      : key1(k1), key2(k2), modifiers(m) {}

  /**
   * Checks if this shortcut matches the current input state.
   * Compares the set keys and modifiers against the keys currently pressed.
   *
   * @param io Reference to ImGuiIO containing the current keyboard state.
   * @return true if the shortcut matches, false otherwise.
   */
  bool matches(const ImGuiIO& io) const {
    if (key1 == ImGuiKey_None) return false;

    bool keyPressed = ImGui::IsKeyPressed(key1) ||
                      (key1 == ImGuiKey_Enter && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    if (key2 != ImGuiKey_None) {
      keyPressed = keyPressed && ImGui::IsKeyPressed(key2);
    }

    const bool wantAlt = HasModifier(modifiers, ShortcutModifier::Alt);
    const bool wantCtrl = HasModifier(modifiers, ShortcutModifier::Ctrl);
    const bool wantShift = HasModifier(modifiers, ShortcutModifier::Shift);

    return keyPressed && io.KeyAlt == wantAlt && io.KeyCtrl == wantCtrl && io.KeyShift == wantShift;
  }
};

/**
 * A text editor widget for Dear ImGui that supports syntax highlighting, undo &
 * redo, and more.
 */
class TextEditor {
  // Test-only access to private members (enterCharacter, backspace, etc.)
  // so headless unit tests can exercise the real user-facing editing
  // paths without going through ImGui input capture. Will be removed
  // once the TextEditor refactor (see text_editor_refactor.md) migrates
  // the tests onto `TextEditorCore` directly (commit C3).
  friend class TextEditorTests;
  friend class TextEditorTests_UndoReversesInsertion_Test;
  friend class TextEditorTests_RedoRestoresAfterUndo_Test;
  friend class TextEditorTests_MultipleUndoStepsBackthrough_Test;
  friend class TextEditorTests_RedoIsClearedAfterNewEdit_Test;
  friend class TextEditorTests_CanUndoReturnsTrueAfterEdit_Test;

public:
  /**
   * Shortcut IDs for various editor actions. Each can be bound to a keyboard
   * combination.
   */
  enum class ShortcutId {
    Undo,
    Redo,
    MoveUp,
    SelectUp,
    MoveDown,
    SelectDown,
    MoveLeft,
    SelectLeft,
    MoveWordLeft,
    SelectWordLeft,
    MoveRight,
    SelectRight,
    MoveWordRight,
    SelectWordRight,
    MoveUpBlock,
    SelectUpBlock,
    MoveDownBlock,
    SelectDownBlock,
    MoveTop,
    SelectTop,
    MoveBottom,
    SelectBottom,
    MoveStartLine,
    SelectStartLine,
    MoveEndLine,
    SelectEndLine,
    ForwardDelete,
    ForwardDeleteWord,
    DeleteRight,
    BackwardDelete,
    BackwardDeleteWord,
    DeleteLeft,
    Copy,
    Paste,
    Cut,
    SelectAll,
    AutocompleteOpen,
    AutocompleteSelect,
    AutocompleteSelectActive,
    AutocompleteUp,
    AutocompleteDown,
    NewLine,
    Indent,
    Unindent,
    Find,
    Replace,
    FindNext,
    DuplicateLine,
    CommentLines,
    UncommentLines,
    Count
  };

  // Type aliases re-exporting the canonical definitions from
  // `TextEditorCore`. These keep the existing `TextEditor::Palette`,
  // `TextEditor::LanguageDefinition`, etc. names working so call sites do
  // not change during the refactor.
  using SelectionMode = ::donner::editor::SelectionMode;
  using Identifier = ::donner::editor::Identifier;
  using Identifiers = ::donner::editor::Identifiers;
  using Keywords = ::donner::editor::Keywords;
  using ErrorMarkers = ::donner::editor::ErrorMarkers;
  using Palette = ::donner::editor::Palette;
  using LanguageDefinition = ::donner::editor::LanguageDefinition;
  using String = RcString;

  // Constants
  static constexpr int kLineNumberSpace = 20;  //!< Width of line number margin in pixels

  // Construction/Destruction
  TextEditor();
  ~TextEditor();

  // Delete copy/move operations since this class manages resources
  TextEditor(const TextEditor&) = delete;
  TextEditor& operator=(const TextEditor&) = delete;
  TextEditor(TextEditor&&) = delete;
  TextEditor& operator=(TextEditor&&) = delete;

  // Configuration
  void setLanguageDefinition(const LanguageDefinition& langDef);
  void setPalette(const Palette& value);
  void setErrorMarkers(const ErrorMarkers& markers) { core_.setErrorMarkers(markers); }

  // Search and replace

  /**
   * Process find operation to locate text in editor.
   * Updates cursor position and selection to found text.
   *
   * @param findWord Text to search for
   * @param findNext Whether this is a "find next" operation
   */
  void processFind(const std::string& findWord, bool findNext);

  /**
   * Process replace operation to replace text in editor.
   * Can replace single occurrence or all occurrences.
   *
   * @param findWord Text to search for
   * @param replaceWord Text to replace with
   * @param replaceAll Whether to replace all occurrences
   */
  void processReplace(const std::string& findWord, const std::string& replaceWord,
                      bool replaceAll = false);

  // Core functionality
  void render(std::string_view title, const ImVec2& size = ImVec2(), bool showBorder = false);

  /**
   * Replace the entire buffer with new text.
   *
   * @param text The new text to load into the buffer.
   */
  void setText(std::string_view text);

  /// True if the editor needs to be re-rendered.
  bool needsRerender() const { return scrollToTop_; }

  /**
   * Get all text in the editor.
   * @return String containing entire editor contents
   */
  std::string getText() const;

  /**
   * Get currently selected text.
   * @return String containing selected text, or empty if no selection
   */
  std::string getSelectedText() const;

  // State queries
  bool isFocused() const { return focused_; }
  bool isTextChanged() const { return core_.isTextChanged(); }
  bool isCursorPositionChanged() const { return cursorPositionChanged_; }
  void resetTextChanged() { core_.resetTextChanged(); }

  // Accessors
  const LanguageDefinition& getLanguageDefinition() const { return core_.getLanguageDefinition(); }
  const Palette& getPalette() const { return core_.getPalette(); }

  /**
   * Returns true if syntax highlighting is enabled.
   */
  bool isColorizerEnabled() const { return core_.isColorizerEnabled(); }

  /**
   * Enable or disable syntax highlighting.
   *
   * @param enabled Whether to enable the colorizer
   */
  void setColorizerEnabled(bool enabled);

  // Cursor position

  /**
   * Get the cursor position adjusted for tab characters.
   * Returns cursor position with the column adjusted to account for tab size.
   *
   * @return Adjusted cursor coordinates
   */
  Coordinates getCorrectCursorPosition();

  /**
   * Get the current cursor position in the editor.
   *
   * @return Current cursor coordinates
   */
  Coordinates getCursorPosition() const { return getActualCursorCoordinates(); }

  /**
   * Set the cursor position and ensure it's visible.
   * Updates cursor position and scrolls view if needed.
   *
   * @param position New cursor coordinates
   */
  void setCursorPosition(const Coordinates& position);

  // Input handling
  void setHandleMouseInputs(bool value) { handleMouseInputs_ = value; }
  bool isHandleMouseInputsEnabled() const { return handleKeyboardInputs_; }

  void setHandleKeyboardInputs(bool value) { handleKeyboardInputs_ = value; }
  bool isHandleKeyboardInputsEnabled() const { return handleKeyboardInputs_; }

  void setImGuiChildIgnored(bool value) { ignoreImGuiChild_ = value; }
  bool isImGuiChildIgnored() const { return ignoreImGuiChild_; }

  /**
   * Show or hide whitespace characters in the editor.
   */
  void setShowWhitespaces(bool value) { showWhitespaces_ = value; }
  bool isShowingWhitespaces() const { return showWhitespaces_; }

  /**
   * Insert text at current cursor position.
   * Handles selection replacement and auto-indentation.
   *
   * @param text Text to insert
   * @param indent Whether to apply auto-indentation
   */
  void insertText(std::string_view text, bool indent = false);

  // Cursor movement

  /**
   * Move cursor up by specified number of lines.
   * Updates selection if shift is held.
   *
   * @param amount Number of lines to move up
   * @param select Whether to extend selection while moving
   */
  void moveUp(int amount = 1, bool select = false);

  /**
   * Move cursor down by specified number of lines.
   * Updates selection if shift is held.
   *
   * @param amount Number of lines to move down
   * @param select Whether to extend selection while moving
   */
  void moveDown(int amount = 1, bool select = false);

  void moveLeft(int amount = 1, bool select = false, bool wordMode = false);
  void moveRight(int amount = 1, bool select = false, bool wordMode = false);
  void moveTop(bool select = false);
  void moveBottom(bool select = false);
  void moveHome(bool select = false);
  void moveEnd(bool select = false);

  // Selection

  /**
   * Set the start position of the current selection.
   * Ensures selection range remains valid by swapping start/end if needed.
   *
   * @param position Selection start coordinates
   */
  void setSelectionStart(const Coordinates& position);

  /**
   * Set the end position of the current selection.
   * Ensures selection range remains valid by swapping start/end if needed.
   *
   * @param position Selection end coordinates
   */
  void setSelectionEnd(const Coordinates& position);

  /**
   * Set a new selection and scroll to it.
   *
   * @param start Selection start
   * @param end Selection end.
   */
  void selectAndFocus(const Coordinates& start, const Coordinates& end) {
    setSelection(start, end);
    setCursorPosition(start);
    // scrollToCursor_ = true;
  }

  /**
   * Set the current text selection with specified mode.
   * Updates selection range and handles word/line selection modes.
   *
   * @param start Start coordinates of selection
   * @param end End coordinates of selection
   * @param mode Selection mode (Normal, Word, or Line)
   */
  void setSelection(const Coordinates& start, const Coordinates& end,
                    SelectionMode mode = SelectionMode::Normal);

  /**
   * Select the word at the current cursor position.
   */
  void selectWordUnderCursor();

  /**
   * Select all text in the editor.
   */
  void selectAll();

  /**
   * Check if there is currently selected text.
   * @return true if there is a selection
   */
  bool hasSelection() const;

  // Clipboard operations

  /**
   * Copy current selection or current line to clipboard.
   */
  void copy();

  /**
   * Cut current selection to clipboard.
   */
  void cut();

  /**
   * Paste clipboard contents at cursor position.
   */
  void paste();

  /**
   * Handles deletion of a line ending at the given coordinates.
   *
   * This merges the current line with the next line when the delete key is
   * pressed at the end of a line. The operation is recorded in the undo record
   * for history tracking.
   *
   * @param pos The coordinates where the end-of-line deletion occurs
   * @param undo The undo record to store the deletion information
   */
  void handleEndOfLineDelete(Coordinates pos, UndoRecord& undo);

  /**
   * Handles deletion of a character in the middle of a line.
   *
   * Deletes a single character (which may be a multi-byte UTF-8 sequence) at
   * the given position and updates any affected fold markers. The operation is
   * recorded in the undo record for history tracking.
   *
   * @param pos The coordinates where the deletion occurs
   * @param undo The undo record to store the deletion information
   */
  void handleMidLineDelete(Coordinates pos, UndoRecord& undo);

  /**
   * Handles deletion of text at the current cursor position or selection.
   *
   * If there is a selection, deletes the selected text. Otherwise, deletes
   * either:
   * - A single character at the cursor position if mid-line
   * - Merges with the next line if at the end of a line
   *
   * The operation is recorded for undo history tracking. After deletion:
   * - Change tracking is updated
   * - Text changed flag is set
   * - Content update callback is triggered if set
   * - Syntax highlighting is updated for the affected line
   */
  void delete_();  // Using delete_ to avoid keyword conflict

  // Undo/Redo

  /**
   * Check if undo operation is available.
   * @return true if undo is possible
   */
  bool canUndo() const;

  /**
   * Check if redo operation is available.
   * @return true if redo is possible
   */
  bool canRedo() const;

  /**
   * Undo specified number of operations.
   * @param steps Number of operations to undo
   */
  void undo(int steps = 1);

  /**
   * Redo specified number of operations.
   * @param steps Number of operations to redo
   */
  void redo(int steps = 1);

  // Line highlighting
  void setHighlightedLines(const std::vector<int>& lines) { highlightedLines_ = lines; }
  void clearHighlightedLines() { highlightedLines_.clear(); }

  // Editor settings
  void setTabSize(int size) { core_.setTabSize(size); }
  int getTabSize() const { return core_.getTabSize(); }

  void setInsertSpaces(bool value) { core_.setInsertSpaces(value); }
  bool getInsertSpaces() const { return core_.getInsertSpaces(); }

  void setSmartIndent(bool value) { core_.setSmartIndent(value); }
  void setAutoIndentOnPaste(bool value) { core_.setAutoIndentOnPaste(value); }
  void setHighlightLine(bool value) { highlightLine_ = value; }
  void setCompleteBraces(bool value) { core_.setCompleteBraces(value); }
  void setHorizontalScroll(bool value) { horizontalScroll_ = value; }
  void setSmartPredictions(bool value) { autocomplete_ = value; }
  void setFunctionDeclarationTooltip(bool value) { functionDeclarationTooltipEnabled_ = value; }
  void setFunctionTooltips(bool value) { funcTooltips_ = value; }
  void setActiveAutocomplete(bool value) { core_.setActiveAutocomplete(value); }
  void setScrollbarMarkers(bool value) { core_.setScrollbarMarkers(value); }
  void setSidebarVisible(bool value) { sidebar_ = value; }
  void setSearchEnabled(bool value) { hasSearch_ = value; }
  void setHighlightBrackets(bool value) { highlightBrackets_ = value; }
  void setFoldEnabled(bool value) { foldEnabled_ = value; }

  // UI scaling
  void setUIScale(float scale) { uiScale_ = scale; }
  void setUIFontSize(float size) { uiFontSize_ = size; }
  void setEditorFontSize(float size) { editorFontSize_ = size; }

  /**
   * Bind a keyboard shortcut to an editor action.
   *
   * @param id The action to bind
   * @param shortcut The keyboard shortcut to bind to the action
   */
  void setShortcut(ShortcutId id, Shortcut shortcut);

  /**
   * Configure line number display.
   *
   * @param show Whether to show line numbers
   */
  void setShowLineNumbers(bool show) {
    showLineNumbers_ = show;
    // If showing line numbers, we reserve space. Otherwise, minimal left
    // spacing.
    textStart_ = show ? 20.0f : 6.0f;
    leftMargin_ = show ? kLineNumberSpace : 0;
  }

  /**
   * Get the horizontal offset where text content starts.
   */
  float getTextStart() const { return showLineNumbers_ ? 7.0f : 3.0f; }

  // Syntax highlighting

  /**
   * Update color range for syntax highlighting.
   * @param fromLine Starting line number
   * @param lines Number of lines to colorize (-1 for all remaining lines)
   */
  void colorize(int fromLine = 0, int count = -1);
  void colorizeRange(int fromLine = 0, int toLine = 0);
  void colorizeInternal();

  // Autocomplete
  void clearAutocompleteData() {}
  void clearAutocompleteEntries() {
    autocompleteEntries_.clear();
    autocompleteSearchTerms_.clear();
  }

  /**
   * Add an entry to the autocomplete suggestions.
   *
   * @param searchTerm Term to match against when filtering
   * @param displayText Text to show in the autocomplete popup
   * @param insertText Text to insert when selected
   */
  void addAutocompleteEntry(std::string_view searchTerm, std::string_view displayText,
                            std::string_view insertText) {
    autocompleteSearchTerms_.emplace_back(searchTerm);
    autocompleteEntries_.emplace_back(displayText, insertText);
  }

  // Static utilities

  /**
   * Returns the default set of keyboard shortcuts for the text editor.
   *
   * These shortcuts map various editor actions (like navigation, editing,
   * and search) to specific key combinations. The returned shortcuts
   * can be further customized at runtime.
   *
   * @return A vector of Shortcut objects representing the default shortcuts.
   */
  static std::vector<Shortcut> getDefaultShortcuts();
  static const Palette& getDarkPalette();

  // Callbacks
  std::function<void(TextEditor*, std::string_view)> onIdentifierHover;
  std::function<bool(TextEditor*, std::string_view)> hasIdentifierHover;
  std::function<void(TextEditor*, std::string_view)> onExpressionHover;
  std::function<bool(TextEditor*, std::string_view)> hasExpressionHover;
  std::function<void(TextEditor*, std::string_view, Coordinates)> onCtrlAltClick;
  std::function<void(TextEditor*)> onContentUpdate;

  void deleteRange(const Coordinates& start, const Coordinates& end);

  /**
   * Inserts text at the given coordinates, handling auto-indentation and fold
   * markers.
   *
   * This method handles:
   * - Multi-line text insertion with proper indentation
   * - UTF-8 character sequences
   * - Fold marker updates for braces
   * - Tab expansion and space-based indentation
   * - Snippet tag position updates
   *
   * @param where The coordinates where text should be inserted. Modified to
   * point to the end of insertion.
   * @param text The text to insert
   * @param indent Whether to apply auto-indentation for newlines
   * @return The number of new lines created
   */
  int insertTextAt(Coordinates& /* inout */ where, std::string_view text, bool indent = false);

private:
  using RegexList = std::vector<std::pair<std::regex, ColorIndex>>;
  using UndoBuffer = TextEditorCore::UndoBuffer;

  float getTextDistanceToLineStart(const Coordinates& from) const;
  void ensureCursorVisible();
  int getPageSize() const;
  std::string getText(const Coordinates& start, const Coordinates& end) const;
  Coordinates getActualCursorCoordinates() const;
  Coordinates sanitizeCoordinates(const Coordinates& value) const;
  void advance(Coordinates& coordinates) const;
  void addUndo(UndoRecord& value);

  /**
   * Convert screen position to text coordinates.
   * Handles folded text regions and calculates column position accounting for tabs.
   *
   * @param position Screen position in pixels
   * @return Text coordinates at the given screen position
   */
  Coordinates screenPosToCoordinates(const ImVec2& position) const;

  /**
   * Convert mouse position to text coordinates.
   * Handles folded text regions and adjusts for tab characters.
   *
   * @param position Mouse position in pixels
   * @return Text coordinates at the given mouse position
   */
  Coordinates mousePosToCoordinates(const ImVec2& position) const;
  ImVec2 coordinatesToScreenPos(const Coordinates& position) const;

  /**
   * Find the start position of the word at or before the given coordinates.
   * Handles UTF-8 characters and syntax highlighting color boundaries.
   *
   * @param from Starting coordinates to start the scan for the word start
   */
  Coordinates findWordStart(const Coordinates& from) const;

  /**
   * Find the end position of the word at or after the given coordinates.
   * Handles UTF-8 characters, spaces, and syntax highlighting color boundaries.
   *
   * @param from Starting coordinates to start the scan for the word end
   */
  Coordinates findWordEnd(const Coordinates& from) const;

  /**
   * Find the start of the next word after the given coordinates.
   * Handles alphanumeric word boundaries and line wrapping.
   *
   * @param from Starting coordinates to start the scan for the next word
   */
  Coordinates findNextWord(const Coordinates& from) const;

  /**
   * Check if the given position is on a word boundary.
   * Uses either syntax highlighting colors or whitespace to determine boundaries.
   *
   * @param at The coordinates to check
   * @return true if position is at a word boundary
   */
  bool isOnWordBoundary(const Coordinates& at) const;

  /**
   * Remove a range of lines from start to end (inclusive).
   * Updates error markers, scrollbar markers, and triggers content update.
   *
   * @param start The first line to remove
   * @param end The last line to remove
   */
  void removeLine(int start, int end);

  /**
   * Remove a single line at the given index.
   * Updates error markers, folds, scrollbar markers, and triggers content update.
   *
   * @param index The line to remove
   */
  void removeLine(int index);

  /**
   * Insert a new line at the specified index and column.
   * Updates fold positions and error markers.
   *
   * @param index The line number to insert at
   * @param column The column position to split at if within existing line
   * @return Reference to the newly inserted line
   */
  Line& insertLine(int index, int column);

  // `handleNewLine` and `handleRegularCharacter` moved into
  // `TextEditorCore`. Shell forwarders are no longer required since
  // these were private helpers only invoked by `enterCharacter`, which
  // now lives in core.

  /**
   * Insert a character at the current cursor position, handling special
   * characters and actions.
   *
   * Handles:
   * - Tab indentation (single line and multi-line selection)
   * - Newlines with auto-indentation
   * - UTF-8 character encoding
   * - Bracket/brace completion
   * - Function tooltips
   * - Code folding updates
   * - Undo state tracking
   *
   * @param character The character to insert
   * @param shift Whether shift is held, affects tab behavior
   */
  void enterCharacter(ImWchar character, bool shift);

  /**
   * Update the list of changed lines for scrollbar markers.
   */
  void updateChangeTracking();

  /**
   * Show function tooltips when typing function calls or arguments.
   * @param character The character that was just entered
   * @param curPos Current cursor position
   */
  void handleFunctionTooltip(ImWchar character, const Coordinates& curPos);

  // `handleBraceCompletion`, `handleMultiLineTab`,
  // `handleStartOfLineDelete`, and `handleMidLineBackspace` all moved
  // into `TextEditorCore` with `enterCharacter` / `backspace`.

  /**
   * Handles backspace operation at the current cursor position or selection.
   *
   * If there is a selection, deletes the selected text. Otherwise:
   * - At the start of a line: Merges with the previous line
   * - Mid-line: Deletes the character before the cursor
   *
   * After deletion:
   * - Change tracking is updated
   * - Text changed flag is set
   * - Content update callback is triggered if set
   * - Cursor visibility is ensured
   * - Syntax highlighting is updated for the affected line
   * - Autocomplete is re-triggered if active
   *
   * The operation is recorded for undo history tracking.
   */
  void backspace();

  /**
   * Delete the current selection.
   * If no text is selected, this operation has no effect.
   */
  void deleteSelection();

  /**
   * Get the word under the current cursor position.
   * Adjusts cursor column to handle end-of-word positioning.
   *
   * @return The word under the cursor
   */
  RcString getWordUnderCursor() const;

  /**
   * Get the word at the given coordinates.
   * Uses word boundary detection to extract the full word.
   *
   * @param coords The coordinates within the word
   * @return The word at the given coordinates
   */
  RcString getWordAt(const Coordinates& coords) const;
  ImU32 getGlyphColor(const Glyph& glyph) const;

  void detectIndentationStyle();

  // The headless editing substrate. Owned by value; every moved method
  // forwards to it via a one-line wrapper. Reference aliases below bind
  // to its fields so the existing shell code (renderInternal, input
  // handling, layout math) can keep using short names like `text_` and
  // `state_` unchanged.
  TextEditorCore core_;

  // Member variables following trailing underscore convention...
  bool funcTooltips_ = true;
  float uiScale_ = 1.0f;
  float uiFontSize_ = 18.0f;
  float editorFontSize_ = 18.0f;

  // Utility methods
  float calculateUISize(float height) const {
    return height * (uiScale_ + uiFontSize_ / 18.0f - 1.0f);
  }

  float calculateEditorSize(float height) const {
    return height * (uiScale_ + editorFontSize_ / 18.0f - 1.0f);
  }

  bool functionDeclarationTooltipEnabled_ = false;
  Coordinates functionDeclarationCoord_;
  bool functionDeclarationTooltip_ = false;
  std::string functionDeclaration_;

  // Core text content — aliased to fields owned by `core_`. Binding as
  // references avoids churn in every shell method that referenced these
  // short names. The references are initialized in the constructor's
  // member-initializer list.
  TextBuffer& text_;
  float lineSpacing_ = 1.0f;  //!< Vertical spacing between lines
  EditorState& state_;        //!< Current editor state (core-owned)
  int& replaceIndex_;         //!< Replace operation index (core-owned)

  // UI state
  bool sidebar_ = true;          //!< Show sidebar
  bool hasSearch_ = true;        //!< Enable search functionality
  std::string findWord_;         //!< Current search term
  bool findOpened_ = false;      //!< Search dialog is open
  bool findJustOpened_ = false;  //!< Search dialog was just opened
  bool findNext_ = false;        //!< Find next occurrence
  bool findFocused_ = false;     //!< Search input is focused
  bool replaceFocused_ = false;  //!< Replace input is focused
  bool replaceOpened_ = false;   //!< Replace dialog is open
  std::string replaceWord_;      //!< Current replace term

  // Code folding — begin/end positions and sorted flag live in `core_`
  // (mutated by insertion/deletion bookkeeping); the render-side state
  // (fold_, foldConnection_, foldedLines_, foldLastIteration_) stays here.
  bool foldEnabled_ = true;                    //!< Enable code folding
  std::vector<Coordinates>& foldBegin_;        //!< Start positions (core_-owned)
  std::vector<Coordinates>& foldEnd_;          //!< End positions (core_-owned)
  bool& foldSorted_;                           //!< Whether folds are sorted (core_)
  std::vector<int> foldConnection_;            //!< Fold hierarchy connections
  std::vector<bool> fold_;                     //!< Fold states (open/closed)
  int foldedLines_ = 0;                        //!< Number of folded lines.
  uint64_t foldLastIteration_ = 0;             //!< Last fold update iteration
  float lastScroll_ = 0.0f;                    //!< Last scroll position

  // Autocomplete
  std::vector<RcString> autocompleteSearchTerms_;  //!< Terms to match against
  std::vector<std::pair<RcString, RcString>>
      autocompleteEntries_;                   //!< Display text and insert text pairs
  bool isSnippet_ = false;                    //!< Current completion is a snippet
  std::vector<Coordinates> snippetTagStart_;  //!< Snippet placeholder starts
  std::vector<Coordinates> snippetTagEnd_;    //!< Snippet placeholder ends
  std::vector<int> snippetTagId_;             //!< Snippet placeholder IDs
  std::vector<bool> snippetTagHighlight_;     //!< Snippet highlight states
  int snippetTagSelected_ = 0;                //!< Selected snippet placeholder
  int snippetTagLength_ = 0;                  //!< Current placeholder length
  int snippetTagPreviousLength_ = 0;          //!< Previous placeholder length

  // Autocomplete state. `activeAutocomplete_` itself now lives in
  // `core_` so editing-path helpers there can use it.
  bool requestAutocomplete_ = false;   //!< Request to show autocomplete
  bool readyForAutocomplete_ = false;  //!< Ready to show autocomplete
  bool autocomplete_ = true;           //!< Autocomplete enabled
  std::string autocompleteWord_;       //!< Current word being completed
  std::vector<std::pair<RcString, RcString>> autocompleteSuggestions_;  //!< Current suggestions
  int autocompleteIndex_;             //!< Selected suggestion index
  bool autocompleteOpened_ = false;   //!< Autocomplete popup is open
  bool autocompleteSwitched_;         //!< Allow enter to select
  std::string autocompleteObject_;    //!< Object for member completion
  Coordinates autocompletePosition_;  //!< Position of completion

  // Keyboard shortcuts
  std::vector<Shortcut> shortcuts_;  //!< Configured shortcuts

  // Visual markers. The `scrollbarMarkers_` flag + `changedLines_` diff
  // set are owned by `core_` so insert/delete bookkeeping stays in one
  // place; the shell reads them via aliased references.
  bool& scrollbarMarkers_;
  std::vector<int>& changedLines_;
  std::vector<int> highlightedLines_;  //!< Lines to highlight

  // Editor settings. `insertSpaces_`, `smartIndent_`, `tabSize_`,
  // `scrollToCursor_`, `scrollToTop_`, `textChanged_`, `colorizerEnabled_`,
  // `cursorPositionChanged_`, `colorRangeMin_/Max_`, `selectionMode_`,
  // `autoIndentOnPaste_`, `completeBraces_`, `activeAutocomplete_`,
  // `scrollbarMarkers_`, and `changedLines_` all moved into
  // `TextEditorCore`. The reference aliases below preserve the short
  // names used by the render / input-handling code.
  bool horizontalScroll_ = true;        //!< Enable horizontal scrolling
  bool showLineNumbers_ = true;         //!< Show line numbers
  bool highlightLine_ = true;           //!< Highlight current line
  bool highlightBrackets_ = false;      //!< Highlight matching brackets
  bool focused_ = false;                //!< Editor has keyboard focus
  bool withinRender_ = false;           //!< Currently rendering
  float textStart_ = 20.0f;             //!< X offset where text begins
  int leftMargin_ = kLineNumberSpace;   //!< Left margin width
  bool handleKeyboardInputs_ = true;    //!< Process keyboard input
  bool handleMouseInputs_ = true;       //!< Process mouse input
  bool ignoreImGuiChild_ = false;       //!< Ignore ImGui child windows
  bool showWhitespaces_ = false;        //!< Show whitespace characters

  // Reference aliases into `core_` for fields accessed by the shell's
  // render / input-capture code. These are initialized in the ctor
  // member-initializer list.
  int& tabSize_;
  bool& scrollToCursor_;
  bool& scrollToTop_;
  bool& cursorPositionChanged_;
  bool& textChanged_;
  bool& autoIndentOnPaste_;
  SelectionMode& selectionMode_;

  // Colors and syntax highlighting — canonical storage in `core_`.
  Palette& paletteBase_;                     //!< Base color palette (core_)
  Palette& palette_;                         //!< Current (alpha-blended) palette (core_)
  const LanguageDefinition& languageDefinition_;  //!< Language syntax (core_)

  // Layout and rendering. Interactive-selection trackers moved to core_.
  ImVec2 uiCursorPos_;                 //!< Current UI cursor position
  ImVec2 findOrigin_;                  //!< Search dialog origin
  float windowWidth_;                  //!< Current window width
  ImVec2 rightClickPos_;               //!< Position of right click
  ErrorMarkers& errorMarkers_;         //!< Line error markers (core_)
  ImVec2 charAdvance_;                 //!< Character size
  Coordinates& interactiveStart_;      //!< Start of interactive selection (core_)
  Coordinates& interactiveEnd_;        //!< End of interactive selection (core_)
  std::string lineBuffer_;             //!< Buffer for current line
  uint64_t startTime_ = 0;             //!< Editor start time

  // Hover state
  Coordinates lastHoverPosition_;                        //!< Last mouse hover position
  std::chrono::steady_clock::time_point lastHoverTime_;  //!< Time of last hover
  float lastClick_ = -1.0f;                              //!< Time of last click

  // Helper methods for snippets
  RcString autocompleteParseSnippet(std::string_view text, const Coordinates& start);

  // Helper methods for member suggestions
  void buildMemberSuggestions(bool* keepAutocompleteOpen = nullptr);
  void buildSuggestions(bool* keepAutocompleteOpen = nullptr);

  // Function declaration tooltip helpers
  void openFunctionDeclarationTooltip(std::string_view text, const Coordinates& coords);

  /**
   * Remove code folds in the given range.
   */
  void removeFolds(const Coordinates& start, const Coordinates& end);
  void removeFolds(std::vector<Coordinates>& folds, const Coordinates& start,
                   const Coordinates& end);

  /**
   * Find the first occurrence of searchText starting from given coordinates.
   * Only returns matches that occur at word boundaries.
   *
   * @param searchText The text to search for
   * @param start The coordinates to start searching from
   * @return The coordinates of the first match, or end of text if not found
   */
  Coordinates findFirst(std::string_view searchText, const Coordinates& start) const;

  /**
   * Process keyboard and mouse input events.
   */
  void handleKeyboardInputs();
  void handleMouseInputs();

private:
  /**
   * Handle general character input and special keys.
   * Processes keypresses that aren't handled by shortcuts.
   *
   * @param io ImGui IO state for input
   * @param shift Whether shift key is pressed
   * @param keepAutocompleteOpen Whether to keep autocomplete window open
   * @param hasWrittenLetter Set to true if a letter was typed
   */
  void handleCharacterInput(const ImGuiIO& io, bool shift, bool& keepAutocompleteOpen,
                            bool& hasWrittenLetter);

  /**
   * Select the currently highlighted autocomplete suggestion.
   */
  void autocompleteSelect();

  /**
   * Navigate through autocomplete suggestions.
   * @param up True to move up, false to move down
   */
  void autocompleteNavigate(bool up);

  /**
   * Check if snippet support is enabled and snippets exist.
   * @return True if snippets are available
   */
  bool hasSnippets() const;

  /**
   * Handle insertion of snippet placeholder.
   * @param character The character being inserted
   */
  void handleSnippetInsertion(ImWchar character);

  /**
   * Execute a keyboard shortcut action.
   * Handles all editor operations triggered by shortcuts like copy, paste,
   * undo, etc.
   *
   * @param actionId The shortcut action to execute
   * @param keepAutocompleteOpen Whether to keep autocomplete window open after
   * action
   * @param shift Whether shift key is pressed
   * @param ctrl Whether control key is pressed
   */
  void executeAction(ShortcutId actionId, bool& keepAutocompleteOpen, bool shift, bool ctrl);

  // `updateFoldColumn` moved to TextEditorCore as an internal helper.

  /**
   * Calculate character advance width based on current font size.
   * Stores result in charAdvance_ member.
   */
  void calculateCharacterAdvance();

  /**
   * Update palette colors with current UI alpha value.
   * Modifies palette_ based on paletteBase_ and current style alpha.
   */
  void updatePaletteAlpha();

  /**
   * Calculate text start position based on line numbers and margin.
   * Updates textStart_ member.
   */
  void updateTextStart();

  /**
   * Calculate folded lines and update fold state.
   * @param currentLine Starting line number for fold calculation
   * @param totalLines Total number of lines in document
   */
  void calculateFolds(int currentLine, int totalLines);

  /**
   * Update count of folded lines based on current fold state.
   * @param currentLine Starting line number for fold calculation
   * @param totalLines Total number of lines in document
   */
  void updateFoldedLines(int currentLine, int totalLines);

  /**
   * Main internal rendering implementation. Renders editor content and UI.
   * @param title Window title
   */
  void renderInternal(std::string_view title);

  /**
   * Handle editor scrolling.
   * Ensures cursor is visible and updates ImGui scroll state.
   */
  void handleScrolling();

  /**
   * Render a single line of text with selection, syntax highlighting and folds.
   * @param lineNo Line number to render
   * @param lineStart Screen position where line starts
   * @param textStart Screen position where text content starts
   * @param contentSize Available content size
   * @param scrollX Current horizontal scroll position
   * @param drawList ImGui draw list for rendering
   * @param longestLine Updated with line length if longer than current value
   */
  void renderLine(int lineNo, const ImVec2& lineStart, const ImVec2& textStart,
                  const ImVec2& contentSize, float scrollX, ImDrawList* drawList,
                  float& longestLine);

  // TODO: Doc comments
  void renderErrorTooltip(int line, const std::string& message);

  /**
   * Render line background including current line highlight and error markers.
   */
  void renderLineBackground(int lineNo, const ImVec2& start, const ImVec2& contentSize,
                            ImDrawList* drawList);

  /**
   * Render text selection for a line.
   * Draws the highlighted background only for the portion of the selection
   * that intersects with this line.
   *
   * @param lineNo The line number to render selection on
   * @param line The line content
   * @param lineStart The screen position where this line begins
   * @param contentSize The available size for drawing
   * @param drawList The ImGui draw list to render to
   */
  void renderSelection(int lineNo, const Line& line, const ImVec2& lineStart,
                       const ImVec2& contentSize, ImDrawList* drawList);

  /**
   * Render text content with syntax highlighting.
   */
  void renderText(const Line& line, const ImVec2& pos, ImDrawList* drawList);

  /**
   * Render error markers and tooltips for a line.
   */
  void renderErrorMarkers(int lineNo, const ImVec2& start, const ImVec2& end, ImDrawList* drawList);

  /**
   * Render line numbers in the margin.
   */
  void renderLineNumbers(int lineNo, const ImVec2& pos, ImDrawList* drawList);

  /**
   * Render code folding markers and fold state.
   */
  void renderFoldMarkers(int lineNo, const ImVec2& pos, ImDrawList* drawList);

  /**
   * Render cursor at current position.
   */
  void renderCursor(const ImVec2& pos, ImDrawList* drawList);

  /**
   * Render additional UI elements like autocomplete and function tooltips.
   */
  void renderExtraUI(ImDrawList* drawList, const ImVec2& basePos, float scrollX, float scrollY,
                     float longest, const ImVec2& contentSize);

  /**
   * Process all input events.
   */
  void processInputs();
};

}  // namespace donner::editor
