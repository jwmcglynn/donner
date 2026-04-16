#pragma once
/// @file
///
/// Headless editing substrate for the ImGui TextEditor widget.
///
/// Commit 1 of the three-commit TextEditor refactor
/// (docs/design_docs/text_editor_refactor.md). `TextEditorCore` owns the
/// editable buffer, cursor/selection state, undo history, and syntax
/// colorizer. It exposes every editing operation as a plain C++ method and
/// has zero dependency on `imgui.h`, fonts, or any rendering layer.
///
/// `donner::editor::TextEditor` remains the public user-facing widget; it
/// holds a `TextEditorCore core_` by value and forwards every editing-path
/// method to it via a one-line wrapper. Callers of `TextEditor` see no API
/// change. A later commit (C2) introduces a `ClipboardInterface` so that
/// `copy()`, `cut()`, and `paste()` can also migrate to the core. C3 then
/// moves the tests onto `TextEditorCore` directly so they run without an
/// ImGui context.
///
/// ## Palette type note
///
/// The canonical `Palette` type was `std::array<ImU32, N>`. To keep this
/// header ImGui-free we use `std::array<uint32_t, N>` instead — `ImU32` is
/// a `typedef unsigned int` in ImGui, so the representation matches and the
/// shell can freely interchange the two at call sites via implicit
/// conversion in ImGui's drawlist APIs.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/editor/TextBuffer.h"

namespace donner::editor {

class TextEditorCore;

/**
 * Editor cursor and selection state, captured by value for undo/redo.
 */
struct EditorState {
  Coordinates selectionStart;
  Coordinates selectionEnd;
  Coordinates cursorPosition;
};

/**
 * Record of a single text mutation for undo/redo.
 *
 * Stores the text added, the text removed, and the editor state before and
 * after the mutation. `undo` reverses the mutation by removing the added
 * text and re-inserting the removed text; `redo` replays it.
 */
class UndoRecord {
public:
  UndoRecord() = default;
  ~UndoRecord() = default;

  UndoRecord(std::string_view added, const Coordinates& addedStart, const Coordinates& addedEnd,
             std::string_view removed, const Coordinates& removedStart,
             const Coordinates& removedEnd, const EditorState& before, const EditorState& after);

  void undo(TextEditorCore* core);
  void redo(TextEditorCore* core);

  RcString added;
  Coordinates addedStart;
  Coordinates addedEnd;

  RcString removed;
  Coordinates removedStart;
  Coordinates removedEnd;

  EditorState before;
  EditorState after;
};

/**
 * Scratch state threaded through the helpers that handle a single character
 * insertion (`handleNewLine`, `handleRegularCharacter`, etc.).
 */
struct UndoState {
  UndoRecord record;
  Coordinates insertPos;
};

/**
 * Editor selection modes that affect how text selection behaves.
 */
enum class SelectionMode {
  Normal,  //!< Character-by-character selection
  Word,    //!< Select whole words
  Line     //!< Select whole lines
};

/**
 * An editor-recognized identifier (function, variable, attribute name).
 */
struct Identifier {
  Coordinates location;  //!< Where this identifier appears in the text
  RcString declaration;  //!< The declaration text for this identifier

  Identifier() = default;
  /* implicit */ Identifier(std::string_view decl) : declaration(decl) {}
};

using Identifiers = std::unordered_map<std::string, Identifier>;
using Keywords = std::unordered_set<std::string>;
using ErrorMarkers = std::map<int, std::string>;

/**
 * Color palette indexed by `ColorIndex`. Intentionally declared as
 * `std::array<unsigned int, N>` rather than `std::array<ImU32, N>` so
 * this header stays ImGui-free. `ImU32` is a `typedef unsigned int` in
 * ImGui (see `imgui.h`), so the two types are identical and freely
 * interchangeable at call sites in the shell.
 */
using Palette = std::array<unsigned int, static_cast<size_t>(ColorIndex::Max)>;

/**
 * Definition of a programming language's syntax for highlighting.
 */
struct LanguageDefinition {
  using TokenRegexString = std::pair<std::string, ColorIndex>;
  using TokenRegexStrings = std::vector<TokenRegexString>;
  using TokenizeCallback =
      std::function<bool(const char* inBegin, const char* inEnd, const char*& outBegin,
                         const char*& outEnd, ColorIndex& paletteIndex)>;

  RcString name;                        //!< Language name
  Keywords keywords;                    //!< Language keywords
  Identifiers identifiers;              //!< Known identifiers
  RcString commentStart;                //!< Multi-line comment start
  RcString commentEnd;                  //!< Multi-line comment end
  RcString singleLineComment;           //!< Single-line comment marker
  bool autoIndentation = true;          //!< Whether to enable auto-indentation
  TokenizeCallback tokenize;            //!< Custom tokenization callback
  TokenRegexStrings tokenRegexStrings;  //!< Regex patterns for syntax
  bool caseSensitive = true;            //!< Whether keywords are case-sensitive

  LanguageDefinition() = default;

  /**
   * Get the SVG language definition.
   */
  static const LanguageDefinition& SVG();
};

/**
 * Headless editing substrate — the text buffer, cursor, undo history and
 * syntax colorizer with no ImGui dependency.
 *
 * `TextEditor` wraps this class and adds the ImGui rendering and input
 * layer. Direct consumers should go through `TextEditor`; tests (once C3
 * migrates them) will construct `TextEditorCore` directly.
 */
class TextEditorCore {
public:
  using UndoBuffer = std::vector<UndoRecord>;

  TextEditorCore();
  ~TextEditorCore();

  TextEditorCore(const TextEditorCore&) = delete;
  TextEditorCore& operator=(const TextEditorCore&) = delete;
  TextEditorCore(TextEditorCore&&) = delete;
  TextEditorCore& operator=(TextEditorCore&&) = delete;

  // ---------------------------------------------------------------------
  // Text
  // ---------------------------------------------------------------------

  /// Replace the buffer contents with `text`. By default this scrolls
  /// the view back to the top — appropriate for File→Open and similar
  /// "load a different document" flows. Pass `preserveScroll=true` when
  /// the replacement is a small in-place edit (e.g. canvas→text
  /// writeback after a transform drag) so the user's scroll position
  /// isn't yanked out from under them.
  void setText(std::string_view text, bool preserveScroll = false);
  std::string getText() const;
  std::string getText(const Coordinates& start, const Coordinates& end) const;

  bool isTextChanged() const { return textChanged_; }
  void resetTextChanged() {
    textChanged_ = false;
    changedLines_.clear();
  }

  // ---------------------------------------------------------------------
  // Cursor / selection
  // ---------------------------------------------------------------------

  Coordinates getCursorPosition() const { return getActualCursorCoordinates(); }
  void setCursorPosition(const Coordinates& position);

  void setSelection(const Coordinates& start, const Coordinates& end,
                    SelectionMode mode = SelectionMode::Normal);
  /// Update the visible selection during an active mouse interaction while
  /// preserving the raw anchor/current endpoints in `interactiveStart_/End_`.
  void setInteractiveSelection(const Coordinates& start, const Coordinates& end,
                               SelectionMode mode = SelectionMode::Normal);
  void setSelectionStart(const Coordinates& position);
  void setSelectionEnd(const Coordinates& position);

  const Coordinates& getSelectionStart() const { return state_.selectionStart; }
  const Coordinates& getSelectionEnd() const { return state_.selectionEnd; }

  bool hasSelection() const;
  std::string getSelectedText() const;

  void selectAll();
  void selectWordUnderCursor();

  // ---------------------------------------------------------------------
  // Navigation
  // ---------------------------------------------------------------------

  void moveUp(int amount = 1, bool select = false);
  void moveDown(int amount = 1, bool select = false);
  void moveLeft(int amount = 1, bool select = false, bool wordMode = false);
  void moveRight(int amount = 1, bool select = false, bool wordMode = false);
  void moveTop(bool select = false);
  void moveBottom(bool select = false);
  void moveHome(bool select = false);
  void moveEnd(bool select = false);

  // ---------------------------------------------------------------------
  // Editing
  // ---------------------------------------------------------------------

  void enterCharacter(char32_t character, bool shift);
  void insertText(std::string_view text, bool indent = false);
  int insertTextAt(Coordinates& /* inout */ where, std::string_view text, bool indent = false);
  void deleteRange(const Coordinates& start, const Coordinates& end);
  void deleteSelection();
  void backspace();
  void delete_();

  // ---------------------------------------------------------------------
  // Undo / redo
  // ---------------------------------------------------------------------

  bool canUndo() const;
  bool canRedo() const;
  void undo(int steps = 1);
  void redo(int steps = 1);
  void addUndo(UndoRecord& value);

  // ---------------------------------------------------------------------
  // Syntax highlighting / colorizer
  // ---------------------------------------------------------------------

  void setLanguageDefinition(const LanguageDefinition& langDef);
  const LanguageDefinition& getLanguageDefinition() const { return languageDefinition_; }

  void colorize(int fromLine = 0, int count = -1);
  void colorizeRange(int fromLine = 0, int toLine = 0);
  void colorizeInternal();

  bool isColorizerEnabled() const { return colorizerEnabled_; }
  void setColorizerEnabled(bool enabled) { colorizerEnabled_ = enabled; }

  // ---------------------------------------------------------------------
  // Palette
  // ---------------------------------------------------------------------

  void setPalette(const Palette& value) { paletteBase_ = value; }
  const Palette& getPalette() const { return paletteBase_; }

  // ---------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------

  void setTabSize(int size);
  int getTabSize() const { return tabSize_; }

  void setInsertSpaces(bool value) { insertSpaces_ = value; }
  bool getInsertSpaces() const { return insertSpaces_; }

  void setSmartIndent(bool value) { smartIndent_ = value; }
  void setAutoIndentOnPaste(bool value) { autoIndentOnPaste_ = value; }
  void setCompleteBraces(bool value) { completeBraces_ = value; }
  void setActiveAutocomplete(bool value) { activeAutocomplete_ = value; }
  void setScrollbarMarkers(bool value) { scrollbarMarkers_ = value; }
  bool& scrollbarMarkersRef() { return scrollbarMarkers_; }

  // ---------------------------------------------------------------------
  // Error markers
  // ---------------------------------------------------------------------

  void setErrorMarkers(const ErrorMarkers& markers) { errorMarkers_ = markers; }
  const ErrorMarkers& getErrorMarkers() const { return errorMarkers_; }
  ErrorMarkers& mutableErrorMarkers() { return errorMarkers_; }

  // ---------------------------------------------------------------------
  // Word utilities
  // ---------------------------------------------------------------------

  Coordinates findWordStart(const Coordinates& from) const;
  Coordinates findWordEnd(const Coordinates& from) const;
  Coordinates findNextWord(const Coordinates& from) const;
  bool isOnWordBoundary(const Coordinates& at) const;

  RcString getWordUnderCursor() const;
  RcString getWordAt(const Coordinates& coords) const;

  Coordinates findFirst(std::string_view searchText, const Coordinates& start) const;

  // ---------------------------------------------------------------------
  // Coordinate helpers
  // ---------------------------------------------------------------------

  Coordinates getActualCursorCoordinates() const;
  Coordinates sanitizeCoordinates(const Coordinates& value) const;
  void advance(Coordinates& coordinates) const;

  // ---------------------------------------------------------------------
  // Buffer / state accessors for the shell rendering code
  // ---------------------------------------------------------------------

  TextBuffer& buffer() { return text_; }
  const TextBuffer& buffer() const { return text_; }

  EditorState& mutableState() { return state_; }
  const EditorState& state() const { return state_; }

  const UndoBuffer& undoBuffer() const { return undoBuffer_; }
  int undoIndex() const { return undoIndex_; }

  std::vector<int>& changedLines() { return changedLines_; }
  const std::vector<int>& changedLines() const { return changedLines_; }

  std::vector<Coordinates>& foldBegin() { return foldBegin_; }
  const std::vector<Coordinates>& foldBegin() const { return foldBegin_; }
  std::vector<Coordinates>& foldEnd() { return foldEnd_; }
  const std::vector<Coordinates>& foldEnd() const { return foldEnd_; }
  bool& foldSorted() { return foldSorted_; }

  SelectionMode selectionMode() const { return selectionMode_; }
  void setSelectionMode(SelectionMode mode) { selectionMode_ = mode; }

  Coordinates& interactiveStart() { return interactiveStart_; }
  Coordinates& interactiveEnd() { return interactiveEnd_; }

  Palette& mutablePalette() { return palette_; }
  const Palette& livePalette() const { return palette_; }
  Palette& mutablePaletteBase() { return paletteBase_; }

  bool scrollToCursorRequested() const { return scrollToCursor_; }
  void clearScrollToCursor() { scrollToCursor_ = false; }
  void requestScrollToCursor() { scrollToCursor_ = true; }
  bool& scrollToCursorRef() { return scrollToCursor_; }

  bool scrollToTopRequested() const { return scrollToTop_; }
  void clearScrollToTop() { scrollToTop_ = false; }
  bool& scrollToTopRef() { return scrollToTop_; }

  bool cursorPositionChanged() const { return cursorPositionChanged_; }
  void setCursorPositionChanged(bool value) { cursorPositionChanged_ = value; }
  bool& cursorPositionChangedRef() { return cursorPositionChanged_; }

  int replaceIndex() const { return replaceIndex_; }
  void setReplaceIndex(int index) { replaceIndex_ = index; }
  int& replaceIndexRef() { return replaceIndex_; }

  int& tabSizeRef() { return tabSize_; }
  SelectionMode& selectionModeRef() { return selectionMode_; }
  bool& textChangedRef() { return textChanged_; }
  bool& autoIndentOnPasteRef() { return autoIndentOnPaste_; }

  // Glyph coloring (used by shell rendering). Returns an `unsigned int`
  // that is identical to ImGui's `ImU32`.
  unsigned int getGlyphColor(const Glyph& glyph) const;

  // Low-level line mutation helpers (used by shell + core, updating
  // fold/error-marker bookkeeping that moved into the core).
  void removeLine(int start, int end);
  void removeLine(int index);
  Line& insertLine(int index, int column);
  void removeFolds(const Coordinates& start, const Coordinates& end);
  void removeFolds(std::vector<Coordinates>& folds, const Coordinates& start,
                   const Coordinates& end);

  void detectIndentationStyle();

  // Per-character insertion helpers (threaded through UndoState).
  void handleNewLine(UndoState& state, const Coordinates& coord, bool smartIndent);
  void handleRegularCharacter(UndoState& state, const Coordinates& coord, char32_t character);
  void handleEndOfLineDelete(Coordinates pos, UndoRecord& undo);
  void handleMidLineDelete(Coordinates pos, UndoRecord& undo);
  void handleStartOfLineDelete(const Coordinates& pos, UndoRecord& undo);
  void handleMidLineBackspace(const Coordinates& pos, UndoRecord& undo);
  void handleMultiLineTab(UndoState& state, bool shift);

  void updateChangeTracking();

  // ---------------------------------------------------------------------
  // Shell hooks
  // ---------------------------------------------------------------------
  //
  // A handful of moved methods (`backspace`, `enterCharacter`, etc.)
  // historically invoked helpers that live in the ImGui shell —
  // `ensureCursorVisible` pokes the ImGui scroll state, function tooltips
  // and brace completion read shell-only configuration. The core invokes
  // these via `std::function` hooks set by `TextEditor`. When unset (e.g.
  // headless tests after C3), the hooks are null and no-op, which matches
  // today's behavior where `ensureCursorVisible` early-returns outside
  // `withinRender_`.

  std::function<void()> ensureCursorVisibleHook;
  std::function<void()> requestAutocompleteHook;
  std::function<void(char32_t, const Coordinates&)> functionTooltipHook;
  std::function<void()> onContentUpdateInternal;

private:
  using RegexList = std::vector<std::pair<std::regex, ColorIndex>>;

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  TextBuffer text_;
  EditorState state_;
  UndoBuffer undoBuffer_;
  int undoIndex_ = 0;
  int replaceIndex_ = 0;

  Coordinates interactiveStart_;
  Coordinates interactiveEnd_;

  void applySelection(const Coordinates& start, const Coordinates& end, SelectionMode mode,
                      bool updateInteractiveBounds);
  SelectionMode selectionMode_ = SelectionMode::Normal;

  bool textChanged_ = false;
  bool cursorPositionChanged_ = false;
  bool scrollToCursor_ = false;
  bool scrollToTop_ = false;

  // Change tracking (for the shell's scrollbar diff markers).
  bool scrollbarMarkers_ = false;
  std::vector<int> changedLines_;

  // Error markers (display owned by shell; data lives here because
  // insertion/deletion need to update marker line numbers).
  ErrorMarkers errorMarkers_;

  // Code folding data (rendering lives in shell).
  std::vector<Coordinates> foldBegin_;
  std::vector<Coordinates> foldEnd_;
  bool foldSorted_ = false;

  // Indent / tab configuration.
  int tabSize_ = 2;
  bool insertSpaces_ = true;
  bool smartIndent_ = true;
  bool completeBraces_ = true;
  bool autoIndentOnPaste_ = false;
  bool activeAutocomplete_ = false;

  enum class IndentMode { Spaces, Tabs, Auto };
  IndentMode indentMode_ = IndentMode::Auto;

  // Syntax highlighting state.
  Palette paletteBase_{};
  Palette palette_{};
  LanguageDefinition languageDefinition_;
  RegexList regexList_;
  bool colorizerEnabled_ = true;
  int colorRangeMin_ = 0;
  int colorRangeMax_ = 0;
  bool checkComments_ = true;

  // Internal helpers.
  void requestEnsureCursorVisible();
  void requestAutocomplete();
  void fireContentUpdate();
};

}  // namespace donner::editor
