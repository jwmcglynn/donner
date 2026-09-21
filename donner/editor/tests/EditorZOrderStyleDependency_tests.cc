/// @file
/// Arrange coverage for elements whose paint depends on the group they sit in: a stylesheet
/// selector scoped to the group's id, style inherited from the group, and a reference to the
/// group. Bring to Front / Send to Back may reach past a group only when nothing depends on it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

using ::testing::ElementsAre;
using ::testing::Optional;

using css::Color;
using css::RGBA;

svg::PaintServer SolidPaint(const Color& color) {
  return svg::PaintServer(svg::PaintServer::Solid(color));
}

void SelectById(EditorApp& app, std::string_view id) {
  const std::optional<svg::SVGElement> element =
      app.document().document().querySelector("#" + std::string(id));
  ASSERT_TRUE(element.has_value()) << "missing #" << id;
  app.setSelection(*element);
}

std::optional<svg::SVGElement> FindById(EditorApp& app, std::string_view id) {
  return app.document().document().querySelector("#" + std::string(id));
}

std::optional<svg::PaintServer> ComputedFill(EditorApp& app, std::string_view id) {
  const std::optional<svg::SVGElement> element = FindById(app, id);
  if (!element.has_value()) {
    return std::nullopt;
  }
  return element->getComputedStyle().fill.get();
}

void AppendOutline(const svg::SVGElement& element, int depth, std::vector<std::string>& out) {
  for (std::optional<svg::SVGElement> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::string entry(static_cast<std::size_t>(depth) * 2u, ' ');
    entry += std::string(child->tagName().name.str());
    const RcString id = child->id();
    if (!id.empty()) {
      entry += "#" + std::string(id.str());
    }
    out.push_back(std::move(entry));
    AppendOutline(*child, depth + 1, out);
  }
}

/// The document tree as `<tag>#<id>` entries in paint order, indented by depth, so a structural
/// failure prints the whole shape rather than one wrong parent.
std::vector<std::string> DocumentOutline(EditorApp& app) {
  std::vector<std::string> out;
  AppendOutline(app.document().document().svgElement(), 0, out);
  return out;
}

svg::RendererBitmap RenderApp(EditorApp& app) {
  svg::Renderer renderer;
  renderer.draw(app.document().document());
  return renderer.takeSnapshot();
}

svg::RendererBitmap RenderSource(std::string_view source) {
  EditorApp app;
  EXPECT_TRUE(app.loadFromString(std::string(source)));
  return RenderApp(app);
}

// `#layer rect` paints #r1 blue only while it is inside #layer; the presentation attribute
// underneath it is red. #c1 is not matched by the rule, so a reorder inside the group is visible.
constexpr std::string_view kScopedSelectorDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>#layer rect { fill: blue; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\" fill=\"red\"/>\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

/// The document after the only Bring to Front that leaves #r1 painting the same way: last in its
/// own group.
constexpr std::string_view kScopedSelectorDocumentFrontInParent =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>#layer rect { fill: blue; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\" fill=\"red\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, BringToFrontKeepsAStylesheetScopedChildInsideItsGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kScopedSelectorDocument)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("style", "g#layer", "  circle#c1", "  rect#r1", "rect#top"));
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kScopedSelectorDocumentFrontInParent));
  EXPECT_THAT(ComputedFill(app, "r1"), Optional(SolidPaint(Color(RGBA(0, 0, 0xFF, 0xFF)))))
      << "leaving #layer would drop the `#layer rect` match";
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kScopedSelectorDocumentFrontInParent),
                               "zorder_scoped_selector_bring_to_front",
                               tests::PixelmatchIdentityParams());
}

TEST(EditorZOrderStyleDependencyTest, SendToBackKeepsAStylesheetScopedChildInsideItsGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kScopedSelectorDocumentFrontInParent)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("style", "g#layer", "  rect#r1", "  circle#c1", "rect#top"));
  EXPECT_EQ(std::string(app.document().document().source()), std::string(kScopedSelectorDocument));
  EXPECT_THAT(ComputedFill(app, "r1"), Optional(SolidPaint(Color(RGBA(0, 0, 0xFF, 0xFF)))));
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kScopedSelectorDocument),
                               "zorder_scoped_selector_send_to_back",
                               tests::PixelmatchIdentityParams());
}

TEST(EditorZOrderStyleDependencyTest, UndoAndRedoRestoreTheRestrictedArrange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kScopedSelectorDocument)));
  const std::string sourceBefore(app.document().document().source());
  const svg::RendererBitmap bitmapBefore = RenderApp(app);

  SelectById(app, "r1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());
  const svg::RendererBitmap bitmapAfter = RenderApp(app);

  ASSERT_TRUE(app.canUndo());
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(std::string(app.document().document().source()), sourceBefore);
  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("style", "g#layer", "  rect#r1", "  circle#c1", "rect#top"));
  tests::CompareBitmapToBitmap(RenderApp(app), bitmapBefore, "zorder_scoped_selector_undo",
                               tests::PixelmatchIdentityParams());

  ASSERT_TRUE(app.canRedo());
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kScopedSelectorDocumentFrontInParent));
  EXPECT_THAT(ComputedFill(app, "r1"), Optional(SolidPaint(Color(RGBA(0, 0, 0xFF, 0xFF)))));
  tests::CompareBitmapToBitmap(RenderApp(app), bitmapAfter, "zorder_scoped_selector_redo",
                               tests::PixelmatchIdentityParams());
}

// #r1 has no fill of its own and inherits `fill: purple` from the group the stylesheet styles.
constexpr std::string_view kInheritedStyleDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>#layer { fill: purple; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\"/>\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

constexpr std::string_view kInheritedStyleDocumentFrontInParent =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>#layer { fill: purple; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, BringToFrontKeepsAChildInsideAStylesheetStyledGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kInheritedStyleDocument)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("style", "g#layer", "  circle#c1", "  rect#r1", "rect#top"));
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kInheritedStyleDocumentFrontInParent));
  EXPECT_THAT(ComputedFill(app, "r1"), Optional(SolidPaint(Color(RGBA(0x80, 0, 0x80, 0xFF)))))
      << "leaving #layer would drop the inherited fill";
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kInheritedStyleDocumentFrontInParent),
                               "zorder_inherited_style_bring_to_front",
                               tests::PixelmatchIdentityParams());
}

// A `<use>` instance paints whatever #layer holds, so moving a child out of #layer changes the
// instance as well as the original.
constexpr std::string_view kReferencedGroupDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"5\" y=\"5\" width=\"40\" height=\"40\" fill=\"red\"/>\n"
    "    <circle id=\"c1\" cx=\"35\" cy=\"35\" r=\"22\" fill=\"orange\"/>\n"
    "  </g>\n"
    "  <use id=\"copy\" href=\"#layer\" x=\"45\" y=\"45\"/>\n"
    "</svg>";

constexpr std::string_view kReferencedGroupDocumentFrontInParent =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <g id=\"layer\">\n"
    "    <circle id=\"c1\" cx=\"35\" cy=\"35\" r=\"22\" fill=\"orange\"/>\n"
    "    <rect id=\"r1\" x=\"5\" y=\"5\" width=\"40\" height=\"40\" fill=\"red\"/>\n"
    "  </g>\n"
    "  <use id=\"copy\" href=\"#layer\" x=\"45\" y=\"45\"/>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, BringToFrontKeepsAChildInsideAReferencedGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kReferencedGroupDocument)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app), ElementsAre("g#layer", "  circle#c1", "  rect#r1", "use#copy"));
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kReferencedGroupDocumentFrontInParent));
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kReferencedGroupDocumentFrontInParent),
                               "zorder_referenced_group_bring_to_front",
                               tests::PixelmatchIdentityParams());
}

// The <set> is a child of the group with no href, so it animates the group itself and the child
// inherits the animated value. Nothing in the document names #layer.
constexpr std::string_view kAnimatedGroupDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\"/>\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "    <set attributeName=\"fill\" to=\"blue\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

constexpr std::string_view kAnimatedGroupDocumentFrontInParent =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <g id=\"layer\">\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "    <set attributeName=\"fill\" to=\"blue\"/>\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"60\" y=\"60\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, BringToFrontKeepsAChildInsideAnAnimatedGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kAnimatedGroupDocument)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("g#layer", "  circle#c1", "  set", "  rect#r1", "rect#top"));
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kAnimatedGroupDocumentFrontInParent));
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kAnimatedGroupDocumentFrontInParent),
                               "zorder_animated_group_bring_to_front",
                               tests::PixelmatchIdentityParams());
}

// #r1 is already last in #layer, and the group may not be crossed, so the arrange has nothing
// left to do: no move, no source change, and no undo entry to step back through.
TEST(EditorZOrderStyleDependencyTest, BringToFrontIsANoOpForAnElementAlreadyLastInItsGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kScopedSelectorDocumentFrontInParent)));
  SelectById(app, "r1");

  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  EXPECT_FALSE(app.flushFrame()) << "no move was queued";

  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kScopedSelectorDocumentFrontInParent));
  EXPECT_FALSE(app.canUndo());
}

// The same in the other direction: #bottom paints before the group, so the pre-fix lift had
// somewhere to go, while a within-parent Send to Back does not.
constexpr std::string_view kScopedSelectorDocumentWithEarlierSibling =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>#layer rect { fill: blue; }</style>\n"
    "  <rect id=\"bottom\" x=\"0\" y=\"0\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\" fill=\"red\"/>\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "  </g>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, SendToBackIsANoOpForAnElementAlreadyFirstInItsGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kScopedSelectorDocumentWithEarlierSibling)));
  SelectById(app, "r1");

  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  EXPECT_FALSE(app.flushFrame()) << "no move was queued";

  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kScopedSelectorDocumentWithEarlierSibling));
  EXPECT_FALSE(app.canUndo());
}

// The stylesheet here matches on the element alone and never on the group, so the group is still
// free to be crossed and Bring to Front reaches the front of the whole document.
constexpr std::string_view kIndependentStylesheetDocument =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>rect { stroke: black; stroke-width: 4; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\" fill=\"red\"/>\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"40\" y=\"40\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "</svg>";

constexpr std::string_view kIndependentStylesheetDocumentLifted =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n"
    "  <style>rect { stroke: black; stroke-width: 4; }</style>\n"
    "  <g id=\"layer\">\n"
    "    <circle id=\"c1\" cx=\"45\" cy=\"45\" r=\"30\" fill=\"orange\"/>\n"
    "  </g>\n"
    "  <rect id=\"top\" x=\"40\" y=\"40\" width=\"35\" height=\"35\" fill=\"green\"/>\n"
    "  <rect id=\"r1\" x=\"10\" y=\"10\" width=\"50\" height=\"50\" fill=\"red\"/>\n"
    "</svg>";

TEST(EditorZOrderStyleDependencyTest, BringToFrontStillLiftsWhenNothingDependsOnTheGroup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kIndependentStylesheetDocument)));
  SelectById(app, "r1");

  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(DocumentOutline(app),
              ElementsAre("style", "g#layer", "  circle#c1", "rect#top", "rect#r1"));
  EXPECT_EQ(std::string(app.document().document().source()),
            std::string(kIndependentStylesheetDocumentLifted));
  tests::CompareBitmapToBitmap(RenderApp(app), RenderSource(kIndependentStylesheetDocumentLifted),
                               "zorder_independent_stylesheet_bring_to_front",
                               tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::editor
