#include "donner/editor/TextTool.h"

#include <string>
#include <string_view>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextElement.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using testing::Eq;
using testing::Optional;

constexpr std::string_view kEmptySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400"></svg>)";

constexpr std::string_view kSvgWithRect =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400"><rect id="r" x="0" y="0" width="10" height="10"/></svg>)";

class TextToolTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(app.loadFromString(kEmptySvg)); }

  /// Returns the single `<text>` element in the document.
  svg::SVGTextElement text() {
    auto element = app.document().document().querySelector("text");
    EXPECT_TRUE(element.has_value());
    return element->cast<svg::SVGTextElement>();
  }

  /// Whether the document currently contains any `<text>` element.
  bool hasTextElement() { return app.document().document().querySelector("text").has_value(); }

  /// Number of `<text>` elements directly under the root `<svg>`.
  int textElementCount() {
    int count = 0;
    svg::SVGElement root = app.document().document().svgElement();
    for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
         child = child->nextSibling()) {
      if (child->type() == svg::ElementType::Text) {
        ++count;
      }
    }
    return count;
  }

  /// Number of `<tspan>` children of the session's `<text>` element.
  int tspanCount() {
    int count = 0;
    svg::SVGTextElement element = text();
    for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
         child = child->nextSibling()) {
      if (child->type() == svg::ElementType::TSpan) {
        ++count;
      }
    }
    return count;
  }

  /// Returns the attribute value as a std::string, or empty if absent.
  static std::string attr(const svg::SVGElement& element, std::string_view name) {
    auto value = element.getAttribute(xml::XMLQualifiedNameRef(name));
    return value.has_value() ? std::string(std::string_view(*value)) : std::string();
  }

  /// Click (press + release at the same point) to open a point-text session.
  void clickAt(const Vector2d& documentPoint) {
    tool.onMouseDown(app, documentPoint, MouseModifiers{});
    tool.onMouseUp(app, documentPoint);
  }

  /// Type a string of ASCII characters into the active session.
  void type(std::string_view characters) {
    for (const char ch : characters) {
      tool.insertCodepoint(app, static_cast<char32_t>(ch));
    }
  }

  EditorApp app;
  TextTool tool;
};

TEST_F(TextToolTest, ClickOpensPointTextSession) {
  clickAt(Vector2d(20.0, 30.0));

  EXPECT_TRUE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  svg::SVGTextElement inserted = text();
  EXPECT_THAT(attr(inserted, "x"), Eq("20"));
  EXPECT_THAT(attr(inserted, "y"), Eq("30"));
  EXPECT_THAT(attr(inserted, "font-family"), Eq("sans-serif"));
  EXPECT_THAT(attr(inserted, "font-size"), Eq("32"));
  EXPECT_THAT(attr(inserted, "fill"), Eq("black"));
  EXPECT_THAT(attr(inserted, "data-donner-text-box-width"), Eq(""));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), inserted);
}

TEST_F(TextToolTest, DragOpensBoxTextSessionWithBoxAttributes) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(210.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(210.0, 120.0));

  EXPECT_TRUE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  svg::SVGTextElement inserted = text();
  // The origin is the box's top-left with the first baseline one font-size
  // below the top.
  EXPECT_THAT(attr(inserted, "x"), Eq("10"));
  EXPECT_THAT(attr(inserted, "y"), Eq("52"));
  EXPECT_THAT(attr(inserted, "data-donner-text-box-width"), Eq("200"));
  EXPECT_THAT(attr(inserted, "data-donner-text-box-height"), Eq("100"));
}

TEST_F(TextToolTest, ShortDragStillPlacesPointText) {
  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(22.0, 31.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(22.0, 31.0));

  EXPECT_TRUE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  EXPECT_THAT(attr(text(), "data-donner-text-box-width"), Eq(""));
}

TEST_F(TextToolTest, TypingUpdatesTextContentAndCaret) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  EXPECT_EQ(text().textContent(), "Hi");
  EXPECT_EQ(tool.sessionContent(), U"Hi");
  EXPECT_EQ(tool.caretIndex(), 2u);
}

TEST_F(TextToolTest, CaretMovesWithinLine) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  tool.moveCaret(app, TextTool::CaretMove::Left);
  EXPECT_EQ(tool.caretIndex(), 1u);
  tool.moveCaret(app, TextTool::CaretMove::LineStart);
  EXPECT_EQ(tool.caretIndex(), 0u);
  tool.moveCaret(app, TextTool::CaretMove::Left);
  EXPECT_EQ(tool.caretIndex(), 0u);
  tool.moveCaret(app, TextTool::CaretMove::LineEnd);
  EXPECT_EQ(tool.caretIndex(), 2u);
  tool.moveCaret(app, TextTool::CaretMove::Right);
  EXPECT_EQ(tool.caretIndex(), 2u);
}

TEST_F(TextToolTest, CaretMovesAcrossHardBreakLines) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");
  tool.insertNewline(app);
  type("There");
  ASSERT_EQ(tool.caretIndex(), 8u);

  tool.moveCaret(app, TextTool::CaretMove::Up);
  // Column 5 clamps to the first line's visible length (2).
  EXPECT_EQ(tool.caretIndex(), 2u);
  tool.moveCaret(app, TextTool::CaretMove::Down);
  EXPECT_EQ(tool.caretIndex(), 5u);
  tool.moveCaret(app, TextTool::CaretMove::LineEnd);
  EXPECT_EQ(tool.caretIndex(), 8u);
  tool.moveCaret(app, TextTool::CaretMove::LineStart);
  EXPECT_EQ(tool.caretIndex(), 3u);
}

TEST_F(TextToolTest, InsertAtCaretAfterMove) {
  clickAt(Vector2d(20.0, 30.0));
  type("Ho");
  tool.moveCaret(app, TextTool::CaretMove::Left);
  type("ell");

  EXPECT_EQ(text().textContent(), "Hello");
  EXPECT_EQ(tool.caretIndex(), 4u);
}

TEST_F(TextToolTest, BackspaceDeletesBeforeCaret) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");
  tool.backspace(app);

  EXPECT_EQ(text().textContent(), "H");
  EXPECT_EQ(tool.caretIndex(), 1u);

  tool.backspace(app);
  tool.backspace(app);  // No-op at the start of the content.
  EXPECT_EQ(text().textContent(), "");
  EXPECT_EQ(tool.caretIndex(), 0u);
}

TEST_F(TextToolTest, DeleteForwardDeletesAfterCaret) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");
  tool.moveCaret(app, TextTool::CaretMove::LineStart);
  tool.deleteForward(app);

  EXPECT_EQ(text().textContent(), "i");
  EXPECT_EQ(tool.caretIndex(), 0u);
}

TEST_F(TextToolTest, NewlineCreatesOneTspanPerLine) {
  clickAt(Vector2d(20.0, 30.0));
  type("A");
  tool.insertNewline(app);
  type("B");

  EXPECT_EQ(tool.sessionContent(), U"A\nB");
  EXPECT_EQ(tspanCount(), 2);

  svg::SVGTextElement element = text();
  std::optional<svg::SVGElement> first = element.firstChild();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->cast<svg::SVGTSpanElement>().textContent(), "A");
  EXPECT_THAT(attr(*first, "x"), Eq("20"));
  EXPECT_THAT(attr(*first, "dy"), Eq(""));
  std::optional<svg::SVGElement> second = first->nextSibling();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->cast<svg::SVGTSpanElement>().textContent(), "B");
  EXPECT_THAT(attr(*second, "x"), Eq("20"));
  // dy = font-size (32) * line-height factor (1.2).
  EXPECT_THAT(attr(*second, "dy"), Eq("38.4"));
}

TEST_F(TextToolTest, DeletingNewlineCollapsesBackToSingleTextNode) {
  clickAt(Vector2d(20.0, 30.0));
  type("A");
  tool.insertNewline(app);
  type("B");
  ASSERT_EQ(tspanCount(), 2);

  tool.moveCaret(app, TextTool::CaretMove::Left);
  tool.backspace(app);

  EXPECT_EQ(tool.sessionContent(), U"AB");
  EXPECT_EQ(tspanCount(), 0);
  EXPECT_EQ(text().textContent(), "AB");
}

TEST_F(TextToolTest, ToggleBoldItalicUnderlineSetAndRemoveAttributes) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  tool.toggleBold(app);
  EXPECT_THAT(attr(text(), "font-weight"), Eq("bold"));
  tool.toggleBold(app);
  EXPECT_THAT(attr(text(), "font-weight"), Eq(""));

  tool.toggleItalic(app);
  EXPECT_THAT(attr(text(), "font-style"), Eq("italic"));
  tool.toggleItalic(app);
  EXPECT_THAT(attr(text(), "font-style"), Eq(""));

  tool.toggleUnderline(app);
  EXPECT_THAT(attr(text(), "text-decoration"), Eq("underline"));
  tool.toggleUnderline(app);
  EXPECT_THAT(attr(text(), "text-decoration"), Eq(""));
}

TEST_F(TextToolTest, CommitKeepsTextAndEndsSession) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  EXPECT_TRUE(tool.commit(app));
  EXPECT_FALSE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  EXPECT_EQ(text().textContent(), "Hi");

  // Committing again with no session is a no-op.
  EXPECT_FALSE(tool.commit(app));
}

TEST_F(TextToolTest, CommitUndoRemovesTheWholeSessionAtOnce) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");
  tool.toggleBold(app);
  ASSERT_TRUE(tool.commit(app));
  // The tool flushes each keystroke, so this flush has no pending mutations;
  // it must still record the deferred session undo entry.
  app.flushFrame();

  ASSERT_TRUE(app.canUndo());
  app.undo();
  app.flushFrame();

  // The whole session (insert + typing + bold) is one undo step.
  EXPECT_FALSE(hasTextElement());
}

TEST_F(TextToolTest, EmptyCommitDeletesElementAndRestoresSelection) {
  ASSERT_TRUE(app.loadFromString(kSvgWithRect));
  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  clickAt(Vector2d(20.0, 30.0));
  ASSERT_TRUE(hasTextElement());

  EXPECT_TRUE(tool.commit(app));

  EXPECT_FALSE(hasTextElement());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), *rect);
}

TEST_F(TextToolTest, ClickAwayCommitsAndStartsNewDraft) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  // A click far from the session's text commits it and starts a new draft.
  clickAt(Vector2d(300.0, 300.0));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.sessionContent(), U"");
  EXPECT_EQ(textElementCount(), 2);
}

TEST_F(TextToolTest, EditingChromeTracksCaretAndBox) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(210.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(210.0, 120.0));

  const auto chrome = tool.editingChrome(app);
  ASSERT_TRUE(chrome.has_value());
  // The caret starts at the origin (box top-left, baseline one font-size
  // below the top), spanning the first line.
  EXPECT_DOUBLE_EQ(chrome->caretTopDoc.x, 10.0);
  EXPECT_DOUBLE_EQ(chrome->caretBottomDoc.x, 10.0);
  EXPECT_LT(chrome->caretTopDoc.y, chrome->caretBottomDoc.y);
  ASSERT_TRUE(chrome->boxDoc.has_value());
  EXPECT_DOUBLE_EQ(chrome->boxDoc->topLeft.x, 10.0);
  EXPECT_DOUBLE_EQ(chrome->boxDoc->topLeft.y, 20.0);
  EXPECT_DOUBLE_EQ(chrome->boxDoc->bottomRight.x, 210.0);
  EXPECT_DOUBLE_EQ(chrome->boxDoc->bottomRight.y, 120.0);

  // Typing a character advances the caret X by its measured width.
  type("M");
  const auto afterTyping = tool.editingChrome(app);
  ASSERT_TRUE(afterTyping.has_value());
  EXPECT_GT(afterTyping->caretTopDoc.x, chrome->caretTopDoc.x);
}

TEST_F(TextToolTest, BoxTextWrapsToBoxWidth) {
  // A narrow box: a handful of 32px characters exceeds 60 doc units.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());

  type("MMMM MMMM");

  // The measured line exceeds the 60-unit box, so the content wraps at the
  // word boundary into one <tspan> per display line.
  EXPECT_GE(tspanCount(), 2);
  EXPECT_EQ(tool.sessionContent(), U"MMMM MMMM");
}

TEST_F(TextToolTest, CancelResetsWithoutTouchingDocument) {
  clickAt(Vector2d(20.0, 30.0));
  type("Hi");

  tool.cancel();

  EXPECT_FALSE(tool.isEditing());
  // Cancel is a state reset only (tool teardown); the element stays.
  EXPECT_TRUE(hasTextElement());
}

}  // namespace
}  // namespace donner::editor
