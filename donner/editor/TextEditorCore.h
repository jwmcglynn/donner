#pragma once
/// @file
///
/// Headless editing substrate for the ImGui TextEditor widget.
///
/// Part of the TextEditor refactor. `TextEditorCore` owns the
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
/// header ImGui-free we use `std::array<uint32_t, N>` instead - `ImU32` is
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
#include "donner/editor/SourceEditIntent.h"
#include "donner/editor/TextBuffer.h"

namespace donner::editor {

class TextEditorCore;

/**
 * Editor cursor and selection state, captured by value for undo/redo.
 */
struct EditorState {
  Coordinates selectionStart;  //!< Ordered lower selection bound in this state.
  Coordinates selectionEnd;    //!< Ordered upper selection bound in this state.
  Coordinates cursorPosition;  //!< Cursor location in this state.
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

  /// Records inserted and removed text with their ranges and surrounding editor states.
  /// @param added Inserted text.
  /// @param addedStart Start of the inserted range.
  /// @param addedEnd End of the inserted range.
  /// @param removed Removed text.
  /// @param removedStart Start of the removed range.
  /// @param removedEnd End of the removed range.
  /// @param before State before the edit.
  /// @param after State after the edit.
  UndoRecord(std::string_view added, const Coordinates& addedStart, const Coordinates& addedEnd,
             std::string_view removed, const Coordinates& removedStart,
             const Coordinates& removedEnd, const EditorState& before, const EditorState& after);

  /// Reverse this edit in the owning text editor.
  /// @param core Editor whose buffer and selection are restored.
  void undo(TextEditorCore* core);
  /// Reapply this edit in the owning text editor.
  /// @param core Editor whose buffer and selection are updated.
  void redo(TextEditorCore* core);

  RcString added;          //!< Text inserted by the original edit.
  Coordinates addedStart;  //!< Start of the inserted range.
  Coordinates addedEnd;    //!< End of the inserted range.

  RcString removed;          //!< Text displaced by the original edit.
  Coordinates removedStart;  //!< Start of the removed range.
  Coordinates removedEnd;    //!< End of the removed range.

  EditorState before;  //!< Editor state before the edit.
  EditorState after;   //!< Editor state after the edit.
};

/**
 * Scratch state threaded through the helpers that handle a single character
 * insertion (`handleNewLine`, `handleRegularCharacter`, etc.).
 */
struct UndoState {
  UndoRecord record;      //!< Undo record assembled for this insertion.
  Coordinates insertPos;  //!< Insertion point as characters are added.
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
  /// Construct an identifier from its declaration text.
  /// @param decl Declaration associated with the identifier.
  /* implicit */ Identifier(std::string_view decl) : declaration(decl) {}
};

using Identifiers =
    std::unordered_map<std::string, Identifier>;   //!< Identifiers indexed by their spelling.
using Keywords = std::unordered_set<std::string>;  //!< Words recognized as language keywords.
using ErrorMarkers =
    std::map<int, std::string>;  //!< Line-keyed diagnostics; rendered rows use one-based lookup.

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
  using TokenRegexString =
      std::pair<std::string, ColorIndex>;  //!< Pattern and palette color for one token class.
  using TokenRegexStrings = std::vector<TokenRegexString>;  //!< Ordered regex token classes.
  using TokenizeCallback =  //!< Callback that identifies the next token in a character range.
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
 * Headless editing substrate - the text buffer, cursor, undo history and
 * syntax colorizer with no ImGui dependency.
 *
 * `TextEditor` wraps this class and adds the ImGui rendering and input
 * layer. Direct consumers should go through `TextEditor`; tests (once C3
 * migrates them) will construct `TextEditorCore` directly.
 */
class TextEditorCore {
public:
  using UndoBuffer =
      std::vector<UndoRecord>;  //!< Chronological editing records retained for undo and redo.

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
  /// the view back to the top - appropriate for File→Open and similar
  /// "load a different document" flows. Pass `preserveScroll=true` when
  /// the replacement is a small in-place edit (e.g. canvas→text
  /// writeback after a transform drag) so the user's scroll position
  /// isn't yanked out from under them.
  void setText(std::string_view text, bool preserveScroll = false);

  /**
   * Apply a source-view update that originated outside the text editor.
   *
   * This mutates the visible buffer and recolorizes the affected region, but it does not append an
   * undo record, does not create a pending \ref SourceEditIntent, and leaves \ref isTextChanged
   * false. Use this for XML-owned canvas writebacks that are being mirrored into the source pane.
   *
   * @param offset Byte offset in the current buffer.
   * @param removedLength Number of bytes to remove at \p offset.
   * @param replacement Replacement bytes to insert at \p offset.
   */
  void applyExternalSourceEdit(std::size_t offset, std::size_t removedLength,
                               std::string_view replacement);

  /// Return the entire editable text buffer.
  std::string getText() const;
  /// Return the text in the half-open coordinate range.
  /// @param start First coordinate to read.
  /// @param end Coordinate after the last character to read.
  std::string getText(const Coordinates& start, const Coordinates& end) const;

  /**
   * Resolve a full-buffer byte offset to editor coordinates.
   *
   * @param offset Byte offset in \ref getText().
   */
  Coordinates getCoordinatesAtByteOffset(std::size_t offset) const {
    return text_.getCoordinatesAtByteOffset(offset);
  }

  /// Report whether the text changed since the last reset; external source mirroring clears it.
  bool isTextChanged() const { return textChanged_; }
  /// Clear the text-change flag and recorded changed lines.
  void resetTextChanged() {
    textChanged_ = false;
    changedLines_.clear();
  }
  /// True if user-facing edits have pending byte-level source intents.
  bool hasPendingSourceEditIntents() const { return !pendingSourceEditIntents_.empty(); }
  /// Consume pending byte-level source intents captured from user-facing edits.
  std::vector<SourceEditIntent> takePendingSourceEditIntents();

  // ---------------------------------------------------------------------
  // Cursor / selection
  // ---------------------------------------------------------------------

  /// Return the cursor position clamped to the current buffer.
  Coordinates getCursorPosition() const { return getActualCursorCoordinates(); }
  /// Store a requested cursor position; reads clamp it to the current buffer.
  /// @param position Requested cursor position.
  void setCursorPosition(const Coordinates& position);

  /// Select between two endpoints, storing ordered bounds and expanding words or lines.
  /// @param start First requested endpoint.
  /// @param end Second requested endpoint.
  /// @param mode Boundary expansion mode.
  void setSelection(const Coordinates& start, const Coordinates& end,
                    SelectionMode mode = SelectionMode::Normal);
  /// Update the visible selection during an active mouse interaction while
  /// preserving the raw anchor/current endpoints in `interactiveStart_/End_`.
  void setInteractiveSelection(const Coordinates& start, const Coordinates& end,
                               SelectionMode mode = SelectionMode::Normal);
  /// Replace the ordered lower selection bound, swapping bounds if needed.
  /// @param position Requested lower bound.
  void setSelectionStart(const Coordinates& position);
  /// Replace the ordered upper selection bound, swapping bounds if needed.
  /// @param position Requested upper bound.
  void setSelectionEnd(const Coordinates& position);

  /// Return the ordered lower selection bound.
  const Coordinates& getSelectionStart() const { return state_.selectionStart; }
  /// Return the ordered upper selection bound.
  const Coordinates& getSelectionEnd() const { return state_.selectionEnd; }

  /// Report whether the selection spans any text.
  bool hasSelection() const;
  /// Return the text covered by the current selection.
  std::string getSelectedText() const;

  /// Select the entire text buffer.
  void selectAll();
  /// Select the word containing the cursor.
  void selectWordUnderCursor();

  // ---------------------------------------------------------------------
  // Navigation
  // ---------------------------------------------------------------------

  /// Move the cursor up by logical lines.
  /// @param amount Number of lines to move.
  /// @param select Whether to extend the selection.
  void moveUp(int amount = 1, bool select = false);
  /// Move the cursor down by logical lines.
  /// @param amount Number of lines to move.
  /// @param select Whether to extend the selection.
  void moveDown(int amount = 1, bool select = false);
  /// Move the cursor left by character steps.
  /// @param amount Number of steps to move.
  /// @param select Whether to extend the selection.
  /// @param wordMode Reserved; the current core moves by characters.
  void moveLeft(int amount = 1, bool select = false, bool wordMode = false);
  /// Move the cursor right by character steps.
  /// @param amount Number of steps to move.
  /// @param select Whether to extend the selection.
  /// @param wordMode Reserved; the current core moves by characters.
  void moveRight(int amount = 1, bool select = false, bool wordMode = false);
  /// Move the cursor to the start of the buffer.
  /// @param select Whether to extend the selection.
  void moveTop(bool select = false);
  /// Move the cursor to the start of the final line.
  /// @param select Whether to extend the selection.
  void moveBottom(bool select = false);
  /// Move the cursor to the start of its current line.
  /// @param select Whether to extend the selection.
  void moveHome(bool select = false);
  /// Move the cursor to the end of its current line.
  /// @param select Whether to extend the selection.
  void moveEnd(bool select = false);

  // ---------------------------------------------------------------------
  // Editing
  // ---------------------------------------------------------------------

  /// Insert one character at the cursor or replace the selection.
  /// @param character Unicode scalar to insert.
  /// @param shift Whether Shift is held for indentation behavior.
  void enterCharacter(char32_t character, bool shift);
  /// Insert text at the cursor, optionally applying indentation.
  /// @param text Text to insert.
  /// @param indent Whether to apply indentation to inserted lines.
  void insertText(std::string_view text, bool indent = false);
  /// Insert text at a coordinate and update that coordinate as lines are added.
  /// @param where Insertion point, updated to the end of the inserted text.
  /// @param text Text to insert.
  /// @param indent Whether to apply indentation to inserted lines.
  int insertTextAt(Coordinates& /* inout */ where, std::string_view text, bool indent = false);
  /// Delete the text between two coordinates.
  /// @param start First coordinate to delete.
  /// @param end Coordinate after the deleted range.
  void deleteRange(const Coordinates& start, const Coordinates& end);
  /// Delete the current selection and update the cursor.
  void deleteSelection();
  /// Remove the character before the cursor or the current selection.
  void backspace();
  /// Remove the character after the cursor or the current selection.
  void delete_();

  // ---------------------------------------------------------------------
  // Undo / redo
  // ---------------------------------------------------------------------

  /// Report whether an earlier edit can be undone.
  bool canUndo() const;
  /// Report whether a previously undone edit can be redone.
  bool canRedo() const;
  /// Undo the requested number of edits.
  /// @param steps Maximum number of edits to undo.
  void undo(int steps = 1);
  /// Redo the requested number of edits.
  /// @param steps Maximum number of edits to redo.
  void redo(int steps = 1);
  /// Append an edit to undo history and discard any redo tail.
  /// @param value Edit record to retain.
  void addUndo(UndoRecord& value);

  // ---------------------------------------------------------------------
  // Syntax highlighting / colorizer
  // ---------------------------------------------------------------------

  /// Set the syntax rules and schedule the buffer for recoloring.
  /// @param langDef Language definition to use.
  void setLanguageDefinition(const LanguageDefinition& langDef);
  /// Return the active syntax language definition.
  const LanguageDefinition& getLanguageDefinition() const { return languageDefinition_; }

  /// Schedule syntax coloring for a line range.
  /// @param fromLine First line to recolor.
  /// @param count Number of lines, or -1 for the remainder.
  void colorize(int fromLine = 0, int count = -1);
  /// Recolor the half-open line range [fromLine, toLine).
  /// @param fromLine First line in the range.
  /// @param toLine First line after the range.
  void colorizeRange(int fromLine = 0, int toLine = 0);
  /// Process the pending syntax-coloring range.
  void colorizeInternal();

  /// Report whether syntax coloring is enabled.
  bool isColorizerEnabled() const { return colorizerEnabled_; }
  /// Enable or disable syntax coloring.
  /// @param enabled Whether to colorize tokens.
  void setColorizerEnabled(bool enabled) { colorizerEnabled_ = enabled; }

  // ---------------------------------------------------------------------
  // Palette
  // ---------------------------------------------------------------------

  /// Set the base color palette used by the text editor.
  /// @param value Palette to store.
  void setPalette(const Palette& value) { paletteBase_ = value; }
  /// Return the configured base color palette.
  const Palette& getPalette() const { return paletteBase_; }

  // ---------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------

  /// Set the number of columns represented by one tab, clamped to 0 through 32.
  /// @param size Requested tab width.
  void setTabSize(int size);
  /// Return the configured tab width in columns.
  int getTabSize() const { return tabSize_; }

  /// Choose whether indentation inserts spaces rather than tabs.
  /// @param value True to insert spaces.
  void setInsertSpaces(bool value) { insertSpaces_ = value; }
  /// Report whether indentation inserts spaces.
  bool getInsertSpaces() const { return insertSpaces_; }

  /// Enable or disable context-sensitive indentation.
  /// @param value Whether smart indentation is active.
  void setSmartIndent(bool value) { smartIndent_ = value; }
  /// Enable or disable indentation of pasted lines.
  /// @param value Whether pasted lines are auto-indented.
  void setAutoIndentOnPaste(bool value) { autoIndentOnPaste_ = value; }
  /// Enable or disable automatic closing-brace insertion.
  /// @param value Whether matching braces are completed.
  void setCompleteBraces(bool value) { completeBraces_ = value; }
  /// Enable or disable active autocomplete requests.
  /// @param value Whether autocomplete is active.
  void setActiveAutocomplete(bool value) { activeAutocomplete_ = value; }
  /// Show or hide changed-line markers in the scrollbar.
  /// @param value Whether markers are shown.
  void setScrollbarMarkers(bool value) { scrollbarMarkers_ = value; }
  /// Expose the scrollbar-marker flag to the rendering shell.
  bool& scrollbarMarkersRef() { return scrollbarMarkers_; }

  // ---------------------------------------------------------------------
  // Error markers
  // ---------------------------------------------------------------------

  /// Replace the line-numbered diagnostic markers.
  /// @param markers Markers to display.
  void setErrorMarkers(const ErrorMarkers& markers) { errorMarkers_ = markers; }
  /// Return the current line-numbered diagnostic markers.
  const ErrorMarkers& getErrorMarkers() const { return errorMarkers_; }
  /// Expose diagnostic markers for shell-side edits.
  ErrorMarkers& mutableErrorMarkers() { return errorMarkers_; }

  // ---------------------------------------------------------------------
  // Word utilities
  // ---------------------------------------------------------------------

  /// Find the start of the word containing or preceding a coordinate.
  /// @param from Coordinate used to locate the word.
  Coordinates findWordStart(const Coordinates& from) const;
  /// Find the end of the word containing or following a coordinate.
  /// @param from Coordinate used to locate the word.
  Coordinates findWordEnd(const Coordinates& from) const;
  /// Find the next word boundary after a coordinate.
  /// @param from Coordinate where the search begins.
  Coordinates findNextWord(const Coordinates& from) const;
  /// Report whether a coordinate lies on a word boundary.
  /// @param at Coordinate to inspect.
  bool isOnWordBoundary(const Coordinates& at) const;

  /// Return the word at the current cursor position.
  RcString getWordUnderCursor() const;
  /// Return the word containing a coordinate.
  /// @param coords Coordinate within the word.
  RcString getWordAt(const Coordinates& coords) const;

  /// Find the first whole-word match at or after a coordinate.
  /// @param searchText Word to find.
  /// @param start Coordinate where the search begins.
  /// @return Match location, or (total line count, 0) when none exists.
  Coordinates findFirst(std::string_view searchText, const Coordinates& start) const;

  // ---------------------------------------------------------------------
  // Coordinate helpers
  // ---------------------------------------------------------------------

  /// Return the cursor clamped to the current buffer.
  Coordinates getActualCursorCoordinates() const;
  /// Clamp a coordinate to an existing line and column.
  /// @param value Coordinate to sanitize.
  Coordinates sanitizeCoordinates(const Coordinates& value) const;
  /// Advance a coordinate by one UTF-8 character, crossing lines as needed.
  /// @param coordinates Coordinate updated in place.
  void advance(Coordinates& coordinates) const;

  // ---------------------------------------------------------------------
  // Buffer / state accessors for the shell rendering code
  // ---------------------------------------------------------------------

  /// Expose the editable buffer for shell-side mutation.
  TextBuffer& buffer() { return text_; }
  /// Return the editable buffer for shell-side rendering.
  const TextBuffer& buffer() const { return text_; }

  /// Expose cursor and selection state for shell-side mutation.
  EditorState& mutableState() { return state_; }
  /// Return the current cursor and selection state.
  const EditorState& state() const { return state_; }

  /// Return the retained undo records in chronological order.
  const UndoBuffer& undoBuffer() const { return undoBuffer_; }
  /// Return the current position in the undo history.
  int undoIndex() const { return undoIndex_; }

  /// Expose changed-line markers for shell-side updates.
  std::vector<int>& changedLines() { return changedLines_; }
  /// Return the changed-line markers used by the scrollbar.
  const std::vector<int>& changedLines() const { return changedLines_; }

  /// Expose positions of opening fold markers.
  std::vector<Coordinates>& foldBegin() { return foldBegin_; }
  /// Return positions of opening fold markers.
  const std::vector<Coordinates>& foldBegin() const { return foldBegin_; }
  /// Expose positions of closing fold markers.
  std::vector<Coordinates>& foldEnd() { return foldEnd_; }
  /// Return positions of closing fold markers.
  const std::vector<Coordinates>& foldEnd() const { return foldEnd_; }
  /// Expose whether the fold-marker arrays are sorted.
  bool& foldSorted() { return foldSorted_; }

  /// Return the current character, word, or line selection mode.
  SelectionMode selectionMode() const { return selectionMode_; }
  /// Set how selection boundaries are expanded.
  /// @param mode Selection mode to use.
  void setSelectionMode(SelectionMode mode) { selectionMode_ = mode; }

  /// Expose the raw interactive selection anchor.
  Coordinates& interactiveStart() { return interactiveStart_; }
  /// Expose the raw interactive selection extent.
  Coordinates& interactiveEnd() { return interactiveEnd_; }

  /// Expose the active color palette to the rendering shell.
  Palette& mutablePalette() { return palette_; }
  /// Return the active color palette used for glyphs.
  const Palette& livePalette() const { return palette_; }
  /// Expose the configured base palette to the rendering shell.
  Palette& mutablePaletteBase() { return paletteBase_; }

  /// Report whether the shell should scroll the cursor into view.
  bool scrollToCursorRequested() const { return scrollToCursor_; }
  /// Clear a pending scroll-to-cursor request.
  void clearScrollToCursor() { scrollToCursor_ = false; }
  /// Request that the shell scroll the cursor into view.
  void requestScrollToCursor() { scrollToCursor_ = true; }
  /// Expose the scroll-to-cursor request flag to the shell.
  bool& scrollToCursorRef() { return scrollToCursor_; }

  /// Report whether the shell should scroll to the top.
  bool scrollToTopRequested() const { return scrollToTop_; }
  /// Clear a pending scroll-to-top request.
  void clearScrollToTop() { scrollToTop_ = false; }
  /// Expose the scroll-to-top request flag to the shell.
  bool& scrollToTopRef() { return scrollToTop_; }

  /// Report whether the cursor moved since the last shell update.
  bool cursorPositionChanged() const { return cursorPositionChanged_; }
  /// Set the cursor-position-change flag.
  /// @param value Whether the cursor has moved.
  void setCursorPositionChanged(bool value) { cursorPositionChanged_ = value; }
  /// Expose the cursor-position-change flag to the shell.
  bool& cursorPositionChangedRef() { return cursorPositionChanged_; }

  /// Return the active replacement index used by the text shell.
  int replaceIndex() const { return replaceIndex_; }
  /// Set the replacement index used by the text shell.
  /// @param index Replacement index to store.
  void setReplaceIndex(int index) { replaceIndex_ = index; }
  /// Expose the replacement index to the text shell.
  int& replaceIndexRef() { return replaceIndex_; }

  /// Expose the configured tab width for shell-side editing.
  int& tabSizeRef() { return tabSize_; }
  /// Expose the selection mode for shell-side editing.
  SelectionMode& selectionModeRef() { return selectionMode_; }
  /// Expose the text-change flag for shell-side editing.
  bool& textChangedRef() { return textChanged_; }
  /// Expose the paste-indentation setting for shell-side editing.
  bool& autoIndentOnPasteRef() { return autoIndentOnPaste_; }

  // Glyph coloring (used by shell rendering). Returns an `unsigned int`
  // that is identical to ImGui's `ImU32`.
  /// Return the active palette color for a glyph and its comment state.
  /// @param glyph Glyph whose syntax color is requested.
  unsigned int getGlyphColor(const Glyph& glyph) const;

  // Low-level line mutation helpers (used by shell + core, updating
  // fold/error-marker bookkeeping that moved into the core).
  /// Remove an inclusive range of lines and update error and changed-line markers.
  /// @param start First line to remove.
  /// @param end Last line to remove.
  void removeLine(int start, int end);
  /// Remove one line and update fold, marker, and change positions.
  /// @param index Line to remove.
  void removeLine(int index);
  /// Insert a line and update fold and marker positions.
  /// @param index Index of the inserted line.
  /// @param column Column where the preceding line is split.
  Line& insertLine(int index, int column);
  /// Remove fold markers in a coordinate range.
  /// @param start First coordinate in the range.
  /// @param end Last coordinate in the range.
  void removeFolds(const Coordinates& start, const Coordinates& end);
  /// Remove fold markers from one marker array in a coordinate range.
  /// @param folds Marker array to update.
  /// @param start First coordinate in the range.
  /// @param end Last coordinate in the range.
  void removeFolds(std::vector<Coordinates>& folds, const Coordinates& start,
                   const Coordinates& end);

  /// Infer whether the current text is indented with tabs or spaces.
  void detectIndentationStyle();

  // Per-character insertion helpers (threaded through UndoState).
  /// Insert a newline and record its undo state and indentation.
  /// @param state Undo state accumulated for this insertion.
  /// @param coord Insertion coordinate.
  /// @param smartIndent Whether context-sensitive indentation is enabled.
  void handleNewLine(UndoState& state, const Coordinates& coord, bool smartIndent);
  /// Insert a regular character and update its undo state.
  /// @param state Undo state accumulated for this insertion.
  /// @param coord Insertion coordinate.
  /// @param character Character to insert.
  void handleRegularCharacter(UndoState& state, const Coordinates& coord, char32_t character);
  /// Delete the line break at the end of a line and record the edit.
  /// @param pos Deletion coordinate.
  /// @param undo Undo record updated with the removed text.
  void handleEndOfLineDelete(Coordinates pos, UndoRecord& undo);
  /// Delete a character within a line and record the edit.
  /// @param pos Deletion coordinate.
  /// @param undo Undo record updated with the removed text.
  void handleMidLineDelete(Coordinates pos, UndoRecord& undo);
  /// Delete at the start of a line and record the edit.
  /// @param pos Deletion coordinate.
  /// @param undo Undo record updated with the removed text.
  void handleStartOfLineDelete(const Coordinates& pos, UndoRecord& undo);
  /// Backspace within a line and record the edit.
  /// @param pos Cursor coordinate.
  /// @param undo Undo record updated with the removed text.
  void handleMidLineBackspace(const Coordinates& pos, UndoRecord& undo);
  /// Convert an undo record into a byte-level source edit intent.
  /// @param record Edit whose text and bounds are translated.
  /// @param kind Whether the source intent applies or reverses the edit.
  void recordSourceEditIntent(const UndoRecord& record, SourceEditIntentKind kind);
  /// Queue a nonempty, distinct source edit intent with a new version and notify the shell.
  /// @param intent Source edit intent to queue.
  void appendSourceEditIntent(SourceEditIntent intent);
  /// Indent or outdent selected lines and update the undo state.
  /// @param state Undo state accumulated for the operation.
  /// @param shift True to outdent instead of indenting.
  void handleMultiLineTab(UndoState& state, bool shift);

  /// Mark the cursor line changed when scrollbar markers are enabled.
  void updateChangeTracking();

  // ---------------------------------------------------------------------
  // Shell hooks
  // ---------------------------------------------------------------------
  //
  // A handful of moved methods (`backspace`, `enterCharacter`, etc.)
  // historically invoked helpers that live in the ImGui shell -
  // `ensureCursorVisible` pokes the ImGui scroll state, function tooltips
  // and brace completion read shell-only configuration. The core invokes
  // these via `std::function` hooks set by `TextEditor`. When unset (e.g.
  // headless tests after C3), the hooks are null and no-op, which matches
  // today's behavior where `ensureCursorVisible` early-returns outside
  // `withinRender_`.

  std::function<void()>
      ensureCursorVisibleHook;  //!< Ask the shell to scroll the current cursor into view.
  std::function<void()>
      requestAutocompleteHook;  //!< Ask the shell to update autocomplete suggestions.
  std::function<void(char32_t, const Coordinates&)>
      functionTooltipHook;  //!< Notify the shell of a character that may open a function tooltip.
  std::function<void()>
      onContentUpdateInternal;  //!< Notify the shell that editable content changed.
  std::function<void(const SourceEditIntent&)>
      sourceEditIntentHook;  //!< Notify the shell when a byte-level source edit intent is queued.

private:
  using RegexList = std::vector<std::pair<std::regex, ColorIndex>>;

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  TextBuffer text_;
  EditorState state_;
  UndoBuffer undoBuffer_;
  int undoIndex_ = 0;
  std::vector<SourceEditIntent> pendingSourceEditIntents_;
  std::uint64_t sourceEditIntentVersion_ = 0;
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
