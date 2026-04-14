# Design: TextEditor Widget Behavior Audit

**Status:** Analysis
**Author:** Claude Haiku 4.5
**Created:** 2026-04-12

## Summary

The Donner `TextEditor` widget (1296 lines of header, 4130 lines of implementation) is a port of ImGuiColorTextEdit-style code editor built on Dear ImGui. This document audits its standard text-editor behaviors against VS Code / TextMate / Xcode baseline, inventories implemented features, identifies bugs and deviations, and proposes a test suite to ensure correctness across 14 behavior categories (selection, navigation, editing, undo/redo, clipboard, find/replace, mouse, IME, scrolling, auto-features, multi-cursor, go-to-line, line operations, and case operations).

## Goals

- Provide a comprehensive behavioral inventory of TextEditor, organized by category, with specific `file:line` citations.
- Identify deviations from standard editor behavior (e.g., Alt+arrow word movement not wired, Ctrl+Delete / Ctrl+Backspace word deletion unimplemented, word-mode shortcuts declared but unused).
- Catalog bug-level issues and missing features with priority (P0 = critical, P1 = important, P2 = nice-to-have).
- Establish test coverage baseline and identify gaps.
- Propose a concrete, prioritized Implementation Plan as a checklist of specific test cases (25-40 tests in first batch).

## Non-Goals

- Full refactoring or bug fixes (that is separate work).
- Support for language-specific features (custom syntax highlighting, snippets, etc.) beyond basic infrastructure.
- Multi-document editing or advanced IDE features.
- Undo history UI or visualization.

## Background

### Current State

The TextEditor is a core Donner widget used by the editor application for SVG text editing. It supports:
- **Basic text operations**: insertion, deletion, selection, copy/paste.
- **Undo/redo**: per-keystroke granularity with EditorState snapshots.
- **Syntax highlighting**: regex-based colorization with custom language definitions.
- **Code folding**: collapsible code regions.
- **Keyboard shortcuts**: customizable via Shortcut struct, wired via ShortcutId enum.
- **Mouse interaction**: single/double/triple-click, drag selection, wheel scroll.
- **Auto-features**: bracket completion, auto-indent, smart indentation.
- **Search/replace**: find/replace dialogs (basic).

### Baseline Expectations

VS Code / TextMate / Xcode set user expectations for:
- **Arrow keys + Shift**: character-by-character selection.
- **Ctrl+Arrow (Cmd+Arrow on Mac)**: word boundary movement.
- **Home/End**: move to line start/end.
- **Ctrl+Home/End (Cmd+Home/End)**: move to document start/end.
- **Double-click**: select word at cursor.
- **Triple-click**: select entire line.
- **Backspace/Delete**: remove character or merged lines, undo records each keystroke.
- **Copy with no selection**: copy entire line.
- **Multi-line paste**: insert with proper indentation.

### Known Deviations

From code inspection (`TextEditor.cc` lines 2962–3100):
- **Word movement shortcuts declared but unused** (lines 70, 73, 77, 80): `MoveWordLeft`, `SelectWordLeft`, `MoveWordRight`, `SelectWordRight` are in `ShortcutId` enum and wired in `getDefaultShortcuts()` to Ctrl+Left/Right, but the implementation of `moveLeft()` and `moveRight()` (lines 2962, 2995) **ignore the `wordMode` parameter** (comment `/*wordMode*/` at line 2962).
- **Ctrl+Delete / Ctrl+Backspace not implemented**: shortcuts exist (lines 113, 116) but `executeAction()` (lines 830–874) has no handlers for `ForwardDeleteWord` or `BackwardDeleteWord`.
- **Unindent (Shift+Tab) not fully wired**: shortcut exists (line 194) but `executeAction()` doesn't handle it (lines 830–874).
- **Word boundary detection**: uses syntax-highlighting color changes as word boundaries (lines 423–436, 441–479), which is different from alphanumeric boundaries in VS Code.
- **Tab stop at start of document**: `moveDown()` on empty document or last line doesn't clamp correctly (line 3046 moves to `(totalLines-1, 0)` but doesn't check if that's valid).

## Implementation Plan

### Milestone 1: P0 Behaviors (Critical Correctness)

These form the foundation of any text editor and must work reliably.

- [ ] **Cursor Movement (Position & Selection)**
  - [ ] Test: Single arrow-key movements (up/down/left/right) by 1 step, verify cursor position.
  - [ ] Test: Cursor wraps to next line on right-arrow at line end.
  - [ ] Test: Cursor wraps to previous line on left-arrow at line start.
  - [ ] Test: Up-arrow on first line stays at first line.
  - [ ] Test: Down-arrow on last line stays at last line.
  - [ ] Test: Home key moves cursor to column 0 of current line.
  - [ ] Test: End key moves cursor to end of current line.
  - [ ] Test: Ctrl+Home (MoveTop) moves to (0, 0).
  - [ ] Test: Ctrl+End (MoveBottom) moves to (last_line, 0).
  - [ ] Test: Page Up / Page Down move by page size.

- [ ] **Selection via Keyboard (Shift+Arrow)**
  - [ ] Test: Shift+Right expands selection rightward by 1 column.
  - [ ] Test: Shift+Left contracts selection or reverses direction.
  - [ ] Test: Shift+End selects to end of line.
  - [ ] Test: Shift+Home selects to start of line.
  - [ ] Test: Shift+Ctrl+Home selects to document start.
  - [ ] Test: Shift+Ctrl+End selects to document end.
  - [ ] Test: Ctrl+A selects all text.

- [ ] **Insertion & Deletion**
  - [ ] Test: Type a character; cursor advances; text appears in buffer.
  - [ ] Test: Type multiple characters in sequence.
  - [ ] Test: Backspace deletes character before cursor.
  - [ ] Test: Delete deletes character at cursor.
  - [ ] Test: Backspace at line start merges with previous line.
  - [ ] Test: Delete at line end merges with next line.
  - [ ] Test: Backspace on first character of file does nothing.
  - [ ] Test: Delete on last character of last line does nothing.
  - [ ] Test: Typing over a selection deletes selection and inserts character.
  - [ ] Test: Enter key inserts newline and positions cursor on new line.

- [ ] **Undo / Redo**
  - [ ] Test: Ctrl+Z undo reverts last character insertion.
  - [ ] Test: Ctrl+Z undo reverts deletion.
  - [ ] Test: Multiple Ctrl+Z steps undo back through history.
  - [ ] Test: Ctrl+Y redo restores undone changes.
  - [ ] Test: Redo is cleared when new text is typed after undo.
  - [ ] Test: Undo/redo preserves cursor position (EditorState).

- [ ] **Double-Click Word Selection**
  - [ ] Test: Double-click on a word selects entire word (using findWordStart/findWordEnd).
  - [ ] Test: Double-click on `red` inside `fill="red"` selects just `red` (respects color boundaries, lines 404–479).
  - [ ] Test: Double-click on punctuation selects punctuation run.
  - [ ] Test: Double-click on whitespace selects whitespace run.

- [ ] **Multi-Line Editing**
  - [ ] Test: Paste multi-line text (containing \n) inserts correctly.
  - [ ] Test: Delete across multiple lines removes text and merges.
  - [ ] Test: Selection spanning multiple lines can be deleted.

### Milestone 2: P1 Behaviors (Expected Features)

- [ ] **Word Movement (Ctrl+Arrow)**
  - [ ] Test: Ctrl+Left moves cursor to start of previous word.
  - [ ] Test: Ctrl+Right moves cursor to start of next word.
  - [ ] Test: Ctrl+Shift+Left selects to start of previous word.
  - [ ] Test: Ctrl+Shift+Right selects to end of current / start of next word.
  - [ ] Test: Word boundary respects syntax color transitions (lines 423–436, 461–479).

- [ ] **Word Deletion**
  - [ ] Test: Ctrl+Backspace deletes word to the left of cursor.
  - [ ] Test: Ctrl+Delete deletes word to the right of cursor.
  - [ ] Test: Deletion records undo entry.

- [ ] **Tab & Indentation**
  - [ ] Test: Tab key inserts \t or spaces (depends on insertSpaces_ setting).
  - [ ] Test: Shift+Tab (Unindent) reduces indentation if selection or at line start.
  - [ ] Test: Multi-line tab: tab with multi-line selection indents all lines.
  - [ ] Test: Auto-indent on Enter preserves indentation of previous line (if smartIndent_).

- [ ] **Clipboard Operations**
  - [ ] Test: Copy with selection copies selected text to clipboard.
  - [ ] Test: Copy without selection copies entire line.
  - [ ] Test: Cut removes text and copies to clipboard.
  - [ ] Test: Paste inserts clipboard text at cursor.
  - [ ] Test: Paste after selection replaces selection.

- [ ] **Find & Replace (Basic)**
  - [ ] Test: Ctrl+F opens find dialog.
  - [ ] Test: Ctrl+H opens replace dialog.
  - [ ] Test: Find next (Ctrl+G) moves selection to next match.
  - [ ] Test: Replace single occurrence.
  - [ ] Test: Replace all occurrences.

- [ ] **Mouse Interactions**
  - [ ] Test: Single click positions cursor and clears selection.
  - [ ] Test: Click + drag selects range.
  - [ ] Test: Shift + click extends selection.
  - [ ] Test: Triple-click selects entire line.
  - [ ] Test: Mouse wheel scrolls viewport.

### Milestone 3: P2 Behaviors & Edge Cases

- [ ] **Bracket Matching & Auto-Close**
  - [ ] Test: Typing `{` auto-inserts `}` if completeBraces_ enabled.
  - [ ] Test: Typing `"` auto-inserts closing `"`.
  - [ ] Test: Backspace over matched pair removes both.

- [ ] **Bracket Highlighting**
  - [ ] Test: Cursor adjacent to `(` highlights matching `)`.

- [ ] **Multi-Cursor / Column Selection**
  - [ ] Test: Alt+click inserts additional cursor (infrastructure exists in ShortcutId but not fully wired).
  - [ ] Test: Alt+drag creates column selection.

- [ ] **Go-to-Line**
  - [ ] Test: User command to jump to line N positions cursor at (N, 0).

- [ ] **Find Under Cursor (Cmd+E)**
  - [ ] Test: Shortcut selects word under cursor and populates find field.

- [ ] **Line Operations**
  - [ ] Test: Ctrl+D duplicates current line.
  - [ ] Test: Ctrl+Shift+K deletes current line.
  - [ ] Test: Alt+Up/Down moves line up/down (not wired in defaults).

- [ ] **Case Operations**
  - [ ] Test: Shortcut toggles selection case (not fully implemented).

- [ ] **Comment Toggling**
  - [ ] Test: Ctrl+/ toggles single-line comment.

- [ ] **UTF-8 & Multi-Byte Characters**
  - [ ] Test: Insertion of multi-byte UTF-8 character (e.g., emoji) is handled correctly.
  - [ ] Test: Arrow keys navigate UTF-8 boundaries correctly (lines 426, 459 check `(c & 0xC0) != 0x80`).
  - [ ] Test: Backspace / Delete correctly handle multi-byte sequences.
  - [ ] Test: Regex word boundaries work with non-ASCII characters.

- [ ] **Horizontal Scrolling**
  - [ ] Test: Long lines scroll horizontally when cursor moves beyond view.
  - [ ] Test: Mouse drag near edge auto-scrolls viewport (lines 1069–1084).

- [ ] **Coordinate System Consistency**
  - [ ] Test: Coordinates returned by getSelection/getCursorPosition are consistent with setSelection/setCursorPosition.
  - [ ] Test: Tab expansion is consistent (line.column vs character index, getLineMaxColumn vs getCharacterIndex).

- [ ] **Empty Buffer & Edge Cases**
  - [ ] Test: New TextEditor has single empty line.
  - [ ] Test: Operations on empty file don't crash (null checks).
  - [ ] Test: Delete on empty buffer does nothing.
  - [ ] Test: Backspace on empty buffer does nothing.

## Bug List (from Code Inspection)

### P0 (Critical)

| Issue | File:Line | Observed | Expected | Severity |
|-------|-----------|----------|----------|----------|
| **wordMode parameter unused in moveLeft/moveRight** | TextEditor.cc:2962, 2995 | Ctrl+Left/Right don't move by word; they move by character | Ctrl+Left/Right should skip by word boundary (alphanumeric or color boundary) | P0 |
| **Unindent (Shift+Tab) not handled** | executeAction() lines 830–874 | Shift+Tab does nothing in editor (no case for ShortcutId::Unindent) | Should reduce indentation or shift selection left | P0 |
| **Ctrl+Delete / Ctrl+Backspace not implemented** | executeAction() lines 830–874, backspace()/delete_() | Ctrl+Delete and Ctrl+Backspace are wired as shortcuts but do nothing | Should delete word to right/left | P0 |

### P1 (Important)

| Issue | File:Line | Observed | Expected | Severity |
|-------|-----------|----------|----------|----------|
| **moveBottom() doesn't set cursor to end of last line** | TextEditor.cc:3044–3056 | Cursor moves to (lastLine, 0), not end of last line | Cursor should move to end of last line content | P1 |
| **Copy without selection behavior inconsistent** | copy() lines 3360–3376 | Copies entire current line if no selection | Some editors don't copy if no selection; behavior should be configurable or documented | P1 |
| **Delete at end of line (handleEndOfLineDelete) merges with next line silently** | handleEndOfLineDelete() lines 3100–3131 | No visual feedback; can be surprising | Consider auto-scroll or explicit feedback | P1 |
| **Selection mode not reset after operations** | setSelection() lines 1396–1454 | SelectionMode (Normal/Word/Line) can persist after non-selection operations | Should reset to Normal after certain ops | P1 |

### P2 (Nice-to-have / Polish)

| Issue | File:Line | Observed | Expected | Severity |
|-------|-----------|----------|----------|----------|
| **Multi-line comment detection incomplete** | colorizeInternal() lines 3608+ | Comment nesting, conditional compilation directives not fully recognized | Robust multi-line comment pairing | P2 |
| **Code folding state not preserved across edits** | calculateFolds() lines 1196+ | Folds may become inconsistent after large edits | Folding should track through edits (complex) | P2 |
| **Auto-indent on paste not fully implemented** | autoIndentOnPaste_ setting exists but not used in paste() | autoIndentOnPaste_ flag is dead code | Should adjust pasted lines' indentation | P2 |
| **Function tooltip disabled by default** | functionDeclarationTooltipEnabled_ = false (line 964) | Feature incomplete | Either fully implement or remove | P2 |
| **Snippet support incomplete** | Snippet-related members (1004–1011) | Snippet insertion exists but not wired to autocomplete | Should integrate with autocomplete selection | P2 |

## Test Coverage Inventory

### Existing Tests (`TextBuffer_tests.cc`, 162 lines)

**Coverage:**
- TextBuffer construction (default, load text).
- Multi-line text handling.
- getText(start, end) range extraction.
- insertTextAt() with newlines.
- deleteRange() single-line and multi-line.
- removeLine() single and range.
- Line/column helpers (getLineMaxColumn, getCharacterIndex, getCharacterColumn).

**Gaps:**
- No TextEditor-level tests (only TextBuffer).
- No keyboard shortcuts or navigation.
- No selection semantics (only TextBuffer operations).
- No undo/redo.
- No clipboard.
- No multi-cursor or advanced selection modes.
- No UTF-8 boundary handling in TextEditor context.
- No double-click word selection (findWordStart/findWordEnd).
- No auto-indent or bracket completion.

## Testing and Validation

### Test Plan

**First batch: 25–40 P0 and P1 tests** in `TextEditor_tests.cc`:
- Use public TextEditor API: `setText()`, `setSelection()`, `setCursorPosition()`, `moveLeft()`, `moveRight()`, `getText()`, `getSelectedText()`, `getCursorPosition()`, `copy()`, `paste()`, `undo()`, `redo()`, `backspace()`, `delete_()`, `insertText()`, etc.
- Test via direct API calls, not ImGui input events (those are hard to simulate without a render context).
- For mouse-based behaviors (double-click), call findWordStart/findWordEnd directly (as documented in comment, lines 1011–1020).
- Use **CamelCase test names** per Googletest style (no underscores): `TEST(TextEditorTests, CursorMovesRightOnArrowKey)`.
- Include **specific error messages** in assertions for clarity (TestBot style).
- Tests must **not use malloc or exceptions** (per CLAUDE.md).

**Coverage by category:**
- Cursor movement (8–10 tests): arrow keys, Home/End, Ctrl+Home/End, page up/down.
- Selection (7–8 tests): shift+arrow, extend/contract, Ctrl+A.
- Insertion & deletion (6–8 tests): character insertion, backspace, delete, line merging, selection replacement.
- Undo/redo (5–6 tests): single and multiple steps, state preservation.
- Double-click (2–3 tests): word selection, color-boundary detection.
- Multi-line (2–3 tests): paste, delete across lines.
- Clipboard (4–5 tests): copy/cut/paste, no-selection behavior.

### Build & Run

```bash
bazel test //donner/editor/tests:text_editor_tests
```

### Validation Criteria

- All P0 tests pass (cursor movement, selection, insertion, deletion, undo/redo, double-click).
- P1 tests capture known bugs (word movement, word deletion, unindent) and fail until fixes land.
- Code coverage of TextEditor public API exceeds 70% for navigation, editing, and selection paths.
- No malloc, no exceptions, no undefined behavior (verified by AddressSanitizer / UBSan in CI).

## Next Steps

1. **Write test file** `donner/editor/tests/TextEditor_tests.cc` with 25–40 tests covering P0/P1 behaviors.
2. **Add BUILD.bazel target** `text_editor_tests` to `donner/editor/tests/BUILD.bazel`.
3. **Run tests** via `bazel test //donner/editor/tests:text_editor_tests` and document failures.
4. **Create follow-up issues** for each P0 bug (word movement, unindent, Ctrl+Delete/Ctrl+Backspace) and P1 polish items.
5. **Refactor word movement** to actually use `findWordStart()` / `findNextWord()` helpers and respect word boundaries.
6. **Implement word deletion** (Ctrl+Delete / Ctrl+Backspace) in `executeAction()` and add helper functions.

## Deviations from Standard Editor Behavior

### Alt+Arrow (Word Movement on macOS)

**Baseline (VS Code / TextMate / Xcode):** Alt+Left/Right moves by word boundary.

**Donner:** Not implemented. On macOS, Donner uses Ctrl for word movement (per `getDefaultShortcuts()` line 70–80), which aligns with VS Code on macOS (Cmd on Mac, Ctrl on Windows/Linux). However, **the wordMode parameter is ignored**, so all movement is character-by-character.

### Copy Without Selection

**Baseline:** Varies. VS Code copies line. Some editors require explicit selection.

**Donner:** Always copies entire current line if no selection (line 3367–3375).

### Auto-Indent on Paste

**Baseline:** VS Code and TextMate auto-indent pasted text to match surrounding indentation.

**Donner:** Setting exists (`autoIndentOnPaste_`) but is never used (dead code).

### Bracket Matching Word Boundary

**Baseline:** Word boundaries are alphanumeric (underscore optionally included).

**Donner:** Uses syntax-highlighting color changes as boundaries (lines 423–436, 461–479). This is useful for selecting syntax tokens but differs from alphanumeric words. Example: in `fill="red"`, double-clicking selects `red` alone (respects quote color boundary), not `fill` or `"red"`.

## Appendix: File:Line Reference

### Key Functions

| Function | File:Line | Purpose |
|----------|-----------|---------|
| `moveLeft()` | TextEditor.cc:2962 | Cursor left; unused wordMode param |
| `moveRight()` | TextEditor.cc:2995 | Cursor right; unused wordMode param |
| `moveUp()` | TextEditor.cc:2901 | Cursor up by line |
| `moveDown()` | TextEditor.cc:2924 | Cursor down by line |
| `moveTop()` | TextEditor.cc:3029 | Jump to doc start (Ctrl+Home) |
| `moveBottom()` | TextEditor.cc:3044 | Jump to doc end (Ctrl+End) |
| `moveHome()` | TextEditor.cc:3058 | Jump to line start (Home) |
| `moveEnd()` | TextEditor.cc:3079 | Jump to line end (End) |
| `backspace()` | TextEditor.cc:3300 | Delete char before cursor |
| `delete_()` | TextEditor.cc:3147 | Delete char at cursor or merge lines |
| `copy()` | TextEditor.cc:3360 | Copy selection or line |
| `cut()` | TextEditor.cc:3378 | Cut selection to clipboard |
| `paste()` | TextEditor.cc:3396 | Paste clipboard at cursor |
| `undo()` | TextEditor.cc:3430 | Undo last operation |
| `redo()` | TextEditor.cc:3436 | Redo last undone operation |
| `selectAll()` | TextEditor.cc:3348 | Select (0, 0) to EOF |
| `setSelection()` | TextEditor.cc:1396 | Set and update selection state |
| `findWordStart()` | TextEditor.cc:404 | Find word start using color boundaries |
| `findWordEnd()` | TextEditor.cc:441 | Find word end using color boundaries |
| `findNextWord()` | TextEditor.cc:481 | Find start of next word |
| `getDefaultShortcuts()` | TextEditor.cc:56 | Hardcoded keyboard shortcut bindings |
| `executeAction()` | TextEditor.cc:830 | Execute shortcut action (missing handlers) |
| `handleKeyboardInputs()` | TextEditor.cc:876 | Dispatch keyboard events to shortcuts |
| `handleMouseInputs()` | TextEditor.cc:975 | Handle single/double/triple-click and drag |
| `backspace()` (header) | TextEditor.h:917 | Documentation of backspace semantics |
| `delete_()` (header) | TextEditor.h:538 | Documentation of delete semantics |

---

**End of Design Doc**
