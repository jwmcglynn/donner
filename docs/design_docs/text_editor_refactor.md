# Design: TextEditor Refactor — Headless Core + ImGui Shell

**Status:** Draft
**Author:** Claude Opus 4.6 (1M context)
**Created:** 2026-04-12

## Summary

The `donner::editor::TextEditor` widget is a 5400-line monolith ported from
`ImGuiColorTextEdit`. It mixes six conceptually separate concerns in one
class: text state, editing operations, undo/redo, syntax highlighting,
autocomplete, find/replace, ImGui-based keyboard/mouse input capture, and
ImGui rendering. The 88 member functions on `TextEditor` span all of them.

This coupling has concrete, immediate costs:

1. **Tests must create an ImGui context.** Today's `TextEditor_tests.cc`
   crashes without `ImGui::CreateContext()` + `io.Fonts->Build()` in the
   fixture, because `copy()` / `cut()` / `paste()` route through
   `ImGui::GetClipboardText()` and editing methods implicitly need a
   context for font metrics. One missing `Fonts->Build()` call in an
   unrelated test would land a segfault in CI.

2. **Private-member test access requires friend hacks.** The real
   user-facing editing paths (`enterCharacter`, `backspace`,
   `handleRegularCharacter`) are private because they're only supposed
   to be called from the ImGui keyboard-input loop. Tests that want to
   exercise them have to add `friend class TextEditorTests` to the
   header — test-only coupling that accumulates.

3. **Behavior bugs are hard to isolate.** The undo/redo system, the
   cursor state machine, and the rendering pipeline all share `state_`.
   The 13 failing tests in today's commit each need a debugger walk
   through multiple subsystems to pin root cause.

4. **Refactoring is scary.** Any change risks breaking an obscure
   interaction between rendering state and editing state.

This design extracts a **`TextEditorCore`** class that owns the
headless editing substrate (text buffer, cursor/selection state, undo
history, editing operations, language-agnostic syntax coloring) and
exposes it through a pure C++ API with no ImGui dependencies. The
existing `TextEditor` class becomes a thin ImGui shell that owns a
`TextEditorCore`, adds the ImGui input-capture layer, the ImGui
rendering layer, and the ImGui clipboard bridge, and forwards editing
operations to the core.

This is a **mechanical** refactor. The behavior of `TextEditor` does
not change — every method that exists today keeps working. What
changes is *where* the code lives and what it depends on.

## Goals

- **Headless `TextEditorCore`** — a class with zero ImGui dependencies
  that exposes every editing operation (`moveLeft`, `moveRight`,
  `enterCharacter`, `backspace`, `delete_`, `copy`, `cut`, `paste`,
  `undo`, `redo`, `selectWordUnderCursor`, etc.) as public methods.
  Verified by: `TextEditorCore_tests.cc` builds and runs without
  `ImGui::CreateContext()`.
- **Every editing test is headless.** The refactor moves the current
  73 tests (minus any that genuinely need ImGui rendering) to
  `TextEditorCore_tests.cc`, which has no ImGui context. The remaining
  ImGui-specific tests (rendering, input capture) stay in
  `TextEditor_tests.cc` with a context.
  Verified by: `TextEditorCore_tests.cc`'s fixture does not include
  `imgui.h` or call `ImGui::CreateContext`.
- **No friend-class declarations in production headers.**
  `TextEditorCore` exposes the editing API as `public`. Tests call
  those methods directly. The current `friend class TextEditorTests`
  declarations get removed.
  Verified by: grep for `friend class` in `TextEditorCore.h` returns
  zero matches.
- **Clipboard is abstracted behind an interface.** A small
  `ClipboardInterface` (3 methods: `getText`, `setText`, `hasText`)
  lets `TextEditorCore` call out to ImGui in production and to an
  in-memory fake in tests.
  Verified by: `TextEditorCore::copy()` doesn't reference `ImGui::`.
- **No behavior regressions.** The existing 60 passing tests still
  pass. The existing 13 failing tests still fail (they expose real
  bugs; fixing them is a separate workstream) — the refactor doesn't
  hide or change those failures.
  Verified by: `bazel test //donner/editor/tests/...` green for the
  previously-passing set.
- **Single-commit-revertible.** The refactor lands as a sequence of
  small commits, each preserving `bazel build //donner/editor/... &&
  bazel test //donner/editor/tests:...`. No multi-commit "now it's
  broken, now it works again" sequence.

## Non-Goals

- **Fixing the 13 failing-test bugs.** The refactor is mechanical; bug
  fixes come in follow-up commits once the extracted code is in its
  own file.
- **Rewriting the ImGui rendering code.** `renderInternal`,
  `renderLine`, `renderCursor`, etc. stay as-is in `TextEditor.cc`.
  The refactor doesn't touch them.
- **Replacing the undo system.** The existing `UndoRecord` +
  `addUndo` + `undo` + `redo` machinery moves to `TextEditorCore`
  unchanged. A better undo model (command pattern, incremental diffs)
  is Future Work.
- **Decoupling syntax highlighting from editing.** The colorize
  pipeline stays inside `TextEditorCore` for now — its input is the
  text buffer and its output is glyph colors, all of which lives in
  the core. The imgui-rendering code reads colors from glyphs but
  doesn't compute them. Extracting a standalone `SyntaxHighlighter`
  class is Future Work.
- **Multi-cursor, column selection, LSP, tree-sitter.** Out of scope.

## Next Steps

1. Land the three-commit refactor (core extraction, clipboard
   abstraction, test migration). Each commit is small enough to
   review in under 10 minutes.
2. Once `TextEditorCore` exists, begin fixing the 13 failing tests in
   a follow-up — now easier because the bugs have a narrower file to
   live in.
3. Remove the friend-class declarations from `TextEditor.h` in the
   migration commit.

## Implementation Plan

The refactor is three independent commits. Each is reviewable and
testable on its own.

### Commit 1: Extract `TextEditorCore` — move editing state + ops

- [ ] Create `donner/editor/TextEditorCore.{h,cc}`.
- [ ] Move the following fields from `TextEditor` to
      `TextEditorCore`:
  - Text buffer (`text_` — already a `TextBuffer`)
  - Cursor/selection state (`state_: EditorState`)
  - Interactive selection tracking (`interactiveStart_`,
    `interactiveEnd_`, `selectionMode_`)
  - Undo/redo stack (`undoBuffer_`, `undoIndex_`)
  - Colorize state (`colorRangeMin_`, `colorRangeMax_`,
    `checkComments_`, `regexList_`, `languageDefinition_`)
  - Change tracking (`textChanged_`)
  - Indent settings (`tabSize_`, `insertSpaces_`, `indentMode_`)
- [ ] Move the following methods (copy, then remove from
      `TextEditor`):
  - `setText` / `getText`
  - `setCursorPosition` / `getCursorPosition`
  - `setSelection` / `getSelection` / `hasSelection` /
    `getSelectedText` / `selectAll`
  - `moveLeft` / `moveRight` / `moveUp` / `moveDown`
  - `enterCharacter` / `backspace` / `delete_` / `insertText` /
    `deleteSelection` / `deleteRange`
  - `undo` / `redo` / `canUndo` / `canRedo` / `addUndo`
  - `selectWordUnderCursor` / `findWordStart` / `findWordEnd`
  - `isTextChanged` / `resetTextChanged`
  - `setLanguageDefinition` / `colorize` / `colorizeRange` /
    `colorizeInternal`
  - `setPalette`
- [ ] `TextEditor` holds a `TextEditorCore core_` member and forwards
      every ported method via a one-line wrapper:
      ```cpp
      void TextEditor::setText(string_view t) { core_.setText(t); }
      ```
      (This keeps the public `TextEditor` API identical so callers
      don't need changes.)
- [ ] Build: `bazel build //donner/editor:text_editor`.
- [ ] Tests: `bazel test //donner/editor/tests:all` — same 60 pass /
      13 fail as before.

### Commit 2: Abstract the clipboard

- [ ] Add `donner/editor/ClipboardInterface.h`:
      ```cpp
      class ClipboardInterface {
       public:
        virtual ~ClipboardInterface() = default;
        virtual std::string getText() const = 0;
        virtual void setText(std::string_view text) = 0;
        virtual bool hasText() const = 0;
      };
      ```
- [ ] Add `ImGuiClipboard` implementation in
      `donner/editor/TextEditor.cc` (anonymous namespace) that calls
      `ImGui::GetClipboardText` / `ImGui::SetClipboardText`.
- [ ] Add `InMemoryClipboard` implementation in
      `donner/editor/tests/InMemoryClipboard.h` for test use.
- [ ] `TextEditorCore` holds a `ClipboardInterface*` (non-owning
      pointer, injected at construction or via a setter). Default is
      `nullptr`, which makes `copy()` / `cut()` / `paste()` no-ops
      — acceptable because production always injects the ImGui
      clipboard before use.
- [ ] `TextEditor`'s constructor injects the `ImGuiClipboard` into
      its `core_`.
- [ ] Migrate `copy()`, `cut()`, `paste()` from `TextEditor` to
      `TextEditorCore` — they call through the interface instead of
      `ImGui::`.
- [ ] Build + existing tests stay green.

### Commit 3: Migrate tests to headless core

- [ ] Create `donner/editor/tests/TextEditorCore_tests.cc`.
- [ ] Move every test from `TextEditor_tests.cc` that doesn't need
      ImGui rendering/input-capture (basically all 73 of them in the
      current file) over to the new file. They now construct a
      `TextEditorCore` directly — no ImGui context, no fonts, no
      display size.
- [ ] `TextEditor_tests.cc` shrinks to just the rendering-path tests
      (if any; may end up empty for now) and keeps the ImGui context
      fixture for them.
- [ ] Remove `friend class TextEditorTests` and friends from
      `TextEditor.h`.
- [ ] BUILD.bazel: new `text_editor_core_tests` target that depends
      on `//donner/editor:text_editor_core` and
      `//donner/editor/tests:in_memory_clipboard`. No imgui dep.
- [ ] `bazel test //donner/editor/tests:text_editor_core_tests` —
      fixture builds a `TextEditorCore`, injects an
      `InMemoryClipboard`, runs the tests. Same pass/fail ratio.

## Proposed Architecture

### Dependency graph (after)

```
    donner/editor/TextEditorCore (headless)
         ↑              ↑
         |              |
         |        donner/editor/tests/TextEditorCore_tests
         |                  ↓
         |          InMemoryClipboard (test)
         |
    donner/editor/TextEditor (ImGui shell)
         ↓
        @imgui
```

`TextEditorCore` has no outbound dependency on ImGui, fonts, or
any rendering. `TextEditor` depends on both `TextEditorCore` and
`@imgui`, and owns the bridge between them.

### What stays in `TextEditor` (the ImGui shell)

- `render(title, size, showBorder)` — the top-level ImGui entry point
- `renderInternal` + all the `renderXxx` helpers (cursor, selection
  highlight, line numbers, scroll markers, tooltips, fold markers)
- `handleKeyboardInputs` — reads `ImGuiIO`, dispatches shortcuts to
  `core_`
- `handleMouseInputs` — reads ImGui mouse state, translates screen
  coordinates to `Coordinates`, calls `core_.setCursorPosition` /
  `core_.setSelection` / `core_.selectWordUnderCursor`
- `handleCharacterInput` — reads `io.InputQueueCharacters`, calls
  `core_.enterCharacter`
- `executeAction` — dispatches `ShortcutId` → `core_` calls
- The shortcut table + ShortcutId enum — these are inherently
  ImGui-keyed (they reference `ImGuiKey` values), so they stay in
  the shell layer
- `coordinatesToScreenPos` and the scroll-handling helpers — all
  layout math that depends on ImGui window state
- `ImGuiClipboard` implementation of `ClipboardInterface`

### What moves to `TextEditorCore` (headless)

The full editing substrate, unchanged in behavior. Key surface:

```cpp
// donner/editor/TextEditorCore.h (sketch)

class TextEditorCore {
 public:
  explicit TextEditorCore(ClipboardInterface* clipboard = nullptr);

  // Text ----------------------------------------------------------------
  void setText(std::string_view text);
  std::string getText() const;
  std::string getText(const Coordinates& start, const Coordinates& end) const;
  bool isTextChanged() const;
  void resetTextChanged();

  // Cursor / selection --------------------------------------------------
  Coordinates getCursorPosition() const;
  void setCursorPosition(const Coordinates& pos);
  void setSelection(const Coordinates& start, const Coordinates& end,
                    SelectionMode mode = SelectionMode::Normal);
  bool hasSelection() const;
  std::string getSelectedText() const;
  void selectAll();
  void selectWordUnderCursor();

  // Navigation ----------------------------------------------------------
  void moveLeft(int amount, bool select, bool wordMode);
  void moveRight(int amount, bool select, bool wordMode);
  void moveUp(int amount, bool select);
  void moveDown(int amount, bool select);
  void moveHome(bool select);
  void moveEnd(bool select);

  // Editing -------------------------------------------------------------
  void enterCharacter(char32_t character, bool shift);
  void insertText(std::string_view text, bool indent = false);
  void backspace();
  void delete_();
  void deleteSelection();

  // Undo / redo ---------------------------------------------------------
  void undo(int steps = 1);
  void redo(int steps = 1);
  bool canUndo() const;
  bool canRedo() const;

  // Clipboard (routes through ClipboardInterface) ----------------------
  void copy();
  void cut();
  void paste();

  // Syntax highlighting -------------------------------------------------
  void setLanguageDefinition(const LanguageDefinition& langDef);
  const LanguageDefinition& getLanguageDefinition() const;
  void colorize(int fromLine = 0, int count = -1);

  // Configuration -------------------------------------------------------
  void setTabSize(int size);
  int getTabSize() const;
  void setInsertSpaces(bool insertSpaces);

  // Change notification -------------------------------------------------
  std::function<void(TextEditorCore*)> onContentUpdate;

  // Internal state (friend'd to the ImGui shell for rendering access)
  friend class TextEditor;  // Reads cursor/selection/glyphs for rendering.

 private:
  TextBuffer text_;
  EditorState state_;
  std::vector<UndoRecord> undoBuffer_;
  int undoIndex_ = 0;
  bool textChanged_ = false;
  // ... colorize state, language def, tab settings, etc.
  ClipboardInterface* clipboard_ = nullptr;
};
```

The `friend class TextEditor` here is intentional and *different*
from a test-only friend: the rendering code in `TextEditor` needs to
read per-glyph color indices and cursor coordinates to draw them,
which is a legitimate production coupling. Tests don't need it
because they exercise `TextEditorCore` through its public API.

### Clipboard interface

```cpp
// donner/editor/ClipboardInterface.h

class ClipboardInterface {
 public:
  virtual ~ClipboardInterface() = default;
  virtual std::string getText() const = 0;
  virtual void setText(std::string_view text) = 0;
  [[nodiscard]] virtual bool hasText() const = 0;
};
```

Production: `ImGuiClipboard` calls `ImGui::GetClipboardText` /
`ImGui::SetClipboardText`. Defined in `TextEditor.cc`'s anonymous
namespace since it's the only place that needs it.

Tests: `InMemoryClipboard` in `donner/editor/tests/InMemoryClipboard.h`
— a one-line implementation that stores the text in a
`std::string`. Injected into the test's `TextEditorCore`.

### Test fixture after

```cpp
class TextEditorCoreTests : public ::testing::Test {
 protected:
  InMemoryClipboard clipboard_;
  TextEditorCore editor{&clipboard_};  // No ImGui context. No fonts.
};
```

Compared to today's fixture:

```cpp
class TextEditorTests : public ::testing::Test {
 protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->Build();
  }
  void TearDown() override {
    if (imguiContext_ != nullptr) ImGui::DestroyContext(imguiContext_);
  }
  ImGuiContext* imguiContext_ = nullptr;
  TextEditor editor;
};
```

The new fixture is 2 lines. The old one is 12 lines and has been the
source of today's crash when the font atlas wasn't built.

## Error Handling

Every editing operation is total on the state it accepts — clamp
invalid coordinates to buffer bounds, no-op on no-selection when a
selection is required, etc. No exceptions (donner is
`-fno-exceptions`), no aborts on invalid input.

## Performance

The refactor is a syntactic move, not a semantic change. Zero
expected runtime overhead. The one-line forwarding wrappers in
`TextEditor` (`void setText(x) { core_.setText(x); }`) inline at
-O1 or higher.

## Security / Privacy

No new trust boundaries. The clipboard abstraction is the only new
surface, and it's a simple 3-method interface — input validation
belongs in the `ImGuiClipboard` implementation (which today just
passes bytes through to ImGui's clipboard).

## Testing and Validation

- Existing 60 passing tests still pass.
- Existing 13 failing tests still fail at the same assertions (the
  refactor doesn't fix bugs).
- New `TextEditorCore_tests.cc` builds without `imgui.h` — verified
  by the test file's includes.
- `donner/editor/TextEditor.h` has no `friend class TextEditorTests`
  after commit 3 — verified by grep.
- `TextEditorCore.h` has no `#include "imgui.h"` and no `ImGui::`
  references — verified by grep.

## Alternatives Considered

### Leave it alone and add `friend class` per test
Rejected. The header accumulates test-only coupling and the ImGui
context crash class of bugs remains (today's fixture fix was
reactive — the next clipboard-adjacent test can hit the same trap).

### Full rewrite with a command pattern + event sourcing
Rejected as over-engineering. The existing undo/redo system works;
the goal is to isolate it, not replace it. A command pattern is
Future Work if the current model shows scaling limits.

### Extract everything including rendering into a standalone lib
Rejected. The rendering code is deeply ImGui-coupled and extracting
it would mean porting every `ImGui::` call to an abstract interface
for little payoff. The two-layer split (core + shell) captures the
80/20 of the testability win.

## Open Questions

- **Should `TextEditorCore` own the `TextBuffer` or hold a reference?**
  Initial answer: own it. The buffer is an implementation detail
  of the core; no external caller needs access.
- **Should `ClipboardInterface` be in `donner/editor/` or in a
  shared location?** Initial answer: `donner/editor/` — it's
  editor-specific for now. If another consumer appears, promote it
  to `donner/base/`.
- **Font metrics dependency.** `calculateCharacterAdvance` and a few
  other methods read font data from ImGui. These stay in the
  `TextEditor` shell, since layout is inherently a rendering
  concern. `TextEditorCore` works in `Coordinates(line, column)`
  space and never touches pixels.

## Future Work

- [ ] Extract a standalone `SyntaxHighlighter` from
      `TextEditorCore`'s colorize pipeline.
- [ ] Replace the undo system with a command pattern (better
      granularity, easier to test).
- [ ] Multi-cursor + column selection (requires rewriting most of
      the cursor state machine, hence Future Work).
- [ ] LSP integration for SVG / XML / CSS diagnostics in the source
      pane.
