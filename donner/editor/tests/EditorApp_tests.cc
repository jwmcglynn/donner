#include "donner/editor/EditorApp.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/ViewportGeometry.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

bool HasCommand(const Path& path, Path::Verb verb) {
  for (const Path::Command& command : path.commands()) {
    if (command.verb == verb) {
      return true;
    }
  }
  return false;
}

void ExpectBoxInside(const Box2d& inner, const Box2d& outer, double tolerance) {
  EXPECT_GE(inner.topLeft.x, outer.topLeft.x - tolerance) << inner << " not inside " << outer;
  EXPECT_GE(inner.topLeft.y, outer.topLeft.y - tolerance) << inner << " not inside " << outer;
  EXPECT_LE(inner.bottomRight.x, outer.bottomRight.x + tolerance)
      << inner << " not inside " << outer;
  EXPECT_LE(inner.bottomRight.y, outer.bottomRight.y + tolerance)
      << inner << " not inside " << outer;
}

std::optional<std::string> ReadDonnerSplash() {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    return std::nullopt;
  }

  std::ostringstream buffer;
  buffer << splashStream.rdbuf();
  return buffer.str();
}

bool GeometryContainsDocumentPoint(const svg::SVGElement& element, const Vector2d& point) {
  if (!element.isa<svg::SVGGeometryElement>()) {
    return false;
  }

  const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
  const std::optional<Path> spline = geometry.computedSpline();
  if (!spline.has_value()) {
    return false;
  }

  const Transform2d documentFromElement = geometry.elementFromWorld();
  const Transform2d elementFromDocument = documentFromElement.inverse();
  const Vector2d elementPoint = elementFromDocument.transformPosition(point);
  return spline->isInside(elementPoint, geometry.getComputedStyle().fillRule.get().value());
}

std::vector<svg::SVGPathElement> RootPathChildren(EditorApp& app) {
  std::vector<svg::SVGPathElement> paths;
  for (std::optional<svg::SVGElement> child = app.document().document().svgElement().firstChild();
       child.has_value(); child = child->nextSibling()) {
    if (child->isa<svg::SVGPathElement>()) {
      paths.push_back(child->cast<svg::SVGPathElement>());
    }
  }
  return paths;
}

std::vector<std::string> PathData(const std::vector<svg::SVGPathElement>& paths) {
  std::vector<std::string> data;
  data.reserve(paths.size());
  for (const svg::SVGPathElement& path : paths) {
    data.emplace_back(path.d());
  }
  return data;
}

template <typename Elements>
std::vector<std::string> ElementIds(const Elements& elements) {
  std::vector<std::string> ids;
  ids.reserve(elements.size());
  for (const auto& element : elements) {
    ids.emplace_back(element.id().str());
  }
  return ids;
}

template <typename Elements>
std::vector<std::optional<std::string>> AttributeValues(const Elements& elements,
                                                        std::string_view name) {
  std::vector<std::optional<std::string>> values;
  values.reserve(elements.size());
  for (const auto& element : elements) {
    if (const std::optional<RcString> value = element.getAttribute(name)) {
      values.emplace_back(value->str());
    } else {
      values.emplace_back(std::nullopt);
    }
  }
  return values;
}

std::vector<std::string> SelectedPathData(EditorApp& app) {
  std::vector<std::string> data;
  data.reserve(app.selectedElements().size());
  for (const svg::SVGElement& element : app.selectedElements()) {
    if (element.isa<svg::SVGPathElement>()) {
      data.emplace_back(element.cast<svg::SVGPathElement>().d());
    } else {
      data.emplace_back("<non-path:" + std::string(element.id().str()) + ">");
    }
  }
  return data;
}

std::optional<Vector2d> FindMembershipSample(const svg::SVGElement& first,
                                             const svg::SVGElement& second,
                                             const Box2d& searchBounds, bool expectedFirstInside,
                                             bool expectedSecondInside) {
  const auto matches = [&](const Vector2d& point) {
    return GeometryContainsDocumentPoint(first, point) == expectedFirstInside &&
           GeometryContainsDocumentPoint(second, point) == expectedSecondInside;
  };

  constexpr double kSampleStep = 2.0;
  constexpr std::array<Vector2d, 5> kInteriorProbeOffsets = {
      Vector2d(0.0, 0.0),  Vector2d(0.75, 0.0),  Vector2d(-0.75, 0.0),
      Vector2d(0.0, 0.75), Vector2d(0.0, -0.75),
  };
  for (double y = searchBounds.topLeft.y; y <= searchBounds.bottomRight.y; y += kSampleStep) {
    for (double x = searchBounds.topLeft.x; x <= searchBounds.bottomRight.x; x += kSampleStep) {
      const Vector2d point(x, y);
      bool stable = true;
      for (const Vector2d& offset : kInteriorProbeOffsets) {
        if (!matches(point + offset)) {
          stable = false;
          break;
        }
      }
      if (stable) {
        return point;
      }
    }
  }
  return std::nullopt;
}

TEST(EditorAppTest, EmptyByDefault) {
  EditorApp app;
  EXPECT_FALSE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.selectedElement().has_value());
  EXPECT_TRUE(app.structuredEditingEnabled());
}

TEST(EditorAppTest, StructuredEditingKillSwitchCanBeDisabled) {
  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());

  app.setStructuredEditingEnabled(false);
  EXPECT_FALSE(app.structuredEditingEnabled());

  app.setStructuredEditingEnabled(true);
  EXPECT_TRUE(app.structuredEditingEnabled());
}

TEST(EditorAppTest, LoadFromStringPopulatesDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_TRUE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());

  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#r2").has_value());
}

TEST(EditorAppTest, LoadFromStringClearsExistingSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);
  EXPECT_TRUE(app.hasSelection());

  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_FALSE(app.hasSelection());
}

TEST(EditorAppTest, HitTestReturnsTopElementAtPoint) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // Inside r1 (10,10 → 30,30).
  auto inR1 = app.hitTest(Vector2d(15.0, 15.0));
  ASSERT_TRUE(inR1.has_value());
  EXPECT_EQ(inR1->id(), "r1");

  // Inside r2 (50,50 → 70,70).
  auto inR2 = app.hitTest(Vector2d(60.0, 60.0));
  ASSERT_TRUE(inR2.has_value());
  EXPECT_EQ(inR2->id(), "r2");

  // Empty space outside both rects.
  auto miss = app.hitTest(Vector2d(80.0, 80.0));
  EXPECT_FALSE(miss.has_value());
}

// Clicking on rendered text selects it: the pointer contract for text is a
// hit anywhere inside the laid-out glyph ink bounds (including the gaps
// between letters), like any design tool's selection pointer.
TEST(EditorAppTest, HitTestPicksTextByGlyphInkBounds) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <text id="label" x="20" y="120" font-size="64" fill="#0033aa">SVG</text>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTextSvg));

  // A point on the first glyph's ink (the "S" around x=40, baseline y=120).
  auto onGlyph = app.hitTest(Vector2d(40.0, 100.0));
  ASSERT_TRUE(onGlyph.has_value()) << "clicking on rendered text must hit the <text> element";
  EXPECT_EQ(onGlyph->id(), "label");

  // A point between glyphs but inside the text's ink bounds still hits.
  auto betweenGlyphs = app.hitTest(Vector2d(80.0, 100.0));
  ASSERT_TRUE(betweenGlyphs.has_value());
  EXPECT_EQ(betweenGlyphs->id(), "label");

  // Far from the text: no hit.
  EXPECT_FALSE(app.hitTest(Vector2d(20.0, 180.0)).has_value());
}

// A marquee over rendered text selects it, mirroring the click contract.
TEST(EditorAppTest, MarqueePicksTextByGlyphInkBounds) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <text id="label" x="20" y="120" font-size="64" fill="#0033aa">SVG</text>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTextSvg));

  const auto hits = app.hitTestRect(Box2d(Vector2d(10.0, 40.0), Vector2d(190.0, 140.0)));
  ASSERT_EQ(hits.size(), 1u) << "marquee over text must select the <text> element";
  EXPECT_EQ(hits.front().id(), "label");

  EXPECT_TRUE(app.hitTestRect(Box2d(Vector2d(0.0, 150.0), Vector2d(20.0, 190.0))).empty());
}

TEST(EditorAppTest, HitTestReturnsNulloptWhenNoDocument) {
  EditorApp app;
  EXPECT_FALSE(app.hitTest(Vector2d(0.0, 0.0)).has_value());
}

TEST(EditorAppTest, MarqueeHitTestRectUnderConcurrentDomHoldsAccess) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // After the first async render the editor leaves the live document in
  // ConcurrentDom for its lifetime (#615). Marquee selection runs on the UI
  // thread through `hitTestRect`, whose DOM walk (isa / worldBounds /
  // firstChild / nextSibling) hits guarded `ElementAnchor` accessors. Without a
  // scoped access guard around the walk those reads trip the ConcurrentDom
  // release assertion - the marquee path flagged in #619 review.
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  // Marquee covering r1 (10,10 → 30,30) but not r2 (50,50 → 70,70).
  const std::vector<svg::SVGGraphicsElement> hits =
      app.hitTestRect(Box2d(Vector2d(0.0, 0.0), Vector2d(40.0, 40.0)));

  ASSERT_EQ(hits.size(), 1u);
  app.document().document().withReadAccess(
      [&](svg::DocumentReadAccess&) { EXPECT_EQ(hits.front().id(), "r1"); });
}

TEST(EditorAppTest, ApplyMutationFlowsThroughDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  app.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(99.0, 0.0))));

  EXPECT_EQ(app.document().queue().size(), 1u);
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, SelectionSetAndClear) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.setSelection(*r1);
  EXPECT_TRUE(app.hasSelection());
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);

  app.setSelection(std::nullopt);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
}

// Multi-select API (Milestone 4 of the editor UX design doc): a
// vector-shaped backing store for shift+click and marquee
// selections, with a single-element compatibility shim for back-compat
// callers like the source-pane highlight and the inspector readout.
TEST(EditorAppTest, MultiSelectionStoresEveryElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  EXPECT_TRUE(app.hasSelection());
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r1", "r2"));
  // Single-element compat: returns the *first* element.
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);

  // clearSelection reads as the natural opposite of "set N elements".
  app.clearSelection();
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
  EXPECT_FALSE(app.selectedElement().has_value());
}

TEST(EditorAppTest, SelectableElementsReturnsEveryGeometryElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  std::optional<svg::SVGElement> r1 = app.document().document().querySelector("#r1");
  std::optional<svg::SVGElement> r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  // "Select All" returns both rects regardless of position, in document order - the same selectable
  // set marquee selection uses.
  const std::vector<svg::SVGElement> selectable = app.selectableElements();
  ASSERT_EQ(selectable.size(), 2u);
  EXPECT_EQ(selectable.at(0), *r1);
  EXPECT_EQ(selectable.at(1), *r2);
}

TEST(EditorAppTest, SelectableElementsExcludesNonGeometryNodes) {
  EditorApp app;
  // <defs> and its gradient never render as geometry, so they are not selectable; only the rect is.
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <defs><linearGradient id="grad"/></defs>
           <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         </svg>)"));

  std::optional<svg::SVGElement> r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  const std::vector<svg::SVGElement> selectable = app.selectableElements();
  ASSERT_EQ(selectable.size(), 1u);
  EXPECT_EQ(selectable.at(0), *r1);
}

TEST(EditorAppTest, SelectableElementsIsEmptyWithoutDocument) {
  EditorApp app;
  EXPECT_TRUE(app.selectableElements().empty());
}

TEST(EditorAppTest, ToggleInSelectionAddsThenRemoves) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.toggleInSelection(*r1);
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r1"));

  app.toggleInSelection(*r2);
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r1", "r2"));

  // Toggling an already-selected element removes it without
  // disturbing the other entries.
  app.toggleInSelection(*r1);
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r2"));

  // Re-toggling brings it back.
  app.toggleInSelection(*r1);
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r2", "r1"));
}

TEST(EditorAppTest, AddToSelectionIsIdempotent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.addToSelection(*r1);
  app.addToSelection(*r1);
  app.addToSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
}

TEST(EditorAppTest, SetAttributeOnSelectionQueuesEverySelectedElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.setAttributeOnSelection("fill", "#112233"));
  EXPECT_EQ(app.document().queue().size(), 2u);
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  auto updatedR2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(updatedR1.has_value());
  ASSERT_TRUE(updatedR2.has_value());
  EXPECT_EQ(updatedR1->getAttribute("fill"), "#112233");
  EXPECT_EQ(updatedR2->getAttribute("fill"), "#112233");
}

TEST(EditorAppTest, EmptySelectionMutatorsReturnFalseWithoutQueueingCommands) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  EXPECT_FALSE(app.setAttributeOnSelection("fill", "#112233"));
  EXPECT_FALSE(app.setStylePropertyOnSelection("fill", "#112233"));
  EXPECT_FALSE(app.setStrokeWidthOnSelection(5.0));
  EXPECT_FALSE(app.deleteSelectionWithUndo(app.document().document().source()));
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, DeleteSelectionWithUndoKeepsLockedElementsSelected) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setElementLocked(*r1, true);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  const std::string sourceBefore(app.document().document().source());
  EXPECT_TRUE(app.deleteSelectionWithUndo(sourceBefore));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r2").has_value());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().id(), "r1");
  ASSERT_EQ(app.undoTimeline().entryCount(), 1u);
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Delete element");
}

TEST(EditorAppTest, DeleteSelectionWithUndoRefusesAllLockedSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setElementLocked(*r1, true);
  ASSERT_TRUE(app.flushFrame());

  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);
  EXPECT_FALSE(app.deleteSelectionWithUndo(app.document().document().source()));
  EXPECT_FALSE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().id(), "r1");
}

TEST(EditorAppTest, VisibilityAndLockTogglesBypassLockGate) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setElementLocked(*r1, true);
  ASSERT_TRUE(app.flushFrame());

  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute(kLockedAttributeName), kLockedAttributeValue);

  app.setElementVisible(*r1, false);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("display"), "none");

  app.setElementVisible(*r1, true);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("display"), "inline");

  app.setElementLocked(*r1, false);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_FALSE(r1->getAttribute(kLockedAttributeName).has_value());
}

// Hiding an element with an authored non-`none` `display` value (e.g.
// `display="block"`) must not clobber it: showing again should restore the
// author's exact value rather than forcing `display="inline"`.
TEST(EditorAppTest, SetElementVisibleRestoresAuthorDisplayValueOnShow) {
  constexpr std::string_view kSvgWithBlockDisplay =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <rect id="r1" x="10" y="10" width="20" height="20" fill="red" display="block"/>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvgWithBlockDisplay));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  ASSERT_EQ(r1->getAttribute("display"), "block");

  app.setElementVisible(*r1, false);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("display"), "none");

  app.setElementVisible(*r1, true);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("display"), "block")
      << "Show should restore the author's display=block instead of forcing display=inline";
}

// When the element had no author `display` value to begin with (the common
// case), Show still falls back to writing a definitively-visible
// `display="inline"` - covered already by
// `VisibilityAndLockTogglesBypassLockGate` above. This test covers the other
// fallback: showing an element that was never hidden through this toggle
// (no captured entry at all, e.g. a fresh element or a second, redundant Show
// call) still lands on `display="inline"` rather than crashing or no-oping.
TEST(EditorAppTest, SetElementVisibleShowWithoutPriorHideWritesInline) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  ASSERT_FALSE(r1->getAttribute("display").has_value());

  app.setElementVisible(*r1, true);
  ASSERT_TRUE(app.flushFrame());
  r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->getAttribute("display"), "inline");
}

TEST(EditorAppTest, SetStylePropertyOnSelectionMergesIntoStyleAttribute) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"
               style="stroke: blue; opacity: 0.5"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#112233"));
  EXPECT_EQ(app.document().queue().size(), 1u);
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("fill"), "red");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: blue; opacity: 0.5; fill: #112233");
}

TEST(EditorAppTest, SetStylePropertyOnSelectionOverridesExistingStyleProperty) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"
               style="fill: blue; stroke: black"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#112233"));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; fill: #112233");
}

TEST(EditorAppTest, SetStylePropertyOnSelectionSkipsUnparseableDeclarations) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  EXPECT_FALSE(app.setStylePropertyOnSelection("", "#112233"));
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, SetStrokeWidthOnSelectionClampsNegativeValues) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" stroke-width="4"
               style="stroke: black; opacity: 0.5"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStrokeWidthOnSelection(-4.0));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("stroke-width"), "4");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; opacity: 0.5; stroke-width: 0");
}

TEST(EditorAppTest, SetStrokeWidthOnSelectionOverridesExistingStyleProperty) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" stroke-width="4"
               style="stroke-width: 9; stroke: black"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStrokeWidthOnSelection(2.5));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("stroke-width"), "4");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; stroke-width: 2.5");
}

TEST(EditorAppTest, SetActiveStrokeWidthClampsNegativeValues) {
  EditorApp app;

  app.setActiveStrokeWidth(-4.0);

  EXPECT_EQ(app.activePaintStyle().strokeWidth, 0.0);
}

TEST(EditorAppTest, PathOperationAvailabilityReportsNoDocument) {
  EditorApp app;

  const PathOperationAvailability availability =
      app.pathOperationAvailability(PathOperationKind::Union);

  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "No SVG document is loaded");
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Union));
}

TEST(EditorAppTest, PathOperationAvailabilityRequiresMultipleGeometryElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Union).canApply);

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(*r1);
  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Union).canApply);

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  EXPECT_TRUE(app.pathOperationAvailability(PathOperationKind::Union).canApply);
  EXPECT_TRUE(app.pathOperationAvailability(PathOperationKind::SubtractFront).canApply);
}

TEST(EditorAppTest, PathOperationAvailabilityRejectsUnsupportedSelectionMembers) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(std::vector<svg::SVGElement>{app.document().document().svgElement(), *r1});

  const PathOperationAvailability availability =
      app.pathOperationAvailability(PathOperationKind::Union);
  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "Selection includes unsupported or empty geometry");
}

TEST(EditorAppTest, PathOperationAvailabilityRejectsDetachedSelectionMembers) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  svg::SVGPathElement detachedPath = svg::SVGPathElement::Create(app.document().document());
  detachedPath.setAttribute("d", "M 0 0 L 10 0 L 10 10 Z");

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(std::vector<svg::SVGElement>{detachedPath, *r1});

  const PathOperationAvailability availability =
      app.pathOperationAvailability(PathOperationKind::Union);
  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "Selection includes detached geometry");
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Union));
}

TEST(EditorAppTest, PathOperationAvailabilityUnderConcurrentDomHoldsAccess) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  const PathOperationAvailability availability =
      app.pathOperationAvailability(PathOperationKind::Union);

  EXPECT_TRUE(availability.canApply) << availability.reason;
}

TEST(EditorAppTest, PathOperationUnavailableWhileDocumentMutationIsPending) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  app.applyMutation(EditorCommand::SetAttributeCommand(*r1, "fill", "green"));

  const PathOperationAvailability availability =
      app.pathOperationAvailability(PathOperationKind::Union);
  EXPECT_FALSE(availability.canApply);
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Union));
}

TEST(EditorAppTest, CompoundPathUnbundleAvailabilityReportsNoDocument) {
  EditorApp app;

  const PathOperationAvailability availability = app.compoundPathUnbundleAvailability();

  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "No SVG document is loaded");
  EXPECT_FALSE(app.unbundleCompoundPath());
}

TEST(EditorAppTest, CompoundPathUnbundleAvailabilityRequiresMultipleContours) {
  constexpr std::string_view kCompoundSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <path id="single" d="M 0 0 L 10 0 L 10 10 L 0 10 Z"/>
         <path id="compound"
               d="M 20 0 L 30 0 L 30 10 L 20 10 Z
                  M 40 0 L 50 0 L 50 10 L 40 10 Z"/>
         <path id="donut" fill-rule="evenodd"
               d="M 0 20 L 40 20 L 40 60 L 0 60 Z
                  M 10 30 L 30 30 L 30 50 L 10 50 Z"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCompoundSvg));

  auto single = app.document().document().querySelector("#single");
  auto compound = app.document().document().querySelector("#compound");
  auto donut = app.document().document().querySelector("#donut");
  ASSERT_TRUE(single.has_value());
  ASSERT_TRUE(compound.has_value());
  ASSERT_TRUE(donut.has_value());

  app.setSelection(*single);
  EXPECT_FALSE(app.compoundPathUnbundleAvailability().canApply);

  app.setSelection(*donut);
  EXPECT_TRUE(app.compoundPathUnbundleAvailability().canApply);

  app.setSelection(*compound);
  EXPECT_TRUE(app.compoundPathUnbundleAvailability().canApply);
}

TEST(EditorAppTest, CompoundPathUnbundleAvailabilityRejectsUnsupportedTargets) {
  constexpr std::string_view kUnsupportedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="rect" x="10" y="10" width="20" height="20" fill="red"/>
         <path id="empty" d=""/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kUnsupportedSvg));

  auto rect = app.document().document().querySelector("#rect");
  auto empty = app.document().document().querySelector("#empty");
  ASSERT_TRUE(rect.has_value());
  ASSERT_TRUE(empty.has_value());

  app.setSelection(*rect);
  PathOperationAvailability availability = app.compoundPathUnbundleAvailability();
  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "Target is not a path");

  app.setSelection(*empty);
  availability = app.compoundPathUnbundleAvailability();
  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "Path has no geometry");

  svg::SVGPathElement detachedPath = svg::SVGPathElement::Create(app.document().document());
  detachedPath.setAttribute("d", "M 20 20 L 30 20 L 30 30 Z M 40 40 L 50 40 L 50 50 Z");
  availability = app.compoundPathUnbundleAvailability(detachedPath);
  EXPECT_FALSE(availability.canApply);
  EXPECT_EQ(availability.reason, "Target path is detached");
}

TEST(EditorAppTest, UnbundleCompoundPathReplacesExplicitTargetAndRestoresUndoSelection) {
  constexpr std::string_view kCompoundSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <path id="compound" fill="red" stroke="black" transform="translate(2 3)"
               d="M 10 10 L 30 10 L 30 30 L 10 30 Z
                  M 50 50 L 70 50 L 70 70 L 50 70 Z"/>
       </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCompoundSvg));

  auto compound = app.document().document().querySelector("#compound");
  ASSERT_TRUE(compound.has_value());
  EXPECT_TRUE(app.compoundPathUnbundleAvailability(*compound).canApply);

  ASSERT_TRUE(app.unbundleCompoundPath(*compound));
  ASSERT_EQ(app.selectedElements().size(), 2u);
  for (const svg::SVGElement& selected : app.selectedElements()) {
    ASSERT_TRUE(selected.isa<svg::SVGPathElement>());
    EXPECT_FALSE(selected.getAttribute("id").has_value());
    EXPECT_EQ(selected.getAttribute("fill"), "red");
    EXPECT_EQ(selected.getAttribute("stroke"), "black");
    EXPECT_EQ(selected.getAttribute("transform"), "translate(2 3)");
  }

  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Unbundle compound path");
  EXPECT_FALSE(app.document().document().querySelector("#compound").has_value());

  const std::vector<svg::SVGPathElement> paths = RootPathChildren(app);
  EXPECT_THAT(PathData(paths), ::testing::ElementsAre("M 10 10 L 30 10 L 30 30 L 10 30 Z",
                                                      "M 50 50 L 70 50 L 70 70 L 50 70 Z"));
  EXPECT_NE(app.document().document().source().find("M 10 10 L 30 10"), std::string_view::npos);
  EXPECT_EQ(app.document().document().source().find(R"(id="compound")"), std::string_view::npos);

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#compound").has_value());
  EXPECT_EQ(RootPathChildren(app).size(), 1u);
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("compound"));
}

TEST(EditorAppTest, UnbundleCompoundPathReleasesNestedHoleContours) {
  constexpr std::string_view kCompoundDonutsSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <path id="compound" fill-rule="evenodd" fill="red"
               d="M 0 0 L 40 0 L 40 40 L 0 40 Z
                  M 10 10 L 30 10 L 30 30 L 10 30 Z
                  M 60 0 L 100 0 L 100 40 L 60 40 Z
                  M 70 10 L 90 10 L 90 30 L 70 30 Z"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCompoundDonutsSvg));

  auto compound = app.document().document().querySelector("#compound");
  ASSERT_TRUE(compound.has_value());
  app.setSelection(*compound);

  ASSERT_TRUE(app.unbundleCompoundPath());
  ASSERT_TRUE(app.flushFrame());

  const std::vector<svg::SVGPathElement> paths = RootPathChildren(app);
  for (const svg::SVGPathElement& path : paths) {
    EXPECT_EQ(path.getAttribute("fill-rule"), "evenodd");
  }
  EXPECT_THAT(PathData(paths), ::testing::ElementsAre("M 0 0 L 40 0 L 40 40 L 0 40 Z",
                                                      "M 10 10 L 30 10 L 30 30 L 10 30 Z",
                                                      "M 60 0 L 100 0 L 100 40 L 60 40 Z",
                                                      "M 70 10 L 90 10 L 90 30 L 70 30 Z"));
}

TEST(EditorAppTest, UnbundleCompoundPathSupportsDonnerDWithCounter) {
  const std::optional<std::string> splash = ReadDonnerSplash();
  ASSERT_TRUE(splash.has_value());

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(*splash));

  auto donnerD = app.document().document().querySelector("#Donner_D");
  ASSERT_TRUE(donnerD.has_value());
  ASSERT_TRUE(donnerD->isa<svg::SVGPathElement>());
  EXPECT_TRUE(app.compoundPathUnbundleAvailability(*donnerD).canApply);

  ASSERT_TRUE(app.unbundleCompoundPath(*donnerD));
  EXPECT_THAT(AttributeValues(app.selectedElements(), "class"),
              ::testing::ElementsAre(std::optional<std::string>("cls-5"),
                                     std::optional<std::string>("cls-5")));
  EXPECT_THAT(AttributeValues(app.selectedElements(), "id"),
              ::testing::ElementsAre(std::nullopt, std::nullopt));

  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().document().querySelector("#Donner_D").has_value());
  EXPECT_THAT(SelectedPathData(app), ::testing::ElementsAre(::testing::HasSubstr("M 302.82"),
                                                            ::testing::HasSubstr("M 312.81")));
}

TEST(EditorAppTest, PathUnionReplacesSelectionWithBooleanPath) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Union));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  ASSERT_TRUE(app.selectedElements().front().isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(app.selectedElements().front().cast<svg::SVGPathElement>().d()),
            "M 10 10 L 30 10 L 30 30 L 10 30 Z M 50 50 L 70 50 L 70 70 L 50 70 Z");

  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r2").has_value());
  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(result->cast<svg::SVGPathElement>().d()),
            "M 10 10 L 30 10 L 30 30 L 10 30 Z M 50 50 L 70 50 L 70 70 L 50 70 Z");
  EXPECT_NE(app.document().document().source().find(R"(<path d="M 10 10 L 30 10)"),
            std::string_view::npos);
}

TEST(EditorAppTest, PathOperationRecordsStructuralUndo) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Union));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(app.undoTimeline().entryCount(), 1u);
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Path operation");
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_TRUE(app.document().document().querySelector("path").has_value());

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#r2").has_value());
  EXPECT_FALSE(app.document().document().querySelector("path").has_value());
  EXPECT_THAT(ElementIds(app.selectedElements()), ::testing::ElementsAre("r1", "r2"));
  EXPECT_TRUE(app.canRedo());

  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r2").has_value());
  EXPECT_TRUE(app.document().document().querySelector("path").has_value());
  EXPECT_THAT(SelectedPathData(app),
              ::testing::ElementsAre(::testing::HasSubstr("M 10 10 L 30 10")));
}

TEST(EditorAppTest, PathIntersectReplacesSelectionWithOverlapPath) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="r2" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(result->cast<svg::SVGPathElement>().d()),
            "M 30 25 L 50 25 L 50 45 L 30 45 Z");
}

TEST(EditorAppTest, PathIntersectUnavailableWhenBoundsDoNotOverlap) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  const std::string sourceBefore(app.document().document().source());
  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Intersect).canApply);
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Intersect));
  EXPECT_EQ(app.document().document().source(), sourceBefore);
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(app.selectedElements().size(), 2u);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
}

TEST(EditorAppTest, EmptyPathIntersectLeavesDocumentUnchanged) {
  constexpr std::string_view kHoleSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <path id="donut" fill-rule="evenodd"
               d="M 0 0 L 100 0 L 100 100 L 0 100 Z
                  M 25 25 L 75 25 L 75 75 L 25 75 Z"/>
         <rect id="inside-hole" x="40" y="40" width="10" height="10" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHoleSvg));

  auto donut = app.document().document().querySelector("#donut");
  auto insideHole = app.document().document().querySelector("#inside-hole");
  ASSERT_TRUE(donut.has_value());
  ASSERT_TRUE(insideHole.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*donut, *insideHole});

  const std::string sourceBefore(app.document().document().source());
  EXPECT_TRUE(app.pathOperationAvailability(PathOperationKind::Intersect).canApply);
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Intersect));
  EXPECT_EQ(app.document().document().source(), sourceBefore);
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(app.selectedElements().size(), 2u);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
}

TEST(EditorAppTest, PathSubtractFrontReplacesSelectionWithDifferencePath) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="r2" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::SubtractFront));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({20, 20}));
  EXPECT_FALSE(spline->isInside({35, 30}));
}

TEST(EditorAppTest, PathSubtractFrontUsesSvgPaintOrderWhenSelectionIsReversed) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="back" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="front" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto back = app.document().document().querySelector("#back");
  auto front = app.document().document().querySelector("#front");
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(front.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*front, *back});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::SubtractFront));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->getAttribute("fill"), "red");
  const std::optional<Path> spline = result->cast<svg::SVGPathElement>().computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({20, 20}));
  EXPECT_FALSE(spline->isInside({35, 30}));
  EXPECT_FALSE(spline->isInside({60, 30}));
}

TEST(EditorAppTest, PathSubtractBackUsesFrontmostElementAsBase) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="back" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="front" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto back = app.document().document().querySelector("#back");
  auto front = app.document().document().querySelector("#front");
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(front.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*back, *front});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::SubtractBack));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  EXPECT_EQ(result->getAttribute("fill"), "blue");
  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_FALSE(spline->isInside({35, 30}));
  EXPECT_TRUE(spline->isInside({60, 30}));
}

TEST(EditorAppTest, PathSubtractBackUsesSvgPaintOrderWhenSelectionIsReversed) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="back" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="front" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto back = app.document().document().querySelector("#back");
  auto front = app.document().document().querySelector("#front");
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(front.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*front, *back});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::SubtractBack));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->getAttribute("fill"), "blue");
  const std::optional<Path> spline = result->cast<svg::SVGPathElement>().computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_FALSE(spline->isInside({20, 20}));
  EXPECT_FALSE(spline->isInside({35, 30}));
  EXPECT_TRUE(spline->isInside({60, 30}));
}

TEST(EditorAppTest, PathExcludeReplacesSelectionWithXorPath) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="r2" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Exclude));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({20, 20}));
  EXPECT_FALSE(spline->isInside({35, 30}));
  EXPECT_TRUE(spline->isInside({60, 30}));
}

TEST(EditorAppTest, PathOperationAppliesElementTransformsInDocumentSpace) {
  constexpr std::string_view kTransformedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="moved" x="0" y="0" width="30" height="30" fill="red"
               transform="translate(20 10)"/>
         <rect id="fixed" x="30" y="20" width="30" height="30" fill="blue"/>
       </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTransformedSvg));

  auto moved = app.document().document().querySelector("#moved");
  auto fixed = app.document().document().querySelector("#fixed");
  ASSERT_TRUE(moved.has_value());
  ASSERT_TRUE(fixed.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*moved, *fixed});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({35, 25}));
  EXPECT_FALSE(spline->isInside({15, 15}));
  EXPECT_FALSE(spline->isInside({55, 45}));
}

TEST(EditorAppTest, PathIntersectUsesBezierGeometryNotAabb) {
  constexpr std::string_view kCurvedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="left" cx="40" cy="50" r="25" fill="red"/>
         <circle id="right" cx="60" cy="50" r="25" fill="blue"/>
       </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCurvedSvg));

  auto left = app.document().document().querySelector("#left");
  auto right = app.document().document().querySelector("#right");
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*left, *right});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(HasCommand(*spline, Path::Verb::CurveTo));
  EXPECT_TRUE(spline->isInside({50, 50}));
  EXPECT_FALSE(spline->isInside({25, 50}));
  EXPECT_FALSE(spline->isInside({75, 50}));
  ExpectBoxInside(spline->bounds(), Box2d::FromXYWH(15.0, 25.0, 70.0, 50.0), 0.01);
  EXPECT_LE(spline->commands().size(), 12u);
}

TEST(EditorAppTest, PathOperationConvertsDocumentResultToTransformedParentSpace) {
  constexpr std::string_view kGroupedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <g id="layer" transform="translate(20 10)">
           <rect id="back" x="0" y="0" width="30" height="30" fill="red"/>
           <rect id="front" x="10" y="10" width="30" height="30" fill="blue"/>
         </g>
       </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGroupedSvg));

  auto back = app.document().document().querySelector("#back");
  auto front = app.document().document().querySelector("#front");
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(front.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*back, *front});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  ASSERT_TRUE(result->parentElement().has_value());
  EXPECT_EQ(result->parentElement()->id(), "layer");

  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({15, 15}));
  EXPECT_FALSE(spline->isInside({35, 25}));

  const std::string source(app.document().document().source());
  const std::size_t groupOpen = source.find(R"(<g id="layer")");
  const std::size_t pathOffset = source.find("<path");
  const std::size_t groupClose = source.find("</g>");
  ASSERT_NE(groupOpen, std::string::npos) << source;
  ASSERT_NE(pathOffset, std::string::npos) << source;
  ASSERT_NE(groupClose, std::string::npos) << source;
  EXPECT_LT(groupOpen, pathOffset) << source;
  EXPECT_LT(pathOffset, groupClose) << source;
  EXPECT_EQ(source.find("<rect"), std::string::npos) << source;
}

TEST(EditorAppTest, PathOperationConvertsDocumentResultThroughRootViewBox) {
  constexpr std::string_view kViewBoxSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"
                 viewBox="0 0 50 50">
         <rect id="back" x="0" y="0" width="30" height="30" fill="red"/>
         <rect id="front" x="10" y="10" width="30" height="30" fill="blue"/>
       </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kViewBoxSvg));

  auto back = app.document().document().querySelector("#back");
  auto front = app.document().document().querySelector("#front");
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(front.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*back, *front});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());

  const svg::SVGPathElement resultPath = result->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = resultPath.computedSpline();
  ASSERT_TRUE(spline.has_value());
  EXPECT_TRUE(spline->isInside({15, 15}));
  EXPECT_FALSE(spline->isInside({35, 25}));
}

TEST(EditorAppTest, PathOperationsHandleOverlappingDonnerLetters) {
  const std::optional<std::string> splashSource = ReadDonnerSplash();
  if (!splashSource.has_value()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }

  const auto applyOperation = [&](PathOperationKind operation) {
    EditorApp app;
    EXPECT_TRUE(app.loadFromString(*splashSource));

    auto donnerD = app.document().document().querySelector("#Donner_D");
    auto donnerO = app.document().document().querySelector("#Donner_O");
    EXPECT_TRUE(donnerD.has_value());
    EXPECT_TRUE(donnerO.has_value());
    if (!donnerD.has_value() || !donnerO.has_value()) {
      return std::optional<Path>();
    }

    app.applyMutation(
        EditorCommand::SetTransformCommand(*donnerO, Transform2d::Translate(Vector2d(-45.0, 0.0))));
    EXPECT_TRUE(app.flushFrame());

    donnerD = app.document().document().querySelector("#Donner_D");
    donnerO = app.document().document().querySelector("#Donner_O");
    EXPECT_TRUE(donnerD.has_value());
    EXPECT_TRUE(donnerO.has_value());
    if (!donnerD.has_value() || !donnerO.has_value()) {
      return std::optional<Path>();
    }

    const Box2d searchBounds =
        Box2d::Union(*donnerD->cast<svg::SVGGeometryElement>().worldBounds(),
                     *donnerO->cast<svg::SVGGeometryElement>().worldBounds());
    const std::optional<Vector2d> dOnly =
        FindMembershipSample(*donnerD, *donnerO, searchBounds, true, false);
    const std::optional<Vector2d> oOnly =
        FindMembershipSample(*donnerD, *donnerO, searchBounds, false, true);
    const std::optional<Vector2d> overlap =
        FindMembershipSample(*donnerD, *donnerO, searchBounds, true, true);
    EXPECT_TRUE(dOnly.has_value());
    EXPECT_TRUE(oOnly.has_value());
    EXPECT_TRUE(overlap.has_value());
    if (!dOnly.has_value() || !oOnly.has_value() || !overlap.has_value()) {
      return std::optional<Path>();
    }

    app.setSelection(std::vector<svg::SVGElement>{*donnerD, *donnerO});
    EXPECT_TRUE(app.applyPathOperation(operation));
    EXPECT_TRUE(app.flushFrame());

    EXPECT_EQ(app.selectedElements().size(), 1u);
    if (app.selectedElements().empty() ||
        !app.selectedElements().front().isa<svg::SVGPathElement>()) {
      return std::optional<Path>();
    }

    const std::optional<Path> spline =
        app.selectedElements().front().cast<svg::SVGPathElement>().computedSpline();
    EXPECT_TRUE(spline.has_value());
    if (!spline.has_value()) {
      return std::optional<Path>();
    }

    switch (operation) {
      case PathOperationKind::Union:
        EXPECT_TRUE(spline->isInside(*dOnly)) << spline->bounds() << " dOnly=" << *dOnly;
        EXPECT_TRUE(spline->isInside(*overlap)) << spline->bounds() << " overlap=" << *overlap;
        EXPECT_TRUE(spline->isInside(*oOnly)) << spline->bounds() << " oOnly=" << *oOnly;
        break;
      case PathOperationKind::Intersect:
        EXPECT_FALSE(spline->isInside(*dOnly));
        EXPECT_TRUE(spline->isInside(*overlap)) << spline->bounds() << " overlap=" << *overlap;
        EXPECT_FALSE(spline->isInside(*oOnly));
        break;
      case PathOperationKind::SubtractFront:
        EXPECT_TRUE(spline->isInside(*dOnly)) << spline->bounds() << " dOnly=" << *dOnly;
        EXPECT_FALSE(spline->isInside(*overlap));
        EXPECT_FALSE(spline->isInside(*oOnly));
        break;
      case PathOperationKind::SubtractBack:
        EXPECT_FALSE(spline->isInside(*dOnly));
        EXPECT_FALSE(spline->isInside(*overlap));
        EXPECT_TRUE(spline->isInside(*oOnly)) << spline->bounds() << " oOnly=" << *oOnly;
        break;
      case PathOperationKind::Exclude:
        EXPECT_TRUE(spline->isInside(*dOnly)) << spline->bounds() << " dOnly=" << *dOnly;
        EXPECT_FALSE(spline->isInside(*overlap));
        EXPECT_TRUE(spline->isInside(*oOnly)) << spline->bounds() << " oOnly=" << *oOnly;
        break;
    }

    return spline;
  };

  EXPECT_TRUE(applyOperation(PathOperationKind::Union).has_value());
  EXPECT_TRUE(applyOperation(PathOperationKind::Intersect).has_value());
  EXPECT_TRUE(applyOperation(PathOperationKind::SubtractFront).has_value());
  EXPECT_TRUE(applyOperation(PathOperationKind::SubtractBack).has_value());
  EXPECT_TRUE(applyOperation(PathOperationKind::Exclude).has_value());
}

TEST(EditorAppTest, HitTestRectFindsAllIntersectingElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // r1 lives at (10,10..30,30), r2 at (50,50..70,70). A marquee
  // covering the full document grabs both.
  auto bothHits = app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(bothHits.size(), 2u);

  // A marquee that only overlaps r1 returns just r1.
  auto r1Only = app.hitTestRect(Box2d::FromXYWH(5.0, 5.0, 20.0, 20.0));
  EXPECT_THAT(ElementIds(r1Only), ::testing::ElementsAre("r1"));

  // A marquee that misses both returns empty.
  auto noHits = app.hitTestRect(Box2d::FromXYWH(80.0, 80.0, 5.0, 5.0));
  EXPECT_TRUE(noHits.empty());

  // Edge contact (marquee touches r1's edge) counts as intersection.
  auto edgeHits = app.hitTestRect(Box2d::FromXYWH(30.0, 30.0, 5.0, 5.0));
  EXPECT_THAT(ElementIds(edgeHits), ::testing::ElementsAre("r1"));
}

TEST(EditorAppTest, HitTestRectUsesFilledShapeIntersectionNotAabb) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
           <path id="triangle" d="M 10 10 L 100 10 L 10 100 Z" fill="red"/>
         </svg>)"));

  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(80.0, 80.0, 10.0, 10.0)).empty())
      << "The marquee intersects the triangle AABB, but not the filled triangle.";

  auto hits = app.hitTestRect(Box2d::FromXYWH(5.0, 5.0, 20.0, 20.0));
  EXPECT_THAT(ElementIds(hits), ::testing::ElementsAre("triangle"));
}

TEST(EditorAppTest, HitTestRectDoesNotSelectShapeThatContainsMarquee) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <rect id="background" x="0" y="0" width="200" height="200" fill="white"/>
           <rect id="target" x="70" y="70" width="20" height="20" fill="red"/>
         </svg>)svg"));

  auto hits = app.hitTestRect(Box2d::FromXYWH(65.0, 65.0, 30.0, 30.0));
  EXPECT_THAT(ElementIds(hits), ::testing::ElementsAre("target"));
}

TEST(EditorAppTest, HitTestRectAppliesElementTransformToShapeIntersection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="160">
           <path id="triangle" d="M 0 0 L 100 0 L 0 100 Z" fill="red"
                 transform="translate(20 30)"/>
         </svg>)svg"));

  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(100.0, 100.0, 10.0, 10.0)).empty())
      << "The marquee intersects the transformed AABB, but not the transformed fill.";

  auto hits = app.hitTestRect(Box2d::FromXYWH(15.0, 25.0, 30.0, 30.0));
  EXPECT_THAT(ElementIds(hits), ::testing::ElementsAre("triangle"));
}

TEST(EditorAppTest, HitTestRectSkipsSingularTransformedFill) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
           <path id="collapsed" d="M 0 0 L 100 0 L 0 100 Z" fill="red"
                 transform="scale(0)"/>
         </svg>)svg"));

  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(20.0, 20.0, 10.0, 10.0)).empty());
}

TEST(EditorAppTest, HitTestRectUsesStrokeShapeIntersection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
           <path id="stroke" d="M 10 50 L 90 50" fill="none" stroke="black" stroke-width="10"/>
         </svg>)"));

  auto hits = app.hitTestRect(Box2d::FromXYWH(40.0, 53.0, 10.0, 4.0));
  EXPECT_THAT(ElementIds(hits), ::testing::ElementsAre("stroke"));
}

TEST(EditorAppTest, HitTestRectCoversStrokeCapAndJoinVariants) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="220" height="120">
           <path id="round-cap" d="M 10 10 L 50 10" fill="none" stroke="black"
                 stroke-width="8" stroke-linecap="round"/>
           <path id="square-cap" d="M 10 30 L 50 30" fill="none" stroke="black"
                 stroke-width="8" stroke-linecap="square"/>
           <path id="miter-clip-join" d="M 80 10 L 100 30 L 120 10" fill="none"
                 stroke="black" stroke-width="8" stroke-linejoin="miter-clip"/>
           <path id="round-join" d="M 80 35 L 100 55 L 120 35" fill="none"
                 stroke="black" stroke-width="8" stroke-linejoin="round"/>
           <path id="bevel-join" d="M 140 10 L 160 30 L 180 10" fill="none"
                 stroke="black" stroke-width="8" stroke-linejoin="bevel"/>
           <path id="arcs-join" d="M 140 35 L 160 55 L 180 35" fill="none"
                 stroke="black" stroke-width="8" stroke-linejoin="arcs"/>
         </svg>)svg"));

  std::vector<std::string> hitIds;
  for (const svg::SVGGraphicsElement& hit :
       app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 220.0, 120.0))) {
    hitIds.push_back(std::string(hit.id().str()));
  }

  EXPECT_THAT(hitIds, ::testing::UnorderedElementsAre("round-cap", "square-cap", "miter-clip-join",
                                                      "round-join", "bevel-join", "arcs-join"));
}

TEST(EditorAppTest, HitTestRectSelectsTextRootNotXmlTextNodeChildren) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="label" x="5" y="15">Hello</text>
           <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         </svg>)"));

  // The marquee returns the <text> ROOT (selectable like any design-tool
  // object) plus the rect; the text's raw XML data child never appears and
  // never breaks the walk.
  auto hits = app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_THAT(ElementIds(hits), ::testing::ElementsAre("label", "r1"));
}

TEST(EditorAppTest, HitTestRectReturnsEmptyWithoutDocument) {
  EditorApp app;
  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0)).empty());
}

TEST(EditorAppTest, SyncDirtyFromSourceClearsWhenTextReturnsToCleanBaseline) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  app.setCleanSourceText(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());

  const std::string edited = std::string(kTrivialSvg) + "\n<!-- edit -->\n";
  app.syncDirtyFromSource(edited);
  EXPECT_TRUE(app.isDirty());

  app.syncDirtyFromSource(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());
}

TEST(EditorAppTest, RevertToCleanSourceReloadsLastSavedDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.setCleanSourceText(kTrivialSvg);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="replacement" cx="25" cy="25" r="10" fill="green"/>
       </svg>)";
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kEditedSvg)));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.isDirty());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  auto replacement = app.document().document().querySelector("#replacement");
  ASSERT_TRUE(replacement.has_value());
  app.setSelection(*replacement);
  EXPECT_TRUE(app.hasSelection());

  ASSERT_TRUE(app.revertToCleanSource());
  EXPECT_FALSE(app.isDirty());
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#replacement").has_value());
}

// Regression for the "scale is wrong, clicks land on the background" bug in
// the editor's main loop. Mirrors exactly the sequence main.cc runs each
// frame:
//   1. Load a document whose intrinsic viewBox differs from the editor pane.
//   2. Set the canvas size to the pane size (the renderer draws a
//      pane-sized bitmap that the user sees).
//   3. Snapshot `cachedDocViewBox` from the document transform the way the
//      render-request path does.
//   4. Build a `DrawingViewportLayout` with the full pane filled at zoom=1,
//      pan=0, and ask `screenToDocument` to map the pane center.
//
// The pane center must hit the center of the viewBox. `canvasFromDocument`
// bakes in the preserveAspectRatio scale + letterbox offset, so click math
// needs its inverse - leaving it un-inverted (or worse, using the old
// misnamed `documentFromCanvasTransform()`) is exactly the "I keep selecting
// the background when I'm trying to click on a letter path" failure mode.
TEST(EditorAppTest, CenterClickOnPaneHitsCenterOfDocumentViewBox) {
  // donner_splash.svg shape: viewBox 892x512 rendered into a ~square pane.
  constexpr std::string_view kViewBoxDoc =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 892 512">
           <rect id="fill" x="0" y="0" width="892" height="512" fill="white"/>
           <rect id="target" x="436" y="246" width="20" height="20" fill="red"/>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kViewBoxDoc));

  // Pane is 720x800 - narrower than the 892-wide viewBox, so with the
  // default preserveAspectRatio="xMidYMid meet" the rendered content is
  // vertically letterboxed inside the pane.
  constexpr int kPaneW = 720;
  constexpr int kPaneH = 800;
  app.document().document().setCanvasSize(kPaneW, kPaneH);

  // Auto-fit may shrink the canvas to preserve the document's aspect
  // ratio - the renderer draws at this size, not at the pane size.
  // This is what main.cc re-reads as `currentCanvasSize` after calling
  // `setCanvasSize`.
  const Vector2i renderedCanvas = app.document().document().canvasSize();
  ASSERT_GT(renderedCanvas.x, 0);
  ASSERT_GT(renderedCanvas.y, 0);

  // Same snapshot code as main.cc at render-request time. `canvasFromDoc`
  // maps viewBox points to canvas pixels (see
  // `SVGDocument.CanvasFromDocumentTransformScaling`), so click math wants
  // the reverse direction.
  const Transform2d docFromCanvas =
      app.document().document().canvasFromDocumentTransform().inverse();
  const Box2d canvasBox = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(renderedCanvas.x),
                                          static_cast<double>(renderedCanvas.y));
  const Box2d cachedDocViewBox = docFromCanvas.transformBox(canvasBox);

  // Pane lives at screen origin (0, 0). zoom=1, pan=0, so imageSize
  // matches the rendered canvas (720x413), vertically centered in the
  // 720x800 pane by `ComputeDrawingViewportLayout`.
  const DrawingViewportLayout layout = ComputeDrawingViewportLayout(
      Vector2d(0.0, 0.0), Vector2d(kPaneW, kPaneH), Vector2d(renderedCanvas.x, renderedCanvas.y),
      Vector2d(0.0, 0.0), cachedDocViewBox);

  // Image should be vertically centered: origin y = (800-413)/2 = 193.5.
  EXPECT_DOUBLE_EQ(layout.imageOrigin.x, 0.0);
  EXPECT_DOUBLE_EQ(layout.imageOrigin.y, (kPaneH - renderedCanvas.y) / 2.0);

  // A click at the center of the pane must land at the center of the
  // viewBox (892/2, 512/2) = (446, 256) - which is inside #target.
  const auto center = layout.screenToDocument(Vector2d(kPaneW / 2.0, kPaneH / 2.0));
  ASSERT_TRUE(center.has_value());
  EXPECT_NEAR(center->x, 446.0, 1.0) << "center=" << *center;
  EXPECT_NEAR(center->y, 256.0, 1.0) << "center=" << *center;

  // And the same point should hit-test to #target (not the background
  // #fill rect).
  auto hit = app.hitTest(*center);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->id(), "target");
}

constexpr std::string_view kThreeRects =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10" fill="red"/>
         <rect id="r2" x="10" y="0" width="10" height="10" fill="green"/>
         <rect id="r3" x="20" y="0" width="10" height="10" fill="blue"/>
       </svg>)";

std::vector<std::string> ChildIds(EditorApp& app) {
  std::vector<std::string> ids;
  const svg::SVGElement root = app.document().document().svgElement();
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (const std::optional<RcString> id = child->getAttribute("id"); id.has_value()) {
      ids.push_back(std::string(id->str()));
    }
  }
  return ids;
}

void SelectById(EditorApp& app, std::string_view id) {
  const std::optional<svg::SVGElement> element =
      app.document().document().querySelector("#" + std::string(id));
  ASSERT_TRUE(element.has_value()) << "missing #" << id;
  app.setSelection(*element);
}

TEST(EditorAppReorderTest, BringForwardMovesElementOneLater) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringForward));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r2", "r1", "r3"));
}

TEST(EditorAppReorderTest, SendBackwardMovesElementOneEarlier) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r3");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::SendBackward));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r1", "r3", "r2"));
}

TEST(EditorAppReorderTest, BringToFrontMovesElementToLast) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r2", "r3", "r1"));
}

TEST(EditorAppReorderTest, SendToBackMovesElementToFirst) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r3");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r3", "r1", "r2"));
}

TEST(EditorAppReorderTest, ReflectsTheMoveIntoTheSourceText) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());
  // The DOM move is reflected back into the source by structured editing - the
  // source order must match the new DOM order (no source-string surgery).
  const std::string source(app.document().document().source());
  const std::size_t r1 = source.find("id=\"r1\"");
  const std::size_t r2 = source.find("id=\"r2\"");
  const std::size_t r3 = source.find("id=\"r3\"");
  ASSERT_NE(r1, std::string::npos) << source;
  ASSERT_NE(r2, std::string::npos) << source;
  ASSERT_NE(r3, std::string::npos) << source;
  EXPECT_LT(r2, r3) << source;
  EXPECT_LT(r3, r1) << source;
}

TEST(EditorAppReorderTest, NoOpWhenAlreadyAtExtreme) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  SelectById(app, "r3");
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringForward));
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  SelectById(app, "r1");
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::SendBackward));
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  // No command was queued by the no-op reorders, so the DOM is untouched (and
  // there is nothing to flush).
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r1", "r2", "r3"));
}

TEST(EditorAppReorderTest, NoOpWithoutSingleSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  app.setSelection(std::nullopt);
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringForward));
}

TEST(EditorAppReorderTest, NoOpWhenElementLocked) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kThreeRects)));
  {
    const std::optional<svg::SVGElement> r1 = app.document().document().querySelector("#r1");
    ASSERT_TRUE(r1.has_value());
    app.setElementLocked(*r1, true);
  }
  ASSERT_TRUE(app.flushFrame());

  SelectById(app, "r1");
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront))
      << "a locked element must not reorder";
  EXPECT_THAT(ChildIds(app), ::testing::ElementsAre("r1", "r2", "r3"));
}

TEST(EditorAppReorderTest, DirectReorderRejectsRootSelfCrossParentAndCurrentPosition) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <g id="group"><rect id="child" width="10" height="10"/></g>
    <rect id="sibling" x="20" width="10" height="10"/>
  </svg>)svg"));

  svg::SVGElement root = app.document().document().svgElement();
  auto group = app.document().document().querySelector("#group");
  auto child = app.document().document().querySelector("#child");
  auto sibling = app.document().document().querySelector("#sibling");
  ASSERT_TRUE(group.has_value());
  ASSERT_TRUE(child.has_value());
  ASSERT_TRUE(sibling.has_value());

  EXPECT_FALSE(app.reorderElementBeforeSibling(root, std::nullopt));
  EXPECT_FALSE(app.reorderElementBeforeSibling(*child, *child));
  EXPECT_FALSE(app.reorderElementBeforeSibling(*child, *sibling));
  EXPECT_FALSE(app.reorderElementBeforeSibling(*group, *sibling));
  EXPECT_FALSE(app.flushFrame());
}

void CollectFlatIds(const svg::SVGElement& el, std::vector<std::string>& out) {
  if (const std::optional<RcString> id = el.getAttribute("id"); id.has_value()) {
    out.push_back(std::string(id->str()));
  }
  for (auto c = el.firstChild(); c.has_value(); c = c->nextSibling()) {
    CollectFlatIds(*c, out);
  }
}

// Depth-first (paint order) list of every id in the document.
std::vector<std::string> FlatIds(EditorApp& app) {
  std::vector<std::string> out;
  CollectFlatIds(app.document().document().svgElement(), out);
  return out;
}

std::optional<std::string> ParentIdOf(EditorApp& app, std::string_view id) {
  const std::optional<svg::SVGElement> el =
      app.document().document().querySelector("#" + std::string(id));
  if (!el.has_value()) {
    return std::nullopt;
  }
  const std::optional<svg::SVGElement> parent = el->parentElement();
  if (!parent.has_value()) {
    return std::nullopt;
  }
  const std::optional<RcString> pid = parent->getAttribute("id");
  return pid.has_value() ? std::optional<std::string>(std::string(pid->str()))
                         : std::optional<std::string>("<svg>");
}

// Two structural groups, no transforms/paint on the groups themselves.
constexpr std::string_view kNestedGroups =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <g id="gA"><rect id="a1" width="10" height="10"/><rect id="a2" width="10" height="10"/></g>
         <g id="gB"><rect id="b1" width="10" height="10"/><rect id="b2" width="10" height="10"/></g>
       </svg>)";

TEST(EditorAppReorderTest, BringToFrontLiftsNestedLeafToDocumentFront) {
  // a2 is the last child of gA, so a naive within-group swap is a no-op and the
  // element stays behind gB. Tree-aware Bring to Front lifts it out to the
  // document root so it paints in front of everything ("does what it says").
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kNestedGroups)));
  SelectById(app, "a2");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(FlatIds(app), ::testing::ElementsAre("gA", "a1", "gB", "b1", "b2", "a2"));
  EXPECT_EQ(ParentIdOf(app, "a2"), std::optional<std::string>("<svg>"));
}

TEST(EditorAppReorderTest, SendToBackLiftsNestedLeafToDocumentBack) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kNestedGroups)));
  SelectById(app, "b1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_THAT(FlatIds(app), ::testing::ElementsAre("b1", "gA", "a1", "a2", "gB", "b2"));
  EXPECT_EQ(ParentIdOf(app, "b1"), std::optional<std::string>("<svg>"));
}

TEST(EditorAppReorderTest, TreeAwareArrangeIsNoOpAtDocumentExtremes) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kNestedGroups)));
  // b2 already paints last in the whole document; a1 already paints first.
  SelectById(app, "b2");
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  SelectById(app, "a1");
  EXPECT_FALSE(app.reorderSelectedElement(EditorApp::ZOrder::SendToBack));
  EXPECT_FALSE(app.flushFrame()) << "no move was queued";
  EXPECT_THAT(FlatIds(app), ::testing::ElementsAre("gA", "a1", "a2", "gB", "b1", "b2"));
}

TEST(EditorAppReorderTest, BringToFrontDoesNotLiftAcrossANonStructuralGroup) {
  // gA carries a transform, so lifting a child out would drop inherited state.
  // The move must fall back to a within-group reorder (never a silent visual
  // change): a1 fronts within gA and stays parented to gA.
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <g id="gA" transform="translate(5,5)"><rect id="a1" width="10" height="10"/><rect id="a2" width="10" height="10"/></g>
           <g id="gB"><rect id="b1" width="10" height="10"/></g>
         </svg>)svg"));
  SelectById(app, "a1");
  EXPECT_TRUE(app.reorderSelectedElement(EditorApp::ZOrder::BringToFront));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(ParentIdOf(app, "a1"), std::optional<std::string>("gA"))
      << "a child of a transformed group must not be lifted out";
  EXPECT_THAT(FlatIds(app), ::testing::ElementsAre("gA", "a2", "a1", "gB", "b1"));
}

std::optional<std::string> AttrOf(EditorApp& app, std::string_view id, const char* attr) {
  const std::optional<svg::SVGElement> el =
      app.document().document().querySelector("#" + std::string(id));
  if (!el.has_value()) {
    return std::nullopt;
  }
  const std::optional<RcString> value = el->getAttribute(attr);
  return value.has_value() ? std::optional<std::string>(std::string(value->str())) : std::nullopt;
}

constexpr std::string_view kGradientDoc =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs><linearGradient id="grad"><stop offset="0" stop-color="red"/></linearGradient></defs>
         <rect id="r" x="0" y="0" width="50" height="50" fill="url(#grad)"
               style="stroke:url(#grad)"/>
         <use id="u" href="#r"/>
       </svg>)svg";

TEST(EditorAppRenameTest, RenameRepointsUrlAndStyleReferences) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kGradientDoc)));
  SelectById(app, "grad");
  EXPECT_TRUE(app.renameSelectedElement("g2"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_TRUE(app.document().document().querySelector("#g2").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#grad").has_value());
  // Both the presentation attribute and the inline style reference are repointed.
  EXPECT_EQ(AttrOf(app, "r", "fill"), "url(#g2)");
  EXPECT_EQ(AttrOf(app, "r", "style"), "stroke:url(#g2)");
}

TEST(EditorAppRenameTest, RenameRepointsHrefReference) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kGradientDoc)));
  SelectById(app, "r");
  EXPECT_TRUE(app.renameSelectedElement("rect2"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_TRUE(app.document().document().querySelector("#rect2").has_value());
  EXPECT_EQ(AttrOf(app, "u", "href"), "#rect2");
}

TEST(EditorAppRenameTest, RenameLeavesPrefixedHrefReferenceUntouched) {
  constexpr std::string_view kXlinkDoc =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"
                 xmlns:xlink="http://www.w3.org/1999/xlink"
                 width="100" height="100">
         <rect id="r" x="0" y="0" width="50" height="50"/>
         <use id="u" href="#r" xlink:href="#r"/>
       </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kXlinkDoc)));
  SelectById(app, "r");
  EXPECT_TRUE(app.renameSelectedElement("rect2"));
  ASSERT_TRUE(app.flushFrame());

  const std::optional<svg::SVGElement> use = app.document().document().querySelector("#u");
  ASSERT_TRUE(use.has_value());
  EXPECT_EQ(AttrOf(app, "u", "href"), "#rect2");
  EXPECT_EQ(use->getAttribute(xml::XMLQualifiedNameRef("xlink", "href")), "#r");
}

// `#grad` appears both as a standalone id selector (must repoint) and as the
// prefix of `#gradient` (must NOT repoint - different, longer token).
constexpr std::string_view kStyleSelectorDoc =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <style>#grad { fill: red }
#gradient { fill: blue }</style>
         <rect id="grad" x="0" y="0" width="50" height="50"/>
       </svg>)svg";

std::optional<std::string> StyleTextOf(EditorApp& app) {
  const std::optional<svg::SVGElement> styleElement =
      app.document().document().querySelector("style");
  if (!styleElement.has_value() || !styleElement->isa<svg::SVGStyleElement>()) {
    return std::nullopt;
  }
  return std::string(styleElement->cast<svg::SVGStyleElement>().textContent().str());
}

TEST(EditorAppRenameTest, RenameRepointsStyleBlockIdSelector) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kStyleSelectorDoc)));
  // Sanity: the original stylesheet text is readable before the rename.
  EXPECT_THAT(StyleTextOf(app),
              ::testing::Optional(std::string("#grad { fill: red }\n#gradient { fill: blue }")));
  SelectById(app, "grad");
  EXPECT_TRUE(app.renameSelectedElement("g2"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_TRUE(app.document().document().querySelector("#g2").has_value());
  // The `#grad` id selector inside <style> is repointed to `#g2`, while the
  // longer `#gradient` token is left untouched (boundary-correct rewrite). The
  // whole text is asserted exactly so any stale `#grad` selector trips.
  EXPECT_THAT(StyleTextOf(app),
              ::testing::Optional(std::string("#g2 { fill: red }\n#gradient { fill: blue }")));
}

// Renaming an element whose id is also a valid hex color (`abc`) must only
// repoint id *selectors* - `#abc` color literals inside declaration blocks,
// comments, and strings are unrelated CSS and must survive untouched.
// `url(#abc)` references inside declarations DO repoint (they reference the
// element).
TEST(EditorAppRenameTest, RenameLeavesHexColorLiteralsAndCommentsUntouched) {
  constexpr std::string_view kHexColorDoc =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <style>#abc { fill: red }
.swatch { fill: #abc; stroke: #abc }
/* #abc historical note */
.badge { content: "#abc"; clip-path: url(#abc) }
@media (min-width: 10px) { #abc { opacity: 0.5 } }</style>
         <rect id="abc" x="0" y="0" width="50" height="50"/>
       </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kHexColorDoc)));
  SelectById(app, "abc");
  EXPECT_TRUE(app.renameSelectedElement("brandBox"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(StyleTextOf(app), ::testing::Optional(std::string(
                                    R"css(#brandBox { fill: red }
.swatch { fill: #abc; stroke: #abc }
/* #abc historical note */
.badge { content: "#abc"; clip-path: url(#brandBox) }
@media (min-width: 10px) { #brandBox { opacity: 0.5 } })css")));
}

TEST(EditorAppRenameTest, RenameLeavesEscapedQuotedCssAndUnclosedCommentsUntouched) {
  constexpr std::string_view kCssStringDoc =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <style>#target { fill: red }
.badge { content: "#target \"literal\""; clip-path: url(#target); marker: url(#targeted) }
@supports (display: grid) { #target { opacity: 0.5 } }
/* #target unfinished comment</style>
         <rect id="target" x="0" y="0" width="50" height="50"/>
       </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kCssStringDoc)));
  SelectById(app, "target");
  EXPECT_TRUE(app.renameSelectedElement("renamed"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(StyleTextOf(app), ::testing::Optional(std::string(
                                    R"css(#renamed { fill: red }
.badge { content: "#target \"literal\""; clip-path: url(#renamed); marker: url(#targeted) }
@supports (display: grid) { #renamed { opacity: 0.5 } }
/* #target unfinished comment)css")));
}

TEST(EditorAppRenameTest, RenameRepointsOnlyExactUrlIdTokensWithIdCharSuffixes) {
  constexpr std::string_view kBoundaryDoc =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs><linearGradient id="grad"/></defs>
         <rect id="r" x="0" y="0" width="50" height="50"
               data-refs="url(#grad) url(#grad-a) url(#grad_a) url(#grad:variant) url(#grad.variant) url(#grad9) url(#gradA)"/>
       </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kBoundaryDoc)));
  SelectById(app, "grad");
  EXPECT_TRUE(app.renameSelectedElement("g2"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(AttrOf(app, "r", "data-refs"),
            "url(#g2) url(#grad-a) url(#grad_a) url(#grad:variant) "
            "url(#grad.variant) url(#grad9) url(#gradA)");
}

TEST(EditorAppRenameTest, RefusesEmptySameAndDuplicateIds) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kGradientDoc)));
  SelectById(app, "r");
  EXPECT_FALSE(app.renameSelectedElement("")) << "empty id";
  EXPECT_FALSE(app.renameSelectedElement("r")) << "same id";
  EXPECT_FALSE(app.renameSelectedElement("grad")) << "id already used by another element";
  // None of the refusals mutated the document.
  EXPECT_EQ(AttrOf(app, "r", "fill"), "url(#grad)");
}

TEST(EditorAppRenameTest, RefusesWithoutSingleSelectionOrWhenLocked) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kGradientDoc)));
  app.setSelection(std::nullopt);
  EXPECT_FALSE(app.renameSelectedElement("x"));

  {
    const std::optional<svg::SVGElement> r = app.document().document().querySelector("#r");
    ASSERT_TRUE(r.has_value());
    app.setElementLocked(*r, true);
  }
  ASSERT_TRUE(app.flushFrame());
  SelectById(app, "r");
  EXPECT_FALSE(app.renameSelectedElement("rect2")) << "a locked element must not rename";
  EXPECT_TRUE(app.document().document().querySelector("#r").has_value());
}

}  // namespace
}  // namespace donner::editor
