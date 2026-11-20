/// @file
/// An edit of an element that another element draws through a reference puts the referencing
/// element in the dirty set, so a renderer that redraws by dirty element redraws it too, without
/// turning an edit the retained render-tree update applies into a whole-document rebuild.
#include "donner/svg/components/ReferenceInvalidation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "donner/base/Transform.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
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

constexpr Vector2i kCanvasSize(96, 96);

/// Matches dirty flags that include every flag in \p expected.
MATCHER_P(HasDirtyFlags, expected, "has dirty flags " + testing::PrintToString(expected)) {
  return (arg & expected) == expected;
}

/// Parses \p body at a 96x96 canvas and draws it once, so the render tree is built.
SVGDocument DrawnDocument(std::string_view body) {
  SVGDocument document = instantiateSubtree(body, {}, kCanvasSize);
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

TEST(ReferenceInvalidationTest, PathEditRelaysOutTextAlongIt) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  document.querySelector("#first")->setAttribute("d", "M 10 40 L 90 20");
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry |
                                     DirtyFlagsComponent::RenderInstance)));
  EXPECT_THAT(DirtyFlagsOf(document, "#followsSecond"), Eq(std::nullopt));
}

TEST(ReferenceInvalidationTest, PathTransformRelaysOutTextAlongIt) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  Translate(document, "#second", Vector2d(0, -12));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsSecond"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry)));
  EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"), Eq(std::nullopt));
}

TEST(ReferenceInvalidationTest, PathRestyleRelaysOutTextAlongIt) {
  SVGDocument document = DrawnDocument(kTwoTextPaths);

  document.querySelector("#second")->setStyle("stroke: blue");
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsSecond"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry)));
}

// Text along a path ignores the transforms of the path's ancestors, and compositing changes cannot
// move the path, so these edits keep the text's layout.
TEST(ReferenceInvalidationTest, EditsThatCannotMoveThePathKeepTheLayout) {
  for (const auto& [label, edit] :
       std::initializer_list<std::pair<const char*, void (*)(SVGDocument&)>>{
           {"ancestor transform", [](SVGDocument& d) { Translate(d, "#group", Vector2d(0, 20)); }},
           {"ancestor opacity",
            [](SVGDocument& d) { d.querySelector("#group")->setAttribute("opacity", "0.5"); }},
           {"path opacity",
            [](SVGDocument& d) { d.querySelector("#first")->setAttribute("opacity", "0.5"); }},
       }) {
    SCOPED_TRACE(label);
    SVGDocument document = DrawnDocument(kTwoTextPaths);
    edit(document);
    InvalidateReferenceDependents(document.unsafeRegistry());
    EXPECT_THAT(DirtyFlagsOf(document, "#followsFirst"), Eq(std::nullopt));
  }
}

// A restyle of the path's ancestor can move the path: here it changes the font size that the
// path's `em` geometry resolves against.
TEST(ReferenceInvalidationTest, AncestorRestyleRelaysOutTextAlongThePath) {
  SVGDocument document = DrawnDocument(R"svg(
    <g id="sized" font-size="10"><rect id="box" x="1em" y="2em" width="5em" height="2em"/></g>
    <text id="followsBox" font-size="12"><textPath href="#box">Around</textPath></text>)svg");

  document.querySelector("#sized")->setAttribute("font-size", "14");
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#followsBox"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::TextGeometry)));
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
  <rect id="hostOfGroup" x="64" y="40" width="30" height="20" filter="url(#ofGroup)"/>
  <rect id="listHostOfA" y="62" width="30" height="20" filter="url(#ofA) saturate(1)"/>
  <rect id="secondStageOfA" x="32" y="62" width="30" height="20"
        filter="url(#ofGroup) url(#ofA)"/>)svg";

TEST(ReferenceInvalidationTest, FeImageTargetEditRedrawsEveryFilterThatDrawsIt) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  SVGDocument document = DrawnDocument(kFeImageHosts);

  document.querySelector("#a")->setAttribute("x", "8");
  InvalidateReferenceDependents(document.unsafeRegistry());

  for (const char* host : {"#hostOfA", "#hostOfAlias", "#listHostOfA", "#secondStageOfA"}) {
    EXPECT_THAT(DirtyFlagsOf(document, host),
                Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent |
                                       DirtyFlagsComponent::RenderInstance)))
        << host;
  }
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
              Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  EXPECT_THAT(DirtyFlagsOf(document, "#secondStageOfA"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfA"), Eq(std::nullopt));
  EXPECT_THAT(DirtyFlagsOf(document, "#hostOfAlias"), Eq(std::nullopt));
}

// An feImage of a group that holds an element filtered with another feImage redraws when the inner
// feImage's target moves.
TEST(ReferenceInvalidationTest, NestedFeImageRedrawsTheOuterFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  SVGDocument document = DrawnDocument(R"svg(
    <defs>
      <filter id="inner" filterUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <feImage href="#a"/>
      </filter>
      <filter id="outer" filterUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <feImage href="#wrap"/>
      </filter>
    </defs>
    <rect id="a" x="4" y="4" width="10" height="10" fill="red"/>
    <g id="wrap"><rect id="innerHost" y="40" width="30" height="20" filter="url(#inner)"/></g>
    <rect id="outerHost" x="40" y="40" width="30" height="20" filter="url(#outer)"/>)svg");

  Translate(document, "#a", Vector2d(6, 0));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#innerHost"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  EXPECT_THAT(DirtyFlagsOf(document, "#outerHost"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
}

/// A chain of \p length filters, each taking its primitives from the previous one through `href`;
/// the first holds `<feImage href="#a">`, and when \p cycle is set it takes its primitives from the
/// last. `#host` uses the last filter.
std::string FilterHrefChain(int length, bool cycle) {
  std::string defs =
      "<defs><filter id='f0' filterUnits='userSpaceOnUse' x='0' y='0' width='96' "
      "height='96'" +
      std::string(cycle ? " href='#f" + std::to_string(length - 1) + "'" : "") +
      "><feImage href='#a'/></filter>";
  for (int i = 1; i < length; ++i) {
    defs += "<filter id='f" + std::to_string(i) + "' href='#f" + std::to_string(i - 1) + "'/>";
  }
  return defs + "</defs><rect id='a' width='10' height='10' fill='red'/>" +
         "<rect id='host' y='40' width='30' height='20' filter='url(#f" +
         std::to_string(length - 1) + ")'/>";
}

TEST(ReferenceInvalidationTest, LongAndCyclicFilterHrefChainsReachTheHost) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  for (const bool cycle : {false, true}) {
    SCOPED_TRACE(cycle ? "cycle" : "chain");
    SVGDocument document = DrawnDocument(FilterHrefChain(2000, cycle));
    Translate(document, "#a", Vector2d(6, 0));
    InvalidateReferenceDependents(document.unsafeRegistry());
    EXPECT_THAT(DirtyFlagsOf(document, "#host"),
                Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  }
}

TEST(ReferenceInvalidationTest, CleanDocumentStaysClean) {
  SVGDocument document = DrawnDocument(kFeImageHosts);

  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_TRUE(document.unsafeRegistry().view<DirtyFlagsComponent>().empty());
}

constexpr std::string_view kClipAndMaskHosts = R"svg(
  <defs>
    <clipPath id="clip"><use href="#target"/></clipPath>
    <mask id="copy" maskUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
      <use href="#target"/>
    </mask>
    <clipPath id="other"><rect width="10" height="10"/></clipPath>
    <clipPath id="own"><rect id="ownChild" width="40" height="40"/></clipPath>
  </defs>
  <g id="holder"><circle id="target" cx="20" cy="20" r="8" fill="white"/></g>
  <rect id="free" x="70" y="4" width="10" height="10"/>
  <rect id="clipped" y="40" width="30" height="20" clip-path="url(#clip)"/>
  <rect id="cssClipped" x="32" y="40" width="30" height="20" style="clip-path: url(#clip)"/>
  <rect id="masked" x="64" y="40" width="30" height="20" mask="url(#copy)"/>
  <rect id="cssMasked" y="62" width="30" height="20" style="mask: url(#copy)"/>
  <rect id="otherClipped" x="32" y="62" width="30" height="20" clip-path="url(#other)"/>
  <rect id="ownClipped" x="64" y="62" width="30" height="20" clip-path="url(#own)"/>)svg";

constexpr std::initializer_list<const char*> kHostsOfTarget = {"#clipped", "#cssClipped", "#masked",
                                                               "#cssMasked"};

TEST(ReferenceInvalidationTest, UseTargetEditRedrawsEveryElementClippedOrMaskedByACopy) {
  for (const auto& [label, edit] :
       std::initializer_list<std::pair<const char*, void (*)(SVGDocument&)>>{
           {"move", [](SVGDocument& d) { Translate(d, "#target", Vector2d(6, 0)); }},
           {"resize", [](SVGDocument& d) { d.querySelector("#target")->setAttribute("r", "12"); }},
       }) {
    SCOPED_TRACE(label);
    SVGDocument document = DrawnDocument(kClipAndMaskHosts);
    edit(document);
    InvalidateReferenceDependents(document.unsafeRegistry());

    for (const char* host : kHostsOfTarget) {
      EXPECT_THAT(DirtyFlagsOf(document, host),
                  Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent |
                                         DirtyFlagsComponent::RenderInstance)))
          << host;
    }
    EXPECT_THAT(DirtyFlagsOf(document, "#otherClipped"), Eq(std::nullopt));
    EXPECT_THAT(DirtyFlagsOf(document, "#ownClipped"), Eq(std::nullopt));
  }
}

// A clip path whose own child moves, with no `<use>`, redraws what it clips.
TEST(ReferenceInvalidationTest, ClipPathChildEditRedrawsTheClippedElement) {
  SVGDocument document = DrawnDocument(kClipAndMaskHosts);

  Translate(document, "#ownChild", Vector2d(6, 0));
  InvalidateReferenceDependents(document.unsafeRegistry());

  EXPECT_THAT(DirtyFlagsOf(document, "#ownClipped"),
              Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  EXPECT_THAT(DirtyFlagsOf(document, "#clipped"), Eq(std::nullopt));
}

// A `<use>` copy is placed by the `<use>`, so moving the copied element's ancestor, editing an
// unrelated element, or moving the copied element of a `<use>` that no clip path or mask holds
// leaves every clipped and masked element alone.
TEST(ReferenceInvalidationTest, EditsThatLeaveTheClipAndMaskContentMarkNoHost) {
  for (const auto& [label, body, edit] :
       std::initializer_list<std::tuple<const char*, std::string_view, void (*)(SVGDocument&)>>{
           {"ancestor transform", kClipAndMaskHosts,
            [](SVGDocument& d) { Translate(d, "#holder", Vector2d(0, 20)); }},
           {"unrelated element", kClipAndMaskHosts,
            [](SVGDocument& d) { Translate(d, "#free", Vector2d(0, 6)); }},
           {"use outside clip and mask", R"svg(
              <defs><clipPath id="clip"><rect width="40" height="40"/></clipPath></defs>
              <circle id="target" cx="20" cy="20" r="8"/>
              <use href="#target" x="40"/>
              <rect id="clipped" y="40" width="30" height="20" clip-path="url(#clip)"/>)svg",
            [](SVGDocument& d) { Translate(d, "#target", Vector2d(6, 0)); }},
       }) {
    SCOPED_TRACE(label);
    SVGDocument document = DrawnDocument(body);
    edit(document);
    InvalidateReferenceDependents(document.unsafeRegistry());
    for (const char* host : kHostsOfTarget) {
      if (document.querySelector(host).has_value()) {
        EXPECT_THAT(DirtyFlagsOf(document, host), Eq(std::nullopt)) << host;
      }
    }
  }
}

// The copy reaches the clipped or masked element through a clip path that clips itself, a clip
// path child that is clipped, a mask inside a mask, a `<use>` of the copied element's group and a
// chain of `<use>` copies.
TEST(ReferenceInvalidationTest, NestedClipAndMaskReferencesRedrawTheHost) {
  SVGDocument document = DrawnDocument(R"svg(
    <defs>
      <clipPath id="inner"><use href="#target"/></clipPath>
      <clipPath id="ownClip" clip-path="url(#inner)"><rect width="96" height="96"/></clipPath>
      <clipPath id="childClip"><rect width="96" height="96" clip-path="url(#inner)"/></clipPath>
      <mask id="innerMask" maskUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <use href="#target"/>
      </mask>
      <mask id="outerMask" maskUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <rect width="96" height="96" fill="white" mask="url(#innerMask)"/>
      </mask>
      <mask id="groupMask" maskUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <use href="#group"/>
      </mask>
      <use id="link" href="#target"/>
      <mask id="chainMask" maskUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
        <use href="#link"/>
      </mask>
    </defs>
    <g id="group"><circle id="target" cx="20" cy="20" r="8" fill="white"/></g>
    <rect id="ownClipHost" y="40" width="30" height="20" clip-path="url(#ownClip)"/>
    <rect id="childClipHost" x="32" y="40" width="30" height="20" clip-path="url(#childClip)"/>
    <rect id="maskInMaskHost" x="64" y="40" width="30" height="20" mask="url(#outerMask)"/>
    <rect id="groupCopyHost" y="62" width="30" height="20" mask="url(#groupMask)"/>
    <rect id="chainHost" x="32" y="62" width="30" height="20" mask="url(#chainMask)"/>)svg");

  Translate(document, "#target", Vector2d(6, 0));
  InvalidateReferenceDependents(document.unsafeRegistry());

  for (const char* host :
       {"#ownClipHost", "#childClipHost", "#maskInMaskHost", "#groupCopyHost", "#chainHost"}) {
    EXPECT_THAT(DirtyFlagsOf(document, host),
                Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)))
        << host;
  }
}

/// \p length clip paths, each clipped by the previous one; the first holds `<use href="#a">`, and
/// when \p cycle is set it is clipped by the last. `#host` is clipped by the last.
std::string ClipPathChain(int length, bool cycle) {
  std::string defs =
      "<defs><clipPath id='c0'" +
      std::string(cycle ? " clip-path='url(#c" + std::to_string(length - 1) + ")'" : "") +
      "><use href='#a'/></clipPath>";
  for (int i = 1; i < length; ++i) {
    defs += "<clipPath id='c" + std::to_string(i) + "' clip-path='url(#c" + std::to_string(i - 1) +
            ")'><rect width='96' height='96'/></clipPath>";
  }
  return defs + "</defs><rect id='a' width='10' height='10' fill='red'/>" +
         "<rect id='host' y='40' width='30' height='20' clip-path='url(#c" +
         std::to_string(length - 1) + ")'/>";
}

/// A chain of \p length `<use>` copies, each copying the previous one, the first copying a group
/// that holds `#a`. When \p cycle is set, the group also holds a copy of the last one. A mask
/// draws the last copy, and `#host` is masked by it.
std::string UseChain(int length, bool cycle) {
  std::string defs =
      "<defs><g id='u0'><rect id='a' width='10' height='10' fill='white'/>" +
      std::string(cycle ? "<use href='#u" + std::to_string(length - 1) + "'/>" : "") + "</g>";
  for (int i = 1; i < length; ++i) {
    defs += "<use id='u" + std::to_string(i) + "' href='#u" + std::to_string(i - 1) + "'/>";
  }
  return defs + "<mask id='m' maskUnits='userSpaceOnUse' x='0' y='0' width='96' height='96'>" +
         "<use href='#u" + std::to_string(length - 1) + "'/></mask></defs>" +
         "<rect id='host' y='40' width='30' height='20' mask='url(#m)'/>";
}

// Each chain is walked once per edit, so the walk stays linear in its length; parsing and drawing
// the documents dominate the time these cases take.
TEST(ReferenceInvalidationTest, LongAndCyclicClipPathChainsReachTheHost) {
  for (const bool cycle : {false, true}) {
    SCOPED_TRACE(cycle ? "cycle" : "chain");
    SVGDocument document = DrawnDocument(ClipPathChain(1000, cycle));
    Translate(document, "#a", Vector2d(6, 0));
    InvalidateReferenceDependents(document.unsafeRegistry());
    EXPECT_THAT(DirtyFlagsOf(document, "#host"),
                Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  }
}

TEST(ReferenceInvalidationTest, LongAndCyclicUseChainsReachTheHost) {
  for (const bool cycle : {false, true}) {
    SCOPED_TRACE(cycle ? "cycle" : "chain");
    SVGDocument document = DrawnDocument(UseChain(1000, cycle));
    Translate(document, "#a", Vector2d(6, 0));
    InvalidateReferenceDependents(document.unsafeRegistry());
    EXPECT_THAT(DirtyFlagsOf(document, "#host"),
                Optional(HasDirtyFlags(DirtyFlagsComponent::ReferencedContent)));
  }
}

/// Preparation counters after drawing \p document with \p renderer.
RenderTreeState::Counters DrawAndCount(RendererInterface& renderer, SVGDocument& document) {
  renderer.draw(document);
  return document.unsafeRegistry().ctx().get<RenderTreeState>().counters;
}

/// One edit that the retained render-tree update applies.
struct RetainedEditCase {
  const char* name;                     //!< Label for the comparison outputs.
  RendererBackendFeature feature;       //!< Feature the document needs.
  const char* body;                     //!< Document content before the edit.
  void (*edit)(SVGDocument& document);  //!< Edit applied through the DOM.
};

void PrintTo(const RetainedEditCase& value, std::ostream* os) {
  *os << value.name;
}

class ReferenceRetainedUpdateTest : public testing::TestWithParam<RetainedEditCase> {};

// The edit takes the retained update, not the whole-document rebuild, and the frame still matches a
// fresh render of the edited document.
TEST_P(ReferenceRetainedUpdateTest, EditTakesTheRetainedUpdate) {
  const RetainedEditCase& item = GetParam();
  if (!ActiveRendererSupportsFeature(item.feature)) {
    GTEST_SKIP() << "Feature disabled in this configuration";
  }
  SVGDocument document = instantiateSubtree(item.body, {}, kCanvasSize);
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  const RenderTreeState::Counters before = DrawAndCount(*renderer, document);

  item.edit(document);
  const RenderTreeState::Counters after = DrawAndCount(*renderer, document);
  EXPECT_EQ(after.fullRebuilds, before.fullRebuilds);
  EXPECT_EQ(after.retainedUpdates, before.retainedUpdates + 1);

  SVGDocument fresh = instantiateSubtree(item.body, {}, kCanvasSize);
  item.edit(fresh);
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(),
                                       RenderDocumentWithActiveBackend(fresh), item.name,
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(
    Edits, ReferenceRetainedUpdateTest,
    testing::Values(RetainedEditCase{"feimage_target_moved", RendererBackendFeature::FilterEffects,
                                     R"svg(
          <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="96" height="96">
            <feImage href="#t"/></filter></defs>
          <rect id="t" x="10" y="10" width="20" height="20" fill="#c00"/>
          <rect y="60" width="96" height="36" fill="#ccc" filter="url(#f) saturate(1)"/>)svg",
                                     [](SVGDocument& d) { Translate(d, "#t", Vector2d(14, 3)); }},
                    RetainedEditCase{"textpath_path_opacity", RendererBackendFeature::Text,
                                     R"svg(
          <g id="g"><path id="p" d="M 10 40 L 90 40" stroke="#c00" stroke-width="3"/></g>
          <text font-size="14" fill="#00c"><textPath href="#p">Hello there</textPath></text>)svg",
                                     [](SVGDocument& d) {
                                       d.querySelector("#p")->setAttribute("opacity", "0.4");
                                     }},
                    RetainedEditCase{"textpath_path_ancestor_opacity", RendererBackendFeature::Text,
                                     R"svg(
          <g id="g"><path id="p" d="M 10 40 L 90 40" stroke="#c00" stroke-width="3"/></g>
          <text font-size="14" fill="#00c"><textPath href="#p">Hello there</textPath></text>)svg",
                                     [](SVGDocument& d) {
                                       d.querySelector("#g")->setAttribute("opacity", "0.4");
                                     }}),
    [](const testing::TestParamInfo<RetainedEditCase>& info) {
      return std::string(info.param.name);
    });

}  // namespace
}  // namespace donner::svg::components
