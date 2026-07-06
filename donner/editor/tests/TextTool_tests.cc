#include "donner/editor/TextTool.h"

#include <string>
#include <string_view>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/DocumentState.h"
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

  /// Plain click (press + release at the same point). Moves the caret on the
  /// session's text, opens an edit session on existing text, and creates
  /// nothing on empty canvas.
  void clickAt(const Vector2d& documentPoint) {
    tool.onMouseDown(app, documentPoint, MouseModifiers{});
    tool.onMouseUp(app, documentPoint);
  }

  /// Double-click (press + release at the same point) to open a point-text
  /// session on empty canvas.
  void doubleClickAt(const Vector2d& documentPoint) {
    MouseModifiers modifiers;
    modifiers.doubleClick = true;
    tool.onMouseDown(app, documentPoint, modifiers);
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

TEST_F(TextToolTest, DoubleClickOpensPointTextSession) {
  doubleClickAt(Vector2d(20.0, 30.0));

  EXPECT_TRUE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  svg::SVGTextElement inserted = text();
  EXPECT_THAT(attr(inserted, "x"), Eq("20"));
  EXPECT_THAT(attr(inserted, "y"), Eq("30"));
  EXPECT_THAT(attr(inserted, "font-family"), Eq("sans-serif"));
  EXPECT_THAT(attr(inserted, "font-size"), Eq("32"));
  // Fill lands in the style attribute (the channel the fill-color picker
  // edits), not as a presentation attribute.
  EXPECT_THAT(attr(inserted, "style"), Eq("fill: black"));
  EXPECT_THAT(attr(inserted, "fill"), Eq(""));
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

TEST_F(TextToolTest, PlainClickOnEmptyCanvasCreatesNothing) {
  clickAt(Vector2d(20.0, 30.0));

  EXPECT_FALSE(tool.isEditing());
  EXPECT_FALSE(hasTextElement());
}

TEST_F(TextToolTest, DoubleClickWithShortDragStillPlacesPointText) {
  MouseModifiers modifiers;
  modifiers.doubleClick = true;
  tool.onMouseDown(app, Vector2d(20.0, 30.0), modifiers);
  tool.onMouseMove(app, Vector2d(22.0, 31.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(22.0, 31.0));

  EXPECT_TRUE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  EXPECT_THAT(attr(text(), "data-donner-text-box-width"), Eq(""));
}

TEST_F(TextToolTest, TypingUpdatesTextContentAndCaret) {
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  EXPECT_EQ(text().textContent(), "Hi");
  EXPECT_EQ(tool.sessionContent(), U"Hi");
  EXPECT_EQ(tool.caretIndex(), 2u);
}

TEST_F(TextToolTest, CaretMovesWithinLine) {
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Ho");
  tool.moveCaret(app, TextTool::CaretMove::Left);
  type("ell");

  EXPECT_EQ(text().textContent(), "Hello");
  EXPECT_EQ(tool.caretIndex(), 4u);
}

TEST_F(TextToolTest, BackspaceDeletesBeforeCaret) {
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");
  tool.moveCaret(app, TextTool::CaretMove::LineStart);
  tool.deleteForward(app);

  EXPECT_EQ(text().textContent(), "i");
  EXPECT_EQ(tool.caretIndex(), 0u);
}

TEST_F(TextToolTest, NewlineCreatesOneTspanPerLine) {
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
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
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  EXPECT_TRUE(tool.commit(app));
  EXPECT_FALSE(tool.isEditing());
  ASSERT_TRUE(hasTextElement());
  EXPECT_EQ(text().textContent(), "Hi");

  // Committing again with no session is a no-op.
  EXPECT_FALSE(tool.commit(app));
}

TEST_F(TextToolTest, CommitUndoRemovesTheWholeSessionAtOnce) {
  doubleClickAt(Vector2d(20.0, 30.0));
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

  doubleClickAt(Vector2d(20.0, 30.0));
  ASSERT_TRUE(hasTextElement());

  EXPECT_TRUE(tool.commit(app));

  EXPECT_FALSE(hasTextElement());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), *rect);
}

TEST_F(TextToolTest, ClickAwayCommitsWithoutCreating) {
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  // A plain click far from the session's text commits it and creates nothing.
  clickAt(Vector2d(300.0, 300.0));

  EXPECT_FALSE(tool.isEditing());
  EXPECT_EQ(textElementCount(), 1);
  EXPECT_EQ(text().textContent(), "Hi");
}

TEST_F(TextToolTest, DoubleClickAwayCommitsAndStartsNewDraft) {
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  // A double-click far from the session's text commits it and starts a new
  // point-text draft.
  doubleClickAt(Vector2d(300.0, 300.0));

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

class TextToolExistingTextTest : public TextToolTest {
protected:
  static constexpr std::string_view kSvgWithText =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
         </svg>)";

  void SetUp() override { ASSERT_TRUE(app.loadFromString(kSvgWithText)); }

  /// A document point inside the glyph cell of character @p charIndex:
  /// the left or right quarter point of its extent, on the baseline side.
  Vector2d pointInChar(std::size_t charIndex, bool rightHalf) {
    svg::SVGTextElement element = text();
    const Box2d extent =
        element.withWriteAccess([&element, charIndex](svg::DocumentWriteAccess&, EntityHandle) {
          return element.getExtentOfChar(charIndex);
        });
    EXPECT_FALSE(extent.isEmpty());
    const double x = rightHalf ? extent.topLeft.x + extent.size().x * 0.75
                               : extent.topLeft.x + extent.size().x * 0.25;
    const double y = extent.topLeft.y + extent.size().y * 0.5;
    return Vector2d(x, y);
  }
};

TEST_F(TextToolExistingTextTest, ClickOnExistingTextOpensEditSessionWithCaretAtClick) {
  clickAt(pointInChar(1, /*rightHalf=*/false));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(textElementCount(), 1);
  EXPECT_EQ(tool.sessionContent(), U"Hello");
  EXPECT_EQ(tool.caretIndex(), 1u);

  type("X");
  EXPECT_EQ(text().textContent(), "HXello");
}

TEST_F(TextToolExistingTextTest, ClickToEditUsesScopedAccessUnderConcurrentDom) {
  // The live editor runs with ConcurrentDom threading, where every DOM read
  // (children, textContent, attributes, transforms) must happen inside a
  // scoped access — the session-open path SIGABRTs on
  // assertScopedEntityHandleAccessAllowed otherwise.
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  clickAt(pointInChar(1, /*rightHalf=*/false));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.sessionContent(), U"Hello");
  EXPECT_EQ(tool.caretIndex(), 1u);
}

TEST_F(TextToolExistingTextTest, ClickInTrailingHalfPlacesCaretAfterCharacter) {
  clickAt(pointInChar(1, /*rightHalf=*/true));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.caretIndex(), 2u);
}

TEST_F(TextToolExistingTextTest, EditingExistingTextCommitRecordsEditUndo) {
  clickAt(pointInChar(4, /*rightHalf=*/true));
  ASSERT_EQ(tool.caretIndex(), 5u);
  type("!");
  ASSERT_TRUE(tool.commit(app));
  app.flushFrame();

  EXPECT_EQ(text().textContent(), "Hello!");
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Edit text");

  app.undo();
  app.flushFrame();
  EXPECT_EQ(text().textContent(), "Hello");
}

TEST_F(TextToolExistingTextTest, ClickInAndAwayWithoutTypingRecordsNoUndo) {
  clickAt(pointInChar(0, /*rightHalf=*/false));
  ASSERT_TRUE(tool.isEditing());

  clickAt(Vector2d(300.0, 300.0));
  app.flushFrame();

  EXPECT_FALSE(tool.isEditing());
  EXPECT_EQ(text().textContent(), "Hello");
  EXPECT_FALSE(app.canUndo());
}

TEST_F(TextToolExistingTextTest, EmptyingExistingTextDeletesItUndoably) {
  clickAt(pointInChar(4, /*rightHalf=*/true));
  ASSERT_EQ(tool.caretIndex(), 5u);
  for (int i = 0; i < 5; ++i) {
    tool.backspace(app);
  }
  ASSERT_TRUE(tool.commit(app));
  app.flushFrame();

  EXPECT_FALSE(hasTextElement());
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Delete text");

  app.undo();
  app.flushFrame();
  ASSERT_TRUE(hasTextElement());
  EXPECT_EQ(text().textContent(), "Hello");
}

TEST_F(TextToolExistingTextTest, ExistingTspanLinesReconstructAsHardBreaks) {
  constexpr std::string_view kMultiLineSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif"
             ><tspan x="50">One</tspan><tspan x="50" dy="24">Two</tspan></text>
         </svg>)";
  ASSERT_TRUE(app.loadFromString(kMultiLineSvg));

  clickAt(pointInChar(0, /*rightHalf=*/false));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.sessionContent(), U"One\nTwo");
}

TEST_F(TextToolExistingTextTest, TransformedTextClickMapsThroughElementTransform) {
  constexpr std::string_view kTransformedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif"
                 transform="translate(100 40)">Hello</text>
         </svg>)svg";
  ASSERT_TRUE(app.loadFromString(kTransformedSvg));

  // pointInChar reads text-local extents; the click arrives in document
  // space, offset by the element transform.
  const Vector2d documentPoint = pointInChar(1, /*rightHalf=*/false) + Vector2d(100.0, 40.0);
  clickAt(documentPoint);

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.caretIndex(), 1u);

  // The caret chrome maps back into document space through the transform.
  const auto chrome = tool.editingChrome(app);
  ASSERT_TRUE(chrome.has_value());
  EXPECT_NEAR(chrome->caretBottomDoc.y, 80.0 + 40.0 + 20.0 * 0.25, 1.0);
  EXPECT_GT(chrome->caretTopDoc.x, 150.0);
}

TEST_F(TextToolExistingTextTest, BoxTextSoftWrapRoundTripsThroughReedit) {
  ASSERT_TRUE(app.loadFromString(kEmptySvg));

  // Author box text narrow enough to soft-wrap.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("MMMM MMMM");
  ASSERT_GE(tspanCount(), 2);
  ASSERT_TRUE(tool.commit(app));
  app.flushFrame();

  // Re-open the session by clicking the first glyph: soft-wrapped tspans
  // join back without hard breaks.
  clickAt(pointInChar(0, /*rightHalf=*/false));
  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.sessionContent(), U"MMMM MMMM");
}

TEST_F(TextToolTest, FrameHandleResizeReflowsBoxWithoutScalingGlyphs) {
  // Box text narrow enough to wrap: box (10,20)-(70,120).
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("MMMM MMMM");
  ASSERT_GE(tspanCount(), 2);

  // Grab the bottom-right frame handle and drag it out to (410,140): with
  // the text tool this resizes the BOX (reflow), never the glyphs.
  tool.onMouseDown(app, Vector2d(70.0, 120.0), MouseModifiers{});
  EXPECT_TRUE(tool.isAdjustingFrame());
  tool.onMouseMove(app, Vector2d(410.0, 140.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(410.0, 140.0));
  EXPECT_FALSE(tool.isAdjustingFrame());
  EXPECT_TRUE(tool.isEditing());

  svg::SVGTextElement element = text();
  EXPECT_THAT(attr(element, "data-donner-text-box-width"), Eq("400"));
  EXPECT_THAT(attr(element, "data-donner-text-box-height"), Eq("120"));
  EXPECT_THAT(attr(element, "x"), Eq("10"));
  EXPECT_THAT(attr(element, "y"), Eq("52"));
  // No glyph scaling: font-size and transform untouched.
  EXPECT_THAT(attr(element, "font-size"), Eq("32"));
  EXPECT_THAT(attr(element, "transform"), Eq(""));
  // The wider frame no longer wraps the content.
  EXPECT_EQ(tspanCount(), 0);
  EXPECT_EQ(element.textContent(), "MMMM MMMM");
}

TEST_F(TextToolTest, FrameResizeConvertsPointTextToUserSizedBox) {
  doubleClickAt(Vector2d(50.0, 80.0));
  type("Hello");
  ASSERT_THAT(attr(text(), "data-donner-text-box-width"), Eq(""));

  // Point text: the frame is the computed ink rect. Grab its bottom-right
  // corner and drag outward — the text becomes user-sized box text.
  svg::SVGTextElement element = text();
  const Box2d ink = element.withWriteAccess(
      [&element](svg::DocumentWriteAccess&, EntityHandle) { return element.inkBoundingBox(); });
  ASSERT_FALSE(ink.isEmpty());

  tool.onMouseDown(app, ink.bottomRight, MouseModifiers{});
  EXPECT_TRUE(tool.isAdjustingFrame());
  tool.onMouseMove(app, ink.bottomRight + Vector2d(60.0, 40.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, ink.bottomRight + Vector2d(60.0, 40.0));
  EXPECT_TRUE(tool.isEditing());

  const std::string widthAttr = attr(text(), "data-donner-text-box-width");
  const std::string heightAttr = attr(text(), "data-donner-text-box-height");
  ASSERT_FALSE(widthAttr.empty());
  ASSERT_FALSE(heightAttr.empty());
  EXPECT_NEAR(std::stod(widthAttr), ink.size().x + 60.0, 1e-6);
  EXPECT_NEAR(std::stod(heightAttr), ink.size().y + 40.0, 1e-6);
  EXPECT_EQ(text().textContent(), "Hello");
  EXPECT_THAT(attr(text(), "font-size"), Eq("32"));
  EXPECT_THAT(attr(text(), "transform"), Eq(""));
}

TEST_F(TextToolTest, RotateRingRotatesElementKeepingFrameAttributes) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("Hi");

  // Press in the rotate ring outside the bottom-right corner (distance
  // ~19.8px at zoom 1: outside the resize handle, inside the ring).
  tool.onMouseDown(app, Vector2d(84.0, 134.0), MouseModifiers{});
  EXPECT_TRUE(tool.isAdjustingFrame());
  // Sweep the pointer around the frame center to rotate.
  tool.onMouseMove(app, Vector2d(10.0, 134.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(10.0, 134.0));
  EXPECT_TRUE(tool.isEditing());

  svg::SVGTextElement element = text();
  // `setTransform` writes the transform component (the attribute string is
  // reflected by the shell's source writeback, not at this layer).
  const Transform2d transform =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        return element.cast<svg::SVGGraphicsElement>().transform();
      });
  EXPECT_FALSE(transform.isIdentity()) << "rotate ring must rotate the element";
  // Rotate never rewrites the frame: box attributes and font-size stay.
  EXPECT_THAT(attr(element, "data-donner-text-box-width"), Eq("60"));
  EXPECT_THAT(attr(element, "data-donner-text-box-height"), Eq("100"));
  EXPECT_THAT(attr(element, "font-size"), Eq("32"));
}

TEST_F(TextToolTest, FrameHandleIntentReportsResizeRotateAndNoneForHover) {
  // Box (10,20)-(70,120); the intent drives the shell's hover cursor.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("Hi");

  const auto intentAt = [&](const Vector2d& documentPoint) {
    return tool.frameHandleIntentAt(documentPoint, /*pixelsPerDocUnit=*/1.0,
                                    /*includeRotate=*/true);
  };

  // On the bottom-right corner handle.
  EXPECT_EQ(intentAt(Vector2d(70.0, 120.0)).kind, SelectionTransformHandleKind::Resize);
  EXPECT_EQ(intentAt(Vector2d(70.0, 120.0)).corner, SelectionTransformCorner::BottomRight);
  // In the rotate ring outside the corner.
  EXPECT_EQ(intentAt(Vector2d(84.0, 134.0)).kind, SelectionTransformHandleKind::Rotate);
  // Inside the frame, away from every handle.
  EXPECT_EQ(intentAt(Vector2d(40.0, 70.0)).kind, SelectionTransformHandleKind::None);
  // Far outside.
  EXPECT_EQ(intentAt(Vector2d(300.0, 300.0)).kind, SelectionTransformHandleKind::None);

  // No rotate intents when the ring is excluded (shift held).
  EXPECT_EQ(tool.frameHandleIntentAt(Vector2d(84.0, 134.0), 1.0, /*includeRotate=*/false).kind,
            SelectionTransformHandleKind::None);
}

TEST_F(TextToolTest, RotatingFrameExposesGestureForCursorLock) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("Hi");

  tool.onMouseDown(app, Vector2d(84.0, 134.0), MouseModifiers{});
  ASSERT_TRUE(tool.isAdjustingFrame());
  EXPECT_TRUE(tool.isRotatingFrame());
  EXPECT_EQ(tool.frameCorner(), SelectionTransformCorner::BottomRight);
  tool.onMouseUp(app, Vector2d(84.0, 134.0));
  EXPECT_FALSE(tool.isRotatingFrame());
}

TEST_F(TextToolTest, ClickInsideFrameOffGlyphsParksCaretAtEnd) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("Hi");
  tool.moveCaret(app, TextTool::CaretMove::LineStart);
  ASSERT_EQ(tool.caretIndex(), 0u);

  // Click in the empty lower half of the box: no glyph there, but the
  // session stays open with the caret parked at the end.
  clickAt(Vector2d(40.0, 100.0));

  EXPECT_TRUE(tool.isEditing());
  EXPECT_EQ(tool.caretIndex(), 2u);
  EXPECT_EQ(textElementCount(), 1);
}

TEST_F(TextToolTest, CancelResetsWithoutTouchingDocument) {
  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  tool.cancel();

  EXPECT_FALSE(tool.isEditing());
  // Cancel is a state reset only (tool teardown); the element stays.
  EXPECT_TRUE(hasTextElement());
}

}  // namespace
}  // namespace donner::editor
