#include "donner/editor/TextEditor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {

void PrintTo(const SourceByteRange& range, std::ostream* os) {
  *os << "SourceByteRange{start=" << range.start << ", end=" << range.end << "}";
}

void PrintTo(const SourcePoint& point, std::ostream* os) {
  *os << "SourcePoint{line=" << point.line << ", column=" << point.column << "}";
}

void PrintTo(const Coordinates& coords, std::ostream* os) {
  *os << "Coordinates{line=" << coords.line << ", column=" << coords.column << "}";
}

void PrintTo(SourceEditIntentKind kind, std::ostream* os) {
  switch (kind) {
    case SourceEditIntentKind::Unknown: *os << "Unknown"; return;
    case SourceEditIntentKind::Insert: *os << "Insert"; return;
    case SourceEditIntentKind::Delete: *os << "Delete"; return;
    case SourceEditIntentKind::Replace: *os << "Replace"; return;
    case SourceEditIntentKind::Undo: *os << "Undo"; return;
    case SourceEditIntentKind::Redo: *os << "Redo"; return;
  }

  *os << "SourceEditIntentKind(" << static_cast<int>(kind) << ")";
}

namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Gt;
using ::testing::IsEmpty;

auto SourceByteRangeIs(std::size_t start, std::size_t end) {
  return AllOf(Field("start", &SourceByteRange::start, start),
               Field("end", &SourceByteRange::end, end));
}

auto SourcePointIs(SourcePoint expected) {
  return AllOf(Field("line", &SourcePoint::line, expected.line),
               Field("column", &SourcePoint::column, expected.column));
}

auto LineRangeIs(int startLine, int endLine) {
  return AllOf(Field("startLine", &LineRange::startLine, startLine),
               Field("endLine", &LineRange::endLine, endLine));
}

auto FocusReferenceLinkIs(SourcePoint from, SourcePoint to) {
  return AllOf(Field("from", &FocusReferenceLink::from, SourcePointIs(from)),
               Field("to", &FocusReferenceLink::to, SourcePointIs(to)));
}

auto ActiveFlashIs(std::size_t start, std::size_t end, auto intensityMatcher = Gt(0.0f)) {
  return AllOf(Field("byteRange", &ActiveFlash::byteRange, SourceByteRangeIs(start, end)),
               Field("intensity", &ActiveFlash::intensity, intensityMatcher));
}

auto SourceEditIntentIs(std::size_t offset, std::size_t removedLength, std::string_view replacement,
                        SourceEditIntentKind kind) {
  return AllOf(Field("offset", &SourceEditIntent::offset, offset),
               Field("removedLength", &SourceEditIntent::removedLength, removedLength),
               Field("replacement", &SourceEditIntent::replacement, std::string(replacement)),
               Field("kind", &SourceEditIntent::kind, kind));
}

auto SourceStyleDecorationIs(std::size_t id, SourceByteRange range, bool ineffective, bool showChip,
                             int chipCount, std::string_view tooltip = "",
                             std::optional<SourceByteRange> chipRange = std::nullopt) {
  const SourceByteRange expectedChipRange = chipRange.value_or(range);

  return AllOf(Field("id", &TextEditor::SourceStyleDecoration::id, id),
               Field("range", &TextEditor::SourceStyleDecoration::range,
                     SourceByteRangeIs(range.start, range.end)),
               Field("chipRange", &TextEditor::SourceStyleDecoration::chipRange,
                     SourceByteRangeIs(expectedChipRange.start, expectedChipRange.end)),
               Field("ineffective", &TextEditor::SourceStyleDecoration::ineffective, ineffective),
               Field("showChip", &TextEditor::SourceStyleDecoration::showChip, showChip),
               Field("chipCount", &TextEditor::SourceStyleDecoration::chipCount, chipCount),
               Field("tooltip", &TextEditor::SourceStyleDecoration::tooltip, std::string(tooltip)));
}

std::vector<int> CoordinateColumnWidths(const std::vector<Coordinates>& starts,
                                        const std::vector<Coordinates>& ends) {
  std::vector<int> widths;
  widths.reserve(std::min(starts.size(), ends.size()));
  for (std::size_t index = 0; index < starts.size() && index < ends.size(); ++index) {
    widths.push_back(ends[index].column - starts[index].column);
  }

  return widths;
}

/// In-memory clipboard backing for the test ImGui context. ImGui's default
/// clipboard handler can fall back to a host service (window-server pasteboard)
/// that is absent under sandboxed / remote test execution, which left `copy()`
/// / `paste()` round-trips empty on RE workers. Backing the context's clipboard
/// with a plain string makes the round-trip deterministic on every host.
std::string& TestClipboardStorage() {
  static std::string storage;
  return storage;
}

}  // namespace

/// Fixture for `TextEditor` tests. Creates a per-test ImGui context so
/// calls that route through ImGui's clipboard (`copy()`, `cut()`,
/// `paste()`, `GetClipboardText()`) don't crash. The context is
/// destroyed in TearDown so each test starts fresh.
class TextEditorTests : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    // ImGui needs a valid font atlas even for non-rendering operations
    // (clipboard, input processing). Build a default atlas so functions
    // that query glyph metrics don't crash.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    // ImGui defaults ConfigMacOSXBehaviors to true on Apple, which remaps shortcut chords from
    // Ctrl to Cmd (Super). Force it off so the Ctrl-based shortcut tests below exercise the same
    // modifier on every host OS instead of failing on macOS runners.
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();

    // Route the context's clipboard through an in-memory string so copy/paste
    // are deterministic and host-independent (see TestClipboardStorage). Start
    // each test with an empty clipboard.
    TestClipboardStorage().clear();
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
      TestClipboardStorage() = text != nullptr ? text : "";
    };
    platformIo.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
      return TestClipboardStorage().c_str();
    };
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

  void RenderEditorFrame(const ImVec2& editorSize = ImVec2(240.0f, 180.0f)) {
    editor.setHandleKeyboardInputs(false);
    editor.setHandleMouseInputs(false);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    editor.render("##editor", editorSize);
    ImGui::End();
    ImGui::Render();
  }

  void RenderEditorFrameWithMouse(const ImVec2& mousePos, bool mouseDown,
                                  const ImVec2& editorSize = ImVec2(240.0f, 180.0f),
                                  bool ctrl = false, bool shift = false, bool alt = false) {
    editor.setHandleKeyboardInputs(false);
    editor.setHandleMouseInputs(true);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddMousePosEvent(mousePos.x, mousePos.y);
    io.AddMouseButtonEvent(0, mouseDown);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    editor.render("##editor", editorSize);
    ImGui::End();
    ImGui::Render();
  }

  // Drives one render frame with keyboard input handling enabled, injecting
  // the supplied modifier state, key presses, and typed characters. ImGui
  // reports `IsKeyPressed` on the frame a key transitions from up→down, so
  // each key is released first (via `ResetKeyboardState`) and pressed here so
  // the editor's shortcut matcher (`Shortcut::matches`) sees a fresh press.
  void RenderEditorFrameWithKeyboard(const std::vector<ImGuiKey>& keys,
                                     std::string_view characters = "", bool ctrl = false,
                                     bool shift = false, bool alt = false,
                                     const ImVec2& editorSize = ImVec2(240.0f, 180.0f)) {
    EnsureEditorChildWindow(editorSize);
    editor.setHandleKeyboardInputs(true);
    editor.setHandleMouseInputs(false);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    if (ctrl) {
      io.AddKeyEvent(ImGuiMod_Ctrl, true);
    }
    if (shift) {
      io.AddKeyEvent(ImGuiMod_Shift, true);
    }
    if (alt) {
      io.AddKeyEvent(ImGuiMod_Alt, true);
    }
    for (ImGuiKey key : keys) {
      io.AddKeyEvent(key, true);
    }
    for (char c : characters) {
      io.AddInputCharacter(static_cast<unsigned int>(static_cast<unsigned char>(c)));
    }

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    FocusEditorChildWindow();
    // Match the live editor, which always has a font on the stack; the
    // find/replace dialog branch pops and re-pushes it.
    ImGui::PushFont(io.Fonts->Fonts[0]);
    editor.render("##editor", editorSize);
    ImGui::PopFont();
    ImGui::End();
    ImGui::Render();
  }

  // Renders a frame that releases every modifier and key fed in the previous
  // keyboard frame so the next `IsKeyPressed` query observes a clean
  // up→down transition.
  void ResetKeyboardState(const std::vector<ImGuiKey>& keys, bool ctrl = false, bool shift = false,
                          bool alt = false, const ImVec2& editorSize = ImVec2(240.0f, 180.0f)) {
    EnsureEditorChildWindow(editorSize);
    editor.setHandleKeyboardInputs(true);
    editor.setHandleMouseInputs(false);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    for (ImGuiKey key : keys) {
      io.AddKeyEvent(key, false);
    }
    if (ctrl) {
      io.AddKeyEvent(ImGuiMod_Ctrl, false);
    }
    if (shift) {
      io.AddKeyEvent(ImGuiMod_Shift, false);
    }
    if (alt) {
      io.AddKeyEvent(ImGuiMod_Alt, false);
    }

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    FocusEditorChildWindow();
    editor.render("##editor", editorSize);
    ImGui::End();
    ImGui::Render();
  }

  // True if the editor's `BeginChild` window has been created in a prior frame.
  [[nodiscard]] bool EditorChildWindowExists() const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return false;
    }
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if (name.find("##editor") != std::string_view::npos &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) != 0) {
        return true;
      }
    }
    return false;
  }

  // Renders one input-free frame so the editor's child window exists and can be
  // focused on the following keyboard frame.
  void EnsureEditorChildWindow(const ImVec2& editorSize) {
    if (EditorChildWindowExists()) {
      return;
    }
    RenderEditorFrame(editorSize);
  }

  // Focuses the editor's child window so `handleKeyboardInputs()` sees
  // `ImGui::IsWindowFocused()` as true. The editor builds its content inside a
  // `BeginChild("##editor", ...)` whose ImGui name is
  // "<parent>/##editor_<hash>"; the window object persists across frames once
  // created, so focusing it by name before `editor.render()` re-begins the
  // child makes the child the active nav window for that frame.
  void FocusEditorChildWindow() {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return;
    }
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if (name.find("##editor") != std::string_view::npos &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) != 0) {
        ImGui::FocusWindow(window);
        return;
      }
    }
  }

  // Renders one frame with an editor font pushed on the ImGui font stack. The
  // find/replace dialog branch in `render()` unconditionally pops the editor
  // font and pushes it back, so it requires a font to already be on the stack
  // (the live editor always pushes one before `render()`).
  void RenderEditorFrameWithFont(const ImVec2& editorSize = ImVec2(320.0f, 180.0f)) {
    editor.setHandleKeyboardInputs(false);
    editor.setHandleMouseInputs(false);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(editorSize.x + 40.0f, editorSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(io.Fonts->Fonts[0]);
    editor.render("##editor", editorSize);
    ImGui::PopFont();
    ImGui::End();
    ImGui::Render();
  }

  void SetFindOpened(bool opened, bool justOpened = false) {
    editor.findOpened_ = opened;
    editor.findJustOpened_ = justOpened;
  }

  void SetReplaceOpened(bool opened) { editor.replaceOpened_ = opened; }

  [[nodiscard]] bool FindOpened() const { return editor.findOpened_; }
  [[nodiscard]] bool ReplaceOpened() const { return editor.replaceOpened_; }
  [[nodiscard]] bool AutocompleteOpened() const { return editor.autocompleteOpened_; }
  [[nodiscard]] int AutocompleteIndex() const { return editor.autocompleteIndex_; }
  [[nodiscard]] int AutocompleteSuggestionCount() const {
    return static_cast<int>(editor.autocompleteSuggestions_.size());
  }
  void SetAutocompleteSwitched(bool value) { editor.autocompleteSwitched_ = value; }
  void BuildSuggestions(bool* keepAutocompleteOpen = nullptr) {
    editor.buildSuggestions(keepAutocompleteOpen);
  }
  [[nodiscard]] bool AutocompleteReplacementActive() const {
    return editor.autocompleteReplacementActive_;
  }
  [[nodiscard]] std::size_t AutocompleteReplacementStartOffset() const {
    return editor.autocompleteReplacementStartOffset_;
  }
  [[nodiscard]] std::size_t AutocompleteReplacementEndOffset() const {
    return editor.autocompleteReplacementEndOffset_;
  }
  [[nodiscard]] Coordinates AutocompletePosition() const { return editor.autocompletePosition_; }
  [[nodiscard]] std::pair<RcString, RcString> AutocompleteSuggestion(int index) const {
    return editor.autocompleteSuggestions_[index];
  }
  [[nodiscard]] RcString ParseAutocompleteSnippet(std::string_view snippet,
                                                  Coordinates start = Coordinates(0, 0)) {
    return editor.autocompleteParseSnippet(snippet, start);
  }
  [[nodiscard]] bool IsSnippet() const { return editor.isSnippet_; }
  [[nodiscard]] const std::vector<Coordinates>& SnippetStarts() const {
    return editor.snippetTagStart_;
  }
  [[nodiscard]] const std::vector<Coordinates>& SnippetEnds() const {
    return editor.snippetTagEnd_;
  }
  [[nodiscard]] const std::vector<int>& SnippetIds() const { return editor.snippetTagId_; }
  [[nodiscard]] const std::vector<bool>& SnippetHighlights() const {
    return editor.snippetTagHighlight_;
  }

  // Opens the autocomplete popup with the provided (display == insert)
  // suggestions, anchored at the current cursor.
  void OpenAutocompleteWithSuggestions(const std::vector<std::string>& suggestions) {
    editor.autocompleteOpened_ = true;
    editor.autocompleteSuggestions_.clear();
    for (const std::string& suggestion : suggestions) {
      editor.autocompleteSuggestions_.emplace_back(RcString(suggestion), RcString(suggestion));
    }
    editor.autocompleteIndex_ = 0;
    editor.autocompletePosition_ = editor.getCursorPosition();
  }

  void RequestAutocompleteBuildOnNextKeyboardFrame() {
    editor.requestAutocomplete_ = true;
    editor.readyForAutocomplete_ = true;
  }

  [[nodiscard]] int VisualLineCount() const { return static_cast<int>(editor.visualLines_.size()); }

  [[nodiscard]] int VisualLineStartColumn(int visualIndex) const {
    return editor.visualLines_[visualIndex].startColumn;
  }

  [[nodiscard]] int VisualLineEndColumn(int visualIndex) const {
    return editor.visualLines_[visualIndex].endColumn;
  }

  [[nodiscard]] int VisualLineIndentColumns(int visualIndex) const {
    return editor.visualLines_[visualIndex].indentColumns;
  }

  [[nodiscard]] bool VisualLineIsContinuation(int visualIndex) const {
    return editor.visualLines_[visualIndex].continuation;
  }

  [[nodiscard]] bool VisualLineIsFocusHiddenPlaceholder(int visualIndex) const {
    return editor.visualLines_[visualIndex].focusHiddenPlaceholder;
  }

  [[nodiscard]] LineRange VisualLineHiddenRange(int visualIndex) const {
    return editor.visualLines_[visualIndex].hiddenRange;
  }

  [[nodiscard]] bool HorizontalScrollEnabled() const { return editor.horizontalScroll_; }
  [[nodiscard]] bool HighlightLineEnabled() const { return editor.highlightLine_; }
  [[nodiscard]] bool SidebarVisible() const { return editor.sidebar_; }
  [[nodiscard]] bool SearchEnabled() const { return editor.hasSearch_; }
  [[nodiscard]] bool HighlightBracketsEnabled() const { return editor.highlightBrackets_; }
  [[nodiscard]] bool FoldEnabled() const { return editor.foldEnabled_; }
  [[nodiscard]] bool SmartPredictionsEnabled() const { return editor.autocomplete_; }
  [[nodiscard]] bool FunctionDeclarationTooltipEnabled() const {
    return editor.functionDeclarationTooltipEnabled_;
  }
  [[nodiscard]] bool FunctionTooltipsEnabled() const { return editor.funcTooltips_; }
  [[nodiscard]] float UiScale() const { return editor.uiScale_; }
  [[nodiscard]] float UiFontSize() const { return editor.uiFontSize_; }
  [[nodiscard]] float EditorFontSize() const { return editor.editorFontSize_; }
  [[nodiscard]] bool ScrollbarMarkersEnabled() const { return editor.scrollbarMarkers_; }
  [[nodiscard]] bool AutoIndentOnPasteEnabled() const { return editor.autoIndentOnPaste_; }

  void RequestSourceFocusModeContextMenuToggle() {
    editor.sourceFocusModeContextMenuToggleRequested_ = true;
  }

  [[nodiscard]] bool SourceFocusModeContextMenuVisible() const {
    return editor.sourceFocusModeContextMenuVisible_;
  }

  [[nodiscard]] bool SourceFocusModeContextMenuChecked() const {
    return editor.sourceFocusModeContextMenuChecked_;
  }

  [[nodiscard]] std::vector<int> VisualLineLogicalLines() const {
    std::vector<int> lines;
    lines.reserve(editor.visualLines_.size());
    for (const auto& visualLine : editor.visualLines_) {
      lines.push_back(visualLine.lineNo);
    }
    return lines;
  }

  [[nodiscard]] int FirstContinuationVisualLineForLine(int lineNo) const {
    for (int i = 0; i < static_cast<int>(editor.visualLines_.size()); ++i) {
      const auto& visualLine = editor.visualLines_[i];
      if (visualLine.lineNo == lineNo && visualLine.continuation) {
        return i;
      }
    }
    return -1;
  }

  [[nodiscard]] int VisualLineIndexForCoordinates(const Coordinates& position) const {
    return editor.visualLineIndexForCoordinates(position);
  }

  [[nodiscard]] float LastScrollY() const { return editor.lastScroll_; }
  [[nodiscard]] float LastScrollX() const { return editor.lastScrollX_; }
  [[nodiscard]] float LastScrollViewportHeight() const { return editor.scrollViewportHeight_; }
  [[nodiscard]] float CharacterAdvanceX() const { return editor.charAdvance_.x; }
  [[nodiscard]] float CharacterAdvanceY() const { return editor.charAdvance_.y; }
  [[nodiscard]] float TextStart() const { return editor.textStart_; }
  void EnterCharacter(ImWchar character) { editor.enterCharacter(character, /*shift=*/false); }

  void OpenAutocompleteAtCursor(std::string_view displayText) {
    editor.autocompleteOpened_ = true;
    editor.autocompleteSuggestions_.clear();
    editor.autocompleteSuggestions_.emplace_back(RcString(displayText), RcString(displayText));
    editor.autocompleteIndex_ = 0;
    editor.autocompletePosition_ = editor.getCursorPosition();
  }

  void ReplaceAutocompleteSuggestion(std::string_view displayText) {
    ASSERT_FALSE(editor.autocompleteSuggestions_.empty());
    editor.autocompleteSuggestions_[0] = {RcString(displayText), RcString(displayText)};
  }

  [[nodiscard]] int AutocompleteChildWindowCount() const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return 0;
    }

    int childWindowCount = 0;
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if ((name.find("Autocomplete") != std::string_view::npos ||
           name.find("autocompl") != std::string_view::npos) &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) != 0) {
        ++childWindowCount;
      }
    }
    return childWindowCount;
  }

  [[nodiscard]] int AutocompleteTopLevelWindowCount() const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return 0;
    }

    int topLevelWindowCount = 0;
    for (ImGuiWindow* window : context->Windows) {
      const std::string_view name(window->Name);
      if ((name.find("Autocomplete") != std::string_view::npos ||
           name.find("autocompl") != std::string_view::npos) &&
          (window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
        ++topLevelWindowCount;
      }
    }
    return topLevelWindowCount;
  }

  [[nodiscard]] Coordinates CoordinatesAtVisualTextOffset(int visualIndex,
                                                          int visualColumnOffset) const {
    const auto& visualLine = editor.visualLines_[visualIndex];
    const ImVec2 screenPos{
        editor.uiCursorPos_.x + editor.textStart_ +
            static_cast<float>(visualLine.indentColumns + visualColumnOffset) *
                editor.charAdvance_.x,
        editor.uiCursorPos_.y + static_cast<float>(visualIndex) * editor.charAdvance_.y +
            editor.charAdvance_.y * 0.5f,
    };
    return editor.screenPosToCoordinates(screenPos);
  }

  [[nodiscard]] ImVec2 ScreenPointAtVisualTextOffset(int visualIndex,
                                                     int visualColumnOffset) const {
    const auto& visualLine = editor.visualLines_[visualIndex];
    return ImVec2{
        editor.uiCursorPos_.x + editor.textStart_ +
            static_cast<float>(visualLine.indentColumns + visualColumnOffset) *
                editor.charAdvance_.x,
        editor.uiCursorPos_.y + static_cast<float>(visualIndex) * editor.charAdvance_.y +
            editor.charAdvance_.y * 0.5f,
    };
  }

  [[nodiscard]] ImVec2 SourceGutterHandlePoint(int visualIndex) const {
    return ImVec2{
        editor.uiCursorPos_.x + editor.textStart_ - 8.0f * editor.uiScale_,
        editor.uiCursorPos_.y + static_cast<float>(visualIndex) * editor.charAdvance_.y +
            editor.charAdvance_.y * 0.5f,
    };
  }

  [[nodiscard]] int LineMaxColumn(int line) const { return editor.text_.getLineMaxColumn(line); }

  [[nodiscard]] bool HasActiveSourceFlash() const {
    return editor.nextFlashWakeSeconds().has_value() &&
           !editor.flashDecorations_.activeBackgrounds(FlashDecorations::Clock::now()).empty();
  }

  [[nodiscard]] std::optional<TextEditor::FocusReferenceConnectorLayout> FocusReferenceLayout(
      const FocusReferenceLink& link, int linkIndex) const {
    return editor.focusReferenceConnectorLayout(link, linkIndex);
  }

  [[nodiscard]] std::optional<TextEditor::FocusReferenceSourceUnderline>
  FocusReferenceSourceUnderlineDirect(const SourcePoint& source) const {
    return editor.focusReferenceSourceUnderline(source);
  }

  [[nodiscard]] std::vector<FocusReferenceLink> FocusReferenceLinks() const {
    return editor.focusPartition_.referenceLinks;
  }

  [[nodiscard]] RopeSimulationOptions FocusReferenceRopeOptions() const {
    return editor.focusReferenceRopeOptions();
  }

  [[nodiscard]] const TextEditor::FocusReferenceRopeState* FocusReferenceRope(
      const FocusReferenceLink& link) const {
    const auto it = editor.focusReferenceRopes_.find(link);
    return it == editor.focusReferenceRopes_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] bool FocusReferenceRopeHit(const FocusReferenceLink& link,
                                           const ImVec2& point) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    return rope != nullptr && editor.isFocusReferenceRopeHit(
                                  rope->path, point, std::max(7.0f * editor.uiScale_, 5.0f));
  }

  [[nodiscard]] bool FocusReferenceRopeHovered(const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    return rope != nullptr && rope->hovered;
  }

  void ExpectFocusReferenceArrowMatchesTangent(const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    ASSERT_NE(rope, nullptr);
    EXPECT_EQ(rope->arrowDirection, rope->rope.endTangent());
  }

  [[nodiscard]] bool FocusReferenceRopePathIsBezier(const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    return rope != nullptr && !rope->path.empty() && !rope->path.commands().empty() &&
           rope->path.commands().back().verb == Path::Verb::QuadTo;
  }

  [[nodiscard]] std::optional<Box2d> FocusReferenceRopeBounds(
      const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    if (rope == nullptr || rope->path.empty()) {
      return std::nullopt;
    }

    return rope->path.bounds();
  }

  [[nodiscard]] std::vector<Vector2d> FocusReferenceRopePoints(
      const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    if (rope == nullptr) {
      return {};
    }

    return std::vector<Vector2d>(rope->rope.points().begin(), rope->rope.points().end());
  }

  void ResetFocusReferenceRopeToSettledCatenary(const FocusReferenceLink& link, int linkIndex,
                                                float previousScrollY) {
    const std::optional<TextEditor::FocusReferenceConnectorLayout> layout =
        FocusReferenceLayout(link, linkIndex);
    ASSERT_TRUE(layout.has_value());

    TextEditor::FocusReferenceRopeState& rope = editor.focusReferenceRopes_[link];
    const RopeSimulationOptions options = editor.focusReferenceRopeOptions();
    const Vector2d start(layout->start.x, layout->start.y);
    const Vector2d tip(layout->tip.x, layout->tip.y);
    rope.rope.resetCatenary(start, tip, options);
    rope.path = rope.rope.toPath(options);
    rope.chordLength = start.distance(tip);
    rope.hovered = false;
    rope.initialized = true;
    rope.lastFrameSeen = editor.focusReferenceRopeFrame_;
    editor.lastFocusReferenceRopeScrollY_ = previousScrollY;
  }

  [[nodiscard]] std::optional<ImVec2> FocusReferenceRopeMidpoint(
      const FocusReferenceLink& link) const {
    const TextEditor::FocusReferenceRopeState* rope = FocusReferenceRope(link);
    if (rope == nullptr || rope->path.empty()) {
      return std::nullopt;
    }

    const Path::PointOnPath point = rope->path.pointAtArcLength(rope->path.pathLength() * 0.5);
    return ImVec2(static_cast<float>(point.point.x), static_cast<float>(point.point.y));
  }

  [[nodiscard]] ImVec2 ScreenPointAtCoordinates(const Coordinates& position) const {
    return editor.coordinatesToScreenPos(position);
  }

  [[nodiscard]] ImVec2 ScreenPointAtUnwrappedVisualLine(int visualLine, float columnPixels) const {
    return ImVec2(
        editor.uiCursorPos_.x + editor.textStart_ + columnPixels,
        editor.uiCursorPos_.y + (static_cast<float>(visualLine) + 0.5f) * editor.charAdvance_.y);
  }

  [[nodiscard]] float TextBaselineOffsetY() const {
    const ImFont* font = ImGui::GetFont();
    if (font == nullptr || font->FontSize <= 0.0f) {
      return ImGui::GetTextLineHeight();
    }

    return font->Ascent * (ImGui::GetFontSize() / font->FontSize);
  }

  [[nodiscard]] std::vector<ActiveFlash> ActiveSourceFlashes() const {
    return editor.flashDecorations_.activeBackgrounds(FlashDecorations::Clock::now());
  }

  [[nodiscard]] bool IsByteOffsetInIneffectiveStyleDecoration(std::size_t byteOffset) const {
    return editor.isByteOffsetInIneffectiveStyleDecoration(byteOffset);
  }

  void SetSingleSourceStyleChipHitRect(std::size_t id, const ImVec2& min, const ImVec2& max,
                                       std::string tooltip = "") {
    editor.sourceStyleChipHitRects_ = {TextEditor::SourceStyleChipHitRect{
        .id = id,
        .min = min,
        .max = max,
        .tooltip = std::move(tooltip),
    }};
  }

  void RenderSourceStyleDecorationTooltipDirect(const ImVec2& mousePos,
                                                std::optional<Coordinates> hoveredTextPosition) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mousePos.x, mousePos.y);
    RunFrameWithChild("##source_style_tooltip_host", ImVec2(260.0f, 120.0f),
                      [&](ImGuiWindow*, ImDrawList*) {
                        editor.hoveredTextPosition_ = hoveredTextPosition;
                        editor.renderSourceStyleDecorationTooltip();
                      });
  }

  [[nodiscard]] int RenderErrorMarkersDirect(int lineNo, const ImVec2& start, const ImVec2& end,
                                             std::optional<ImVec2> mousePos = std::nullopt) {
    ImGuiIO& io = ImGui::GetIO();
    if (mousePos.has_value()) {
      io.AddMousePosEvent(mousePos->x, mousePos->y);
    }

    int addedVertices = 0;
    RunFrameWithChild("##error_marker_host", ImVec2(260.0f, 120.0f),
                      [&](ImGuiWindow*, ImDrawList* drawList) {
                        const int before = drawList->VtxBuffer.Size;
                        editor.renderErrorMarkers(lineNo, start, end, drawList);
                        addedVertices = drawList->VtxBuffer.Size - before;
                      });
    return addedVertices;
  }

  [[nodiscard]] std::size_t SourceStyleChipHitRectCount() const {
    return editor.sourceStyleChipHitRects_.size();
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectCenter(std::size_t index) const {
    const auto& hitRect = editor.sourceStyleChipHitRects_[index];
    return ImVec2((hitRect.min.x + hitRect.max.x) * 0.5f, (hitRect.min.y + hitRect.max.y) * 0.5f);
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectMin(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].min;
  }

  [[nodiscard]] ImVec2 SourceStyleChipHitRectMax(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].max;
  }

  [[nodiscard]] std::string SourceStyleChipHitRectTooltip(std::size_t index) const {
    return editor.sourceStyleChipHitRects_[index].tooltip;
  }

  void SetRawHoverSourceRanges(std::vector<SourceByteRange> ranges) {
    editor.hoverSourceRanges_ = std::move(ranges);
  }

  void SetRawSourceStyleDecorations(std::vector<TextEditor::SourceStyleDecoration> decorations) {
    editor.sourceStyleDecorations_ = std::move(decorations);
  }

  void RemapFocusMetadataForSourceEdit(const SourceEditIntent& intent) {
    editor.remapFocusMetadataForSourceEdit(intent);
  }

  [[nodiscard]] const FocusPartition& ActiveFocusPartition() const {
    return editor.focusPartition_;
  }

  [[nodiscard]] const std::vector<LineRange>& ExpandedFocusHiddenRanges() const {
    return editor.expandedFocusHiddenRanges_;
  }

  void SetRawFocusPartition(FocusPartition partition, bool active = true) {
    editor.focusPartition_ = std::move(partition);
    editor.focusPartitionActive_ = active;
  }

  [[nodiscard]] bool IsLineHiddenByFocusDirect(int lineNo) const {
    return editor.isLineHiddenByFocus(lineNo);
  }

  [[nodiscard]] bool IsLineReferenceColoredByFocusDirect(int lineNo) const {
    return editor.isLineReferenceColoredByFocus(lineNo);
  }

  [[nodiscard]] bool IsLineDimmedByFocusDirect(int lineNo) const {
    return editor.isLineDimmedByFocus(lineNo);
  }

  [[nodiscard]] bool IsLineExpandedHiddenByFocusDirect(int lineNo) const {
    return editor.isLineExpandedHiddenByFocus(lineNo);
  }

  [[nodiscard]] std::optional<LineRange> FocusHiddenRangeForLineDirect(int lineNo) const {
    return editor.focusHiddenRangeForLine(lineNo);
  }

  [[nodiscard]] bool IsFocusHiddenRangeExpandedDirect(LineRange range) const {
    return editor.isFocusHiddenRangeExpanded(range);
  }

  void ExpandFocusHiddenRangeDirect(LineRange range) { editor.expandFocusHiddenRange(range); }

  [[nodiscard]] bool TryExpandFocusHiddenPlaceholderAtDirect(const ImVec2& position) {
    return editor.tryExpandFocusHiddenPlaceholderAt(position);
  }

  void RebuildVisualLinesDirect(const ImVec2& contentSize) {
    editor.rebuildVisualLines(contentSize);
  }

  void ClearVisualLinesDirect() { editor.visualLines_.clear(); }

  [[nodiscard]] Coordinates VisibleSelectionEndCoordinatesDirect() const {
    return editor.visibleSelectionEndCoordinates();
  }

  [[nodiscard]] float VisibleTextRegionHeightDirect() const {
    return editor.visibleTextRegionHeight();
  }

  void SetScrollViewportHeight(float height) { editor.scrollViewportHeight_ = height; }

  void ScrollCoordinatesRangeIntoViewDirect(const Coordinates& start, const Coordinates& end) {
    editor.scrollCoordinatesRangeIntoView(start, end);
  }

  [[nodiscard]] std::string TextRange(const Coordinates& start, const Coordinates& end) const {
    return editor.getText(start, end);
  }

  [[nodiscard]] Coordinates SanitizeCoordinates(const Coordinates& value) const {
    return editor.sanitizeCoordinates(value);
  }

  void AdvanceCoordinates(Coordinates& coordinates) const { editor.advance(coordinates); }

  void DeleteRangeDirect(const Coordinates& start, const Coordinates& end) {
    editor.deleteRange(start, end);
  }

  void DeleteSelectionDirect() { editor.deleteSelection(); }

  void BackspaceDirect() { editor.backspace(); }

  int InsertTextAtDirect(Coordinates& where, std::string_view text, bool indent = false) {
    return editor.insertTextAt(where, text, indent);
  }

  void RemoveLineDirect(int start, int end) { editor.removeLine(start, end); }

  void RemoveLineDirect(int index) { editor.removeLine(index); }

  void InsertLineDirect(int index, int column) { editor.insertLine(index, column); }

  void RemoveFoldsDirect(const Coordinates& start, const Coordinates& end) {
    editor.removeFolds(start, end);
  }

  void RemoveFoldsDirect(std::vector<Coordinates>& folds, const Coordinates& start,
                         const Coordinates& end) {
    editor.removeFolds(folds, start, end);
  }

  [[nodiscard]] const std::vector<Coordinates>& FoldBeginDirect() const {
    return editor.foldBegin_;
  }

  [[nodiscard]] const std::vector<Coordinates>& FoldEndDirect() const { return editor.foldEnd_; }

  void UpdateChangeTrackingDirect() { editor.updateChangeTracking(); }

  [[nodiscard]] const std::vector<int>& ChangedLinesDirect() const { return editor.changedLines_; }

  [[nodiscard]] const Shortcut& ShortcutForDirect(TextEditor::ShortcutId id) const {
    return editor.shortcuts_[static_cast<int>(id)];
  }

  void DetectIndentationStyleDirect() { editor.detectIndentationStyle(); }

  [[nodiscard]] Coordinates FindWordStartDirect(const Coordinates& from) const {
    return editor.findWordStart(from);
  }

  [[nodiscard]] Coordinates FindWordEndDirect(const Coordinates& from) const {
    return editor.findWordEnd(from);
  }

  [[nodiscard]] Coordinates FindNextWordDirect(const Coordinates& from) const {
    return editor.findNextWord(from);
  }

  [[nodiscard]] bool IsOnWordBoundaryDirect(const Coordinates& at) const {
    return editor.isOnWordBoundary(at);
  }

  [[nodiscard]] Coordinates ScreenPosToCoordinatesDirect(const ImVec2& position) const {
    return editor.screenPosToCoordinates(position);
  }

  [[nodiscard]] Coordinates MousePosToCoordinatesDirect(const ImVec2& position) const {
    return editor.mousePosToCoordinates(position);
  }

  [[nodiscard]] Coordinates VisualScreenPosToCoordinatesDirect(const ImVec2& position) const {
    return editor.visualScreenPosToCoordinates(position);
  }

  [[nodiscard]] Coordinates FindFirstDirect(std::string_view searchText,
                                            const Coordinates& start) const {
    return editor.findFirst(searchText, start);
  }

  [[nodiscard]] std::optional<Coordinates> HoveredTextPosition() const {
    return editor.hoveredTextPosition_;
  }

  void SetCharAdvanceForTesting(const ImVec2& value) { editor.charAdvance_ = value; }

  void UpdateHoveredTextPositionDirect(const ImVec2& mousePos, bool leftDown = false,
                                       bool rightDown = false,
                                       const ImVec2& childSize = ImVec2(240.0f, 100.0f)) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mousePos.x, mousePos.y);
    io.AddMouseButtonEvent(0, leftDown);
    io.AddMouseButtonEvent(1, rightDown);
    RunFrameWithChild("##hover_host", childSize,
                      [&](ImGuiWindow*, ImDrawList*) { editor.updateHoveredTextPosition(); });
    io.AddMouseButtonEvent(1, false);
    io.AddMouseButtonEvent(0, false);
  }

  void OpenAutocompleteReplacement(std::size_t replaceStart, std::size_t replaceEnd,
                                   std::string_view insertText) {
    editor.autocompleteOpened_ = true;
    editor.autocompleteSuggestions_.clear();
    editor.autocompleteSuggestions_.emplace_back(RcString(insertText), RcString(insertText));
    editor.autocompleteIndex_ = 0;
    editor.autocompletePosition_ = editor.getCoordinatesAtByteOffset(replaceStart);
    editor.autocompleteReplacementActive_ = true;
    editor.autocompleteReplacementStartOffset_ = replaceStart;
    editor.autocompleteReplacementEndOffset_ = replaceEnd;
  }

  void SetAutocompleteIndex(int index) { editor.autocompleteIndex_ = index; }

  void AutocompleteSelectDirect() { editor.autocompleteSelect(); }

  void AutocompleteNavigateDirect(bool up) { editor.autocompleteNavigate(up); }

  void ExecuteActionDirect(TextEditor::ShortcutId actionId, bool& keepAutocompleteOpen,
                           bool shift = false, bool ctrl = false) {
    editor.executeAction(actionId, keepAutocompleteOpen, shift, ctrl);
  }

  void AddUndoRecordForInsertedText(std::string_view added, const Coordinates& start,
                                    const Coordinates& end) {
    EditorState before = editor.state_;
    before.cursorPosition = start;
    before.selectionStart = start;
    before.selectionEnd = start;

    EditorState after = editor.state_;
    after.cursorPosition = end;
    after.selectionStart = end;
    after.selectionEnd = end;

    UndoRecord record(added, start, end, "", start, start, before, after);
    editor.addUndo(record);
  }

  void ConfigureFoldState(std::vector<Coordinates> foldBegin, std::vector<Coordinates> foldEnd,
                          std::vector<bool> fold, std::vector<int> foldConnection) {
    editor.foldEnabled_ = true;
    editor.foldBegin_ = std::move(foldBegin);
    editor.foldEnd_ = std::move(foldEnd);
    editor.fold_ = std::move(fold);
    editor.foldConnection_ = std::move(foldConnection);
  }

  void ConfigureFoldEndpointsForCalculation(std::vector<Coordinates> foldBegin,
                                            std::vector<Coordinates> foldEnd) {
    editor.foldEnabled_ = true;
    editor.foldBegin_ = std::move(foldBegin);
    editor.foldEnd_ = std::move(foldEnd);
    editor.fold_.clear();
    editor.foldConnection_.clear();
    editor.foldSorted_ = false;
    editor.foldLastIteration_ = 0;
  }

  void CalculateFoldsDirect(int currentLine, int totalLines) {
    editor.foldLastIteration_ = 0;
    editor.calculateFolds(currentLine, totalLines);
  }

  void UpdateFoldedLinesDirect(int currentLine, int totalLines) {
    editor.updateFoldedLines(currentLine, totalLines);
  }

  [[nodiscard]] std::vector<int> FoldConnections() const { return editor.foldConnection_; }
  [[nodiscard]] std::vector<bool> FoldStates() const { return editor.fold_; }
  [[nodiscard]] int FoldedLines() const { return editor.foldedLines_; }

  std::pair<bool, bool> HandleCharacterInputDirect(std::span<const ImWchar> characters,
                                                   bool shift = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.InputQueueCharacters.resize(0);
    for (ImWchar character : characters) {
      io.InputQueueCharacters.push_back(character);
    }

    bool keepAutocompleteOpen = false;
    bool hasWrittenLetter = false;
    editor.handleCharacterInput(io, shift, keepAutocompleteOpen, hasWrittenLetter);
    io.InputQueueCharacters.resize(0);
    return {keepAutocompleteOpen, hasWrittenLetter};
  }

  void ActivateSnippetEditing(std::string_view snippetSource) {
    const RcString snippet = ParseAutocompleteSnippet(snippetSource);
    editor.setText(std::string(std::string_view(snippet)));
    ASSERT_FALSE(editor.snippetTagStart_.empty());
    editor.isSnippet_ = true;
    editor.snippetTagSelected_ = 0;
    editor.snippetTagLength_ = 0;
    editor.snippetTagPreviousLength_ =
        editor.snippetTagEnd_[0].column - editor.snippetTagStart_[0].column;
    editor.setSelection(editor.snippetTagStart_[0], editor.snippetTagEnd_[0]);
    editor.setCursorPosition(editor.snippetTagEnd_[0]);
  }

  template <typename Func>
  void RunFrameWithChild(std::string_view childName, const ImVec2& childSize, Func&& func) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(childSize.x + 40.0f, childSize.y + 40.0f), ImGuiCond_Always);
    ImGui::Begin(
        "TextEditorDirectTestWindow", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild(std::string(childName).c_str(), childSize, false);
    func(ImGui::GetCurrentWindow(), ImGui::GetWindowDrawList());
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
  }

  [[nodiscard]] float HandleScrollingDirect(float initialScrollY,
                                            const ImVec2& childSize = ImVec2(240.0f, 80.0f)) {
    float scrollTargetY = 0.0f;
    RunFrameWithChild("##scroll_host", childSize, [&](ImGuiWindow* window, ImDrawList*) {
      window->Scroll.y = initialScrollY;
      editor.scrollViewportHeight_ = childSize.y;
      editor.withinRender_ = true;
      editor.handleScrolling();
      editor.withinRender_ = false;
      scrollTargetY = window->ScrollTarget.y;
    });
    return scrollTargetY;
  }

  [[nodiscard]] float ScrollRangeIntoViewDirectInChild(const Coordinates& start,
                                                       const Coordinates& end, float initialScrollY,
                                                       const ImVec2& childSize = ImVec2(240.0f,
                                                                                        80.0f)) {
    float scrollTargetY = 0.0f;
    RunFrameWithChild("##scroll_range_host", childSize, [&](ImGuiWindow* window, ImDrawList*) {
      window->Scroll.y = initialScrollY;
      editor.scrollViewportHeight_ = childSize.y;
      editor.withinRender_ = true;
      editor.scrollCoordinatesRangeIntoView(start, end);
      editor.withinRender_ = false;
      scrollTargetY = window->ScrollTarget.y;
    });
    return scrollTargetY;
  }

  [[nodiscard]] float EnsureCursorVisibleDirectInChild(float initialScrollY,
                                                       const ImVec2& childSize = ImVec2(240.0f,
                                                                                        80.0f)) {
    float scrollTargetY = 0.0f;
    RunFrameWithChild("##ensure_cursor_host", childSize, [&](ImGuiWindow* window, ImDrawList*) {
      window->Scroll.y = initialScrollY;
      editor.scrollViewportHeight_ = childSize.y;
      editor.withinRender_ = true;
      editor.ensureCursorVisible();
      editor.withinRender_ = false;
      scrollTargetY = window->ScrollTarget.y;
    });
    return scrollTargetY;
  }

  [[nodiscard]] int RenderCursorDirectInChild(const ImVec2& lineStart,
                                              const ImVec2& childSize = ImVec2(240.0f, 80.0f)) {
    int addedVertices = 0;
    RunFrameWithChild("##cursor_host", childSize, [&](ImGuiWindow* window, ImDrawList* drawList) {
      ImGui::FocusWindow(window);
      const int before = drawList->VtxBuffer.Size;
      editor.textChanged_ = true;
      editor.renderCursor(lineStart, drawList);
      editor.textChanged_ = false;
      addedVertices = drawList->VtxBuffer.Size - before;
    });
    return addedVertices;
  }

  [[nodiscard]] float TextDistanceToLineStartDirect(const Coordinates& coordinates) {
    return editor.getTextDistanceToLineStart(coordinates);
  }

  [[nodiscard]] bool ScrollToCursorRequested() const { return editor.scrollToCursor_; }
  [[nodiscard]] bool ScrollSelectionIntoViewRequested() const {
    return editor.scrollSelectionIntoView_;
  }

  void RequestCursorScroll(bool selectionIntoView = false) {
    editor.scrollToCursor_ = true;
    editor.scrollSelectionIntoView_ = selectionIntoView;
  }

  void HandleScrollingOutsideRenderDirect() {
    editor.withinRender_ = false;
    editor.handleScrolling();
  }

  [[nodiscard]] float VisibleTextRegionHeightInChild(const ImVec2& childSize = ImVec2(240.0f,
                                                                                      80.0f)) {
    float height = 0.0f;
    RunFrameWithChild("##visible_region_height_host", childSize, [&](ImGuiWindow*, ImDrawList*) {
      editor.scrollViewportHeight_ = 0.0f;
      height = editor.visibleTextRegionHeight();
    });
    return height;
  }

  [[nodiscard]] ImVec2 FoldButtonCenterForLineStart(const ImVec2& lineStart) const {
    const float spaceSize =
        ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
    const float fontSize = ImGui::GetFontSize();
    const float startX = lineStart.x + editor.textStart_ - spaceSize * 2.0f + 4.0f;
    const float startY = lineStart.y + (fontSize - spaceSize) / 2.0f;
    const ImVec2 min(startX, startY + 2.0f);
    const ImVec2 max(min.x + spaceSize, min.y + spaceSize);
    return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
  }

  void RenderFoldMarkerDirect(int lineNo, const ImVec2& lineStart, const ImVec2& mousePos,
                              bool mouseDown) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mousePos.x, mousePos.y);
    io.AddMouseButtonEvent(0, mouseDown);
    RunFrameWithChild("##fold_host", ImVec2(260.0f, 120.0f),
                      [&](ImGuiWindow* window, ImDrawList* drawList) {
                        window->Scroll.x = 0.0f;
                        editor.renderFoldMarkers(lineNo, lineStart, drawList);
                      });
  }

  [[nodiscard]] bool FoldState(int index) const { return editor.fold_[index]; }

  void OpenFunctionTooltipDirect(std::string_view declaration, const Coordinates& coords) {
    editor.openFunctionDeclarationTooltip(declaration, coords);
  }

  void HandleFunctionTooltipDirect(ImWchar character, const Coordinates& coords) {
    editor.handleFunctionTooltip(character, coords);
  }

  [[nodiscard]] bool FunctionTooltipOpen() const { return editor.functionDeclarationTooltip_; }

  [[nodiscard]] std::string FunctionTooltipDeclaration() const {
    return editor.functionDeclaration_;
  }

  [[nodiscard]] int WindowCountContaining(std::string_view text) const {
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
      return 0;
    }

    int count = 0;
    for (ImGuiWindow* window : context->Windows) {
      if (std::string_view(window->Name).find(text) != std::string_view::npos) {
        ++count;
      }
    }
    return count;
  }

  ImGuiContext* imguiContext_ = nullptr;
  TextEditor editor;
};

// ============================================================================
// CURSOR MOVEMENT TESTS
// ============================================================================

TEST_F(TextEditorTests, CursorStartsAtOrigin) {
  editor.setText("Hello");
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0)) << "Cursor should start at (0, 0)";
}

TEST_F(TextEditorTests, MoveRightAdvancesCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveRight should advance cursor by 1 column";
}

TEST_F(TextEditorTests, KeyboardNavigationMarksCursorPositionChanged) {
  editor.setText("Hello\nWorld");
  RenderEditorFrame();

  editor.moveDown(1, false);
  EXPECT_TRUE(editor.isCursorPositionChanged());
}

TEST_F(TextEditorTests, MouseClickMarksCursorPositionChangedByMouse) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/6);

  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);

  EXPECT_TRUE(editor.isCursorPositionChanged());
  EXPECT_TRUE(editor.didMouseChangeCursorPosition());
  EXPECT_NE(editor.getCursorPosition(), Coordinates(0, 0));
}

TEST_F(TextEditorTests, HoveredTextPositionTracksMouseWithoutMovingCursor) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 hoverPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/6);
  RenderEditorFrameWithMouse(hoverPos, false);

  ASSERT_TRUE(editor.hoveredTextPosition().has_value());
  EXPECT_EQ(*editor.hoveredTextPosition(), Coordinates(0, 6));
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  EXPECT_FALSE(editor.isCursorPositionChanged());
  EXPECT_FALSE(editor.didMouseChangeCursorPosition());
}

TEST_F(TextEditorTests, HoveredTextPositionRejectsOutOfTextAndHiddenPlaceholderRows) {
  editor.setText("root\nhidden-a\nhidden-b\ntarget");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 3, .endLine = 4}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  });

  RenderEditorFrame(ImVec2(300.0f, 160.0f));
  ASSERT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));

  RenderEditorFrameWithMouse(ScreenPointAtVisualTextOffset(/*visualIndex=*/1, 1),
                             /*mouseDown=*/false, ImVec2(300.0f, 160.0f));
  EXPECT_FALSE(editor.hoveredTextPosition().has_value());

  editor.clearFocusPartition();
  editor.setWordWrapEnabled(false);
  RenderEditorFrame(ImVec2(300.0f, 160.0f));

  const ImVec2 leftOfText =
      ScreenPointAtUnwrappedVisualLine(/*visualLine=*/0, /*columnPixels=*/-TextStart() - 4.0f);
  RenderEditorFrameWithMouse(leftOfText, /*mouseDown=*/false, ImVec2(300.0f, 160.0f));
  EXPECT_FALSE(editor.hoveredTextPosition().has_value());

  ImVec2 belowText = ScreenPointAtUnwrappedVisualLine(/*visualLine=*/0, /*columnPixels=*/20.0f);
  belowText.y += 500.0f;
  RenderEditorFrameWithMouse(belowText, /*mouseDown=*/false, ImVec2(300.0f, 160.0f));
  EXPECT_FALSE(editor.hoveredTextPosition().has_value());
}

TEST_F(TextEditorTests, HoverSourceRangesAreClampedAndDeduplicated) {
  editor.setText("abcdef");

  EXPECT_TRUE(editor.setHoverSourceRanges({
      SourceByteRange{.start = 2, .end = 5},
      SourceByteRange{.start = 2, .end = 5},
      SourceByteRange{.start = 5, .end = 100},
      SourceByteRange{.start = 4, .end = 4},
  }));

  EXPECT_THAT(editor.hoverSourceRanges(),
              ElementsAre(SourceByteRangeIs(2, 5), SourceByteRangeIs(5, 6)));

  EXPECT_FALSE(editor.setHoverSourceRanges(editor.hoverSourceRanges()));
  EXPECT_TRUE(editor.clearHoverSourceRanges());
  EXPECT_TRUE(editor.hoverSourceRanges().empty());
}

TEST_F(TextEditorTests, SourceStyleDecorationsAreClampedAndCleared) {
  editor.setText("abcdef");

  EXPECT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 7,
          .range = SourceByteRange{.start = 4, .end = 4},
          .ineffective = true,
      },
      TextEditor::SourceStyleDecoration{
          .id = 2,
          .range = SourceByteRange{.start = 5, .end = 100},
          .showChip = true,
          .chipCount = -4,
          .tooltip = "unused selector",
      },
      TextEditor::SourceStyleDecoration{
          .id = 1,
          .range = SourceByteRange{.start = 2, .end = 5},
          .ineffective = true,
          .tooltip = "fill is overridden",
      },
  }));

  EXPECT_THAT(editor.sourceStyleDecorations(),
              ElementsAre(SourceStyleDecorationIs(1, SourceByteRange{.start = 2, .end = 5},
                                                  /*ineffective=*/true, /*showChip=*/false,
                                                  /*chipCount=*/0, "fill is overridden"),
                          SourceStyleDecorationIs(2, SourceByteRange{.start = 5, .end = 6},
                                                  /*ineffective=*/false, /*showChip=*/true,
                                                  /*chipCount=*/0, "unused selector")));

  EXPECT_FALSE(editor.setSourceStyleDecorations(editor.sourceStyleDecorations()));
  EXPECT_TRUE(editor.clearSourceStyleDecorations());
  EXPECT_TRUE(editor.sourceStyleDecorations().empty());
}

TEST_F(TextEditorTests, SourceStyleDecorationsSortByEndThenId) {
  editor.setText("abcdef");

  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 4,
          .range = SourceByteRange{.start = 0, .end = 5},
      },
      TextEditor::SourceStyleDecoration{
          .id = 3,
          .range = SourceByteRange{.start = 0, .end = 3},
      },
      TextEditor::SourceStyleDecoration{
          .id = 1,
          .range = SourceByteRange{.start = 0, .end = 3},
          .chipRange = SourceByteRange{.start = 5, .end = 5},
      },
      TextEditor::SourceStyleDecoration{
          .id = 2,
          .range = SourceByteRange{.start = 0, .end = 3},
      },
  }));

  EXPECT_THAT(editor.sourceStyleDecorations(),
              ElementsAre(SourceStyleDecorationIs(1, SourceByteRange{.start = 0, .end = 3},
                                                  /*ineffective=*/false, /*showChip=*/false,
                                                  /*chipCount=*/0),
                          SourceStyleDecorationIs(2, SourceByteRange{.start = 0, .end = 3},
                                                  /*ineffective=*/false, /*showChip=*/false,
                                                  /*chipCount=*/0),
                          SourceStyleDecorationIs(3, SourceByteRange{.start = 0, .end = 3},
                                                  /*ineffective=*/false, /*showChip=*/false,
                                                  /*chipCount=*/0),
                          SourceStyleDecorationIs(4, SourceByteRange{.start = 0, .end = 5},
                                                  /*ineffective=*/false, /*showChip=*/false,
                                                  /*chipCount=*/0)));
}

TEST_F(TextEditorTests, IneffectiveStyleLookupSkipsEffectiveDecorationsAtSameOffset) {
  editor.setText("abcdef");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 1,
          .range = SourceByteRange{.start = 0, .end = 4},
          .ineffective = false,
      },
      TextEditor::SourceStyleDecoration{
          .id = 2,
          .range = SourceByteRange{.start = 1, .end = 3},
          .ineffective = true,
      },
  }));

  EXPECT_FALSE(IsByteOffsetInIneffectiveStyleDecoration(0));
  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(1));
  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(2));
  EXPECT_FALSE(IsByteOffsetInIneffectiveStyleDecoration(3));
}

TEST_F(TextEditorTests, SourceStyleDecorationsStrikeRangesWithoutRenderingHiddenChips) {
  editor.setText("fill: red;");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 11,
          .range = SourceByteRange{.start = 0, .end = 4},
          .ineffective = true,
          .showChip = false,
          .chipCount = 5,
          .tooltip = "fill is overridden",
      },
  }));

  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(0));
  EXPECT_TRUE(IsByteOffsetInIneffectiveStyleDecoration(3));
  EXPECT_FALSE(IsByteOffsetInIneffectiveStyleDecoration(4));

  RenderEditorFrame(ImVec2(360.0f, 120.0f));
  EXPECT_EQ(SourceStyleChipHitRectCount(), 0u);
}

TEST_F(TextEditorTests, IneffectiveSourceStyleDecorationShowsTooltipOnSourceHover) {
  editor.setText("fill: red;");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 12,
          .range = SourceByteRange{.start = 0, .end = 4},
          .ineffective = true,
          .tooltip = "fill is overridden",
      },
  }));

  RenderEditorFrame(ImVec2(360.0f, 120.0f));
  RenderEditorFrameWithMouse(ScreenPointAtVisualTextOffset(/*visualIndex=*/0, 2),
                             /*mouseDown=*/false, ImVec2(360.0f, 120.0f));

  EXPECT_GT(WindowCountContaining("##Tooltip"), 0);
}

TEST_F(TextEditorTests, SourceStyleDecorationChipClickIsConsumable) {
  editor.setText(".cls { fill: red; }\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 42,
          .range = SourceByteRange{.start = 7, .end = 16},
          .showChip = true,
          .chipCount = 3,
          .tooltip = "3 matched elements",
      },
  }));

  RenderEditorFrame(ImVec2(420.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);
  const ImVec2 chipCenter = SourceStyleChipHitRectCenter(0);

  RenderEditorFrameWithMouse(chipCenter, true, ImVec2(420.0f, 120.0f));

  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::optional<std::size_t>(42));
  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::nullopt);
}

TEST_F(TextEditorTests, SourceStyleDecorationChipUsesChipRangeAnchor) {
  editor.setText(".hit {\n  fill: red;\n}\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 84,
          .range = SourceByteRange{.start = 9, .end = 19},
          .chipRange = SourceByteRange{.start = 0, .end = 4},
          .showChip = true,
          .chipCount = 2,
          .tooltip = "fill is overridden",
          .chipTooltip = "Selector matches 2 elements",
      },
  }));

  RenderEditorFrame(ImVec2(420.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);

  const float selectorLineCenterY =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/4).y;
  const float propertyLineCenterY =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/12).y;
  const float chipCenterY = SourceStyleChipHitRectCenter(0).y;
  EXPECT_LT(std::abs(chipCenterY - selectorLineCenterY),
            std::abs(chipCenterY - propertyLineCenterY));
  EXPECT_EQ(SourceStyleChipHitRectTooltip(0), "Selector matches 2 elements");
}

TEST_F(TextEditorTests, SourceStyleDecorationOverflowMarkerHasTooltipAndIsNotClickable) {
  editor.setText("<linearGradient id=\"paint\">\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 85,
          .range = SourceByteRange{.start = 0, .end = 27},
          .chipRange = SourceByteRange{.start = 0, .end = 27},
          .showChip = true,
          .chipCount = 6,
          .showOverflowMarker = true,
          .chipTooltip = "Referenced 6 times",
          .overflowTooltip = "Too many reverse refs to draw lines",
      },
  }));

  RenderEditorFrame(ImVec2(520.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 2u);
  EXPECT_LT(SourceStyleChipHitRectCenter(0).x, SourceStyleChipHitRectCenter(1).x);
  EXPECT_EQ(SourceStyleChipHitRectTooltip(0), "Referenced 6 times");
  EXPECT_EQ(SourceStyleChipHitRectTooltip(1), "Too many reverse refs to draw lines");

  RenderEditorFrameWithMouse(SourceStyleChipHitRectCenter(1), true, ImVec2(520.0f, 120.0f));
  EXPECT_EQ(editor.takeClickedSourceStyleChipId(), std::nullopt);
}

TEST_F(TextEditorTests, HoveringSourceStyleChipSuppressesRangeTooltip) {
  editor.setText("fill: red;");
  SetRawSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 21,
          .range = SourceByteRange{.start = 0, .end = 4},
          .ineffective = true,
          .tooltip = "range tooltip",
      },
  });
  SetSingleSourceStyleChipHitRect(/*id=*/21, ImVec2(-100.0f, -100.0f), ImVec2(1000.0f, 1000.0f),
                                  "chip tooltip");

  RenderSourceStyleDecorationTooltipDirect(ImVec2(16.0f, 16.0f), Coordinates(0, 2));

  EXPECT_EQ(SourceStyleChipHitRectCount(), 1u);
}

TEST_F(TextEditorTests, RemapFocusMetadataNoOpLeavesExistingRanges) {
  editor.setText("abcdef");
  SetRawHoverSourceRanges({SourceByteRange{.start = 1, .end = 4}});
  SetRawSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 3,
          .range = SourceByteRange{.start = 2, .end = 5},
          .chipRange = SourceByteRange{.start = 1, .end = 2},
          .showChip = true,
      },
  });

  RemapFocusMetadataForSourceEdit(SourceEditIntent{});

  EXPECT_THAT(editor.hoverSourceRanges(), ElementsAre(SourceByteRangeIs(1, 4)));
  EXPECT_THAT(editor.sourceStyleDecorations(),
              ElementsAre(SourceStyleDecorationIs(3, SourceByteRange{.start = 2, .end = 5},
                                                  /*ineffective=*/false, /*showChip=*/true,
                                                  /*chipCount=*/0, "",
                                                  SourceByteRange{.start = 1, .end = 2})));
}

TEST_F(TextEditorTests, RemapFocusMetadataMapsRangesAndFocusPartitionAfterEdit) {
  editor.setText("aWXYZdef\nsecond\nthird");
  SetRawHoverSourceRanges({
      SourceByteRange{.start = 0, .end = 1},
      SourceByteRange{.start = 3, .end = 9},
      SourceByteRange{.start = 1, .end = 3},
  });
  SetRawSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 9,
          .range = SourceByteRange{.start = 3, .end = 9},
          .chipRange = SourceByteRange{.start = 1, .end = 3},
          .showChip = true,
      },
  });
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 2}},
      .referenceColor = {LineRange{.startLine = 1, .endLine = 3}},
      .dimmed = {LineRange{.startLine = 2, .endLine = 3}},
      .hidden = {LineRange{.startLine = 1, .endLine = 2}},
      .referenceLinks =
          {
              FocusReferenceLink{
                  .from = SourcePoint{.line = 0, .column = 3},
                  .to = SourcePoint{.line = 2, .column = 0},
              },
          },
  });

  SourceEditIntent intent;
  intent.offset = 1;
  intent.removedLength = 2;
  intent.replacement = "WXYZ";
  intent.start = SourceEditPoint{.line = 0, .column = 1};
  intent.removedEnd = SourceEditPoint{.line = 0, .column = 3};
  intent.replacementEnd = SourceEditPoint{.line = 0, .column = 5};

  RemapFocusMetadataForSourceEdit(intent);

  EXPECT_THAT(editor.hoverSourceRanges(),
              ElementsAre(SourceByteRangeIs(0, 5), SourceByteRangeIs(5, 11)));
  EXPECT_THAT(editor.sourceStyleDecorations(),
              ElementsAre(SourceStyleDecorationIs(9, SourceByteRange{.start = 5, .end = 11},
                                                  /*ineffective=*/false, /*showChip=*/true,
                                                  /*chipCount=*/0, "",
                                                  SourceByteRange{.start = 5, .end = 5})));
  EXPECT_TRUE(editor.hasFocusPartition());
  EXPECT_THAT(ActiveFocusPartition().referenceLinks,
              ElementsAre(FocusReferenceLinkIs(SourcePoint{.line = 0, .column = 5},
                                               SourcePoint{.line = 2, .column = 0})));
}

TEST_F(TextEditorTests, RemapFocusMetadataHandlesInsertionAtRangeBoundary) {
  editor.setText("abXYZ\ncd\nef");
  SetRawHoverSourceRanges({
      SourceByteRange{.start = 2, .end = 3},
      SourceByteRange{.start = 0, .end = 1},
  });
  SetRawSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 12,
          .range = SourceByteRange{.start = 2, .end = 4},
          .chipRange = SourceByteRange{.start = 2, .end = 2},
          .showChip = true,
      },
  });
  SetRawFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 1}},
      .referenceLinks =
          {
              FocusReferenceLink{
                  .from = SourcePoint{.line = 0, .column = 2},
                  .to = SourcePoint{.line = 1, .column = 0},
              },
          },
  });

  SourceEditIntent intent;
  intent.offset = 2;
  intent.removedLength = 0;
  intent.replacement = "XYZ";
  intent.start = SourceEditPoint{.line = 0, .column = 2};
  intent.removedEnd = SourceEditPoint{.line = 0, .column = 2};
  intent.replacementEnd = SourceEditPoint{.line = 0, .column = 5};

  RemapFocusMetadataForSourceEdit(intent);

  EXPECT_THAT(editor.hoverSourceRanges(),
              ElementsAre(SourceByteRangeIs(5, 6), SourceByteRangeIs(0, 1)));
  EXPECT_THAT(editor.sourceStyleDecorations(),
              ElementsAre(SourceStyleDecorationIs(12, SourceByteRange{.start = 5, .end = 7},
                                                  /*ineffective=*/false, /*showChip=*/true,
                                                  /*chipCount=*/0, "",
                                                  SourceByteRange{.start = 5, .end = 5})));
  EXPECT_THAT(ActiveFocusPartition().referenceLinks,
              ElementsAre(FocusReferenceLinkIs(SourcePoint{.line = 0, .column = 5},
                                               SourcePoint{.line = 1, .column = 0})));
}

TEST_F(TextEditorTests, RemapFocusMetadataHandlesShrinkingDeletionAndRawInvalidRanges) {
  editor.setText("ab\ncd\nef");
  SetRawHoverSourceRanges({
      SourceByteRange{.start = 7, .end = 9},
      SourceByteRange{.start = 8, .end = 4},
  });
  SetRawFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 1}},
      .hidden = {LineRange{.startLine = 0, .endLine = 3}},
      .referenceLinks =
          {
              FocusReferenceLink{
                  .from = SourcePoint{.line = 0, .column = 3},
                  .to = SourcePoint{.line = 2, .column = 0},
              },
          },
  });

  SourceEditIntent intent;
  intent.offset = 2;
  intent.removedLength = 3;
  intent.replacement = "";
  intent.start = SourceEditPoint{.line = 0, .column = 2};
  intent.removedEnd = SourceEditPoint{.line = 0, .column = 5};
  intent.replacementEnd = SourceEditPoint{.line = 0, .column = 2};

  RemapFocusMetadataForSourceEdit(intent);

  EXPECT_THAT(editor.hoverSourceRanges(), ElementsAre(SourceByteRangeIs(4, 6)));
  EXPECT_THAT(ActiveFocusPartition().fullColor, IsEmpty());
  EXPECT_THAT(ActiveFocusPartition().referenceLinks,
              ElementsAre(FocusReferenceLinkIs(SourcePoint{.line = 0, .column = 2},
                                               SourcePoint{.line = 2, .column = 0})));
}

TEST_F(TextEditorTests, ContentUpdateHookRunsForShellEdits) {
  int updateCount = 0;
  editor.onContentUpdate = [&updateCount](TextEditor*) { ++updateCount; };
  editor.setText("alpha");
  editor.setCursorPosition(Coordinates(0, 5));

  editor.insertText("!");

  EXPECT_EQ(editor.getText(), "alpha!");
  EXPECT_GE(updateCount, 1);
}

TEST_F(TextEditorTests, PrivateShellForwardersDelegateToCore) {
  editor.setText("alpha beta\ngamma");

  EXPECT_EQ(TextRange(Coordinates(0, 0), Coordinates(0, 5)), "alpha");
  EXPECT_EQ(SanitizeCoordinates(Coordinates(99, 99)), Coordinates(1, 5));

  Coordinates advanced(0, 0);
  AdvanceCoordinates(advanced);
  EXPECT_EQ(advanced, Coordinates(0, 1));

  EXPECT_EQ(FindWordStartDirect(Coordinates(0, 8)), Coordinates(0, 6));
  EXPECT_EQ(FindWordEndDirect(Coordinates(0, 6)), Coordinates(0, 10));
  EXPECT_EQ(FindNextWordDirect(Coordinates(0, 0)), Coordinates(0, 6));
  EXPECT_FALSE(IsOnWordBoundaryDirect(Coordinates(0, 5)));

  Coordinates insertAt(0, 5);
  EXPECT_EQ(InsertTextAtDirect(insertAt, " inserted"), 0);
  EXPECT_EQ(editor.getText(), "alpha inserted beta\ngamma");
  EXPECT_EQ(insertAt, Coordinates(0, 14));

  DeleteRangeDirect(Coordinates(0, 5), Coordinates(0, 14));
  EXPECT_EQ(editor.getText(), "alpha beta\ngamma");

  InsertLineDirect(1, 2);
  EXPECT_EQ(editor.getText(), "alpha beta\nga\nmma");
  RemoveLineDirect(1);
  EXPECT_EQ(editor.getText(), "alpha beta\nmma");
  RemoveLineDirect(0, 1);
  EXPECT_EQ(editor.getText(), "");
}

TEST_F(TextEditorTests, EditingForwardersDelegateToCore) {
  editor.setText("ab\ncd");
  editor.setCursorPosition(Coordinates(0, 2));
  UndoRecord lineJoinUndo;
  editor.handleEndOfLineDelete(Coordinates(0, 2), lineJoinUndo);
  EXPECT_EQ(editor.getText(), "abcd");
  EXPECT_EQ(lineJoinUndo.removed, "\n");
  EXPECT_EQ(lineJoinUndo.removedStart, Coordinates(0, 2));
  EXPECT_EQ(lineJoinUndo.removedEnd, Coordinates(1, 0));

  editor.setText("abcd");
  editor.setCursorPosition(Coordinates(0, 1));
  UndoRecord characterDeleteUndo;
  editor.handleMidLineDelete(Coordinates(0, 1), characterDeleteUndo);
  EXPECT_EQ(editor.getText(), "acd");
  EXPECT_EQ(characterDeleteUndo.removed, "b");

  editor.setSelection(Coordinates(0, 1), Coordinates(0, 2));
  DeleteSelectionDirect();
  EXPECT_EQ(editor.getText(), "ad");
  EXPECT_FALSE(editor.hasSelection());

  editor.setText("line0\nline1\nline2\nline3");
  std::vector<Coordinates> folds = {Coordinates(0, 0), Coordinates(3, 0)};
  RemoveFoldsDirect(folds, Coordinates(1, 0), Coordinates(2, 100000));
  EXPECT_THAT(folds, ElementsAre(Coordinates(0, 0), Coordinates(1, 0)));

  ConfigureFoldState({Coordinates(0, 0), Coordinates(3, 0)}, {Coordinates(0, 1), Coordinates(3, 1)},
                     {false, false}, {0, 1});
  RemoveFoldsDirect(Coordinates(1, 0), Coordinates(2, 100000));
  EXPECT_THAT(FoldBeginDirect(), ElementsAre(Coordinates(0, 0), Coordinates(1, 0)));
  EXPECT_THAT(FoldEndDirect(), ElementsAre(Coordinates(0, 1), Coordinates(1, 1)));

  editor.setScrollbarMarkers(true);
  editor.setCursorPosition(Coordinates(1, 0));
  UpdateChangeTrackingDirect();
  EXPECT_THAT(ChangedLinesDirect(), ElementsAre(1));

  editor.setShortcut(TextEditor::ShortcutId::MoveUp, Shortcut(ImGuiKey_W, ShortcutModifier::Ctrl));
  const Shortcut& moveUpShortcut = ShortcutForDirect(TextEditor::ShortcutId::MoveUp);
  EXPECT_EQ(moveUpShortcut.key1, ImGuiKey_W);
  EXPECT_EQ(moveUpShortcut.modifiers, ShortcutModifier::Ctrl);

  editor.setColorizerEnabled(false);
  editor.colorize(0, -1);
  editor.colorizeRange(0, 1);

  editor.setText("  alpha\n    beta");
  editor.setTabSize(8);
  editor.setInsertSpaces(false);
  DetectIndentationStyleDirect();
  EXPECT_TRUE(editor.getInsertSpaces());
  EXPECT_EQ(editor.getTabSize(), 2);
}

TEST_F(TextEditorTests, DirectCoordinateConversionHandlesTabsAndMouseAdjustment) {
  editor.setWordWrapEnabled(false);
  editor.setText("\tabc\nz");

  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  const ImVec2 beforeTabMidpoint = ScreenPointAtUnwrappedVisualLine(0, 1.0f);
  EXPECT_EQ(ScreenPosToCoordinatesDirect(beforeTabMidpoint), Coordinates(0, 0));
  EXPECT_EQ(MousePosToCoordinatesDirect(beforeTabMidpoint), Coordinates(0, 0));

  const ImVec2 tabLinePoint = ScreenPointAtCoordinates(Coordinates(0, 1));
  EXPECT_EQ(ScreenPosToCoordinatesDirect(tabLinePoint).line, 0);
  EXPECT_GE(ScreenPosToCoordinatesDirect(tabLinePoint).column, 1);

  const Coordinates mouseCoords = MousePosToCoordinatesDirect(tabLinePoint);
  EXPECT_EQ(mouseCoords.line, 0);
  EXPECT_GE(mouseCoords.column, 0);

  const ImVec2 belowText(tabLinePoint.x, tabLinePoint.y + 500.0f);
  EXPECT_EQ(ScreenPosToCoordinatesDirect(belowText).line, 1);
  EXPECT_EQ(MousePosToCoordinatesDirect(belowText).line, 1);
}

TEST_F(TextEditorTests, DirectCoordinateConversionHandlesUtf8GlyphWidths) {
  editor.setWordWrapEnabled(false);
  editor.setText("\xC3\xA9x");

  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  const ImVec2 afterFirstGlyph = ScreenPointAtCoordinates(Coordinates(0, 1));
  EXPECT_EQ(ScreenPosToCoordinatesDirect(afterFirstGlyph), Coordinates(0, 2));
  EXPECT_EQ(MousePosToCoordinatesDirect(afterFirstGlyph), Coordinates(0, 2));
}

TEST_F(TextEditorTests, WrappedMouseCoordinatesUseVisualLineMapping) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");

  RenderEditorFrame(ImVec2(180.0f, 120.0f));

  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  ASSERT_GT(VisualLineEndColumn(continuationIndex), VisualLineStartColumn(continuationIndex) + 3);

  const ImVec2 continuationPoint =
      ScreenPointAtVisualTextOffset(continuationIndex, /*visualColumnOffset=*/3);

  EXPECT_EQ(MousePosToCoordinatesDirect(continuationPoint),
            Coordinates(0, VisualLineStartColumn(continuationIndex) + 3));
}

TEST_F(TextEditorTests, FindFirstDirectSkipsSubstringMatchesAndInvalidStarts) {
  editor.setText("alpha beta alphabet\nbeta");

  EXPECT_EQ(FindFirstDirect("beta", Coordinates(0, 0)), Coordinates(0, 6));
  EXPECT_EQ(FindFirstDirect("beta", Coordinates(0, 7)), Coordinates(1, 0));
  EXPECT_EQ(FindFirstDirect("delta", Coordinates(0, 0)), Coordinates(2, 0));
  EXPECT_EQ(FindFirstDirect("alpha", Coordinates(99, 0)), Coordinates(2, 0));
}

TEST_F(TextEditorTests, VisualLineHelpersHandleCacheEmptyLayoutAndFallbacks) {
  editor.setText("alpha\nbeta\ngamma");
  RenderEditorFrame(ImVec2(320.0f, 140.0f));

  RebuildVisualLinesDirect(ImVec2(320.0f, 140.0f));
  const int visualLineCount = VisualLineCount();
  RebuildVisualLinesDirect(ImVec2(320.0f, 140.0f));
  EXPECT_EQ(VisualLineCount(), visualLineCount);

  ClearVisualLinesDirect();
  EXPECT_EQ(VisualLineIndexForCoordinates(Coordinates(3, 0)), 3);
  EXPECT_EQ(VisualScreenPosToCoordinatesDirect(ImVec2(12.0f, 18.0f)), Coordinates(0, 0));

  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 3}},
      .hidden = {LineRange{.startLine = 1, .endLine = 2}},
  });
  RenderEditorFrame(ImVec2(320.0f, 140.0f));
  ASSERT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));

  EXPECT_EQ(VisualLineIndexForCoordinates(Coordinates(1, 99)), 1);

  const ImVec2 farLeft =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/2, /*visualColumnOffset=*/-100);
  EXPECT_EQ(VisualScreenPosToCoordinatesDirect(farLeft), Coordinates(2, 0));
}

TEST_F(TextEditorTests, VisibleSelectionEndAndScrollHelpersCoverBoundaryCases) {
  editor.setText("alpha\nbeta");

  editor.setSelection(Coordinates(0, 2), Coordinates(1, 0));
  EXPECT_EQ(VisibleSelectionEndCoordinatesDirect(), Coordinates(0, 5));

  editor.setSelection(Coordinates(0, 1), Coordinates(0, 4));
  EXPECT_EQ(VisibleSelectionEndCoordinatesDirect(), Coordinates(0, 3));

  SetScrollViewportHeight(37.0f);
  EXPECT_FLOAT_EQ(VisibleTextRegionHeightDirect(), 37.0f);
  EXPECT_FLOAT_EQ(VisibleTextRegionHeightInChild(ImVec2(240.0f, 64.0f)), 64.0f);

  editor.setCursorPosition(Coordinates(1, 0));
  ScrollCoordinatesRangeIntoViewDirect(Coordinates(1, 0), Coordinates(1, 2));
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0));
}

TEST_F(TextEditorTests, HandleScrollingOutsideRenderKeepsPendingScrollRequest) {
  editor.setText("alpha\nbeta");

  HandleScrollingOutsideRenderDirect();
  EXPECT_FALSE(ScrollToCursorRequested());

  RequestCursorScroll();
  HandleScrollingOutsideRenderDirect();
  EXPECT_TRUE(ScrollToCursorRequested());
}

TEST_F(TextEditorTests, WrappedScrollHelpersUseVisualLineRanges) {
  editor.setText("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda");
  editor.setWordWrapEnabled(true);
  RenderEditorFrame(ImVec2(140.0f, 80.0f));
  RebuildVisualLinesDirect(ImVec2(140.0f, 80.0f));
  ASSERT_GT(VisualLineCount(), 1);

  const float scrollTarget = ScrollRangeIntoViewDirectInChild(
      Coordinates(0, 45), Coordinates(0, 55), 0.0f, ImVec2(140.0f, CharacterAdvanceY() * 2.0f));
  EXPECT_GT(scrollTarget, 0.0f);

  editor.setCursorPosition(Coordinates(0, 55));
  const float cursorScrollTarget =
      EnsureCursorVisibleDirectInChild(0.0f, ImVec2(140.0f, CharacterAdvanceY() * 2.0f));
  EXPECT_GT(cursorScrollTarget, 0.0f);
}

TEST_F(TextEditorTests, FocusPartitionScrollHelpersUseVisualLineRangesWithoutWordWrap) {
  editor.setWordWrapEnabled(false);
  editor.setText("alpha\nhidden one\nhidden two\nvisible tail");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 1},
                    LineRange{.startLine = 3, .endLine = 4}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  });
  RenderEditorFrame(ImVec2(220.0f, 80.0f));
  ASSERT_GT(VisualLineCount(), 1);

  const float rangeTarget = ScrollRangeIntoViewDirectInChild(Coordinates(3, 0), Coordinates(3, 8),
                                                             /*initialScrollY=*/0.0f,
                                                             ImVec2(220.0f, CharacterAdvanceY()));
  EXPECT_GT(rangeTarget, 0.0f);

  editor.setCursorPosition(Coordinates(3, 4));
  const float cursorTarget =
      EnsureCursorVisibleDirectInChild(0.0f, ImVec2(220.0f, CharacterAdvanceY()));
  EXPECT_GT(cursorTarget, 0.0f);
}

TEST_F(TextEditorTests, WrappedCursorUsesVisualLineColumn) {
  editor.setText("alpha beta gamma delta epsilon zeta eta theta");
  editor.setWordWrapEnabled(true);
  RenderEditorFrame(ImVec2(140.0f, 80.0f));
  RebuildVisualLinesDirect(ImVec2(140.0f, 80.0f));
  ASSERT_GT(VisualLineCount(), 1);

  editor.setCursorPosition(Coordinates(0, 24));
  EXPECT_GT(
      RenderCursorDirectInChild(ImVec2(0.0f, 0.0f), ImVec2(140.0f, CharacterAdvanceY() * 2.0f)), 0);
}

TEST_F(TextEditorTests, FocusPartitionCursorUsesVisualLineColumnWithoutWordWrap) {
  editor.setWordWrapEnabled(false);
  editor.setText("alpha\nhidden one\nhidden two\nvisible tail");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 1},
                    LineRange{.startLine = 3, .endLine = 4}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  });
  RenderEditorFrame(ImVec2(220.0f, 80.0f));
  ASSERT_GT(VisualLineCount(), 1);

  editor.setCursorPosition(Coordinates(3, 7));
  EXPECT_GT(RenderCursorDirectInChild(ImVec2(0.0f, CharacterAdvanceY()),
                                      ImVec2(220.0f, CharacterAdvanceY() * 2.0f)),
            0);
}

TEST_F(TextEditorTests, TextDistanceHandlesTruncatedUtf8GlyphBytes) {
  editor.setWordWrapEnabled(false);
  editor.setText(std::string("a\xF0\x9F", 3));
  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  EXPECT_GT(TextDistanceToLineStartDirect(Coordinates(0, 2)),
            TextDistanceToLineStartDirect(Coordinates(0, 1)));
}

TEST_F(TextEditorTests, FoldedScreenPositionMapsPastCollapsedRegion) {
  editor.setWordWrapEnabled(false);
  editor.setText("zero\none\ntwo\nthree");
  RenderEditorFrame(ImVec2(320.0f, 160.0f));

  ConfigureFoldState({Coordinates(0, 0)}, {Coordinates(2, 0)}, {true}, {0});

  const ImVec2 secondVisibleLine = ScreenPointAtUnwrappedVisualLine(1, 40.0f);
  EXPECT_EQ(ScreenPosToCoordinatesDirect(secondVisibleLine).line, 3);
  EXPECT_EQ(MousePosToCoordinatesDirect(secondVisibleLine).line, 3);
}

TEST_F(TextEditorTests, FoldedScreenPositionIgnoresInvalidFoldConnection) {
  editor.setWordWrapEnabled(false);
  editor.setText("zero\none\ntwo");
  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  ConfigureFoldState({Coordinates(0, 0)}, {Coordinates(2, 0)}, {true}, {-1});

  const ImVec2 secondVisibleLine = ScreenPointAtUnwrappedVisualLine(1, 40.0f);
  EXPECT_EQ(ScreenPosToCoordinatesDirect(secondVisibleLine).line, 1);
}

TEST_F(TextEditorTests, FoldedScreenPositionIgnoresOutOfRangeFoldMetadata) {
  editor.setWordWrapEnabled(false);
  editor.setText("zero\none\ntwo");
  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  ConfigureFoldState({Coordinates(0, 0)}, {Coordinates(2, 0)}, {true}, {7});

  const ImVec2 secondVisibleLine = ScreenPointAtUnwrappedVisualLine(1, 40.0f);
  EXPECT_EQ(ScreenPosToCoordinatesDirect(secondVisibleLine).line, 1);

  ConfigureFoldState({Coordinates(0, 0), Coordinates(1, 0)}, {Coordinates(2, 0)}, {false}, {0});
  const ImVec2 thirdVisibleLine = ScreenPointAtUnwrappedVisualLine(2, 40.0f);
  EXPECT_EQ(MousePosToCoordinatesDirect(thirdVisibleLine).line, 2);
}

TEST_F(TextEditorTests, HoveredTextPositionRejectsDegenerateCharacterAdvance) {
  editor.setWordWrapEnabled(false);
  editor.setText("alpha");
  RenderEditorFrame(ImVec2(320.0f, 120.0f));

  SetCharAdvanceForTesting(ImVec2(0.0f, CharacterAdvanceY()));
  UpdateHoveredTextPositionDirect(ImVec2(TextStart() + 20.0f, 20.0f));

  EXPECT_FALSE(HoveredTextPosition().has_value());
}

TEST_F(TextEditorTests, CharacterInputSkipsControlCharactersAndTracksLetters) {
  editor.setText("");

  const std::array<ImWchar, 5> characters = {0, 1, 'a', '.', '\n'};
  const auto [keepAutocompleteOpen, hasWrittenLetter] = HandleCharacterInputDirect(characters);

  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_TRUE(hasWrittenLetter);
  EXPECT_EQ(editor.getText(), "a.\n");
}

TEST_F(TextEditorTests, CharacterInputLeavesLetterFlagClearForDigitsAndPunctuation) {
  editor.setText("");

  const std::array<ImWchar, 2> characters = {'1', '_'};
  const auto [keepAutocompleteOpen, hasWrittenLetter] = HandleCharacterInputDirect(characters);

  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_FALSE(hasWrittenLetter);
  EXPECT_EQ(editor.getText(), "1_");
}

TEST_F(TextEditorTests, SnippetTypingPropagatesRepeatedPlaceholderText) {
  ActivateSnippetEditing(R"(<{$1:rect}>{$1}</{$1}>)");

  const std::array<ImWchar, 1> characters = {'x'};
  const auto [keepAutocompleteOpen, hasWrittenLetter] = HandleCharacterInputDirect(characters);

  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_TRUE(hasWrittenLetter);
  EXPECT_EQ(editor.getText(), "<x>x</x>");
  EXPECT_THAT(CoordinateColumnWidths(SnippetStarts(), SnippetEnds()), ElementsAre(1, 1, 1));
}

TEST_F(TextEditorTests, AutocompleteSelectReplacesProviderRange) {
  editor.setText("alpha beta gamma");
  OpenAutocompleteReplacement(/*replaceStart=*/6, /*replaceEnd=*/10, "replacement");

  AutocompleteSelectDirect();

  EXPECT_EQ(editor.getText(), "alpha replacement gamma");
  EXPECT_FALSE(AutocompleteOpened());
  EXPECT_FALSE(AutocompleteReplacementActive());
}

TEST_F(TextEditorTests, AutocompleteSelectOutOfRangeLeavesPopupOpen) {
  editor.setText("alpha");
  OpenAutocompleteWithSuggestions({"beta"});
  SetAutocompleteIndex(9);

  AutocompleteSelectDirect();

  EXPECT_EQ(editor.getText(), "alpha");
  EXPECT_TRUE(AutocompleteOpened());
}

TEST_F(TextEditorTests, AutocompleteNavigateClampsAtBothEnds) {
  OpenAutocompleteWithSuggestions({"alpha", "beta"});

  AutocompleteNavigateDirect(/*up=*/true);
  EXPECT_EQ(AutocompleteIndex(), 0);

  AutocompleteNavigateDirect(/*up=*/false);
  EXPECT_EQ(AutocompleteIndex(), 1);
  AutocompleteNavigateDirect(/*up=*/false);
  EXPECT_EQ(AutocompleteIndex(), 1);
}

TEST_F(TextEditorTests, ExecuteActionDirectCoversAutocompleteCases) {
  editor.setText("");
  OpenAutocompleteWithSuggestions({"alpha", "beta"});

  bool keepAutocompleteOpen = false;
  ExecuteActionDirect(TextEditor::ShortcutId::AutocompleteDown, keepAutocompleteOpen);
  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_EQ(AutocompleteIndex(), 1);

  keepAutocompleteOpen = false;
  ExecuteActionDirect(TextEditor::ShortcutId::AutocompleteUp, keepAutocompleteOpen);
  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_EQ(AutocompleteIndex(), 0);

  keepAutocompleteOpen = true;
  ExecuteActionDirect(TextEditor::ShortcutId::AutocompleteSelectActive, keepAutocompleteOpen);
  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "alpha");
  EXPECT_FALSE(AutocompleteOpened());

  editor.setText("");
  OpenAutocompleteWithSuggestions({"gamma"});
  keepAutocompleteOpen = true;
  ExecuteActionDirect(TextEditor::ShortcutId::AutocompleteSelect, keepAutocompleteOpen);
  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "gamma");
  EXPECT_FALSE(AutocompleteOpened());
}

TEST_F(TextEditorTests, AddUndoWrapperFeedsCoreUndoStack) {
  editor.setText("abc");
  AddUndoRecordForInsertedText("b", Coordinates(0, 1), Coordinates(0, 2));

  ASSERT_TRUE(editor.canUndo());
  editor.undo();

  EXPECT_EQ(editor.getText(), "ac");
}

TEST_F(TextEditorTests, ExecuteActionDirectCoversNavigationAndEditingCases) {
  bool keepAutocompleteOpen = false;

  editor.setText("a\nb\nc");
  editor.setCursorPosition(Coordinates(1, 0));
  ExecuteActionDirect(TextEditor::ShortcutId::MoveUp, keepAutocompleteOpen);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  ExecuteActionDirect(TextEditor::ShortcutId::MoveDown, keepAutocompleteOpen);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0));
  ExecuteActionDirect(TextEditor::ShortcutId::MoveRight, keepAutocompleteOpen);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 1));
  ExecuteActionDirect(TextEditor::ShortcutId::MoveLeft, keepAutocompleteOpen);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0));

  ExecuteActionDirect(TextEditor::ShortcutId::SelectDown, keepAutocompleteOpen);
  EXPECT_EQ(editor.getSelectedText(), "b\n");
  ExecuteActionDirect(TextEditor::ShortcutId::SelectUp, keepAutocompleteOpen);
  EXPECT_FALSE(editor.hasSelection());

  editor.setText("abc");
  editor.setCursorPosition(Coordinates(0, 1));
  ExecuteActionDirect(TextEditor::ShortcutId::ForwardDelete, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ac");

  editor.setText("abc");
  editor.setCursorPosition(Coordinates(0, 2));
  ExecuteActionDirect(TextEditor::ShortcutId::BackwardDelete, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ac");

  editor.setText("abc");
  editor.setCursorPosition(Coordinates(0, 0));
  ExecuteActionDirect(TextEditor::ShortcutId::SelectAll, keepAutocompleteOpen);
  EXPECT_EQ(editor.getSelectedText(), "abc");

  editor.setText("ab");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.setSelection(Coordinates(0, 1), Coordinates(0, 1));
  ExecuteActionDirect(TextEditor::ShortcutId::NewLine, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "a\nb");

  editor.setText("");
  editor.setInsertSpaces(false);
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 0));
  ExecuteActionDirect(TextEditor::ShortcutId::Indent, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "\t");

  editor.setText("ab");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 2));
  ExecuteActionDirect(TextEditor::ShortcutId::Indent, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ab");

  editor.setText("abc");
  AddUndoRecordForInsertedText("b", Coordinates(0, 1), Coordinates(0, 2));
  ExecuteActionDirect(TextEditor::ShortcutId::Undo, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ac");
  ExecuteActionDirect(TextEditor::ShortcutId::Redo, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "abc");
}

TEST_F(TextEditorTests, ExecuteActionDirectCoversAliasesClipboardAndDefaultCases) {
  bool keepAutocompleteOpen = false;

  editor.setText("abc");
  editor.setCursorPosition(Coordinates(0, 1));
  ExecuteActionDirect(TextEditor::ShortcutId::DeleteRight, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ac");

  editor.setText("abc");
  editor.setCursorPosition(Coordinates(0, 2));
  ExecuteActionDirect(TextEditor::ShortcutId::DeleteLeft, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "ac");

  editor.setText("copy paste");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 4));
  ExecuteActionDirect(TextEditor::ShortcutId::Copy, keepAutocompleteOpen);
  EXPECT_STREQ(ImGui::GetClipboardText(), "copy");

  editor.setSelection(Coordinates(0, 5), Coordinates(0, 10));
  ExecuteActionDirect(TextEditor::ShortcutId::Cut, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "copy ");
  EXPECT_STREQ(ImGui::GetClipboardText(), "paste");

  editor.setCursorPosition(Coordinates(0, 5));
  ExecuteActionDirect(TextEditor::ShortcutId::Paste, keepAutocompleteOpen);
  EXPECT_EQ(editor.getText(), "copy paste");

  const TextEditor::ShortcutId defaultOnlyActions[] = {
      TextEditor::ShortcutId::MoveWordLeft,      TextEditor::ShortcutId::SelectWordLeft,
      TextEditor::ShortcutId::MoveWordRight,     TextEditor::ShortcutId::SelectWordRight,
      TextEditor::ShortcutId::MoveUpBlock,       TextEditor::ShortcutId::SelectUpBlock,
      TextEditor::ShortcutId::MoveDownBlock,     TextEditor::ShortcutId::SelectDownBlock,
      TextEditor::ShortcutId::MoveTop,           TextEditor::ShortcutId::SelectTop,
      TextEditor::ShortcutId::MoveBottom,        TextEditor::ShortcutId::SelectBottom,
      TextEditor::ShortcutId::MoveStartLine,     TextEditor::ShortcutId::SelectStartLine,
      TextEditor::ShortcutId::MoveEndLine,       TextEditor::ShortcutId::SelectEndLine,
      TextEditor::ShortcutId::ForwardDeleteWord, TextEditor::ShortcutId::BackwardDeleteWord,
      TextEditor::ShortcutId::Unindent,          TextEditor::ShortcutId::Find,
      TextEditor::ShortcutId::Replace,           TextEditor::ShortcutId::FindNext,
      TextEditor::ShortcutId::DuplicateLine,     TextEditor::ShortcutId::CommentLines,
      TextEditor::ShortcutId::UncommentLines,    TextEditor::ShortcutId::Count,
  };

  for (TextEditor::ShortcutId action : defaultOnlyActions) {
    SCOPED_TRACE(static_cast<int>(action));
    ExecuteActionDirect(action, keepAutocompleteOpen);
  }
  EXPECT_EQ(editor.getText(), "copy paste");
}

TEST_F(TextEditorTests, UpdateFoldedLinesAccountsForConnectedAndInvalidFolds) {
  editor.setText("zero\none\ntwo\nthree\nfour\nfive");

  ConfigureFoldState({Coordinates(0, 0), Coordinates(2, 0), Coordinates(3, 0)},
                     {Coordinates(2, 0), Coordinates(5, 0), Coordinates(4, 0)}, {true, true, true},
                     {-1, 99, 2});
  UpdateFoldedLinesDirect(/*currentLine=*/5, /*totalLines=*/6);
  EXPECT_EQ(FoldedLines(), 1);

  ConfigureFoldState({Coordinates(0, 0)}, {Coordinates(3, 0)}, {true}, {0});
  UpdateFoldedLinesDirect(/*currentLine=*/1, /*totalLines=*/6);
  EXPECT_EQ(FoldedLines(), 3);

  UpdateFoldedLinesDirect(/*currentLine=*/0, /*totalLines=*/6);
  EXPECT_EQ(FoldedLines(), 0);
}

TEST_F(TextEditorTests, CalculateFoldsSortsAndPairsNestedFoldEndpoints) {
  editor.setText("{\n  {\n  }\n}\n");
  ConfigureFoldEndpointsForCalculation({Coordinates(1, 2), Coordinates(0, 0)},
                                       {Coordinates(3, 0), Coordinates(2, 2)});

  CalculateFoldsDirect(/*currentLine=*/3, /*totalLines=*/4);

  EXPECT_EQ(FoldConnections(), (std::vector<int>{1, 0}));
  EXPECT_EQ(FoldStates(), (std::vector<bool>{false, false}));
}

TEST_F(TextEditorTests, MoveLeftRetractsCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveLeft(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveLeft should move cursor back by 1 column";
}

TEST_F(TextEditorTests, MoveRightAtLineEndWrapsToNextLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(0, 5));  // End of "Hello"
  editor.moveRight(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0))
      << "moveRight at line end should wrap to start of next line";
}

TEST_F(TextEditorTests, MoveLeftAtLineStartWrapsToEndOfPreviousLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(1, 0));  // Start of "World"
  editor.moveLeft(1, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveLeft at line start should wrap to end of previous line";
}

TEST_F(TextEditorTests, MoveUpAdvancesLineUp) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 2));
  editor.moveUp(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveUp should move cursor up by 1 line";
}

TEST_F(TextEditorTests, MoveDownAdvancesLineDown) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveDown(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveDown should move cursor down by 1 line";
}

TEST_F(TextEditorTests, MoveUpOnFirstLineStaysAtFirstLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveUp(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 2))
      << "moveUp on first line should not change position";
}

TEST_F(TextEditorTests, MoveDownOnLastLineStaysAtLastLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(1, 2));
  editor.moveDown(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 2))
      << "moveDown on last line should not change position";
}

TEST_F(TextEditorTests, MoveHomeSetsColumnToZero) {
  editor.setText("Line1");
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveHome(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0))
      << "moveHome should move cursor to column 0 of current line";
}

TEST_F(TextEditorTests, MoveEndSetsColumnToLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveEnd(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveEnd should move cursor to end of current line";
}

TEST_F(TextEditorTests, MoveTopJumpsToDocumentStart) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 3));
  editor.moveTop(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0))
      << "moveTop should move cursor to (0, 0)";
}

TEST_F(TextEditorTests, MoveBottomJumpsToDocumentEnd) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveBottom(false);
  // moveBottom moves to (lastLine, 0), not end of last line
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(2, 0))
      << "moveBottom should move cursor to (lastLine, 0)";
}

TEST_F(TextEditorTests, PublicEditingWrappersForwardToCore) {
  editor.setText("alpha\n\tbeta gamma");
  editor.setTabSize(4);
  editor.setCursorPosition(Coordinates(1, 4));

  EXPECT_EQ(editor.getCorrectCursorPosition(), Coordinates(1, 1));

  editor.moveLeft(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 3));
  editor.moveRight(2, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 5));
  editor.moveHome(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0));
  editor.moveEnd(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 12));
  editor.moveUp(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5));
  editor.moveDown(1, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 12));
  editor.moveTop(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  editor.moveBottom(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0));

  editor.setCursorPosition(Coordinates(0, 1));
  editor.selectWordUnderCursor();
  EXPECT_EQ(editor.getSelectedText(), "alpha");
  editor.clearSelection();
  EXPECT_FALSE(editor.hasSelection());

  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  DeleteSelectionDirect();
  EXPECT_EQ(editor.getText(), "\n\tbeta gamma");

  editor.insertText("start");
  EXPECT_EQ(editor.getText(), "start\n\tbeta gamma");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "tart\n\tbeta gamma");
  BackspaceDirect();
  EXPECT_EQ(editor.getText(), "tart\n\tbeta gamma");

  editor.setShortcut(TextEditor::ShortcutId::MoveUp, Shortcut(ImGuiKey_W, ShortcutModifier::Ctrl));
  const Shortcut& moveUpShortcut = ShortcutForDirect(TextEditor::ShortcutId::MoveUp);
  EXPECT_EQ(moveUpShortcut.key1, ImGuiKey_W);
  EXPECT_EQ(moveUpShortcut.modifiers, ShortcutModifier::Ctrl);
}

// ============================================================================
// SELECTION TESTS
// ============================================================================

TEST_F(TextEditorTests, SelectAllSelectsEntireBuffer) {
  editor.setText("Hello world");
  editor.selectAll();
  EXPECT_EQ(editor.getSelectedText(), "Hello world")
      << "selectAll should select entire buffer content";
}

TEST_F(TextEditorTests, ClearSelectionCollapsesActiveSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setCursorPosition(Coordinates(0, 5));
  ASSERT_TRUE(editor.hasSelection()) << "precondition: a selection exists";

  editor.clearSelection();

  EXPECT_FALSE(editor.hasSelection())
      << "clearSelection should collapse the active text selection to the caret";
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "clearSelection should leave the cursor where it was";
}

TEST_F(TextEditorTests, ClearSelectionIsNoOpWhenEmpty) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 3));
  ASSERT_FALSE(editor.hasSelection()) << "precondition: no selection";

  editor.clearSelection();

  EXPECT_FALSE(editor.hasSelection()) << "clearSelection should be a no-op with no selection";
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 3))
      << "clearSelection should not move the cursor when there is nothing to clear";
}

TEST_F(TextEditorTests, ShiftRightExpandsSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(1, true, false);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Should have selection after Shift+Right";
  EXPECT_EQ(editor.getSelectedText(), "H") << "Selection should contain single character 'H'";
}

TEST_F(TextEditorTests, ShiftLeftContractsSelection) {
  editor.setText("Hello");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 3));
  // `setSelection` doesn't move the cursor, so put it explicitly at
  // the selection's end before contracting - `moveLeft(_, select)`
  // grows/shrinks relative to the cursor position, not the
  // selection bounds.
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveLeft(1, true, false);
  // After contracting left, selection should be from (0,0) to (0,2)
  EXPECT_EQ(editor.getSelectedText(), "He") << "Selection should be contracted to 'He'";
}

TEST_F(TextEditorTests, SelectionStartAndEnd) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  EXPECT_EQ(editor.getSelectedText(), "Hello") << "setSelection should select specified range";
}

TEST_F(TextEditorTests, VisibleSelectionEndBacksUpFromExclusiveMultilineEnd) {
  editor.setText("abc\ndef");

  editor.setCursorPosition(Coordinates(0, 1));
  EXPECT_EQ(VisibleSelectionEndCoordinatesDirect(), Coordinates(0, 0));

  editor.setSelection(Coordinates(0, 1), Coordinates(0, 3));
  EXPECT_EQ(VisibleSelectionEndCoordinatesDirect(), Coordinates(0, 2));

  editor.setSelection(Coordinates(0, 1), Coordinates(1, 0));
  EXPECT_EQ(VisibleSelectionEndCoordinatesDirect(), Coordinates(0, 3));
}

TEST_F(TextEditorTests, HasSelectionReturnsFalseWhenNoSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));
  EXPECT_FALSE(editor.hasSelection())
      << "hasSelection should return false after setCursorPosition clears selection";
}

TEST_F(TextEditorTests, SetSelectionStartPreservesEnd) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setSelectionStart(Coordinates(0, 2));
  // Selection should now span from (0, 2) to (0, 5)
  EXPECT_EQ(editor.getSelectedText(), "llo")
      << "setSelectionStart should adjust start while preserving end";
}

TEST_F(TextEditorTests, SetSelectionEndPreservesStart) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setSelectionEnd(Coordinates(0, 8));
  // Selection should now span from (0, 0) to (0, 8)
  EXPECT_EQ(editor.getSelectedText(), "Hello wo")
      << "setSelectionEnd should adjust end while preserving start";
}

// ============================================================================
// INSERTION & DELETION TESTS
// ============================================================================

TEST_F(TextEditorTests, InsertTextAddsCharacters) {
  editor.setText("");
  editor.insertText("Hello");
  EXPECT_EQ(editor.getText(), "Hello") << "insertText should add text to buffer";
}

TEST_F(TextEditorTests, DeleteDeletesCharacterAtCursor) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Hllo") << "delete should remove character at cursor";
}

TEST_F(TextEditorTests, DeleteAtLineEndMergesWithNextLine) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "HelloWorld") << "delete at line end should merge with next line";
}

TEST_F(TextEditorTests, DeleteOnLastCharacterOfLastLineDoesNothing) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Hello") << "delete on last character should not delete";
}

TEST_F(TextEditorTests, InsertTextWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.insertText("Hi");
  EXPECT_EQ(editor.getText(), "Hi world") << "insertText with selection should replace selection";
}

// ============================================================================
// UNDO/REDO TESTS
//
// IMPORTANT: `editor.insertText()` is a raw-primitive that does NOT record
// undo entries - only the user-facing path (`enterCharacter`, `backspace`,
// `delete_`, `cut`, `paste`) records undo. These tests exercise the real
// user-facing path. If a future refactor makes `insertText` participate in
// the undo system, additional tests for that direct-API case should be
// added separately.
// ============================================================================

TEST_F(TextEditorTests, UndoReversesInsertion) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  EXPECT_EQ(editor.getText(), "Hello!");
  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello") << "undo should revert insertion";
}

TEST_F(TextEditorTests, UndoReversesDeletion) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  // `delete_()` removes the character at the cursor (column 2 = first
  // 'l'), so "Hello" → "Helo". Previously this test expected "Hllo",
  // which is what *backspace* would produce.
  editor.delete_();
  EXPECT_EQ(editor.getText(), "Helo");
  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello") << "undo should revert deletion";
}

TEST_F(TextEditorTests, RedoRestoresAfterUndo) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  editor.undo();
  editor.redo();
  EXPECT_EQ(editor.getText(), "Hello!") << "redo should restore undone change";
}

TEST_F(TextEditorTests, MultipleUndoStepsBackthrough) {
  editor.setText("H");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.enterCharacter('e', /*shift=*/false);
  editor.enterCharacter('l', /*shift=*/false);
  editor.enterCharacter('l', /*shift=*/false);
  editor.enterCharacter('o', /*shift=*/false);
  EXPECT_EQ(editor.getText(), "Hello");
  editor.undo(3);
  EXPECT_EQ(editor.getText(), "He") << "undo(3) should step back 3 operations";
}

TEST_F(TextEditorTests, RedoIsClearedAfterNewEdit) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  editor.undo();
  EXPECT_TRUE(editor.canRedo()) << "Should be able to redo after undo";
  editor.enterCharacter('?', /*shift=*/false);
  EXPECT_FALSE(editor.canRedo()) << "redo should be cleared after new edit";
}

TEST_F(TextEditorTests, CanUndoReturnsFalseAtStart) {
  editor.setText("Hello");
  EXPECT_FALSE(editor.canUndo()) << "canUndo should return false with no undo history";
}

TEST_F(TextEditorTests, CanUndoReturnsTrueAfterEdit) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.enterCharacter('!', /*shift=*/false);
  EXPECT_TRUE(editor.canUndo()) << "canUndo should return true after edit";
}

// ============================================================================
// DOUBLE-CLICK WORD SELECTION TESTS
// ============================================================================

TEST_F(TextEditorTests, SelectWordUnderCursorSelectsWord) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 2));  // Inside "Hello"
  editor.selectWordUnderCursor();
  EXPECT_EQ(editor.getSelectedText(), "Hello") << "selectWordUnderCursor should select entire word";
}

TEST_F(TextEditorTests, SelectWordAtPunctuation) {
  editor.setText("Hello, world");
  editor.setCursorPosition(Coordinates(0, 5));  // On the comma
  editor.selectWordUnderCursor();
  // Comma is a punctuation token, so it should be selected as a single unit
  EXPECT_EQ(editor.getSelectedText(), ",")
      << "selectWordUnderCursor on punctuation should select punctuation run";
}

TEST_F(TextEditorTests, SelectWordOnWhitespace) {
  editor.setText("Hello  world");
  editor.setCursorPosition(Coordinates(0, 6));  // On the space
  editor.selectWordUnderCursor();
  // Should select the whitespace run
  EXPECT_EQ(editor.getSelectedText(), "  ")
      << "selectWordUnderCursor on whitespace should select whitespace run";
}

// ============================================================================
// MULTI-LINE EDITING TESTS
// ============================================================================

TEST_F(TextEditorTests, PasteMultiLineText) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.insertText("!\nWorld");
  EXPECT_EQ(editor.getText(), "Hello!\nWorld") << "insertText should handle multi-line text";
}

TEST_F(TextEditorTests, SelectionSpanningMultipleLines) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setSelection(Coordinates(0, 2), Coordinates(2, 2));
  EXPECT_EQ(editor.getSelectedText(), "ne1\nLine2\nLi")
      << "setSelection should select across multiple lines";
}

TEST_F(TextEditorTests, InsertNewlineAtLineStart) {
  editor.setText("Hello\nWorld");
  editor.setCursorPosition(Coordinates(1, 0));
  editor.insertText("\n");
  EXPECT_EQ(editor.getText(), "Hello\n\nWorld") << "insertText newline should split line";
}

// ============================================================================
// CLIPBOARD TESTS
// ============================================================================

TEST_F(TextEditorTests, CopyWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello") << "copy with selection should copy selected text";
}

TEST_F(TextEditorTests, CopyWithoutSelectionCopiesLine) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello") << "copy without selection should copy entire line";
}

TEST_F(TextEditorTests, CutWithSelection) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.cut();
  EXPECT_EQ(editor.getText(), " world") << "cut should remove selected text and copy to clipboard";
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Hello");
}

TEST_F(TextEditorTests, CutCapturesDeleteIntentAndUndoRestoresText) {
  editor.setText("Hello world");
  editor.resetTextChanged();
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));

  editor.cut();

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  EXPECT_THAT(intents, ElementsAre(SourceEditIntentIs(0u, 5u, "", SourceEditIntentKind::Delete)));

  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello world");
}

TEST_F(TextEditorTests, CutWithoutSelectionDoesNothing) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.cut();
  EXPECT_EQ(editor.getText(), "Hello") << "cut without selection should not delete";
}

TEST_F(TextEditorTests, PasteInsertsClipboardText) {
  editor.setText("Hello");
  editor.resetTextChanged();
  ImGui::SetClipboardText(" world");
  editor.setCursorPosition(Coordinates(0, 5));
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hello world") << "paste should insert clipboard text at cursor";

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  EXPECT_THAT(intents,
              ElementsAre(SourceEditIntentIs(5u, 0u, " world", SourceEditIntentKind::Insert)));
}

TEST_F(TextEditorTests, PasteWithSelectionReplacesSelection) {
  editor.setText("Hello world");
  editor.resetTextChanged();
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  ImGui::SetClipboardText("Hi");
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hi world") << "paste with selection should replace selection";

  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  EXPECT_THAT(intents,
              ElementsAre(SourceEditIntentIs(0u, 5u, "Hi", SourceEditIntentKind::Replace)));

  editor.undo();
  EXPECT_EQ(editor.getText(), "Hello world");
}

TEST_F(TextEditorTests, PasteWithEmptyOrUnavailableClipboardIsNoOp) {
  editor.setText("Hello");
  editor.resetTextChanged();
  editor.setCursorPosition(Coordinates(0, 5));

  ImGui::SetClipboardText("");
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hello");
  EXPECT_TRUE(editor.takePendingSourceEditIntents().empty());

  ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
  platformIo.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* { return nullptr; };
  editor.paste();
  EXPECT_EQ(editor.getText(), "Hello");
  EXPECT_TRUE(editor.takePendingSourceEditIntents().empty());
}

TEST_F(TextEditorTests, ProcessReplaceCapturesReplaceIntentAndUndoRestoresText) {
  editor.setText("red blue red");
  editor.resetTextChanged();

  editor.processReplace("red", "green");

  EXPECT_EQ(editor.getText(), "green blue red");
  std::vector<SourceEditIntent> intents = editor.takePendingSourceEditIntents();
  EXPECT_THAT(intents,
              ElementsAre(SourceEditIntentIs(0u, 3u, "green", SourceEditIntentKind::Replace)));

  editor.undo();
  EXPECT_EQ(editor.getText(), "red blue red");
}

// ============================================================================
// RENDERED SOURCE VIEW TESTS
// ============================================================================

TEST_F(TextEditorTests, RenderBuildsWrappedVisualRowsForLongXmlLine) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  ASSERT_GT(VisualLineCount(), 1);
  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  EXPECT_TRUE(VisualLineIsContinuation(continuationIndex));
  EXPECT_EQ(VisualLineIndentColumns(continuationIndex), 8);
  EXPECT_FALSE(editor.isImGuiChildIgnored());
  EXPECT_TRUE(editor.wordWrapEnabled());
  EXPECT_FALSE(HorizontalScrollEnabled());
  EXPECT_FLOAT_EQ(LastScrollX(), 0.0f);
}

TEST_F(TextEditorTests, HorizontalScrollSetterDoesNotEnableTextViewHorizontalScroll) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");
  editor.setHorizontalScroll(true);

  RenderEditorFrame(ImVec2(180.0f, 180.0f));

  EXPECT_FALSE(HorizontalScrollEnabled());
  EXPECT_FLOAT_EQ(LastScrollX(), 0.0f);
}

TEST_F(TextEditorTests, InlineSettingsSetExpectedEditorState) {
  editor.setSmartIndent(false);
  editor.setAutoIndentOnPaste(true);
  editor.setHighlightLine(false);
  editor.setCompleteBraces(false);
  editor.setHorizontalScroll(true);
  editor.setSmartPredictions(false);
  editor.setFunctionDeclarationTooltip(false);
  editor.setFunctionTooltips(false);
  editor.setActiveAutocomplete(true);
  editor.setScrollbarMarkers(true);
  editor.setSidebarVisible(false);
  editor.setSearchEnabled(false);
  editor.setHighlightBrackets(false);
  editor.setFoldEnabled(true);
  editor.setWordWrapEnabled(false);
  editor.setUIScale(1.25f);
  editor.setUIFontSize(14.0f);
  editor.setEditorFontSize(15.0f);

  EXPECT_TRUE(AutoIndentOnPasteEnabled());
  EXPECT_FALSE(HighlightLineEnabled());
  EXPECT_FALSE(HorizontalScrollEnabled());
  EXPECT_FALSE(SmartPredictionsEnabled());
  EXPECT_FALSE(FunctionDeclarationTooltipEnabled());
  EXPECT_FALSE(FunctionTooltipsEnabled());
  EXPECT_TRUE(ScrollbarMarkersEnabled());
  EXPECT_FALSE(SidebarVisible());
  EXPECT_FALSE(SearchEnabled());
  EXPECT_FALSE(HighlightBracketsEnabled());
  EXPECT_TRUE(FoldEnabled());
  EXPECT_FALSE(editor.wordWrapEnabled());
  EXPECT_FLOAT_EQ(UiScale(), 1.25f);
  EXPECT_FLOAT_EQ(UiFontSize(), 14.0f);
  EXPECT_FLOAT_EQ(EditorFontSize(), 15.0f);
}

TEST_F(TextEditorTests, WrappedHitTestingMapsContinuationRowToLogicalColumn) {
  editor.setText(R"(  <rect id="target" x="10" y="20" width="30" height="40" fill="red"/>)");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  ASSERT_GT(VisualLineEndColumn(continuationIndex), VisualLineStartColumn(continuationIndex) + 2);

  const Coordinates hit = CoordinatesAtVisualTextOffset(continuationIndex, 2);

  EXPECT_EQ(hit, Coordinates(0, VisualLineStartColumn(continuationIndex) + 2));
}

TEST_F(TextEditorTests, FocusPartitionHidesLinesFromRenderedVisualLayout) {
  editor.setText("root\nhidden-a\nhidden-b\ntarget\nclose");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 3, .endLine = 4}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 4, .endLine = 5}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  });

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_THAT(VisualLineLogicalLines(), ElementsAre(0, 1, 3, 4));
  ASSERT_GE(VisualLineCount(), 3);
  EXPECT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));
  EXPECT_EQ(VisualLineHiddenRange(1), (LineRange{.startLine = 1, .endLine = 3}));
  EXPECT_EQ(CoordinatesAtVisualTextOffset(2, 2), Coordinates(3, 2));
}

TEST_F(TextEditorTests, FocusPartitionDirectPredicatesCoverOverlapAndExpansion) {
  const LineRange hiddenRange{.startLine = 4, .endLine = 5};

  EXPECT_FALSE(IsLineHiddenByFocusDirect(4));
  EXPECT_FALSE(IsLineReferenceColoredByFocusDirect(1));
  EXPECT_FALSE(IsLineDimmedByFocusDirect(3));
  EXPECT_FALSE(IsLineExpandedHiddenByFocusDirect(4));
  EXPECT_FALSE(FocusHiddenRangeForLineDirect(4).has_value());
  EXPECT_FALSE(IsFocusHiddenRangeExpandedDirect(hiddenRange));

  const FocusPartition partition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 1},
                    LineRange{.startLine = 2, .endLine = 3}},
      .referenceColor = {LineRange{.startLine = 1, .endLine = 3}},
      .dimmed = {LineRange{.startLine = 3, .endLine = 5}},
      .hidden = {hiddenRange},
  };
  editor.setFocusPartition(partition);

  EXPECT_FALSE(IsLineHiddenByFocusDirect(3));
  EXPECT_TRUE(IsLineHiddenByFocusDirect(4));
  EXPECT_TRUE(IsLineReferenceColoredByFocusDirect(1));
  EXPECT_FALSE(IsLineReferenceColoredByFocusDirect(2));
  EXPECT_TRUE(IsLineDimmedByFocusDirect(3));
  EXPECT_FALSE(IsLineDimmedByFocusDirect(2));
  EXPECT_EQ(FocusHiddenRangeForLineDirect(4), hiddenRange);
  EXPECT_FALSE(FocusHiddenRangeForLineDirect(5).has_value());

  ExpandFocusHiddenRangeDirect(LineRange{.startLine = 5, .endLine = 5});
  EXPECT_THAT(ExpandedFocusHiddenRanges(), IsEmpty());

  ExpandFocusHiddenRangeDirect(hiddenRange);
  EXPECT_THAT(ExpandedFocusHiddenRanges(), ElementsAre(LineRangeIs(4, 5)));
  EXPECT_TRUE(IsFocusHiddenRangeExpandedDirect(hiddenRange));
  EXPECT_TRUE(IsLineExpandedHiddenByFocusDirect(4));
  EXPECT_FALSE(IsLineHiddenByFocusDirect(4));
  EXPECT_TRUE(IsLineDimmedByFocusDirect(4));

  ExpandFocusHiddenRangeDirect(hiddenRange);
  EXPECT_THAT(ExpandedFocusHiddenRanges(), ElementsAre(LineRangeIs(4, 5)));

  editor.setFocusPartition(partition);
  EXPECT_THAT(ExpandedFocusHiddenRanges(), ElementsAre(LineRangeIs(4, 5)));

  FocusPartition changed = partition;
  changed.hidden = {LineRange{.startLine = 4, .endLine = 6}};
  editor.setFocusPartition(changed);
  EXPECT_THAT(ExpandedFocusHiddenRanges(), IsEmpty());
}

TEST_F(TextEditorTests, ClickingFocusHiddenPlaceholderExpandsRangeWithoutMovingCursor) {
  editor.setText("root\nhidden-a\nhidden-b\ntarget\nclose");
  const FocusPartition partition{
      .fullColor = {LineRange{.startLine = 3, .endLine = 4}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 4, .endLine = 5}},
      .hidden = {LineRange{.startLine = 1, .endLine = 3}},
  };
  editor.setFocusPartition(partition);
  editor.setCursorPosition(Coordinates(3, 0));

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  ASSERT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/1);
  RenderEditorFrameWithMouse(clickPos, false, ImVec2(300.0f, 180.0f));
  RenderEditorFrameWithMouse(clickPos, true, ImVec2(300.0f, 180.0f));

  EXPECT_THAT(VisualLineLogicalLines(), ElementsAre(0, 1, 2, 3, 4));
  EXPECT_FALSE(VisualLineIsFocusHiddenPlaceholder(1));
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(3, 0));
  EXPECT_FALSE(editor.isCursorPositionChanged());

  editor.setFocusPartition(partition);
  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_THAT(VisualLineLogicalLines(), ElementsAre(0, 1, 2, 3, 4));
}

TEST_F(TextEditorTests, FocusHiddenPlaceholderExpansionRejectsInvalidRows) {
  editor.setText("root\nhidden\ntarget");
  EXPECT_FALSE(TryExpandFocusHiddenPlaceholderAtDirect(ImVec2(0.0f, 0.0f)));

  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 3}},
      .hidden = {LineRange{.startLine = 1, .endLine = 2}},
  });
  RenderEditorFrame(ImVec2(300.0f, 160.0f));
  ASSERT_TRUE(VisualLineIsFocusHiddenPlaceholder(1));

  EXPECT_FALSE(TryExpandFocusHiddenPlaceholderAtDirect(
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/1)));

  ImVec2 above = ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/1);
  above.y -= CharacterAdvanceY() * 2.0f;
  EXPECT_FALSE(TryExpandFocusHiddenPlaceholderAtDirect(above));

  ImVec2 below = ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/1);
  below.y += CharacterAdvanceY() * 20.0f;
  EXPECT_FALSE(TryExpandFocusHiddenPlaceholderAtDirect(below));

  EXPECT_TRUE(TryExpandFocusHiddenPlaceholderAtDirect(
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/1)));
  EXPECT_EQ(ExpandedFocusHiddenRanges(),
            (std::vector<LineRange>{LineRange{.startLine = 1, .endLine = 2}}));
}

TEST_F(TextEditorTests, CursorInsideFocusRangeTracksVisibleFocusBrightnessLines) {
  editor.setText("root\nreference\ntarget\nclosing\nhidden");
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 3}},
      .referenceColor = {LineRange{.startLine = 1, .endLine = 2}},
      .dimmed = {LineRange{.startLine = 0, .endLine = 1}, LineRange{.startLine = 3, .endLine = 4}},
      .hidden = {LineRange{.startLine = 4, .endLine = 5}},
  });

  editor.setCursorPosition(Coordinates(2, 1));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(0, 0));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(1, 0));
  EXPECT_TRUE(editor.isCursorInsideFocusRange());

  editor.setCursorPosition(Coordinates(4, 0));
  EXPECT_FALSE(editor.isCursorInsideFocusRange());

  editor.clearFocusPartition();
  editor.setCursorPosition(Coordinates(2, 1));
  EXPECT_FALSE(editor.isCursorInsideFocusRange());
}

TEST_F(TextEditorTests, ExternalSourceEditQueuesRenderedFlashDecoration) {
  editor.setText("<svg></svg>");
  editor.resetTextChanged();

  editor.applyExternalSourceEdit(5, 0, "<g/>");
  RenderEditorFrame(ImVec2(300.0f, 120.0f));

  ASSERT_TRUE(HasActiveSourceFlash());
  const std::vector<ActiveFlash> flashes = ActiveSourceFlashes();
  EXPECT_THAT(flashes, ElementsAre(ActiveFlashIs(5, 9, Gt(0.0f))));
}

TEST_F(TextEditorTests, ExternalSourceEditInsideSelectionExtendsSelection) {
  constexpr std::string_view kSource =
      R"(<svg><rect id="r1" x="0" y="0" width="10" height="10"/></svg>)";
  constexpr std::string_view kInserted = " transform=\"translate(5)\"";
  editor.setText(kSource);
  editor.resetTextChanged();

  const std::size_t selectionStart = kSource.find("<rect");
  ASSERT_NE(selectionStart, std::string_view::npos);
  const std::size_t selectionEnd = kSource.find("/>", selectionStart);
  ASSERT_NE(selectionEnd, std::string_view::npos);
  const std::size_t selectedSourceEnd = selectionEnd + 2;
  editor.setSelection(editor.getCoordinatesAtByteOffset(selectionStart),
                      editor.getCoordinatesAtByteOffset(selectedSourceEnd));
  editor.setCursorPosition(editor.getCoordinatesAtByteOffset(selectionStart));

  editor.applyExternalSourceEdit(selectionEnd, 0, kInserted);

  EXPECT_EQ(
      editor.getSelectedText(),
      R"expected(<rect id="r1" x="0" y="0" width="10" height="10" transform="translate(5)"/>)expected");
  EXPECT_EQ(editor.getCursorPosition(), editor.getCoordinatesAtByteOffset(selectionStart));
}

TEST_F(TextEditorTests, ExternalSourceEditInsideReverseSelectionExtendsSelection) {
  constexpr std::string_view kSource =
      R"(<svg><rect id="r1" x="0" y="0" width="10" height="10"/></svg>)";
  constexpr std::string_view kInserted = " transform=\"translate(5)\"";
  editor.setText(kSource);
  editor.resetTextChanged();

  const std::size_t selectionStart = kSource.find("<rect");
  ASSERT_NE(selectionStart, std::string_view::npos);
  const std::size_t selectionEnd = kSource.find("/>", selectionStart);
  ASSERT_NE(selectionEnd, std::string_view::npos);
  const std::size_t selectedSourceEnd = selectionEnd + 2;
  editor.setSelection(editor.getCoordinatesAtByteOffset(selectedSourceEnd),
                      editor.getCoordinatesAtByteOffset(selectionStart));

  editor.applyExternalSourceEdit(selectionEnd, 0, kInserted);

  EXPECT_EQ(
      editor.getSelectedText(),
      R"expected(<rect id="r1" x="0" y="0" width="10" height="10" transform="translate(5)"/>)expected");
}

TEST_F(TextEditorTests, SourceFocusModeContextMenuStateAndToggleRequestAreConsumable) {
  editor.setSourceFocusModeContextMenu(true);

  EXPECT_TRUE(SourceFocusModeContextMenuVisible());
  EXPECT_TRUE(SourceFocusModeContextMenuChecked());
  EXPECT_FALSE(editor.takeSourceFocusModeContextMenuToggleRequest());

  RequestSourceFocusModeContextMenuToggle();
  EXPECT_TRUE(editor.takeSourceFocusModeContextMenuToggleRequest());
  EXPECT_FALSE(editor.takeSourceFocusModeContextMenuToggleRequest());

  editor.clearSourceFocusModeContextMenu();
  EXPECT_FALSE(SourceFocusModeContextMenuVisible());
}

TEST_F(TextEditorTests, FocusReferenceConnectorsUseCatenaryRopesBetweenEndpoints) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n"
      "  <circle stroke=\"url(#grad)\"/>\n");
  const FocusReferenceLink rectLink{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  const FocusReferenceLink circleLink{
      .from = SourcePoint{.line = 4, .column = 23},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 5}},
      .referenceLinks = {rectLink, circleLink},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));

  auto rectLayout = FocusReferenceLayout(rectLink, 0);
  auto circleLayout = FocusReferenceLayout(circleLink, 1);
  ASSERT_TRUE(rectLayout.has_value());
  ASSERT_TRUE(circleLayout.has_value());

  const float baselineY = TextBaselineOffsetY();
  const ImVec2 rectSourceTop = ScreenPointAtCoordinates(Coordinates(3, 19));
  const ImVec2 gradientTargetTop = ScreenPointAtCoordinates(Coordinates(1, 4));
  const ImVec2 rectReferenceStart = ScreenPointAtCoordinates(Coordinates(3, 18));
  const ImVec2 rectReferenceEnd = ScreenPointAtCoordinates(Coordinates(3, 23));
  EXPECT_TRUE(rectLayout->hasSourceUnderline);
  EXPECT_FLOAT_EQ(rectLayout->sourceUnderline.start.x, rectReferenceStart.x);
  EXPECT_FLOAT_EQ(rectLayout->sourceUnderline.end.x, rectReferenceEnd.x);
  EXPECT_FLOAT_EQ(rectLayout->start.x,
                  (rectLayout->sourceUnderline.start.x + rectLayout->sourceUnderline.end.x) * 0.5f);
  EXPECT_FLOAT_EQ(rectLayout->start.y, rectLayout->sourceUnderline.start.y);
  EXPECT_GT(rectLayout->start.y, rectSourceTop.y + baselineY);
  EXPECT_FLOAT_EQ(rectLayout->tip.y, gradientTargetTop.y + baselineY);

  EXPECT_NE(rectLayout->color, circleLayout->color);
  const float alpha = ImGui::ColorConvertU32ToFloat4(rectLayout->color).w;
  EXPECT_GE(alpha, 0.45f);
  EXPECT_LE(alpha, 0.55f);

  EXPECT_TRUE(FocusReferenceRopePathIsBezier(rectLink));
  const FrameCostBreakdown::SourceRopes& ropeCost = editor.lastSourceRopeCost();
  EXPECT_EQ(ropeCost.candidateCount, 2);
  EXPECT_EQ(ropeCost.laidOutCount, 2);
  EXPECT_EQ(ropeCost.drawnCount, 2);
  EXPECT_EQ(ropeCost.activeStateCount, 2);
  EXPECT_GE(ropeCost.layoutMs, 0.0);
  EXPECT_GE(ropeCost.updateMs, 0.0);
  EXPECT_GE(ropeCost.drawMs, 0.0);
  const std::optional<Box2d> rectBounds = FocusReferenceRopeBounds(rectLink);
  ASSERT_TRUE(rectBounds.has_value());
  EXPECT_LE(rectBounds->bottomRight.x, std::max(rectLayout->start.x, rectLayout->tip.x) + 3.0f)
      << "Catenary rope should hang between endpoints instead of routing through a right lane";
  EXPECT_GT(rectBounds->bottomRight.y, std::max(rectLayout->start.y, rectLayout->tip.y));
  ASSERT_TRUE(editor.nextRopeAnimationWakeSeconds().has_value());
  EXPECT_FLOAT_EQ(*editor.nextRopeAnimationWakeSeconds(), 1.0f / 60.0f);
}

TEST_F(TextEditorTests, FocusReferenceRopeOptionsUseDenseFastRopeTuning) {
  const RopeSimulationOptions options = FocusReferenceRopeOptions();

  EXPECT_GE(options.segmentCount, 28);
  EXPECT_GE(options.constraintIterations, 10);
  EXPECT_GE(options.gravityPxPerSec2, 180.0);
  EXPECT_LE(options.scrollResponse, 0.01);
  EXPECT_LE(options.maxScrollImpulsePx, 0.8);
  EXPECT_GE(options.catenarySlackRatio, 0.18);
  EXPECT_GE(options.catenaryMinSlackPx, 30.0);
  EXPECT_GE(options.catenaryMaxSlackPx, 120.0);
  EXPECT_LE(options.initialImpulsePx, 0.25);
  EXPECT_LE(options.settleRestDistanceThresholdPx, 0.45);
  EXPECT_LT(options.overdueDamping, options.damping);
  EXPECT_LE(options.overdueDampingRampSeconds, 1.5);
  EXPECT_GE(options.catenaryRestoringForcePerSec2, 600.0);
  EXPECT_EQ(options.endpointImpulse, 0.0);
  EXPECT_EQ(options.maxEndpointImpulsePx, 0.0);
  EXPECT_LE(options.endpointMotionVelocityRetention, 0.25);
  EXPECT_GE(options.endpointCatenaryBlend, 0.15);
}

TEST_F(TextEditorTests, FocusReferenceConnectorLayoutRejectsHiddenEndpoints) {
  editor.setText("from\nto\nvisible");

  const FocusReferenceLink hiddenSource{
      .from = SourcePoint{.line = 0, .column = 0},
      .to = SourcePoint{.line = 2, .column = 0},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 2, .endLine = 3}},
      .hidden = {LineRange{.startLine = 0, .endLine = 1}},
      .referenceLinks = {hiddenSource},
  });
  RenderEditorFrame(ImVec2(360.0f, 140.0f));
  EXPECT_FALSE(FocusReferenceLayout(hiddenSource, 0).has_value());

  const FocusReferenceLink hiddenTarget{
      .from = SourcePoint{.line = 0, .column = 0},
      .to = SourcePoint{.line = 1, .column = 0},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 1}},
      .hidden = {LineRange{.startLine = 1, .endLine = 2}},
      .referenceLinks = {hiddenTarget},
  });
  RenderEditorFrame(ImVec2(360.0f, 140.0f));
  EXPECT_FALSE(FocusReferenceLayout(hiddenTarget, 0).has_value());
}

TEST_F(TextEditorTests, FocusReferenceSourceUnderlineHandlesReferenceCharactersAndPunctuation) {
  const std::string source = "\nfill:url(#foo-bar.baz:50%) !";
  editor.setText(source);
  RenderEditorFrame(ImVec2(420.0f, 140.0f));

  EXPECT_FALSE(FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 0, .column = 0}));

  const int percentColumn = static_cast<int>(source.find('%') - source.find('\n') - 1);
  const int closeParenColumn = static_cast<int>(source.find(')') - source.find('\n') - 1);
  ASSERT_GE(percentColumn, 0);
  ASSERT_GE(closeParenColumn, 0);

  const auto percentUnderline =
      FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 1, .column = percentColumn});
  const auto closeParenUnderline =
      FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 1, .column = closeParenColumn});
  ASSERT_TRUE(percentUnderline.has_value());
  ASSERT_TRUE(closeParenUnderline.has_value());
  EXPECT_FLOAT_EQ(closeParenUnderline->start.x, percentUnderline->start.x);
  EXPECT_FLOAT_EQ(closeParenUnderline->end.x, percentUnderline->end.x);

  const auto bangUnderline =
      FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 1, .column = closeParenColumn + 2});
  ASSERT_TRUE(bangUnderline.has_value());
  EXPECT_LT(bangUnderline->start.x, bangUnderline->end.x);
}

TEST_F(TextEditorTests, FocusReferenceRopesCullOffscreenLinksBeforeSimulation) {
  std::ostringstream source;
  source << "  <defs>\n"
         << "    <linearGradient id=\"grad\"/>\n"
         << "  </defs>\n"
         << "  <rect fill=\"url(#grad)\"/>\n";
  for (int i = 0; i < 80; ++i) {
    source << "  <!-- filler " << i << " -->\n";
  }
  source << "  <circle stroke=\"url(#grad)\"/>\n";
  editor.setText(source.str());
  editor.resetTextChanged();

  const FocusReferenceLink visibleLink{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 6},
  };
  const FocusReferenceLink offscreenLink{
      .from = SourcePoint{.line = 84, .column = 23},
      .to = SourcePoint{.line = 1, .column = 6},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 85}},
      .referenceLinks = {visibleLink, offscreenLink},
  });

  RenderEditorFrame(ImVec2(520.0f, 120.0f));

  const FrameCostBreakdown::SourceRopes& ropeCost = editor.lastSourceRopeCost();
  EXPECT_EQ(ropeCost.candidateCount, 2);
  EXPECT_EQ(ropeCost.laidOutCount, 1);
  EXPECT_EQ(ropeCost.culledCount, 1);
  EXPECT_EQ(ropeCost.drawnCount, 1);
  EXPECT_EQ(ropeCost.activeStateCount, 1);
  EXPECT_NE(FocusReferenceRope(visibleLink), nullptr);
  EXPECT_EQ(FocusReferenceRope(offscreenLink), nullptr);
}

TEST_F(TextEditorTests, FocusReferenceRopesClipToScrolledSourceViewport) {
  std::ostringstream source;
  source << "  <defs>\n"
         << "    <linearGradient id=\"grad\"/>\n"
         << "  </defs>\n"
         << "  <rect fill=\"url(#grad)\"/>\n";
  for (int i = 0; i < 80; ++i) {
    source << "  <!-- filler " << i << " -->\n";
  }
  source << "  <circle stroke=\"url(#grad)\"/>\n";
  editor.setText(source.str());
  editor.resetTextChanged();

  const FocusReferenceLink scrolledLink{
      .from = SourcePoint{.line = 84, .column = 23},
      .to = SourcePoint{.line = 1, .column = 6},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 85}},
      .referenceLinks = {scrolledLink},
  });

  constexpr ImVec2 kEditorSize(520.0f, 120.0f);
  RenderEditorFrame(kEditorSize);
  editor.selectAndFocus(Coordinates(84, 2), Coordinates(84, 10));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  EXPECT_GT(LastScrollY(), 0.0f);
  const FrameCostBreakdown::SourceRopes& ropeCost = editor.lastSourceRopeCost();
  EXPECT_EQ(ropeCost.candidateCount, 1);
  EXPECT_EQ(ropeCost.laidOutCount, 1);
  EXPECT_EQ(ropeCost.culledCount, 0);
  EXPECT_EQ(ropeCost.drawnCount, 1);
  EXPECT_EQ(ropeCost.activeStateCount, 1);
  EXPECT_NE(FocusReferenceRope(scrolledLink), nullptr);
}

TEST_F(TextEditorTests, FocusReferenceRopesUseStaticConnectorsAfterAnimatedCap) {
  constexpr int kLinkCount = 66;
  std::ostringstream source;
  source << "  <defs>\n"
         << "    <linearGradient id=\"grad\"/>\n"
         << "  </defs>\n";

  std::vector<FocusReferenceLink> links;
  links.reserve(kLinkCount);
  for (int i = 0; i < kLinkCount; ++i) {
    source << "  <rect fill=\"url(#grad)\"/>\n";
    links.push_back(FocusReferenceLink{
        .from = SourcePoint{.line = 3 + i, .column = 19},
        .to = SourcePoint{.line = 1, .column = 6},
    });
  }
  editor.setText(source.str());
  editor.resetTextChanged();
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = kLinkCount + 3}},
      .referenceLinks = links,
  });

  RenderEditorFrame(ImVec2(760.0f, 1600.0f));

  const FrameCostBreakdown::SourceRopes& ropeCost = editor.lastSourceRopeCost();
  EXPECT_EQ(ropeCost.candidateCount, kLinkCount);
  EXPECT_EQ(ropeCost.culledCount, 0);
  EXPECT_EQ(ropeCost.drawnCount, kLinkCount);
  EXPECT_EQ(ropeCost.staticDrawnCount, 2);
  EXPECT_EQ(ropeCost.activeStateCount, 64);
}

TEST_F(TextEditorTests, FocusReferenceConnectorTerminatesOnClosestSourceStyleChipEdge) {
  editor.setText(
      "<linearGradient id=\"paint\"> padding padding padding\n"
      "<rect fill=\"url(#paint)\"/>\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 94,
          .range = SourceByteRange{.start = 0, .end = 27},
          .chipRange = SourceByteRange{.start = 0, .end = 27},
          .showChip = true,
          .chipCount = 1,
          .chipKind = TextEditor::SourceStyleChipKind::ReferenceCount,
          .chipTooltip = "Referenced 1 time",
      },
  }));

  const FocusReferenceLink link{
      .from = SourcePoint{.line = 1, .column = 17},
      .to = SourcePoint{.line = 0, .column = 27},
  };
  const FocusReferenceLink rightLink{
      .from = SourcePoint{.line = 0, .column = 45},
      .to = SourcePoint{.line = 0, .column = 27},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 2}},
      .referenceLinks = {link, rightLink},
  });

  RenderEditorFrame(ImVec2(520.0f, 140.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);

  const auto layout = FocusReferenceLayout(link, 0);
  ASSERT_TRUE(layout.has_value());
  EXPECT_FALSE(layout->hasSourceStyleChip);
  EXPECT_TRUE(layout->hasSourceUnderline);

  const ImVec2 chipMin = SourceStyleChipHitRectMin(0);
  const ImVec2 chipMax = SourceStyleChipHitRectMax(0);
  EXPECT_FLOAT_EQ(layout->tip.x, (chipMin.x + chipMax.x) * 0.5f);
  EXPECT_FLOAT_EQ(layout->tip.y, chipMax.y);
  ExpectFocusReferenceArrowMatchesTangent(link);

  const auto rightLayout = FocusReferenceLayout(rightLink, 1);
  ASSERT_TRUE(rightLayout.has_value());
  EXPECT_FLOAT_EQ(rightLayout->tip.x, chipMax.x);
  EXPECT_FLOAT_EQ(rightLayout->tip.y, (chipMin.y + chipMax.y) * 0.5f);
  ExpectFocusReferenceArrowMatchesTangent(rightLink);
}

TEST_F(TextEditorTests, FocusReferenceConnectorTargetsSelectorChipByRangeStart) {
  editor.setText(
      ".hit { fill: red; }\n"
      "\n"
      "\n"
      "<rect class=\"hit\"/>\n");
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 95,
          .range = SourceByteRange{.start = 7, .end = 16},
          .chipRange = SourceByteRange{.start = 0, .end = 4},
          .showChip = true,
          .chipCount = 1,
          .chipTooltip = "Selector matches 1 element",
      },
  }));

  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 13},
      .to = SourcePoint{.line = 0, .column = 0},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 4}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(360.0f, 120.0f));
  ASSERT_EQ(SourceStyleChipHitRectCount(), 1u);

  const auto layout = FocusReferenceLayout(link, 0);
  ASSERT_TRUE(layout.has_value());

  const ImVec2 chipMin = SourceStyleChipHitRectMin(0);
  const ImVec2 chipMax = SourceStyleChipHitRectMax(0);
  EXPECT_FLOAT_EQ(layout->tip.x, (chipMin.x + chipMax.x) * 0.5f);
  EXPECT_FLOAT_EQ(layout->tip.y, chipMax.y);
  EXPECT_TRUE(layout->hasSourceStyleChip);
  EXPECT_FALSE(layout->hasSourceUnderline);
  EXPECT_GT(layout->sourceStyleChip.min.x,
            ScreenPointAtCoordinates(Coordinates(3, LineMaxColumn(3))).x);
  EXPECT_GT(layout->sourceStyleChip.min.y, layout->tip.y);
  EXPECT_FLOAT_EQ(layout->start.x, layout->sourceStyleChip.min.x);
  EXPECT_FLOAT_EQ(layout->start.y,
                  (layout->sourceStyleChip.min.y + layout->sourceStyleChip.max.y) * 0.5f);
  ExpectFocusReferenceArrowMatchesTangent(link);
}

TEST_F(TextEditorTests, FocusReferenceSourceUnderlineHandlesDelimitersAndInvalidPositions) {
  editor.setText("fill=\"url(#grad)\";\n\n");
  RenderEditorFrame(ImVec2(420.0f, 120.0f));

  EXPECT_EQ(FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 9, .column = 0}), std::nullopt);
  EXPECT_EQ(FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 1, .column = 0}), std::nullopt);

  const auto underline = FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 0, .column = 15});

  ASSERT_TRUE(underline.has_value());
  EXPECT_FLOAT_EQ(underline->start.x, ScreenPointAtCoordinates(Coordinates(0, 10)).x);
  EXPECT_FLOAT_EQ(underline->end.x, ScreenPointAtCoordinates(Coordinates(0, 15)).x);
  EXPECT_GT(underline->end.x, underline->start.x);
}

TEST_F(TextEditorTests, FocusReferenceSourceUnderlineSanitizesExtremeColumns) {
  editor.setText("fill=\"url(#grad)\";\n");
  RenderEditorFrame(ImVec2(420.0f, 120.0f));

  const auto negativeUnderline =
      FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 0, .column = -3});
  ASSERT_TRUE(negativeUnderline.has_value());
  EXPECT_FLOAT_EQ(negativeUnderline->start.x, ScreenPointAtCoordinates(Coordinates(0, 0)).x);
  EXPECT_FLOAT_EQ(negativeUnderline->end.x, ScreenPointAtCoordinates(Coordinates(0, 4)).x);

  const auto underline = FocusReferenceSourceUnderlineDirect(SourcePoint{.line = 0, .column = 999});
  ASSERT_TRUE(underline.has_value());
  EXPECT_LT(underline->start.x, underline->end.x);
  EXPECT_LE(underline->end.x, ScreenPointAtCoordinates(Coordinates(0, LineMaxColumn(0))).x);
}

TEST_F(TextEditorTests, FocusReferenceConnectorLayoutFallsBackToRawCoordinateEndpoints) {
  editor.setText("\n\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 0, .column = 0},
      .to = SourcePoint{.line = 1, .column = 0},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 2}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(300.0f, 120.0f));

  const auto layout = FocusReferenceLayout(link, 0);
  ASSERT_TRUE(layout.has_value());
  EXPECT_FALSE(layout->hasSourceUnderline);
  EXPECT_FALSE(layout->hasSourceStyleChip);
  EXPECT_FLOAT_EQ(layout->start.x,
                  ScreenPointAtCoordinates(Coordinates(0, 0)).x + CharacterAdvanceX() * 0.5f);
  EXPECT_FLOAT_EQ(layout->tip.x, ScreenPointAtCoordinates(Coordinates(1, 0)).x - 2.0f);
  EXPECT_GT(layout->start.y, ScreenPointAtCoordinates(Coordinates(0, 0)).y);
  EXPECT_GT(layout->tip.y, ScreenPointAtCoordinates(Coordinates(1, 0)).y);
}

TEST_F(TextEditorTests, TypingRemapsFocusReferenceRopeEndpointsImmediately) {
  const std::string source =
      "<linearGradient id=\"grad\"/>\n"
      "<rect fill=\"url(#grad)\"/>\n";
  editor.setText(source);
  ASSERT_TRUE(editor.setSourceStyleDecorations({
      TextEditor::SourceStyleDecoration{
          .id = 96,
          .range = SourceByteRange{.start = 0, .end = 27},
          .chipRange = SourceByteRange{.start = 0, .end = 27},
          .showChip = true,
          .chipCount = 1,
          .chipTooltip = "Referenced 1 time",
      },
  }));

  const FocusReferenceLink link{
      .from = SourcePoint{.line = 1, .column = 17},
      .to = SourcePoint{.line = 0, .column = 27},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 0, .endLine = 2}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(420.0f, 140.0f));
  const auto beforeLayout = FocusReferenceLayout(link, 0);
  ASSERT_TRUE(beforeLayout.has_value());
  const std::vector<Vector2d> beforePoints = FocusReferenceRopePoints(link);
  ASSERT_FALSE(beforePoints.empty());

  constexpr std::string_view kInsertedLine = "<!-- live -->\n";
  editor.setCursorPosition(Coordinates(0, 0));
  editor.insertText(kInsertedLine);

  const FocusReferenceLink remappedLink{
      .from = SourcePoint{.line = 2, .column = 17},
      .to = SourcePoint{.line = 1, .column = 27},
  };
  EXPECT_THAT(FocusReferenceLinks(), ElementsAre(remappedLink));
  EXPECT_THAT(
      editor.sourceStyleDecorations(),
      ElementsAre(SourceStyleDecorationIs(
          96, SourceByteRange{.start = kInsertedLine.size(), .end = kInsertedLine.size() + 27u},
          /*ineffective=*/false, /*showChip=*/true, /*chipCount=*/1, "",
          SourceByteRange{.start = kInsertedLine.size(), .end = kInsertedLine.size() + 27u})));
  EXPECT_EQ(FocusReferenceRope(link), nullptr);
  EXPECT_NE(FocusReferenceRope(remappedLink), nullptr);

  RenderEditorFrame(ImVec2(420.0f, 160.0f));
  const auto afterLayout = FocusReferenceLayout(remappedLink, 0);
  ASSERT_TRUE(afterLayout.has_value());
  EXPECT_GT(afterLayout->start.y, beforeLayout->start.y);
  EXPECT_GT(afterLayout->tip.y, beforeLayout->tip.y);

  const std::vector<Vector2d> afterPoints = FocusReferenceRopePoints(remappedLink);
  ASSERT_EQ(afterPoints.size(), beforePoints.size());
  EXPECT_GT(afterPoints[afterPoints.size() / 2u].y, beforePoints[beforePoints.size() / 2u].y);
}

TEST_F(TextEditorTests, ReferenceOnlyFocusPartitionLeavesAllLinesVisible) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));

  EXPECT_THAT(VisualLineLogicalLines(), ElementsAre(0, 1, 2, 3));
  EXPECT_TRUE(FocusReferenceLayout(link, 0).has_value());
}

TEST_F(TextEditorTests, FocusReferenceRopeHitTestDetectsMouseNearConnector) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 4}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));

  const std::optional<ImVec2> midpoint = FocusReferenceRopeMidpoint(link);
  ASSERT_TRUE(midpoint.has_value());
  EXPECT_TRUE(FocusReferenceRopeHit(link, *midpoint));
}

TEST_F(TextEditorTests, CanvasScrollFrameDoesNotApplyFocusReferenceRopeScrollImpulse) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 4}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));
  ResetFocusReferenceRopeToSettledCatenary(link, 0, /*previousScrollY=*/-40.0f);
  const std::vector<Vector2d> before = FocusReferenceRopePoints(link);

  RenderEditorFrameWithMouse(ImVec2(760.0f, 560.0f), /*mouseDown=*/false, ImVec2(520.0f, 180.0f));

  EXPECT_EQ(FocusReferenceRopePoints(link), before);
}

TEST_F(TextEditorTests, HoveredFocusReferenceRopeGetsInteractiveStateAndFreezes) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 4}},
      .referenceLinks = {link},
  });

  RenderEditorFrame(ImVec2(520.0f, 180.0f));
  const std::optional<ImVec2> midpoint = FocusReferenceRopeMidpoint(link);
  ASSERT_TRUE(midpoint.has_value());
  const std::vector<Vector2d> beforeHover = FocusReferenceRopePoints(link);

  RenderEditorFrameWithMouse(*midpoint, /*mouseDown=*/false, ImVec2(520.0f, 180.0f));

  EXPECT_TRUE(FocusReferenceRopeHovered(link));
  EXPECT_EQ(FocusReferenceRopePoints(link), beforeHover);
  EXPECT_FALSE(editor.nextRopeAnimationWakeSeconds().has_value());
}

TEST_F(TextEditorTests, ClickingFocusReferenceRopeMovesCursorToDefinition) {
  editor.setText(
      "  <defs>\n"
      "    <linearGradient id=\"grad\"/>\n"
      "  </defs>\n"
      "  <rect fill=\"url(#grad)\"/>\n");
  const FocusReferenceLink link{
      .from = SourcePoint{.line = 3, .column = 19},
      .to = SourcePoint{.line = 1, .column = 4},
  };
  editor.setFocusPartition(FocusPartition{
      .fullColor = {LineRange{.startLine = 1, .endLine = 4}},
      .referenceLinks = {link},
  });
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame(ImVec2(520.0f, 180.0f));
  const std::optional<ImVec2> midpoint = FocusReferenceRopeMidpoint(link);
  ASSERT_TRUE(midpoint.has_value());

  RenderEditorFrameWithMouse(*midpoint, /*mouseDown=*/false, ImVec2(520.0f, 180.0f));
  RenderEditorFrameWithMouse(*midpoint, /*mouseDown=*/true, ImVec2(520.0f, 180.0f));

  EXPECT_EQ(editor.getCursorPosition(), Coordinates(link.to.line, link.to.column));
  EXPECT_FALSE(editor.hasSelection());
  EXPECT_TRUE(editor.isCursorPositionChanged());
  EXPECT_TRUE(editor.didMouseChangeCursorPosition());
}

TEST_F(TextEditorTests, SelectAndFocusScrollsToWrappedVisualCursorLine) {
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n");

  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  const Coordinates targetStart(0, 96);
  const int targetVisualLine = VisualLineIndexForCoordinates(targetStart);
  ASSERT_GT(targetVisualLine, 0);

  editor.selectAndFocus(targetStart, Coordinates(0, 108));
  RenderEditorFrame(ImVec2(240.0f, 80.0f));
  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  EXPECT_GT(LastScrollY(), 0.0f);
}

TEST_F(TextEditorTests, SelectAndFocusScrollsEntireSelectionIntoView) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 180.0f);
  RenderEditorFrame(kEditorSize);

  editor.selectAndFocus(Coordinates(40, 0), Coordinates(45, 6));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const int firstVisible = static_cast<int>(std::floor(LastScrollY() / CharacterAdvanceY()));
  const int lastVisible =
      firstVisible +
      std::max(1, static_cast<int>(std::floor(LastScrollViewportHeight() / CharacterAdvanceY()))) -
      1;
  EXPECT_LE(firstVisible, 40) << "scrollY=" << LastScrollY() << " charY=" << CharacterAdvanceY()
                              << " viewportY=" << LastScrollViewportHeight();
  EXPECT_GE(lastVisible, 45) << "firstVisible=" << firstVisible << " scrollY=" << LastScrollY()
                             << " charY=" << CharacterAdvanceY()
                             << " viewportY=" << LastScrollViewportHeight();
}

TEST_F(TextEditorTests, HandleScrollingDirectScrollsWrappedSelectionRange) {
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n");
  RenderEditorFrame(ImVec2(220.0f, 80.0f));

  editor.selectAndFocus(Coordinates(0, 88), Coordinates(0, 116));

  const float scrollTargetY = HandleScrollingDirect(/*initialScrollY=*/0.0f);

  EXPECT_GT(scrollTargetY, 0.0f);
  EXPECT_FALSE(ScrollToCursorRequested());
  EXPECT_FALSE(ScrollSelectionIntoViewRequested());
}

TEST_F(TextEditorTests, HandleScrollingDirectScrollsWrappedCursorAboveAndBelowViewport) {
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n");
  RenderEditorFrame(ImVec2(220.0f, 80.0f));

  const Coordinates wrappedTail(0, 112);
  ASSERT_GT(VisualLineIndexForCoordinates(wrappedTail), 0);
  editor.setCursorPosition(wrappedTail);
  RequestCursorScroll();
  const float scrollTargetBelow = HandleScrollingDirect(/*initialScrollY=*/0.0f);

  EXPECT_GT(scrollTargetBelow, 0.0f);
  EXPECT_FALSE(ScrollToCursorRequested());

  editor.setCursorPosition(Coordinates(0, 0));
  RequestCursorScroll();
  const float scrolledDownY = 3.0f * CharacterAdvanceY();
  const float scrollTargetAbove = HandleScrollingDirect(scrolledDownY);

  EXPECT_LT(scrollTargetAbove, scrolledDownY);
  EXPECT_FALSE(ScrollToCursorRequested());
}

TEST_F(TextEditorTests, HandleScrollingDirectScrollsUnwrappedCursorAboveAndBelowViewport) {
  editor.setWordWrapEnabled(false);
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());
  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  editor.setCursorPosition(Coordinates(40, 0));
  RequestCursorScroll();
  const float scrollTargetBelow = HandleScrollingDirect(/*initialScrollY=*/0.0f);

  EXPECT_GT(scrollTargetBelow, 0.0f);
  EXPECT_FALSE(ScrollToCursorRequested());

  const float scrolledDownY = 30.0f * CharacterAdvanceY();
  editor.setCursorPosition(Coordinates(2, 0));
  RequestCursorScroll();
  const float scrollTargetAbove = HandleScrollingDirect(scrolledDownY);

  EXPECT_LT(scrollTargetAbove, scrolledDownY);
  EXPECT_FALSE(ScrollToCursorRequested());
}

TEST_F(TextEditorTests, ScrollRangeIntoViewHandlesDirectWrappedAndUnwrappedBranches) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());
  RenderEditorFrame(ImVec2(240.0f, 80.0f));

  const float hugeSelectionTarget = ScrollRangeIntoViewDirectInChild(
      Coordinates(10, 0), Coordinates(40, 0), /*initialScrollY=*/0.0f);
  EXPECT_GT(hugeSelectionTarget, 0.0f);

  const float scrolledDownY = 30.0f * CharacterAdvanceY();
  const float aboveViewportTarget =
      ScrollRangeIntoViewDirectInChild(Coordinates(2, 0), Coordinates(3, 0), scrolledDownY);
  EXPECT_LT(aboveViewportTarget, scrolledDownY);

  const float belowViewportTarget = ScrollRangeIntoViewDirectInChild(
      Coordinates(35, 0), Coordinates(35, 4), /*initialScrollY=*/0.0f);
  EXPECT_GT(belowViewportTarget, 0.0f);

  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n");
  RenderEditorFrame(ImVec2(220.0f, 80.0f));
  const Coordinates wrappedTail(0, 112);
  ASSERT_GT(VisualLineIndexForCoordinates(wrappedTail), 0);

  const float wrappedTarget = ScrollRangeIntoViewDirectInChild(
      Coordinates(0, 0), wrappedTail, /*initialScrollY=*/0.0f, ImVec2(220.0f, 80.0f));
  EXPECT_GE(wrappedTarget, 0.0f);
}

TEST_F(TextEditorTests, VisualCoordinatesClampWrappedRowsAtHorizontalAndVerticalExtremes) {
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n"
      "tail");
  RenderEditorFrame(ImVec2(220.0f, 80.0f));

  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  const ImVec2 farRight = ScreenPointAtVisualTextOffset(continuationIndex, 400);
  EXPECT_EQ(VisualScreenPosToCoordinatesDirect(farRight),
            Coordinates(0, VisualLineEndColumn(continuationIndex)));

  ImVec2 farAbove = farRight;
  farAbove.y -= 500.0f;
  EXPECT_EQ(VisualScreenPosToCoordinatesDirect(farAbove).line, 0);

  ImVec2 farBelow = farRight;
  farBelow.y += 500.0f;
  EXPECT_EQ(VisualScreenPosToCoordinatesDirect(farBelow).line, 1);
}

TEST_F(TextEditorTests, RenderSelectionBranchesCoverUnwrappedAndWrappedLineRanges) {
  editor.setWordWrapEnabled(false);
  editor.setText("alpha\nbeta\ngamma");
  editor.setSelection(Coordinates(0, 2), Coordinates(2, 3));
  RenderEditorFrame(ImVec2(320.0f, 120.0f));
  EXPECT_EQ(editor.getSelectedText(), "pha\nbeta\ngam");

  editor.setWordWrapEnabled(true);
  editor.setText(
      "  <rect id=\"target\" x=\"10\" y=\"20\" width=\"30\" height=\"40\" "
      "fill=\"red\" stroke=\"blue\" opacity=\"0.5\" transform=\"translate(1 2)\"/>\n"
      "after");
  RenderEditorFrame(ImVec2(220.0f, 80.0f));
  const int continuationIndex = FirstContinuationVisualLineForLine(0);
  ASSERT_NE(continuationIndex, -1);
  editor.setSelection(Coordinates(0, 4), Coordinates(1, 3));
  RenderEditorFrame(ImVec2(220.0f, 80.0f));
  EXPECT_TRUE(editor.hasSelection());
}

TEST_F(TextEditorTests, TypingOnTopVisibleLineDoesNotNudgeScrollUp) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 80.0f);
  RenderEditorFrame(kEditorSize);
  editor.selectAndFocus(Coordinates(40, 0), Coordinates(40, 0));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const int topVisibleLine = static_cast<int>(std::floor(LastScrollY() / CharacterAdvanceY()));
  editor.setCursorPosition(Coordinates(topVisibleLine, 0));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);
  const float beforeTypingScrollY = LastScrollY();

  EnterCharacter('x');
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  EXPECT_GE(LastScrollY() + 0.5f, beforeTypingScrollY);
}

TEST_F(TextEditorTests, AutocompletePopupDoesNotPerturbEditorScroll) {
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "<path id=\"line" << i << "\" class=\"cls-" << i << "\"/>\n";
  }
  editor.setText(source.str());

  constexpr ImVec2 kEditorSize(240.0f, 80.0f);
  RenderEditorFrame(kEditorSize);
  editor.selectAndFocus(Coordinates(40, 8), Coordinates(40, 8));
  RenderEditorFrame(kEditorSize);
  RenderEditorFrame(kEditorSize);

  const float beforePopupScrollY = LastScrollY();
  OpenAutocompleteAtCursor("style");
  RenderEditorFrame(kEditorSize);
  const float afterOpenScrollY = LastScrollY();

  ReplaceAutocompleteSuggestion("stroke");
  RenderEditorFrame(kEditorSize);
  const float afterSuggestionChangeScrollY = LastScrollY();

  EXPECT_NEAR(afterOpenScrollY, beforePopupScrollY, 0.5f);
  EXPECT_NEAR(afterSuggestionChangeScrollY, beforePopupScrollY, 0.5f);
  EXPECT_EQ(AutocompleteChildWindowCount(), 0);
  EXPECT_EQ(AutocompleteTopLevelWindowCount(), 1);
}

TEST_F(TextEditorTests, AutocompleteParseSnippetReplacesRepeatedPlaceholders) {
  const RcString parsed =
      ParseAutocompleteSnippet(R"(<{$1:rect} id="{$2:name}">{$1}</{$1}>)", Coordinates(3, 4));

  EXPECT_EQ(std::string(std::string_view(parsed)), R"(<rect id="name">rect</rect>)");
  EXPECT_TRUE(IsSnippet());
  EXPECT_EQ(SnippetIds(), (std::vector<int>{1, 2, 1, 1}));
  EXPECT_EQ(SnippetHighlights(), (std::vector<bool>{true, true, false, false}));
  EXPECT_THAT(SnippetStarts(), ElementsAre(Coordinates(3, 5), Coordinates(3, 14),
                                           Coordinates(3, 20), Coordinates(3, 26)));
  EXPECT_THAT(SnippetEnds(), ElementsAre(Coordinates(3, 9), Coordinates(3, 18), Coordinates(3, 24),
                                         Coordinates(3, 30)));
}

TEST_F(TextEditorTests, AutocompleteParseSnippetWithoutTagsClearsSnippetState) {
  ASSERT_FALSE(ParseAutocompleteSnippet("{$1:value}").empty());
  ASSERT_TRUE(IsSnippet());

  const RcString parsed = ParseAutocompleteSnippet("plain text");

  EXPECT_EQ(std::string(std::string_view(parsed)), "plain text");
  EXPECT_FALSE(IsSnippet());
  EXPECT_TRUE(SnippetStarts().empty());
  EXPECT_TRUE(SnippetEnds().empty());
  EXPECT_TRUE(SnippetIds().empty());
  EXPECT_TRUE(SnippetHighlights().empty());
}

TEST_F(TextEditorTests, BuildSuggestionsUsesStructuredProviderReplacementRange) {
  editor.setText("alpha beta gamma");
  editor.setCursorPosition(Coordinates(0, 8));
  editor.setAutocompleteProvider([](const TextEditor::AutocompleteRequest& request)
                                     -> std::optional<TextEditor::AutocompleteResponse> {
    EXPECT_EQ(request.source, "alpha beta gamma");
    EXPECT_EQ(request.cursorOffset, 8u);
    TextEditor::AutocompleteResponse response;
    response.replaceStartOffset = 6;
    response.replaceEndOffset = 10;
    response.suggestions.push_back(
        TextEditor::AutocompleteSuggestion{RcString("beta item"), RcString("replacement")});
    response.suggestions.push_back(
        TextEditor::AutocompleteSuggestion{RcString("beta item 2"), RcString("replacement2")});
    return response;
  });

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_TRUE(AutocompleteOpened());
  EXPECT_TRUE(AutocompleteReplacementActive());
  EXPECT_EQ(AutocompleteReplacementStartOffset(), 6u);
  EXPECT_EQ(AutocompleteReplacementEndOffset(), 10u);
  EXPECT_EQ(AutocompletePosition(), Coordinates(0, 6));
  ASSERT_EQ(AutocompleteSuggestionCount(), 2);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("beta item"));
  EXPECT_EQ(AutocompleteSuggestion(0).second, RcString("replacement"));
}

TEST_F(TextEditorTests, BuildSuggestionsProviderEmptyResponseSuppressesFallback) {
  editor.setText("alpha beta gamma");
  editor.setCursorPosition(Coordinates(0, 8));
  editor.addAutocompleteEntry("beta", "fallback beta", "fallback");
  editor.setAutocompleteProvider([](const TextEditor::AutocompleteRequest&)
                                     -> std::optional<TextEditor::AutocompleteResponse> {
    return TextEditor::AutocompleteResponse{};
  });

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_FALSE(AutocompleteOpened());
  EXPECT_FALSE(AutocompleteReplacementActive());
  EXPECT_EQ(AutocompleteSuggestionCount(), 0);
}

TEST_F(TextEditorTests, BuildSuggestionsFallsBackToStoredEntriesInMatchOrder) {
  editor.setText("ap");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.setLanguageDefinition(TextEditor::LanguageDefinition{});
  editor.addAutocompleteEntry("snapple", "snapple entry", "snapple()");
  editor.addAutocompleteEntry("apple", "apple entry", "apple()");
  editor.addAutocompleteEntry("apricot", "apricot entry", "apricot()");

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_TRUE(AutocompleteOpened());
  ASSERT_EQ(AutocompleteSuggestionCount(), 3);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("apple entry"));
  EXPECT_EQ(AutocompleteSuggestion(1).first, RcString("apricot entry"));
  EXPECT_EQ(AutocompleteSuggestion(2).first, RcString("snapple entry"));
  EXPECT_EQ(AutocompletePosition(), Coordinates(0, 0));
}

TEST_F(TextEditorTests, BuildSuggestionsIgnoresWordsWithoutLetters) {
  editor.setText("123");
  editor.setCursorPosition(Coordinates(0, 3));
  editor.addAutocompleteEntry("123", "numeric", "numeric");

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_FALSE(keepAutocompleteOpen);
  EXPECT_FALSE(AutocompleteOpened());
  EXPECT_EQ(AutocompleteSuggestionCount(), 0);
}

TEST_F(TextEditorTests, BuildSuggestionsAcceptsUppercaseWords) {
  editor.setText("AP");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.setLanguageDefinition(TextEditor::LanguageDefinition{});
  editor.addAutocompleteEntry("APEX", "upper", "APEX()");

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_TRUE(AutocompleteOpened());
  ASSERT_EQ(AutocompleteSuggestionCount(), 1);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("upper"));
}

TEST_F(TextEditorTests, BuildSuggestionsIncludesLanguageKeywords) {
  editor.setText("str");
  editor.setCursorPosition(Coordinates(0, 3));
  TextEditor::LanguageDefinition language;
  language.keywords.insert("stroke");
  language.keywords.insert("fill");
  editor.setLanguageDefinition(language);

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_TRUE(AutocompleteOpened());
  ASSERT_EQ(AutocompleteSuggestionCount(), 1);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("stroke"));
  EXPECT_EQ(AutocompleteSuggestion(0).second, RcString("stroke"));
}

TEST_F(TextEditorTests, BuildSuggestionsIncludesLanguageIdentifiers) {
  editor.setText("paint");
  editor.setCursorPosition(Coordinates(0, 5));
  TextEditor::LanguageDefinition language;
  language.identifiers.emplace("paintServer", TextEditor::Identifier("gradient declaration"));
  language.identifiers.emplace("clipPath", TextEditor::Identifier("clip declaration"));
  editor.setLanguageDefinition(language);

  bool keepAutocompleteOpen = false;
  BuildSuggestions(&keepAutocompleteOpen);

  EXPECT_TRUE(keepAutocompleteOpen);
  EXPECT_TRUE(AutocompleteOpened());
  ASSERT_EQ(AutocompleteSuggestionCount(), 1);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("paintServer"));
  EXPECT_EQ(AutocompleteSuggestion(0).second, RcString("paintServer"));
}

// ============================================================================
// COORDINATE SYSTEM & EDGE CASE TESTS
// ============================================================================

TEST_F(TextEditorTests, SetTextClearsBuffer) {
  editor.setText("Hello");
  editor.setText("World");
  EXPECT_EQ(editor.getText(), "World") << "setText should replace entire buffer";
}

TEST_F(TextEditorTests, EmptyBufferHasSingleLine) {
  TextEditor emptyEditor;
  EXPECT_EQ(emptyEditor.getText(), "") << "Empty editor should have no text";
}

TEST_F(TextEditorTests, GetTextReturnsExactBuffer) {
  std::string content = "Hello\nWorld\nTest";
  editor.setText(content);
  EXPECT_EQ(editor.getText(), content) << "getText should return exact buffer content";
}

TEST_F(TextEditorTests, SelectionNormalizedIfStartAfterEnd) {
  editor.setText("Hello world");
  // Pass the bounds in reverse order - `setSelection` should
  // normalize them so `start <= end` and the resulting selection
  // covers the same character range as if they'd been in order.
  editor.setSelection(Coordinates(0, 8), Coordinates(0, 2));
  // Half-open range [2, 8) over "Hello world" = "llo wo".
  EXPECT_EQ(editor.getSelectedText(), "llo wo") << "setSelection should normalize reversed ranges";
}

TEST_F(TextEditorTests, MultipleDeletesRemoveChars) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.delete_();
  editor.delete_();
  editor.delete_();
  EXPECT_EQ(editor.getText(), "He") << "Multiple deletes should remove multiple characters";
}

TEST_F(TextEditorTests, InsertAfterUndoDoesNotClearRedo) {
  editor.setText("A");
  // Drop the cursor at the end of "A" before inserting so the
  // following inserts append rather than prepending. `setText`
  // leaves the cursor at (0, 0).
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("B");
  editor.insertText("C");
  EXPECT_EQ(editor.getText(), "ABC");
  editor.undo();
  editor.undo();
  EXPECT_EQ(editor.getText(), "A");
  EXPECT_TRUE(editor.canRedo()) << "Should be able to redo after undo";
  // Now insert new text - this should clear redo
  editor.insertText("X");
  EXPECT_FALSE(editor.canRedo()) << "New edit should clear redo history";
}

// ============================================================================
// HOME/END TESTS
// ============================================================================

TEST_F(TextEditorTests, HomeOnSecondLineMovesToStartOfLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(1, 3));
  editor.moveHome(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(1, 0))
      << "moveHome should move to column 0 of current line";
}

TEST_F(TextEditorTests, EndOnFirstLineMovesToEndOfLine) {
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveEnd(false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveEnd should move to end of current line";
}

// ============================================================================
// SELECTION WITH MOVEMENT TESTS
// ============================================================================

TEST_F(TextEditorTests, ShiftHomeSelectsToLineStart) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 3));
  editor.moveHome(true);  // select=true
  EXPECT_EQ(editor.getSelectedText(), "Hel")
      << "Shift+Home should select from cursor to line start";
}

TEST_F(TextEditorTests, ShiftEndSelectsToLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveEnd(true);  // select=true
  EXPECT_EQ(editor.getSelectedText(), "llo") << "Shift+End should select from cursor to line end";
}

TEST_F(TextEditorTests, ShiftUpSelectsMultipleLinesUp) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 1));
  editor.moveUp(1, true);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Up should create selection";
}

TEST_F(TextEditorTests, ShiftDownSelectsMultipleLinesDown) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.moveDown(1, true);  // select=true
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Down should create selection";
}

TEST_F(TextEditorTests, ShiftCtrlEndSelectsToDocumentEnd) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.moveBottom(true);  // select=true to document end
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Ctrl+End should create selection to document end";
}

TEST_F(TextEditorTests, ShiftCtrlHomeSelectsToDocumentStart) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(2, 2));
  editor.moveTop(true);  // select=true to document start
  EXPECT_TRUE(editor.hasSelection()) << "Shift+Ctrl+Home should create selection to document start";
}

TEST_F(TextEditorTests, MultiLineDeleteAcrossLines) {
  editor.setText("Line1\nLine2\nLine3");
  // Select cols 2..4 across lines 0..2 (half-open). That covers
  // "ne1\nLine2\nLine" - the trailing 4 chars on line 0, all of
  // line 1, and the leading 4 chars on line 2. Replacing it with
  // empty text should leave "Li" + "3" = "Li3".
  editor.setSelection(Coordinates(0, 2), Coordinates(2, 4));
  editor.insertText("");
  EXPECT_EQ(editor.getText(), "Li3") << "Replacing multi-line selection should join remaining text";
}

TEST_F(TextEditorTests, CopyMultipleLines) {
  editor.setText("Line1\nLine2\nLine3");
  editor.setSelection(Coordinates(0, 0), Coordinates(2, 5));
  editor.copy();
  const char* clipboard = ImGui::GetClipboardText();
  EXPECT_STREQ(clipboard, "Line1\nLine2\nLine3") << "copy should handle multi-line selections";
}

TEST_F(TextEditorTests, InsertMultipleCharactersInSequence) {
  editor.setText("");
  editor.insertText("H");
  editor.insertText("e");
  editor.insertText("l");
  editor.insertText("l");
  editor.insertText("o");
  EXPECT_EQ(editor.getText(), "Hello") << "Sequential character insertions should build word";
}

TEST_F(TextEditorTests, GetSelectedTextWithoutSelection) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 2));
  std::string selected = editor.getSelectedText();
  EXPECT_EQ(selected, "") << "getSelectedText without selection should return empty string";
}

TEST_F(TextEditorTests, MoveRightMultipleTimes) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  editor.moveRight(5, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5))
      << "moveRight(5) should move cursor 5 columns";
}

TEST_F(TextEditorTests, MoveLeftMultipleTimes) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 4));
  editor.moveLeft(3, false, false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "moveLeft(3) should move cursor 3 columns";
}

TEST_F(TextEditorTests, SelectionPreservesAfterMove) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  EXPECT_EQ(editor.getSelectedText(), "Hello");
  // Move cursor - selection should remain
  editor.moveRight(2, false, false);
  EXPECT_EQ(editor.getSelectedText(), "Hello")
      << "Selection should be preserved after non-selection move";
}

TEST_F(TextEditorTests, DeleteMultipleSelectionsSuccessively) {
  editor.setText("ABCDEFGH");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 2));
  editor.insertText("");  // Delete "AB"
  EXPECT_EQ(editor.getText(), "CDEFGH");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 2));
  editor.insertText("");  // Delete "CD"
  EXPECT_EQ(editor.getText(), "EFGH") << "Successive deletions should work correctly";
}

TEST_F(TextEditorTests, UndoMultipleOperations) {
  editor.setText("A");
  // Position cursor at end of "A" so the following inserts append
  // rather than prepending. (`setText` leaves the cursor at the
  // start of the buffer.)
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("B");
  editor.insertText("C");
  editor.insertText("D");
  EXPECT_EQ(editor.getText(), "ABCD");
  // Three insertText calls → three undo entries; `undo(4)` walks
  // them all and stops at the start of the undo buffer.
  editor.undo(4);
  EXPECT_EQ(editor.getText(), "A") << "undo(4) should revert to 'A'";
}

TEST_F(TextEditorTests, RedoAfterMultipleUndos) {
  editor.setText("X");
  editor.setCursorPosition(Coordinates(0, 1));
  editor.insertText("Y");
  editor.insertText("Z");
  editor.undo(2);
  EXPECT_EQ(editor.getText(), "X");
  editor.redo(1);
  EXPECT_EQ(editor.getText(), "XY") << "redo(1) should restore one operation";
}

TEST_F(TextEditorTests, SetSelectionOnMultipleLines) {
  editor.setText("AAA\nBBB\nCCC");
  editor.setSelection(Coordinates(0, 1), Coordinates(2, 2));
  std::string selected = editor.getSelectedText();
  EXPECT_EQ(selected, "AA\nBBB\nCC") << "setSelection should correctly span multiple lines";
}

TEST_F(TextEditorTests, CursorStaysInBoundsAfterDelete) {
  editor.setText("AB");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.delete_();
  // Cursor should remain at valid position (column 2 is at end of "AB")
  Coordinates pos = editor.getCursorPosition();
  EXPECT_LE(pos.column, 2) << "Cursor should stay in valid bounds after delete";
}

TEST_F(TextEditorTests, EmptyLineHandling) {
  editor.setText("A\n\nC");
  EXPECT_EQ(editor.getText(), "A\n\nC");
  editor.setCursorPosition(Coordinates(1, 0));
  editor.insertText("B");
  EXPECT_EQ(editor.getText(), "A\nB\nC") << "insertText on empty line should add character";
}

TEST_F(TextEditorTests, SelectWordAtLineEnd) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 4));
  editor.selectWordUnderCursor();
  // Should select "Hello" (the word containing position 4)
  EXPECT_EQ(editor.getSelectedText(), "Hello")
      << "selectWordUnderCursor at line end should select the word";
}

TEST_F(TextEditorTests, SelectAllOnMultilineDocument) {
  std::string content = "Line1\nLine2\nLine3";
  editor.setText(content);
  editor.selectAll();
  EXPECT_EQ(editor.getSelectedText(), content)
      << "selectAll should select entire multi-line document";
}

// ============================================================================
// KEYBOARD INPUT THROUGH THE FULL RENDER PATH
//
// These tests drive `handleKeyboardInputs()` / `executeAction()` /
// `handleCharacterInput()` by injecting ImGui key + character events and
// rendering a frame with keyboard handling enabled, mirroring how the live
// editor processes input. The `Shortcut::matches()` matcher requires a fresh
// up→down key transition, so each test releases keys via `ResetKeyboardState`
// before pressing them.
// ============================================================================

TEST_F(TextEditorTests, TypedCharacterFlowsThroughRenderIntoBuffer) {
  editor.setText("");
  editor.setCursorPosition(Coordinates(0, 0));

  RenderEditorFrameWithKeyboard(/*keys=*/{}, /*characters=*/"Hi");

  EXPECT_EQ(editor.getText(), "Hi")
      << "Characters from InputQueueCharacters should be inserted via the render path";
}

TEST_F(TextEditorTests, ArrowRightKeyMovesCursorThroughRenderPath) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));

  ResetKeyboardState({ImGuiKey_RightArrow});
  RenderEditorFrameWithKeyboard({ImGuiKey_RightArrow});

  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 1))
      << "RightArrow routed through handleKeyboardInputs should advance the cursor";
}

TEST_F(TextEditorTests, ShiftDownArrowSelectsThroughRenderPath) {
  // Note: `executeAction` only wires the vertical select shortcuts
  // (`SelectUp`/`SelectDown`); the horizontal `SelectLeft`/`SelectRight`
  // entries fall through to `default:` and are intentionally no-ops on the
  // keyboard path. Exercise the wired vertical case here.
  editor.setText("Line1\nLine2");
  editor.setCursorPosition(Coordinates(0, 2));

  ResetKeyboardState({ImGuiKey_DownArrow}, /*ctrl=*/false, /*shift=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_DownArrow}, /*characters=*/"", /*ctrl=*/false,
                                /*shift=*/true);

  EXPECT_TRUE(editor.hasSelection()) << "Shift+Down should extend the selection";
  EXPECT_EQ(editor.getSelectedText(), "ne1\nLi");
}

TEST_F(TextEditorTests, EnterKeyInsertsNewlineThroughRenderPath) {
  // Drives the keyboard NewLine path through the full render loop with the default
  // smartIndent enabled. This previously crashed via a use-after-realloc in
  // TextEditorCore::handleNewLine (a Line& held across insertLine) - now fixed.
  editor.setText("  AB");                       // leading indent so smart indent runs
  editor.setCursorPosition(Coordinates(0, 4));  // end of "  AB"

  ResetKeyboardState({ImGuiKey_Enter});
  RenderEditorFrameWithKeyboard({ImGuiKey_Enter});

  EXPECT_EQ(editor.getText(), "  AB\n  ")
      << "Enter should split the line at the cursor and auto-indent the new line";
}

TEST_F(TextEditorTests, BackspaceKeyDeletesThroughRenderPath) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));

  ResetKeyboardState({ImGuiKey_Backspace});
  RenderEditorFrameWithKeyboard({ImGuiKey_Backspace});

  EXPECT_EQ(editor.getText(), "Hell") << "Backspace should delete the character before the cursor";
}

TEST_F(TextEditorTests, DeleteKeyDeletesForwardThroughRenderPath) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 0));

  ResetKeyboardState({ImGuiKey_Delete});
  RenderEditorFrameWithKeyboard({ImGuiKey_Delete});

  EXPECT_EQ(editor.getText(), "ello") << "Delete should remove the character at the cursor";
}

TEST_F(TextEditorTests, TabKeyInsertsTabCharacterThroughRenderPath) {
  editor.setText("");
  // setText runs indentation detection, so configure tab-vs-space behavior
  // afterwards. With hard tabs the Indent shortcut inserts a single '\t'.
  editor.setInsertSpaces(false);
  editor.setCursorPosition(Coordinates(0, 0));

  ResetKeyboardState({ImGuiKey_Tab});
  RenderEditorFrameWithKeyboard({ImGuiKey_Tab});

  EXPECT_EQ(editor.getText(), "\t")
      << "Tab with no selection and hard tabs should insert a tab character";
}

TEST_F(TextEditorTests, TabKeyInsertsSpacesWhenSoftTabsThroughRenderPath) {
  editor.setText("");
  editor.setInsertSpaces(true);
  editor.setTabSize(4);
  editor.setCursorPosition(Coordinates(0, 0));

  ResetKeyboardState({ImGuiKey_Tab});
  RenderEditorFrameWithKeyboard({ImGuiKey_Tab});

  EXPECT_EQ(editor.getText(), "    ")
      << "Tab with no selection and soft tabs should insert tab-sized spaces";
}

TEST_F(TextEditorTests, CtrlAUndoRedoThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));

  ResetKeyboardState({ImGuiKey_A}, /*ctrl=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_A}, /*characters=*/"", /*ctrl=*/true);

  EXPECT_EQ(editor.getSelectedText(), "Hello world")
      << "Ctrl+A through the render path should select all";
}

TEST_F(TextEditorTests, CtrlCAndCtrlVRoundTripThroughRenderPath) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 5));
  editor.setCursorPosition(Coordinates(0, 5));

  ResetKeyboardState({ImGuiKey_C}, /*ctrl=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_C}, /*characters=*/"", /*ctrl=*/true);
  EXPECT_STREQ(ImGui::GetClipboardText(), "Hello")
      << "Ctrl+C through the render path should copy the selection";

  editor.setSelection(Coordinates(0, 0), Coordinates(0, 0));
  editor.setCursorPosition(Coordinates(0, 11));
  ResetKeyboardState({ImGuiKey_V}, /*ctrl=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_V}, /*characters=*/"", /*ctrl=*/true);
  EXPECT_EQ(editor.getText(), "Hello worldHello")
      << "Ctrl+V through the render path should paste at the cursor";
}

TEST_F(TextEditorTests, CtrlXCutsSelectionThroughRenderPath) {
  editor.setText("Hello world");
  editor.setSelection(Coordinates(0, 0), Coordinates(0, 6));
  editor.setCursorPosition(Coordinates(0, 6));

  ResetKeyboardState({ImGuiKey_X}, /*ctrl=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_X}, /*characters=*/"", /*ctrl=*/true);

  EXPECT_EQ(editor.getText(), "world") << "Ctrl+X through the render path should cut the selection";
}

TEST_F(TextEditorTests, CtrlZUndoThroughRenderPath) {
  editor.setText("Hello");
  editor.setCursorPosition(Coordinates(0, 5));
  EnterCharacter('!');
  ASSERT_EQ(editor.getText(), "Hello!");

  ResetKeyboardState({ImGuiKey_Z}, /*ctrl=*/true);
  RenderEditorFrameWithKeyboard({ImGuiKey_Z}, /*characters=*/"", /*ctrl=*/true);

  EXPECT_EQ(editor.getText(), "Hello") << "Ctrl+Z through the render path should undo";
}

// ============================================================================
// RENDER-PATH OPTION TOGGLES
//
// Each test flips a render-affecting option and renders a frame, exercising
// the corresponding draw branch (whitespace dots/arrows, line numbers,
// scrollbar markers, error markers, fold markers, the non-wrapped renderLine
// path, custom palettes) without a GPU backend.
// ============================================================================

TEST_F(TextEditorTests, ShowWhitespacesRendersWithoutCrash) {
  editor.setShowWhitespaces(true);
  EXPECT_TRUE(editor.isShowingWhitespaces());
  editor.setText("a\tb c\n  indented");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_GT(VisualLineCount(), 0) << "Whitespace rendering should produce visual rows";
}

TEST_F(TextEditorTests, LineNumbersDisabledStillRenders) {
  editor.setShowLineNumbers(false);
  editor.setText("Line1\nLine2\nLine3");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_EQ(editor.getTextStart(), 3.0f)
      << "getTextStart should report the no-line-number offset when disabled";
}

TEST_F(TextEditorTests, LineNumbersEnabledRenders) {
  editor.setShowLineNumbers(true);
  editor.setText("Line1\nLine2\nLine3");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_EQ(editor.getTextStart(), 7.0f)
      << "getTextStart should report the line-number offset when enabled";
}

TEST_F(TextEditorTests, NonWrappedRenderPathDrawsLines) {
  editor.setWordWrapEnabled(false);
  EXPECT_FALSE(editor.wordWrapEnabled());
  editor.setText("Line1\nLine2\nLine3");
  editor.setCursorPosition(Coordinates(1, 0));

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  // With wrapping off the visual layout mirrors logical lines one-for-one.
  EXPECT_THAT(VisualLineLogicalLines(), ElementsAre(0, 1, 2));
}

TEST_F(TextEditorTests, ScrollbarMarkersRenderWithChangesAndErrors) {
  editor.setScrollbarMarkers(true);
  std::ostringstream source;
  for (int i = 0; i < 80; ++i) {
    source << "line" << i << "\n";
  }
  editor.setText(source.str());
  editor.setErrorMarkers(ErrorMarkers{{3, "boom"}, {10, "kaboom"}});
  editor.setCursorPosition(Coordinates(20, 0));
  // Produce a change-tracking entry so renderScrollbarChanges has work.
  EnterCharacter('x');

  RenderEditorFrame(ImVec2(240.0f, 120.0f));
  RenderEditorFrame(ImVec2(240.0f, 120.0f));

  EXPECT_GT(VisualLineCount(), 0) << "Scrollbar marker rendering should not disturb layout";
}

TEST_F(TextEditorTests, ErrorMarkersRenderLineBackground) {
  editor.setText("first\nsecond\nthird");
  editor.setErrorMarkers(ErrorMarkers{{2, "syntax error"}});
  editor.setCursorPosition(Coordinates(1, 0));

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_GT(VisualLineCount(), 0);
}

TEST_F(TextEditorTests, SourceDiagnosticsPreferNarrowestRangeAtByteOffset) {
  editor.setText("abcdef");
  ASSERT_TRUE(editor.setSourceDiagnostics({
      SourceDiagnostic{.id = 1, .range = {1, 5}, .message = "wide"},
      SourceDiagnostic{.id = 2, .range = {2, 4}, .message = "narrow"},
  }));

  EXPECT_EQ(editor.sourceDiagnosticAtByteOffset(3), 2u);
  EXPECT_EQ(editor.sourceDiagnosticAtByteOffset(5), std::nullopt);
}

TEST_F(TextEditorTests, SourceDiagnosticsTrackSourceEditsAndValidateActiveId) {
  editor.setText("abcdef");
  ASSERT_TRUE(editor.setSourceDiagnostics({
      SourceDiagnostic{.id = 7, .range = {2, 4}, .line = 1, .column = 2, .message = "issue"},
  }));
  EXPECT_TRUE(editor.setActiveSourceDiagnosticId(7));
  EXPECT_EQ(editor.activeSourceDiagnosticId(), 7u);

  editor.setCursorPosition(Coordinates(0, 0));
  editor.insertText("XX");

  ASSERT_EQ(editor.sourceDiagnostics().size(), 1u);
  EXPECT_EQ(editor.sourceDiagnostics().front().range, (SourceByteRange{4, 6}));
  EXPECT_EQ(editor.sourceDiagnostics().front().column, 4u);
  EXPECT_EQ(editor.sourceDiagnostics().front().endLine, 1u);
  EXPECT_EQ(editor.sourceDiagnostics().front().endColumn, 6u);
  EXPECT_FALSE(editor.setActiveSourceDiagnosticId(99));
  EXPECT_EQ(editor.activeSourceDiagnosticId(), std::nullopt);
}

TEST_F(TextEditorTests, HoveringErrorMarkerRendersTooltip) {
  editor.setText("first\nsecond\nthird");
  editor.setErrorMarkers(ErrorMarkers{{2, "syntax error"}});
  editor.setCursorPosition(Coordinates(0, 0));

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  const ImVec2 errorLinePoint =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/1, /*visualColumnOffset=*/1);

  RenderEditorFrameWithMouse(errorLinePoint, /*mouseDown=*/false, ImVec2(300.0f, 180.0f));

  EXPECT_GT(WindowCountContaining("##Tooltip"), 0);
}

TEST_F(TextEditorTests, ErrorMarkerRendererSkipsLinesWithoutMarkers) {
  editor.setText("first\nsecond");
  editor.setErrorMarkers(ErrorMarkers{{2, "syntax error"}});

  const int addedVertices =
      RenderErrorMarkersDirect(/*lineNo=*/0, ImVec2(20.0f, 20.0f), ImVec2(180.0f, 38.0f));

  EXPECT_EQ(addedVertices, 0);
}

TEST_F(TextEditorTests, ErrorMarkerRendererShowsTooltipWhenHovered) {
  editor.setText("first\nsecond");
  editor.setErrorMarkers(ErrorMarkers{{1, "syntax error"}});

  (void)RenderErrorMarkersDirect(/*lineNo=*/0, ImVec2(20.0f, 20.0f), ImVec2(180.0f, 38.0f),
                                 ImVec2(60.0f, 28.0f));

  EXPECT_GT(WindowCountContaining("##Tooltip"), 0);
}

TEST_F(TextEditorTests, HighlightedLinesRender) {
  editor.setWordWrapEnabled(false);
  editor.setText("one\ntwo\nthree");
  editor.setHighlightedLines({1});

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_GT(VisualLineCount(), 0);

  editor.clearHighlightedLines();
  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_GT(VisualLineCount(), 0);
}

TEST_F(TextEditorTests, FoldMarkersRenderForBracedBlock) {
  editor.setWordWrapEnabled(false);
  editor.setFoldEnabled(true);
  editor.setText("{\n  child\n}\ntrailing");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_GT(VisualLineCount(), 0) << "Folded-document rendering should not disturb layout";
}

TEST_F(TextEditorTests, DirectFoldMarkerRenderingTogglesFoldAndDrawsConnections) {
  editor.setWordWrapEnabled(false);
  editor.setFoldEnabled(true);
  editor.setText("{\n  {\n  }\n}\n");
  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  ConfigureFoldState({Coordinates(0, 0), Coordinates(1, 2)}, {Coordinates(3, 0), Coordinates(2, 2)},
                     {false, false}, {0, 1});

  const ImVec2 lineStart(24.0f, 28.0f);
  const ImVec2 foldCenter = FoldButtonCenterForLineStart(lineStart);
  RenderFoldMarkerDirect(/*lineNo=*/0, lineStart, foldCenter, /*mouseDown=*/false);
  RenderFoldMarkerDirect(/*lineNo=*/0, lineStart, foldCenter, /*mouseDown=*/true);

  EXPECT_TRUE(FoldState(0));

  RenderFoldMarkerDirect(/*lineNo=*/0, lineStart, foldCenter, /*mouseDown=*/false);
  const ImVec2 offscreenMouse(700.0f, 500.0f);
  RenderFoldMarkerDirect(/*lineNo=*/1, ImVec2(lineStart.x, lineStart.y + CharacterAdvanceY()),
                         offscreenMouse, /*mouseDown=*/false);
  RenderFoldMarkerDirect(/*lineNo=*/2,
                         ImVec2(lineStart.x, lineStart.y + 2.0f * CharacterAdvanceY()),
                         offscreenMouse, /*mouseDown=*/false);
  RenderFoldMarkerDirect(/*lineNo=*/3,
                         ImVec2(lineStart.x, lineStart.y + 3.0f * CharacterAdvanceY()),
                         offscreenMouse, /*mouseDown=*/false);
}

TEST_F(TextEditorTests, DarkPaletteUsesGraphiteDesignLanguage) {
  const TextEditor::Palette& palette = TextEditor::getDarkPalette();
  EXPECT_EQ(palette[static_cast<int>(ColorIndex::Background)], 0xff191615u);
  EXPECT_EQ(palette[static_cast<int>(ColorIndex::Keyword)], 0xffb7d135u);
  EXPECT_EQ(palette[static_cast<int>(ColorIndex::String)], 0xff74b7f0u);
  EXPECT_EQ(palette[static_cast<int>(ColorIndex::ErrorMessage)], 0xff6a61f0u);
  EXPECT_NE(palette[static_cast<int>(ColorIndex::Default)],
            palette[static_cast<int>(ColorIndex::Comment)]);
}

TEST_F(TextEditorTests, CustomPaletteIsAppliedThroughRender) {
  TextEditor::Palette palette = TextEditor::getDarkPalette();
  palette[static_cast<int>(ColorIndex::Background)] = 0xff123456;
  editor.setPalette(palette);
  editor.setText("colored");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));

  EXPECT_EQ(editor.getPalette()[static_cast<int>(ColorIndex::Background)], 0xff123456u);
}

TEST_F(TextEditorTests, ColorizerDisabledRenders) {
  editor.setColorizerEnabled(false);
  EXPECT_FALSE(editor.isColorizerEnabled());
  editor.setText("<rect x=\"10\"/>");

  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_GT(VisualLineCount(), 0);

  editor.setColorizerEnabled(true);
  EXPECT_TRUE(editor.isColorizerEnabled());
  RenderEditorFrame(ImVec2(300.0f, 180.0f));
  EXPECT_GT(VisualLineCount(), 0);
}

// ============================================================================
// FIND / REPLACE
// ============================================================================

TEST_F(TextEditorTests, ProcessFindSelectsMatchAndMovesCursor) {
  editor.setText("alpha beta gamma beta");
  editor.setCursorPosition(Coordinates(0, 0));

  // `findNext=true` skips the `ImGui::SetKeyboardFocusHere` call, which can
  // only run inside an active ImGui frame; the located selection/cursor is the
  // same as the initial-find path.
  editor.processFind("beta", /*findNext=*/true);

  EXPECT_EQ(editor.getSelectedText(), "beta") << "processFind should select the first match";
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 10))
      << "processFind should move the cursor to the end of the match";
}

TEST_F(TextEditorTests, ProcessFindInitialOutsideFrameDoesNotCrash) {
  editor.setText("alpha beta gamma beta");
  editor.setCursorPosition(Coordinates(0, 0));

  // Regression: the initial-find path (findNext=false) called
  // `ImGui::SetKeyboardFocusHere(-1)` unconditionally. Outside an active ImGui
  // frame (the fixture has a context but has not called NewFrame) that
  // dereferences a null current window and segfaults. The find/select logic
  // must still run; only the focus side-effect is frame-scoped.
  editor.processFind("beta", /*findNext=*/false);

  EXPECT_EQ(editor.getSelectedText(), "beta");
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 10));
}

TEST_F(TextEditorTests, ProcessFindInitialInsideFrameRestoresKeyboardFocusSafely) {
  editor.setText("alpha beta gamma beta");
  editor.setCursorPosition(Coordinates(0, 0));

  ImGui::NewFrame();
  ImGui::Begin("ProcessFindFocusWindow");
  editor.processFind("beta", /*findNext=*/false);
  ImGui::End();
  ImGui::Render();

  EXPECT_EQ(editor.getSelectedText(), "beta");
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 10));
}

TEST_F(TextEditorTests, ProcessFindNextAdvancesPastCurrentMatch) {
  editor.setText("beta and beta again");
  editor.setCursorPosition(Coordinates(0, 5));

  editor.processFind("beta", /*findNext=*/true);

  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 13))
      << "find-next should locate the match after the cursor";
}

TEST_F(TextEditorTests, ProcessFindWrapsAroundWhenNoMatchAhead) {
  editor.setText("target trailing");
  editor.setCursorPosition(Coordinates(0, 10));

  editor.processFind("target", /*findNext=*/true);

  EXPECT_EQ(editor.getSelectedText(), "target")
      << "processFind should wrap to the document start when nothing matches ahead";
}

TEST_F(TextEditorTests, ProcessReplaceAllReplacesEveryOccurrence) {
  editor.setText("red red red");

  editor.processReplace("red", "blue", /*replaceAll=*/true);

  EXPECT_EQ(editor.getText(), "blue blue blue")
      << "replaceAll should replace every occurrence in the buffer";
}

TEST_F(TextEditorTests, ProcessReplaceSingleWrapsWhenNoMatchAhead) {
  editor.setText("red blue red");
  editor.setCursorPosition(Coordinates(0, 11));

  editor.processReplace("red", "green", /*replaceAll=*/false);

  EXPECT_EQ(editor.getText(), "green blue red")
      << "single replace should wrap to the first match when none remains after replaceIndex";
}

TEST_F(TextEditorTests, ProcessReplaceSingleAdvancesReplaceIndex) {
  editor.setText("red blue red");

  editor.processReplace("red", "green", /*replaceAll=*/false);
  editor.processReplace("red", "green", /*replaceAll=*/false);

  EXPECT_EQ(editor.getText(), "green blue green")
      << "single replace should continue after the previous replacement";
}

TEST_F(TextEditorTests, ProcessReplaceSingleNoMatchLeavesTextUnchanged) {
  editor.setText("unchanged");

  editor.processReplace("missing", "X", /*replaceAll=*/false);

  EXPECT_EQ(editor.getText(), "unchanged")
      << "single replace should be a no-op when the term is absent";
}

TEST_F(TextEditorTests, ProcessReplaceWithEmptyFindIsNoOp) {
  editor.setText("unchanged");

  editor.processReplace("", "X", /*replaceAll=*/true);

  EXPECT_EQ(editor.getText(), "unchanged")
      << "processReplace with an empty find term should make no edits";
}

TEST_F(TextEditorTests, FindDialogRendersAndClosesOnEscape) {
  editor.setText("alpha beta");

  // Prime the child window with the dialog closed, then open it and show it.
  RenderEditorFrameWithFont(ImVec2(320.0f, 180.0f));
  SetFindOpened(true, /*justOpened=*/true);
  RenderEditorFrameWithFont(ImVec2(320.0f, 180.0f));
  EXPECT_TRUE(FindOpened()) << "Find dialog should stay open while rendering";

  // Escape was never pressed before, so a single down-transition frame is a
  // fresh `IsKeyPressed`; the keyboard render helper pushes an editor font so
  // the dialog's pop/push font balance holds.
  RenderEditorFrameWithKeyboard({ImGuiKey_Escape}, /*characters=*/"", /*ctrl=*/false,
                                /*shift=*/false, /*alt=*/false, ImVec2(320.0f, 180.0f));

  EXPECT_FALSE(FindOpened()) << "Escape should dismiss the find dialog";
}

TEST_F(TextEditorTests, ReplaceDialogRendersExpandedRow) {
  editor.setText("red red");
  SetFindOpened(true);
  SetReplaceOpened(true);

  RenderEditorFrameWithFont(ImVec2(360.0f, 200.0f));

  EXPECT_TRUE(FindOpened());
  EXPECT_TRUE(ReplaceOpened()) << "Replace row should render when expanded";
}

TEST_F(TextEditorTests, FunctionTooltipRendersAndClosesOnEscape) {
  editor.setText("drawShape()");
  RenderEditorFrameWithFont(ImVec2(360.0f, 180.0f));
  OpenFunctionTooltipDirect("void drawShape()", Coordinates(0, 0));

  RenderEditorFrameWithFont(ImVec2(360.0f, 180.0f));

  EXPECT_TRUE(FunctionTooltipOpen());
  EXPECT_GT(WindowCountContaining("FunctionTooltip"), 0);

  ResetKeyboardState({ImGuiKey_Escape}, /*ctrl=*/false, /*shift=*/false, /*alt=*/false,
                     ImVec2(360.0f, 180.0f));
  RenderEditorFrameWithKeyboard({ImGuiKey_Escape}, /*characters=*/"", /*ctrl=*/false,
                                /*shift=*/false, /*alt=*/false, ImVec2(360.0f, 180.0f));

  EXPECT_FALSE(FunctionTooltipOpen());
}

TEST_F(TextEditorTests, HandleFunctionTooltipHonorsDisabledStateAndOpeningParen) {
  editor.setText("draw(");
  editor.setCursorPosition(Coordinates(0, 4));

  editor.setFunctionDeclarationTooltip(false);
  HandleFunctionTooltipDirect('(', editor.getCursorPosition());
  EXPECT_FALSE(FunctionTooltipOpen());

  editor.setFunctionDeclarationTooltip(true);
  HandleFunctionTooltipDirect('(', editor.getCursorPosition());
  EXPECT_TRUE(FunctionTooltipOpen());
  EXPECT_FALSE(FunctionTooltipDeclaration().empty());
}

TEST_F(TextEditorTests, HandleFunctionTooltipCommaFindsNearestOpenCall) {
  editor.setText("outer(inner(a, b), c)");
  editor.setCursorPosition(Coordinates(0, 16));
  editor.setFunctionDeclarationTooltip(true);

  HandleFunctionTooltipDirect(',', editor.getCursorPosition());

  EXPECT_TRUE(FunctionTooltipOpen());
  EXPECT_FALSE(FunctionTooltipDeclaration().empty());
}

TEST_F(TextEditorTests, HandleFunctionTooltipCommaIgnoresClosedNestedCall) {
  editor.setText("outer(inner(a), b)");
  editor.setCursorPosition(Coordinates(0, 15));
  editor.setFunctionDeclarationTooltip(true);

  HandleFunctionTooltipDirect(',', editor.getCursorPosition());

  EXPECT_TRUE(FunctionTooltipOpen());
  EXPECT_FALSE(FunctionTooltipDeclaration().empty());
}

// ============================================================================
// AUTOCOMPLETE NAVIGATION & SELECTION THROUGH THE RENDER PATH
// ============================================================================

TEST_F(TextEditorTests, AutocompleteDownArrowNavigatesThroughRenderPath) {
  editor.setText("ab");
  editor.setCursorPosition(Coordinates(0, 2));
  OpenAutocompleteWithSuggestions({"alpha", "beta"});

  ResetKeyboardState({ImGuiKey_DownArrow});
  RenderEditorFrameWithKeyboard({ImGuiKey_DownArrow});

  EXPECT_EQ(AutocompleteIndex(), 1) << "DownArrow should advance the autocomplete selection";
  EXPECT_TRUE(AutocompleteOpened()) << "Navigating should keep the popup open";
}

TEST_F(TextEditorTests, AutocompleteEnterInsertsSelectionThroughRenderPath) {
  editor.setText("al");
  editor.setCursorPosition(Coordinates(0, 2));
  OpenAutocompleteWithSuggestions({"pha"});
  SetAutocompleteSwitched(true);

  ResetKeyboardState({ImGuiKey_Enter});
  RenderEditorFrameWithKeyboard({ImGuiKey_Enter});

  EXPECT_EQ(editor.getText(), "alpha")
      << "Enter on an active autocomplete entry should insert the suggestion";
  EXPECT_FALSE(AutocompleteOpened()) << "Selecting a suggestion should close the popup";
}

TEST_F(TextEditorTests, AutocompletePopupClosesOnEscapeThroughRenderPath) {
  editor.setText("al");
  editor.setCursorPosition(Coordinates(0, 2));
  OpenAutocompleteWithSuggestions({"alpha"});

  RenderEditorFrameWithFont(ImVec2(320.0f, 180.0f));
  EXPECT_TRUE(AutocompleteOpened());

  ResetKeyboardState({ImGuiKey_Escape}, /*ctrl=*/false, /*shift=*/false, /*alt=*/false,
                     ImVec2(320.0f, 180.0f));
  RenderEditorFrameWithKeyboard({ImGuiKey_Escape}, /*characters=*/"", /*ctrl=*/false,
                                /*shift=*/false, /*alt=*/false, ImVec2(320.0f, 180.0f));

  EXPECT_FALSE(AutocompleteOpened());
}

TEST_F(TextEditorTests, KeyboardInputBuildsPendingAutocompleteThroughRenderPath) {
  editor.setText("ap");
  editor.setCursorPosition(Coordinates(0, 2));
  editor.setLanguageDefinition(TextEditor::LanguageDefinition{});
  editor.addAutocompleteEntry("apple", "apple entry", "apple()");
  RequestAutocompleteBuildOnNextKeyboardFrame();

  RenderEditorFrameWithKeyboard({});

  EXPECT_TRUE(AutocompleteOpened());
  EXPECT_EQ(AutocompleteSuggestionCount(), 1);
  EXPECT_EQ(AutocompleteSuggestion(0).first, RcString("apple entry"));
}

TEST_F(TextEditorTests, TypedLetterClosesAutocompleteAndFunctionTooltipThroughRenderPath) {
  editor.setText("");
  OpenAutocompleteWithSuggestions({"alpha"});
  OpenFunctionTooltipDirect("fn(value)", Coordinates(0, 0));

  RenderEditorFrameWithKeyboard({}, "a");

  EXPECT_FALSE(AutocompleteOpened());
  EXPECT_FALSE(FunctionTooltipOpen());
}

// ============================================================================
// MOUSE INTERACTION VARIANTS
// ============================================================================

TEST_F(TextEditorTests, DoubleClickSelectsWordThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/2);
  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);
  // Second press within the double-click window triggers word selection.
  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);

  EXPECT_EQ(editor.getSelectedText(), "Hello")
      << "A double click in the text area should select the word under the cursor";
}

TEST_F(TextEditorTests, TripleClickSelectsLineThroughRenderPath) {
  editor.setText("Hello world\nsecond line");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/2);
  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);
  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);
  RenderEditorFrameWithMouse(clickPos, false);
  RenderEditorFrameWithMouse(clickPos, true);

  EXPECT_NE(editor.getSelectedText().find("Hello world"), std::string::npos)
      << "A triple click should promote the mouse selection to line mode";
}

TEST_F(TextEditorTests, ClickingLineNumberMarginDoesNotMoveCursorThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 5));
  RenderEditorFrame();

  const ImVec2 marginPoint =
      ScreenPointAtUnwrappedVisualLine(/*visualLine=*/0, /*columnPixels=*/-TextStart() + 1.0f);
  RenderEditorFrameWithMouse(marginPoint, false);
  RenderEditorFrameWithMouse(marginPoint, true);

  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 5));
}

TEST_F(TextEditorTests, GutterHandleDragPublishesSourceMoveGestureWithoutEditingText) {
  editor.setText("one\ntwo\nthree");
  editor.resetTextChanged();
  RenderEditorFrame();

  const ImVec2 handle = SourceGutterHandlePoint(/*visualIndex=*/0);
  const ImVec2 target = ScreenPointAtVisualTextOffset(/*visualIndex=*/2,
                                                      /*visualColumnOffset=*/1);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/false);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/true);

  EXPECT_EQ(editor.takeSourceGutterDragStartedLine(), 0);
  ASSERT_TRUE(editor.sourceGutterDragTarget().has_value());
  EXPECT_EQ(editor.sourceGutterDragTarget()->line, 0);

  RenderEditorFrameWithMouse(target, /*mouseDown=*/true);
  ASSERT_TRUE(editor.sourceGutterDragTarget().has_value());
  EXPECT_EQ(editor.sourceGutterDragTarget()->line, 2);

  RenderEditorFrameWithMouse(target, /*mouseDown=*/false);
  const std::optional<Coordinates> drop = editor.takeSourceGutterDropTarget();
  ASSERT_TRUE(drop.has_value());
  EXPECT_EQ(drop->line, 2);
  EXPECT_EQ(editor.getText(), "one\ntwo\nthree");
  EXPECT_FALSE(editor.isTextChanged());
}

TEST_F(TextEditorTests, GutterHandleDropUsesCurrentReleasePosition) {
  editor.setText("one\ntwo\nthree");
  RenderEditorFrame();

  const ImVec2 handle = SourceGutterHandlePoint(/*visualIndex=*/0);
  const ImVec2 release = ScreenPointAtVisualTextOffset(/*visualIndex=*/2,
                                                       /*visualColumnOffset=*/1);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/false);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/true);
  ASSERT_EQ(editor.takeSourceGutterDragStartedLine(), 0);

  // Move and release without a preceding held frame at the destination.
  RenderEditorFrameWithMouse(release, /*mouseDown=*/false);

  const std::optional<Coordinates> drop = editor.takeSourceGutterDropTarget();
  ASSERT_TRUE(drop.has_value());
  EXPECT_EQ(drop->line, 2);
}

TEST_F(TextEditorTests, StructuralMoveDecorationClampsToSourceAndRenders) {
  editor.setText("<svg><rect/></svg>");

  EXPECT_TRUE(editor.setSourceStructuralMoveDecoration(TextEditor::SourceStructuralMoveDecoration{
      .elementRange = SourceByteRange{.start = 5, .end = 999},
      .insertionOffset = 999,
      .valid = true,
      .message = "Move before rect",
  }));
  ASSERT_TRUE(editor.sourceStructuralMoveDecoration().has_value());
  EXPECT_EQ(editor.sourceStructuralMoveDecoration()->elementRange,
            (SourceByteRange{.start = 5, .end = 18}));
  EXPECT_EQ(editor.sourceStructuralMoveDecoration()->insertionOffset, 18u);

  RenderEditorFrame();

  EXPECT_TRUE(editor.setSourceStructuralMoveDecoration(std::nullopt));
  EXPECT_FALSE(editor.sourceStructuralMoveDecoration().has_value());
}

TEST_F(TextEditorTests, CancellingGutterDragClearsPendingGestureState) {
  editor.setText("one\ntwo");
  RenderEditorFrame();

  const ImVec2 handle = SourceGutterHandlePoint(/*visualIndex=*/0);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/false);
  RenderEditorFrameWithMouse(handle, /*mouseDown=*/true);
  ASSERT_TRUE(editor.sourceGutterDragTarget().has_value());

  editor.cancelSourceGutterDrag();

  EXPECT_FALSE(editor.takeSourceGutterDragStartedLine().has_value());
  EXPECT_FALSE(editor.sourceGutterDragTarget().has_value());
  EXPECT_FALSE(editor.takeSourceGutterDropTarget().has_value());
  EXPECT_TRUE(editor.takeSourceGutterDragCancelled());
}

TEST_F(TextEditorTests, AltAndShiftHoverDoNotMoveCursorThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 textPoint =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/6);

  RenderEditorFrameWithMouse(textPoint, /*mouseDown=*/false, ImVec2(240.0f, 180.0f),
                             /*ctrl=*/false, /*shift=*/false, /*alt=*/true);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  EXPECT_FALSE(editor.didMouseChangeCursorPosition());

  RenderEditorFrameWithMouse(textPoint, /*mouseDown=*/false, ImVec2(240.0f, 180.0f),
                             /*ctrl=*/false, /*shift=*/true, /*alt=*/false);
  EXPECT_EQ(editor.getCursorPosition(), Coordinates(0, 0));
  EXPECT_FALSE(editor.didMouseChangeCursorPosition());
}

TEST_F(TextEditorTests, CtrlClickSelectsWordThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 clickPos =
      ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/7);
  RenderEditorFrameWithMouse(clickPos, /*mouseDown=*/false);
  RenderEditorFrameWithMouse(clickPos, /*mouseDown=*/true, ImVec2(240.0f, 180.0f),
                             /*ctrl=*/true);

  EXPECT_EQ(editor.getSelectedText(), "world");
  EXPECT_TRUE(editor.didMouseChangeCursorPosition());
}

TEST_F(TextEditorTests, MouseDragExtendsSelectionThroughRenderPath) {
  editor.setText("Hello world");
  editor.setCursorPosition(Coordinates(0, 0));
  RenderEditorFrame();

  const ImVec2 start = ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/0);
  const ImVec2 end = ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/5);

  RenderEditorFrameWithMouse(start, false);
  RenderEditorFrameWithMouse(start, true);
  RenderEditorFrameWithMouse(end, true);  // drag with button held

  EXPECT_TRUE(editor.hasSelection()) << "Dragging the mouse should create a selection";
}

TEST_F(TextEditorTests, MouseDragNearBottomAutoscrollsThroughRenderPath) {
  editor.setText("line0\nline1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\n");
  editor.setCursorPosition(Coordinates(0, 0));
  const ImVec2 editorSize(240.0f, 80.0f);
  RenderEditorFrame(editorSize);

  const ImVec2 start = ScreenPointAtVisualTextOffset(/*visualIndex=*/0, /*visualColumnOffset=*/0);
  const ImVec2 bottomEdge(start.x + 20.0f, start.y + 54.0f);

  RenderEditorFrameWithMouse(start, /*mouseDown=*/false, editorSize);
  RenderEditorFrameWithMouse(start, /*mouseDown=*/true, editorSize);
  RenderEditorFrameWithMouse(bottomEdge, /*mouseDown=*/true, editorSize);

  EXPECT_TRUE(editor.hasSelection());
}

}  // namespace donner::editor
