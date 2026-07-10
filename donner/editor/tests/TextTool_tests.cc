#include "donner/editor/TextTool.h"

#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>

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
  EXPECT_THAT(attr(inserted, "style"), Eq("fill: white"));
  EXPECT_THAT(attr(inserted, "fill"), Eq(""));
  EXPECT_THAT(attr(inserted, "data-donner-text-box-width"), Eq(""));

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), inserted);
}

TEST_F(TextToolTest, NewTextKeepsCurrentForegroundFill) {
  app.setActiveFill("#31c6b3");

  doubleClickAt(Vector2d(20.0, 30.0));

  EXPECT_THAT(attr(text(), "style"), Eq("fill: #31c6b3"));
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
  ASSERT_TRUE(chrome->frameCornersDoc.has_value());
  const std::array<Vector2d, 4>& corners = *chrome->frameCornersDoc;
  EXPECT_EQ(corners[0], Vector2d(10.0, 20.0));    // Top-left.
  EXPECT_EQ(corners[1], Vector2d(210.0, 20.0));   // Top-right.
  EXPECT_EQ(corners[2], Vector2d(210.0, 120.0));  // Bottom-right.
  EXPECT_EQ(corners[3], Vector2d(10.0, 120.0));   // Bottom-left.

  // Typing a character advances the caret X by its measured width.
  type("M");
  const auto afterTyping = tool.editingChrome(app);
  ASSERT_TRUE(afterTyping.has_value());
  EXPECT_GT(afterTyping->caretTopDoc.x, chrome->caretTopDoc.x);
}

TEST_F(TextToolTest, PointTextFrameUsesStableFontExtentsAndFadesAfterTyping) {
  doubleClickAt(Vector2d(50.0, 80.0));

  // Empty point text has no frame. Typing creates font geometry, but the
  // frame remains hidden until pointer movement.
  ASSERT_TRUE(tool.editingChrome(app).has_value());
  EXPECT_FALSE(tool.editingChrome(app)->frameCornersDoc.has_value());
  type("x");
  const auto hidden = tool.editingChrome(app);
  ASSERT_TRUE(hidden.has_value());
  ASSERT_TRUE(hidden->frameCornersDoc.has_value());
  EXPECT_FLOAT_EQ(hidden->frameOpacity, 0.0f);

  tool.notifyPointerMoved(Vector2d(50.0, 80.0));
  EXPECT_FLOAT_EQ(tool.editingChrome(app)->frameOpacity, 0.0f);
  tool.notifyPointerMoved(Vector2d(52.0, 80.0));
  const auto revealed = tool.editingChrome(app);
  ASSERT_TRUE(revealed.has_value());
  ASSERT_TRUE(revealed->frameCornersDoc.has_value());
  EXPECT_FLOAT_EQ(revealed->frameOpacity, 1.0f);
  const double stableHeight = (*revealed->frameCornersDoc)[3].y - (*revealed->frameCornersDoc)[0].y;

  // A glyph with different ink extents must not change the em-box height.
  type("A");
  const auto fading = tool.editingChrome(app);
  ASSERT_TRUE(fading.has_value());
  ASSERT_TRUE(fading->frameCornersDoc.has_value());
  const double heightAfterTyping =
      (*fading->frameCornersDoc)[3].y - (*fading->frameCornersDoc)[0].y;
  EXPECT_NEAR(heightAfterTyping, stableHeight, 1e-6);
  EXPECT_TRUE(tool.nextPointFrameFadeWakeSeconds().has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(220));
  EXPECT_FLOAT_EQ(tool.editingChrome(app)->frameOpacity, 0.0f);
  EXPECT_FALSE(tool.nextPointFrameFadeWakeSeconds().has_value());
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
  // scoped access - the session-open path SIGABRTs on
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
  // Pointer moves update only the local frame preview. DOM rewrap and source
  // writeback are deferred until release so the drag path stays cheap.
  EXPECT_THAT(attr(text(), "data-donner-text-box-width"), Eq("60"));
  EXPECT_FALSE(app.document().hasPendingMutations());
  const auto resizePreview = tool.editingChrome(app);
  ASSERT_TRUE(resizePreview.has_value());
  ASSERT_TRUE(resizePreview->frameCornersDoc.has_value());
  EXPECT_EQ((*resizePreview->frameCornersDoc)[2], Vector2d(410.0, 140.0));
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

  // Point text: grab the stable font em-box frame's bottom-right corner and
  // drag outward. The text becomes user-sized box text.
  svg::SVGTextElement element = text();
  const auto chrome = tool.editingChrome(app);
  ASSERT_TRUE(chrome.has_value());
  ASSERT_TRUE(chrome->frameCornersDoc.has_value());
  const std::array<Vector2d, 4>& corners = *chrome->frameCornersDoc;
  const Box2d pointFrame(corners[0], corners[2]);

  tool.onMouseDown(app, pointFrame.bottomRight, MouseModifiers{});
  EXPECT_TRUE(tool.isAdjustingFrame());
  tool.onMouseMove(app, pointFrame.bottomRight + Vector2d(60.0, 40.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, pointFrame.bottomRight + Vector2d(60.0, 40.0));
  EXPECT_TRUE(tool.isEditing());

  const std::string widthAttr = attr(text(), "data-donner-text-box-width");
  const std::string heightAttr = attr(text(), "data-donner-text-box-height");
  ASSERT_FALSE(widthAttr.empty());
  ASSERT_FALSE(heightAttr.empty());
  EXPECT_NEAR(std::stod(widthAttr), pointFrame.size().x + 60.0, 1e-6);
  EXPECT_NEAR(std::stod(heightAttr), pointFrame.size().y + 40.0, 1e-6);
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

TEST_F(TextToolTest, RotatedFrameStaysOrientedAndResizesInLocalSpace) {
  // Box (10,20)-(70,120), center (40,70). Rotate ~45 degrees via the rotate
  // ring, then verify the hard invariant: the frame chrome and its handles
  // stay an oriented bounding box aligned to the text's rotation (never the
  // axis-aligned envelope), and a subsequent resize operates in the box's
  // local rotated space.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 120.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 120.0));
  ASSERT_TRUE(tool.isEditing());
  type("Hi");

  // Press in the rotate ring outside the bottom-right corner, then sweep the
  // pointer 45 degrees around the frame center.
  const Vector2d center(40.0, 70.0);
  const Vector2d ringStart(84.0, 134.0);
  tool.onMouseDown(app, ringStart, MouseModifiers{});
  ASSERT_TRUE(tool.isRotatingFrame());
  const Vector2d startDelta = ringStart - center;
  const double startAngle = std::atan2(startDelta.y, startDelta.x);
  const double radius = startDelta.length();
  const double angle = startAngle + MathConstants<double>::kPi / 4.0;
  const Vector2d ringEnd = center + Vector2d(std::cos(angle), std::sin(angle)) * radius;
  tool.onMouseMove(app, ringEnd, /*buttonHeld=*/true);
  tool.onMouseUp(app, ringEnd);
  ASSERT_TRUE(tool.isEditing());

  // The frame chrome is an oriented quad: side lengths match the authored
  // box (60 x 100) and the edges are NOT axis-aligned. It must never snap
  // back to the axis-aligned envelope after the gesture ends.
  const auto chrome = tool.editingChrome(app);
  ASSERT_TRUE(chrome.has_value());
  ASSERT_TRUE(chrome->frameCornersDoc.has_value());
  const std::array<Vector2d, 4>& corners = *chrome->frameCornersDoc;
  const Vector2d topEdge = corners[1] - corners[0];
  const Vector2d sideEdge = corners[3] - corners[0];
  EXPECT_NEAR(topEdge.length(), 60.0, 1e-6);
  EXPECT_NEAR(sideEdge.length(), 100.0, 1e-6);
  EXPECT_GT(std::abs(topEdge.x), 1.0) << "rotated top edge must not be axis-aligned";
  EXPECT_GT(std::abs(topEdge.y), 1.0) << "rotated top edge must not be axis-aligned";

  // Handle hit-testing tracks the ORIENTED corners: the rotated bottom-right
  // corner reports a resize handle with its local corner identity...
  const auto intentAt = [&](const Vector2d& documentPoint) {
    return tool.frameHandleIntentAt(documentPoint, /*pixelsPerDocUnit=*/1.0,
                                    /*includeRotate=*/true);
  };
  EXPECT_EQ(intentAt(corners[2]).kind, SelectionTransformHandleKind::Resize);
  EXPECT_EQ(intentAt(corners[2]).corner, SelectionTransformCorner::BottomRight);
  // ...while the axis-aligned envelope's corner (which is NOT a corner of
  // the rotated quad) reports nothing.
  Box2d envelope = Box2d::CreateEmpty(corners[0]);
  for (const Vector2d& corner : corners) {
    envelope.addPoint(corner);
  }
  EXPECT_EQ(intentAt(envelope.bottomRight).kind, SelectionTransformHandleKind::None);

  // A subsequent resize operates in the box's LOCAL rotated space: drag the
  // rotated bottom-right corner to the document position of local point
  // (80, 140) - the local frame grows by (10, 20) and the box attributes
  // reflect the local size, unaffected by the rotation.
  const Vector2d exDoc = topEdge / 60.0;    // Doc direction of one local x unit.
  const Vector2d eyDoc = sideEdge / 100.0;  // Doc direction of one local y unit.
  const Vector2d resizeTargetDoc = corners[0] + exDoc * (80.0 - 10.0) + eyDoc * (140.0 - 20.0);
  tool.onMouseDown(app, corners[2], MouseModifiers{});
  ASSERT_TRUE(tool.isAdjustingFrame());
  ASSERT_FALSE(tool.isRotatingFrame());
  tool.onMouseMove(app, resizeTargetDoc, /*buttonHeld=*/true);
  tool.onMouseUp(app, resizeTargetDoc);
  ASSERT_TRUE(tool.isEditing());

  svg::SVGTextElement element = text();
  EXPECT_NEAR(std::stod(attr(element, "data-donner-text-box-width")), 70.0, 0.01);
  EXPECT_NEAR(std::stod(attr(element, "data-donner-text-box-height")), 120.0, 0.01);
  // The anchored top-left corner stays put in local space.
  EXPECT_NEAR(std::stod(attr(element, "x")), 10.0, 0.01);
  EXPECT_NEAR(std::stod(attr(element, "y")), 52.0, 0.01);

  // And the frame is STILL oriented after the resize - resizing never
  // flattens the rotation back to the axis-aligned envelope.
  const auto afterResize = tool.editingChrome(app);
  ASSERT_TRUE(afterResize.has_value());
  ASSERT_TRUE(afterResize->frameCornersDoc.has_value());
  const Vector2d topEdgeAfter =
      (*afterResize->frameCornersDoc)[1] - (*afterResize->frameCornersDoc)[0];
  EXPECT_NEAR(topEdgeAfter.length(), 70.0, 1e-6);
  EXPECT_GT(std::abs(topEdgeAfter.x), 1.0);
  EXPECT_GT(std::abs(topEdgeAfter.y), 1.0);
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

TEST_F(TextToolTest, DragPreviewChromeShowsFrameBaselineAndIbeam) {
  // No drag: no preview.
  EXPECT_FALSE(tool.dragPreviewChrome().has_value());

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  // Before any movement there is no live rectangle yet.
  EXPECT_FALSE(tool.dragPreviewChrome().has_value());

  tool.onMouseMove(app, Vector2d(210.0, 120.0), /*buttonHeld=*/true);
  const auto preview = tool.dragPreviewChrome();
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->boxDoc.topLeft, Vector2d(10.0, 20.0));
  EXPECT_EQ(preview->boxDoc.bottomRight, Vector2d(210.0, 120.0));

  // The first baseline sits one default font size below the box top and is
  // inset from the frame edges.
  EXPECT_DOUBLE_EQ(preview->baselineStartDoc.y, 20.0 + TextTool::kDefaultFontSize);
  EXPECT_DOUBLE_EQ(preview->baselineEndDoc.y, preview->baselineStartDoc.y);
  EXPECT_GT(preview->baselineStartDoc.x, preview->boxDoc.topLeft.x);
  EXPECT_LT(preview->baselineEndDoc.x, preview->boxDoc.bottomRight.x);

  // The I-beam bar hangs on the baseline at the future caret position, inside
  // the box.
  EXPECT_DOUBLE_EQ(preview->ibeamTopDoc.x, preview->baselineStartDoc.x);
  EXPECT_DOUBLE_EQ(preview->ibeamBottomDoc.x, preview->ibeamTopDoc.x);
  EXPECT_LT(preview->ibeamTopDoc.y, preview->baselineStartDoc.y);
  EXPECT_GT(preview->ibeamBottomDoc.y, preview->baselineStartDoc.y);
  EXPECT_GE(preview->ibeamTopDoc.y, preview->boxDoc.topLeft.y);
  EXPECT_LE(preview->ibeamBottomDoc.y, preview->boxDoc.bottomRight.y);

  // A shallow drag clamps the baseline and I-beam into the box.
  tool.onMouseMove(app, Vector2d(210.0, 30.0), /*buttonHeld=*/true);
  const auto shallow = tool.dragPreviewChrome();
  ASSERT_TRUE(shallow.has_value());
  EXPECT_LE(shallow->baselineStartDoc.y, shallow->boxDoc.bottomRight.y);
  EXPECT_GE(shallow->ibeamTopDoc.y, shallow->boxDoc.topLeft.y);
  EXPECT_LE(shallow->ibeamBottomDoc.y, shallow->boxDoc.bottomRight.y);

  // Releasing (creating the session) clears the preview.
  tool.onMouseUp(app, Vector2d(210.0, 30.0));
  EXPECT_FALSE(tool.dragPreviewChrome().has_value());
}

TEST(TextToolCaretBlinkTest, VisibleDuringFirstHalfPeriodHiddenDuringSecond) {
  constexpr double kHalf = TextTool::kCaretBlinkHalfPeriodSeconds;

  // Phase start (and negative clock skew) is visible.
  EXPECT_TRUE(TextTool::CaretBlinkVisibleAtPhase(0.0));
  EXPECT_TRUE(TextTool::CaretBlinkVisibleAtPhase(-1.0));

  EXPECT_TRUE(TextTool::CaretBlinkVisibleAtPhase(kHalf * 0.5));
  EXPECT_FALSE(TextTool::CaretBlinkVisibleAtPhase(kHalf * 1.5));
  // The cycle repeats: visible again in the third half-period.
  EXPECT_TRUE(TextTool::CaretBlinkVisibleAtPhase(kHalf * 2.5));
  EXPECT_FALSE(TextTool::CaretBlinkVisibleAtPhase(kHalf * 3.5));
}

TEST(TextToolCaretBlinkTest, SecondsUntilFlipCountsDownWithinHalfPeriod) {
  constexpr double kHalf = TextTool::kCaretBlinkHalfPeriodSeconds;

  EXPECT_DOUBLE_EQ(TextTool::SecondsUntilCaretBlinkFlip(0.0), kHalf);
  EXPECT_DOUBLE_EQ(TextTool::SecondsUntilCaretBlinkFlip(kHalf * 0.25), kHalf * 0.75);
  // Just after a flip, nearly a full half-period remains.
  EXPECT_NEAR(TextTool::SecondsUntilCaretBlinkFlip(kHalf * 1.001), kHalf * 0.999, 1e-9);
  // The wake interval is always positive and never longer than a half-period,
  // and advancing by it always flips visibility.
  for (double t = 0.0; t < kHalf * 6.0; t += kHalf / 7.0) {
    const double untilFlip = TextTool::SecondsUntilCaretBlinkFlip(t);
    EXPECT_GT(untilFlip, 0.0) << "t=" << t;
    EXPECT_LE(untilFlip, kHalf) << "t=" << t;
    EXPECT_NE(TextTool::CaretBlinkVisibleAtPhase(t),
              TextTool::CaretBlinkVisibleAtPhase(t + untilFlip + 1e-9))
        << "t=" << t;
  }
}

TEST_F(TextToolTest, CaretBlinkVisibleImmediatelyAfterTypingAndSchedulesWake) {
  // No session: no blink wake, caret reported visible (nothing to hide).
  EXPECT_FALSE(tool.nextCaretBlinkWakeSeconds().has_value());
  EXPECT_TRUE(tool.caretBlinkVisible());

  doubleClickAt(Vector2d(20.0, 30.0));
  type("Hi");

  // Right after input the caret is in its visible phase, and a wake is
  // scheduled no further out than the half-period.
  EXPECT_TRUE(tool.caretBlinkVisible());
  const std::optional<float> wake = tool.nextCaretBlinkWakeSeconds();
  ASSERT_TRUE(wake.has_value());
  EXPECT_GT(*wake, 0.0f);
  EXPECT_LE(*wake, static_cast<float>(TextTool::kCaretBlinkHalfPeriodSeconds));

  tool.commit(app);
  EXPECT_FALSE(tool.nextCaretBlinkWakeSeconds().has_value());
}

}  // namespace
}  // namespace donner::editor
