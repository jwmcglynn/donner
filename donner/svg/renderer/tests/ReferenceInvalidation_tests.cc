/// @file
/// An edit of an element that another element draws through a reference puts the referencing
/// element in the dirty set, so a renderer that redraws by dirty element redraws it too.
#include "donner/svg/components/ReferenceInvalidation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg::components {
namespace {

using testing::Eq;
using testing::Optional;

/// Matches dirty flags that include every flag in \p expected.
MATCHER_P(HasDirtyFlags, expected, "has dirty flags " + testing::PrintToString(expected)) {
  return (arg & expected) == expected;
}

/// Parses \p body at a 96x96 canvas and draws it once, so the render tree is built.
SVGDocument DrawnDocument(std::string_view body) {
  SVGDocument document = instantiateSubtree(body, {}, Vector2i(96, 96));
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  renderer->draw(document);
  return document;
}

Entity EntityOf(SVGDocument& document, const char* selector) {
  std::optional<SVGElement> element = document.querySelector(selector);
  EXPECT_TRUE(element.has_value()) << selector;
  return element.has_value() ? element->unsafeEntityHandle().entity() : entt::null;
}

/// The dirty flags of \p selector, or `std::nullopt` when it is clean.
std::optional<uint16_t> DirtyFlagsOf(SVGDocument& document, const char* selector) {
  const auto* dirty =
      document.unsafeRegistry().try_get<DirtyFlagsComponent>(EntityOf(document, selector));
  return dirty != nullptr ? std::optional<uint16_t>(dirty->flags) : std::nullopt;
}

void Translate(SVGDocument& document, const char* selector, Vector2d offset) {
  document.querySelector(selector)->cast<SVGGraphicsElement>().setTransform(
      Transform2d::Translate(offset));
}

constexpr std::string_view kTwoTextPaths = R"svg(
  <g id="group">
    <path id="first" d="M 10 30 L 90 30"/>
  </g>
  <path id="second" d="M 10 70 L 90 70"/>
  <text id="followsFirst" font-size="12"><textPath href="#first">One</textPath></text>
  <text id="followsSecond" font-size="12"><textPath href="#second">Two</textPath></text>)svg";

TEST(ReferenceInvalidationTest, PathEditDropsTheLayoutOfTextAlongIt) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  document.querySelector("#first")->setAttribute("d", "M 10 40 L 90 20");
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry |
                                     DirtyFlagsComponent::RenderInstance)));
  EXPECT_THAT(DirtyFlagsOf(document, "#followsSecond"), Eq(std::nullopt));
}

TEST(ReferenceInvalidationTest, PathTransformDropsTheLayoutOfTextAlongIt) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  Translate(document, "#second", Vector2d(0, -12));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsSecond"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry)));
  EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"), Eq(std::nullopt));
}

// Text along a path ignores the transforms of the path's ancestors, so moving them keeps the
// text's layout.
TEST(ReferenceInvalidationTest, PathAncestorTransformKeepsTheLayout) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  Translate(document, "#group", Vector2d(0, 20));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"), Eq(std::nullopt));
}

constexpr std::string_view kFeImageHosts = R"svg(
  <defs>
    <filter id="ofA" filterUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
      <feImage href="#a"/>
    </filter>
    <filter id="ofGroup" filterUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
      <feImage href="#group"/>
    </filter>
    <filter id="aliasOfA" href="#ofA"/>
  </defs>
  <rect id="a" x="4" y="4" width="10" height="10" fill="red"/>
  <g id="group"><rect id="b" x="20" y="4" width="10" height="10" fill="blue"/></g>
  <rect id="hostOfA" y="40" width="30" height="20" filter="url(#ofA)"/>
  <rect id="hostOfAlias" x="32" y="40" width="30" height="20" filter="url(#aliasOfA)"/>
  <rect id="hostOfGroup" x="64" y="40" width="30" height="20" filter="url(#ofGroup)"/>)svg";

TEST(ReferenceInvalidationTest, FeImageTargetEditRedrawsEveryFilterThatDrawsIt) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  SVGDocument document = DrawnDocument(kFeImageHosts);

  document.querySelector("#a")->setAttribute("x", "8");
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(
      DirtyFlagsOf(document, "#hostOfA"),
      Optional(HasDirtyFlags(DirtyFlagsComponent::Filter | DirtyFlagsComponent::RenderInstance)));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfAlias"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::Filter)));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfGroup"), Eq(std::nullopt));
}

TEST(ReferenceInvalidationTest, FeImageTargetDescendantMoveRedrawsTheFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  SVGDocument document = DrawnDocument(kFeImageHosts);

  Translate(document, "#b", Vector2d(6, 0));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfGroup"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::Filter)));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfA"), Eq(std::nullopt));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfAlias"), Eq(std::nullopt));
}

TEST(ReferenceInvalidationTest, CleanDocumentStaysClean) {
  SVGDocument document = DrawnDocument(kFeImageHosts);

  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_TRUE(document.unsafeRegistry().view<DirtyFlagsComponent>().empty());
}

}  // namespace
}  // namespace donner::svg::components
