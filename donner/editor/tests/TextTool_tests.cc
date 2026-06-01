#include "donner/editor/TextTool.h"

#include <string>
#include <string_view>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGTextElement.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kEmptySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)";

constexpr std::string_view kSvgWithGroup =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="grp"></g></svg>)";

constexpr std::string_view kSvgWithRect =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="r" x="0" y="0" width="10" height="10"/></svg>)";

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

  /// Returns the attribute value as a std::string, or empty if absent.
  static std::string attr(const svg::SVGElement& element, std::string_view name) {
    auto value = element.getAttribute(xml::XMLQualifiedNameRef(name));
    return value.has_value() ? std::string(std::string_view(*value)) : std::string();
  }

  EditorApp app;
  TextTool tool;
};

TEST_F(TextToolTest, InsertCreatesTextInsideRootSvg) {
  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  ASSERT_TRUE(hasTextElement());
  svg::SVGTextElement inserted = text();
  EXPECT_EQ(inserted.textContent(), "Text");
  EXPECT_EQ(attr(inserted, "x"), "20");
  EXPECT_EQ(attr(inserted, "y"), "30");
  EXPECT_EQ(attr(inserted, "font-family"), "sans-serif");
  EXPECT_EQ(attr(inserted, "font-size"), "32");
  EXPECT_EQ(attr(inserted, "fill"), "black");

  // Source sync: the new <text> lands inside the root <svg>.
  const std::string source(app.document().document().source());
  const std::size_t textOffset = source.find("<text");
  const std::size_t svgCloseOffset = source.find("</svg>");
  ASSERT_NE(textOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(textOffset, svgCloseOffset);
}

TEST_F(TextToolTest, InsertSelectsNewText) {
  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), text());
  ASSERT_TRUE(tool.insertedTextElement().has_value());
  EXPECT_EQ(*tool.insertedTextElement(), svg::SVGElement(text()));
}

TEST_F(TextToolTest, UndoRemovesAndRestoresSelection) {
  // Pre-select a rect so we can verify the selection is restored on undo.
  ASSERT_TRUE(app.loadFromString(kSvgWithRect));
  auto rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(hasTextElement());

  tool.undoInsert(app);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_FALSE(hasTextElement());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), *rect);
}

TEST_F(TextToolTest, RedoReinsertsAndSelects) {
  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  tool.undoInsert(app);
  ASSERT_TRUE(app.flushFrame());
  ASSERT_FALSE(hasTextElement());

  tool.redoInsert(app);
  ASSERT_TRUE(app.flushFrame());

  ASSERT_TRUE(hasTextElement());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), text());
}

TEST_F(TextToolTest, SetTextContentUpdatesNode) {
  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  svg::SVGTextElement inserted = text();
  ASSERT_EQ(inserted.textContent(), "Text");

  app.applyMutation(EditorCommand::SetTextContentCommand(inserted, "SVG"));
  ASSERT_TRUE(app.flushFrame());

  // The DOM text node reflects the new content immediately; re-querying the
  // same element after the flush returns the updated value.
  EXPECT_EQ(text().textContent(), "SVG");
  EXPECT_EQ(inserted.textContent(), "SVG");
}

TEST_F(TextToolTest, InsertIntoSelectedGroup) {
  ASSERT_TRUE(app.loadFromString(kSvgWithGroup));
  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());
  app.setSelection(*group);

  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  ASSERT_TRUE(hasTextElement());
  svg::SVGTextElement inserted = text();
  auto parent = inserted.parentElement();
  ASSERT_TRUE(parent.has_value());
  EXPECT_EQ(*parent, *group);
}

TEST_F(TextToolTest, InsertRespectsPaintOrder) {
  ASSERT_TRUE(app.loadFromString(kSvgWithRect));

  tool.onMouseDown(app, Vector2d(20.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  // The new <text> is appended after the existing <rect>, so it paints on top.
  ASSERT_TRUE(hasTextElement());
  svg::SVGElement root = app.document().document().svgElement();
  auto lastChild = root.lastChild();
  ASSERT_TRUE(lastChild.has_value());
  EXPECT_EQ(lastChild->type(), svg::ElementType::Text);

  const std::string source(app.document().document().source());
  const std::size_t rectOffset = source.find("<rect");
  const std::size_t textOffset = source.find("<text");
  ASSERT_NE(rectOffset, std::string::npos);
  ASSERT_NE(textOffset, std::string::npos);
  EXPECT_LT(rectOffset, textOffset);
}

}  // namespace
}  // namespace donner::editor
