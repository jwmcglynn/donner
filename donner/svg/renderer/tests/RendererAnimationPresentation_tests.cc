#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <memory>
#include <sstream>

#include "donner/base/encoding/Base64.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument ParsePresentation(std::string_view source) {
  return instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
}

TEST(RendererAnimationPresentation, DefaultParserSourceInsertionAndRemovalUpdatesPresentation) {
  auto document = ParsePresentation(R"svg(<rect id="r" width="4" height="16"/>)svg");
  const std::string source(document.source());
  const auto start = source.find("<rect");
  const auto end = source.find("/>", start) + 2;
  const auto inserted = document.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(end)},
      .replacement = R"svg(<rect id="r" width="4" height="16">
        <animate id="a" attributeName="width" from="4" to="12" dur="2s"/>
      </rect>)svg",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_EQ(inserted.applied, true);
  auto animation = document.querySelector("#a");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  EXPECT_EQ(animation->type(), ElementType::Animate);
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@........"));
  ASSERT_EQ(document.removeElement(*animation).applied, true);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, SourceInsertedMotionReferenceUsesSharedElementFactory) {
  auto document = ParsePresentation(
      "<defs><path id='route' d='M0 0H8'/></defs><rect id='r' width='4' height='16'/>");
  const auto rectangle = document.querySelector("#r");
  ASSERT_TRUE(rectangle.has_value());
  const std::string source(document.source());
  const auto start = source.find("<rect");
  const auto end = source.find("/>", start) + 2;
  ASSERT_NE(start, std::string::npos);
  const auto inserted = document.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(end)},
      .replacement = "<rect id='r' width='4' height='16'><animateMotion id='motion' dur='2s'>"
                     "<mpath id='reference' href='#route'/></animateMotion></rect>",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_TRUE(inserted.applied);
  EXPECT_EQ(document.querySelector("#r"), rectangle);
  const auto motion = document.querySelector("#motion");
  const auto reference = document.querySelector("#reference");
  ASSERT_TRUE(motion.has_value());
  ASSERT_TRUE(reference.has_value());
  EXPECT_EQ(motion->type(), ElementType::AnimateMotion);
  EXPECT_EQ(reference->type(), ElementType::MPath);
  EXPECT_THAT(reference->getAttribute("href"), testing::Optional(RcString("#route")));
  EXPECT_EQ(reference->entityHandle().get<components::MPathComponent>().href, "#route");
  EXPECT_THAT(motion->getAttribute("dur"), testing::Optional(RcString("2s")));
  EXPECT_EQ(reference->entityHandle().get<donner::components::TreeComponent>().parent(),
            motion->entityHandle().entity());
  EXPECT_EQ(motion->entityHandle().get<donner::components::TreeComponent>().parent(),
            rectangle->entityHandle().entity());
  EXPECT_EQ(document.registry().ctx().get<components::SVGDocumentContext>().getEntityById(
                RcString("route")),
            document.querySelector("#route")->entityHandle().entity());
  auto fresh = ParsePresentation(
      "<defs><path id='route' d='M0 0H8'/></defs><rect width='4' height='16'>"
      "<animateMotion dur='2s'><mpath href='#route'/></animateMotion></rect>");
  const std::string insertedSource(document.source());
  for (double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    fresh.setTime(time);
    auto expected = ParsePresentation(time == 1.0 ? "<rect x='4' width='4' height='16'/>"
                                                  : "<rect width='4' height='16'/>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(fresh), RenderDocumentWithActiveBackend(expected),
        "fresh-motion-reference", editor::tests::PixelmatchIdentityParams());
    const auto actual = RenderDocumentWithActiveBackend(document);
    if (time == 1.0) {
      const auto* state = motion->entityHandle().try_get<components::AnimationStateComponent>();
      ASSERT_NE(state, nullptr);
      EXPECT_EQ(state->phase, components::AnimationPhase::Active);
      EXPECT_EQ(state->simpleDuration, 2.0);
      EXPECT_EQ(state->targetEntity, rectangle->entityHandle().entity());
    }
    editor::tests::CompareBitmapToBitmap(actual, RenderDocumentWithActiveBackend(expected),
                                         "inserted-motion-reference",
                                         editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), insertedSource);
  }
  ASSERT_TRUE(document.removeElement(*motion).applied);
  auto expected = ParsePresentation("<rect width='4' height='16'/>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "removed-motion-reference", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, MixedFontUnitsUseTheSameSampledFont) {
  for (const std::string_view from : {"0px", "0em"}) {
    SCOPED_TRACE(from);
    auto document = ParsePresentation(
        "<rect width='2' height='16' font-size='12'>"
        "<set attributeName='font-size' to='24' dur='2s'/>"
        "<animate attributeName='x' from='" +
        std::string(from) + "' to='1em' dur='2s'/></rect>");
    for (const double time : {1.0, 3.0, 1.0}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      auto expected = ParsePresentation(time == 1.0 ? "<rect x='12' width='2' height='16'/>"
                                                    : "<rect width='2' height='16'/>");
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "same-sample-font-geometry", editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(RendererAnimationPresentation, MarkerAngleUnitsInterpolateContinuously) {
  const std::string prefix =
      "<defs><marker id='m' markerUnits='userSpaceOnUse' markerWidth='16' markerHeight='16' "
      "refX='0' refY='0' orient='";
  const std::string suffix =
      "'><rect width='6' height='2'/></marker></defs><path d='M8 8H12' marker-start='url(#m)'/>";
  for (const auto& [from, to] :
       {std::pair{"0", "90"}, std::pair{"0deg", "90deg"}, std::pair{"0rad", "90deg"}}) {
    SCOPED_TRACE(from);
    auto document = ParsePresentation(prefix + "0" + suffix +
                                      "<animate href='#m' attributeName='orient' from='" + from +
                                      "' to='" + to + "' dur='2s'/>");
    for (const double time : {0.5, 3.0, 0.5}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      auto expected = ParsePresentation(prefix + (time == 0.5 ? "22.5" : "0") + suffix);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "marker-angle-interpolation", editor::tests::PixelmatchIdentityParams());
    }
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, ToOnlyTextPositionUsesTypedBase) {
  auto document = ParsePresentation(
      "<text id='t' x='2' y='12' font-size='10'>H"
      "<animate attributeName='x' to='10' dur='2s'/></text>");
  auto text = document.querySelector("#t");
  ASSERT_THAT(text, testing::Ne(std::nullopt));
  text->cast<SVGTextElement>().setX(Lengthd(6));
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    auto expected = ParsePresentation(time == 1.0 ? "<text x='8' y='12' font-size='10'>H</text>"
                                                  : "<text x='6' y='12' font-size='10'>H</text>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "typed-text-position-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, ToOnlyTextXUsesFullTypedCoordinateList) {
  auto document = ParsePresentation(
      "<text id='t' x='2 14' y='12' font-size='10'>HH"
      "<animate attributeName='x' to='10 18' dur='2s'/></text>");
  auto text = document.querySelector("#t");
  ASSERT_THAT(text, testing::Ne(std::nullopt));
  SmallVector<Lengthd, 1> coordinates;
  coordinates.push_back(Lengthd(6));
  coordinates.push_back(Lengthd(14));
  text->cast<SVGTextElement>().setXList(std::move(coordinates));

  document.setTime(1);
  auto expected = ParsePresentation("<text x='8 16' y='12' font-size='10'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "typed-text-list-x-base", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, EmptyChildDeltaListsUseNeutralBaseAndRestoreAncestorLayout) {
  struct Case {
    const char* attribute;
    double base;
    double to;
  };
  for (const Case item : {Case{"dx", 2, 6}, Case{"dy", 1, 5}}) {
    SCOPED_TRACE(item.attribute);
    const std::string prefix = "<text x='2' y='12' font-size='10' " + std::string(item.attribute) +
                               "='" + std::to_string(item.base) + "'><tspan";
    auto document =
        ParsePresentation(prefix + " id='s'>H<animate attributeName='" + item.attribute + "' to='" +
                          std::to_string(item.to) + "' begin='1s' dur='2s'/></tspan></text>");
    for (const double time : {0.0, 2.0, 4.0, 2.0}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      const bool active = time == 2.0;
      const double value = active ? item.to / 2 : item.base;
      const std::string child =
          active ? " " + std::string(item.attribute) + "='" + std::to_string(value) + "'" : "";
      auto expected = ParsePresentation(prefix + child + ">H</tspan></text>");
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "neutral-child-delta-base", editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(RendererAnimationPresentation, MultiValueAncestorListDoesNotBecomeChildBase) {
  const std::string source =
      "<text x='2' y='12' font-size='10' dx='1 4'><tspan>A</tspan><tspan id='s'>B"
      "<animate attributeName='dx' to='8' begin='1s' dur='2s'/></tspan></text>";
  auto document = ParsePresentation(source);
  for (const double time : {0.0, 2.0, 4.0, 2.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const std::string child = time == 2.0 ? " dx='4'" : "";
    auto expected =
        ParsePresentation("<text x='2' y='12' font-size='10' dx='1 4'><tspan>A</tspan><tspan" +
                          child + ">B</tspan></text>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "multi-value-ancestor-is-layout-only", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, RootTextLengthUsesNaturalLengthAcrossNestedContent) {
  auto document = ParsePresentation(
      "<text id='t' x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>"
      "<tspan>H</tspan><a><tspan>H</tspan></a>"
      "<animate attributeName='textLength' to='12' dur='2s'/></text>");
  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());
  const double natural = text->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream value;
  value.precision(std::numeric_limits<double>::max_digits10);
  value << (natural + 12) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='" +
      value.str() + "'><tspan>H</tspan><a><tspan>H</tspan></a></text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "nested-natural-text-length", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NestedTextLengthUsesOwnerAggregateAndFontMetrics) {
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10'><tspan id='owner' font-size='20' "
      "lengthAdjust='spacingAndGlyphs'>H<a><tspan>H</tspan></a>"
      "<animate attributeName='textLength' to='1em' dur='2s'/></tspan></text>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  const double natural = owner->cast<SVGTSpanElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream value;
  value.precision(std::numeric_limits<double>::max_digits10);
  value << (natural + 20) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10'><tspan font-size='20' "
      "lengthAdjust='spacingAndGlyphs' textLength='" +
      value.str() + "'>H<a><tspan>H</tspan></a></tspan></text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "nested-owner-text-length", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, SharedTextLengthUsesEachOccurrenceFontMetrics) {
  auto document = ParsePresentation(
      "<defs><text id='source' lengthAdjust='spacingAndGlyphs'><tspan><a><tspan>HH</tspan></a>"
      "</tspan><animate attributeName='textLength' to='1em' dur='2s'/></text></defs>"
      "<use href='#source' x='1' y='6' font-size='6'/><use href='#source' x='1' y='15' "
      "font-size='10'/>");
  auto measure = [](double fontSize) {
    auto value = ParsePresentation("<text font-size='" + std::to_string(fontSize) + "'>HH</text>");
    return value.querySelector("text")->cast<SVGTextElement>().getComputedTextLength();
  };
  const double first = (measure(6) + 6) / 2;
  const double second = (measure(10) + 10) / 2;
  document.setTime(1);
  std::ostringstream firstValue;
  std::ostringstream secondValue;
  firstValue.precision(std::numeric_limits<double>::max_digits10);
  secondValue.precision(std::numeric_limits<double>::max_digits10);
  firstValue << first;
  secondValue << second;
  auto expected = ParsePresentation(
      "<text x='1' y='6' font-size='6' lengthAdjust='spacingAndGlyphs' textLength='" +
      firstValue.str() +
      "'>HH</text><text x='1' y='15' font-size='10' "
      "lengthAdjust='spacingAndGlyphs' textLength='" +
      secondValue.str() + "'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "shared-text-length-occurrence-fonts", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextPathTextLengthUsesNaturalOwnerScope) {
  const std::string prefix =
      "<defs><path id='p' d='M0 12H100'/></defs><text font-size='10'><textPath id='owner' "
      "href='#p' lengthAdjust='spacingAndGlyphs'";
  auto document = ParsePresentation(
      prefix + ">HH<animate attributeName='textLength' to='50' dur='2s'/></textPath></text>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  const double natural = owner->cast<SVGTextPathElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream value;
  value.precision(std::numeric_limits<double>::max_digits10);
  value << (natural + 50) / 2;
  auto expected =
      ParsePresentation(prefix + " textLength='" + value.str() + "'>HH</textPath></text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-path-natural-text-length", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextLengthWorkRefusalKeepsNestedStaticTargetsAtomic) {
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>H"
      "<tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH</tspan>"
      "<animate attributeName='textLength' to='30' dur='2s'/></text>");
  document.setTime(1);
  auto styleWork = std::make_shared<components::StyleResourceBudget>();
  auto* initialBudget = document.registry().ctx().find<components::AnimationResourceBudget>();
  if (!initialBudget) {
    initialBudget = &document.registry().ctx().emplace<components::AnimationResourceBudget>();
  }
  components::StyleResourceBudget::OwnerScope ownerScope(*styleWork);
  components::StyleResourceBudget::ActiveRegistryScope activeScope(*styleWork, document.registry());
  auto sharedAllowance = initialBudget->contextualWorkBudget();
  ASSERT_TRUE(sharedAllowance);
  ASSERT_TRUE(initialBudget->reserveContextualWork(
      components::AnimationResourceBudget::kMaximumContextualWork));
  auto renderer = CreateRendererInstance(ActiveRendererBackend());
  RendererDriver driver(*renderer, false, nullptr, styleWork);
  driver.draw(document);
  const RendererBitmap actual = renderer->takeSnapshot();
  EXPECT_EQ(sharedAllowance->remaining(), 0u);
  EXPECT_EQ(document.registry()
                .ctx()
                .get<components::AnimationResourceBudget>()
                .remainingContextualWork(),
            0u);
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10'>H"
      "<tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH</tspan></text>");
  editor::tests::CompareBitmapToBitmap(actual, RenderDocumentWithActiveBackend(expected),
                                       "text-length-work-refusal",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NaturalTextLengthSpacingMovesGlyphsWithoutScalingThem) {
  auto document = ParsePresentation(
      "<text id='t' x='1' y='12' font-size='10' lengthAdjust='spacing'>HH"
      "<animate attributeName='textLength' to='14' dur='2s'/></text>");
  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());
  const double natural = text->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream value;
  value.precision(std::numeric_limits<double>::max_digits10);
  value << (natural + 14) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacing' "
      "textLength='" +
      value.str() + "'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "natural-text-length-spacing", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NegativeAnimatedTextLengthPreservesNaturalLayout) {
  const std::string base = "<text x='1' y='12' font-size='10'>HH</text>";
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10'>HH"
      "<animate attributeName='textLength' to='-100' dur='2s'/></text>");
  document.setTime(1);
  auto expected = ParsePresentation(base);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "negative-text-length-preserves-natural", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, InvalidAnimatedTextLengthPreservesExplicitBase) {
  const std::string base =
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' "
      "textLength='8'>HH</text>";
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='8'>HH"
      "<animate attributeName='textLength' to='-100' dur='2s'/></text>");
  document.setTime(1);
  auto expected = ParsePresentation(base);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "invalid-text-length-keeps-explicit-base", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, MixedFlatAndPathTextLengthMatchesStaticTargetAcrossSeeks) {
  const std::string paths =
      "<defs><path id='p1' d='M1 12H30'/><path id='p2' d='M32 12H62'/></defs>";
  const std::string text =
      "<text id='owner' x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>A"
      "<textPath href='#p1'>B</textPath>C<textPath href='#p2'>D</textPath></text>";
  auto document = ParsePresentation(paths + text +
                                    "<animate href='#owner' attributeName='textLength' to='50' "
                                    "dur='2s'/>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  document.setTime(3);
  const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
  std::ostringstream midpoint;
  midpoint.precision(std::numeric_limits<double>::max_digits10);
  midpoint << (natural + 50) / 2;
  for (const double time : {0.0, 1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const std::string target = time == 1 ? " textLength='" + midpoint.str() + "'" : "";
    auto expected = ParsePresentation(
        paths + "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'" + target +
        ">A<textPath href='#p1'>B</textPath>C<textPath href='#p2'>D</textPath></text>");
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         "mixed-flat-path-text-length-" + std::to_string(time),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, NestedFixedOwnerDoesNotCreateParentScalingGap) {
  auto document = ParsePresentation(
      "<text id='owner' x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>"
      "<tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH</tspan>H"
      "<animate attributeName='textLength' to='30' dur='2s'/></text>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  document.setTime(3);
  const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream midpoint;
  midpoint.precision(std::numeric_limits<double>::max_digits10);
  midpoint << (natural + 30) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='" +
      midpoint.str() +
      "'><tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH</tspan>H</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "nested-fixed-owner-parent-scaling", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NestedOwnerAbsoluteResetStopsPriorLengthCarry) {
  auto document = ParsePresentation(
      "<text y='12' font-size='10'><tspan lengthAdjust='spacingAndGlyphs' "
      "textLength='40'>AB</tspan><tspan x='100' textLength='20'>C</tspan></text>");
  auto expected = ParsePresentation(
      "<text y='12' font-size='10'><tspan lengthAdjust='spacingAndGlyphs' "
      "textLength='40'>AB</tspan><tspan x='100'>C</tspan></text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "nested-owner-absolute-reset", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, InvalidNestedTextLengthKeepsOwnBaseOnly) {
  auto document = ParsePresentation(
      "<text id='owner' x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>H"
      "<tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH"
      "<animate attributeName='textLength' to='-1' dur='2s'/></tspan>H"
      "<animate attributeName='textLength' to='40' dur='2s'/></text>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  document.setTime(3);
  const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream midpoint;
  midpoint.precision(std::numeric_limits<double>::max_digits10);
  midpoint << (natural + 40) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='" +
      midpoint.str() +
      "'>H"
      "<tspan lengthAdjust='spacingAndGlyphs' textLength='8'>HH</tspan>H</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "invalid-nested-text-length-own-base", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, VerticalNaturalTextLengthMatchesStaticTarget) {
  auto document = ParsePresentation(
      "<text id='owner' x='12' y='1' font-size='10' writing-mode='vertical-rl' "
      "lengthAdjust='spacingAndGlyphs'>HH"
      "<animate attributeName='textLength' to='20' dur='2s'/></text>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  document.setTime(3);
  const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream midpoint;
  midpoint.precision(std::numeric_limits<double>::max_digits10);
  midpoint << (natural + 20) / 2;
  auto expected = ParsePresentation(
      "<text x='12' y='1' font-size='10' writing-mode='vertical-rl' "
      "lengthAdjust='spacingAndGlyphs' textLength='" +
      midpoint.str() + "'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "vertical-natural-text-length", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NaturalTextLengthRecomputesAfterSourceMutation) {
  auto document = ParsePresentation(
      "<text id='owner' x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'>HH</text>"
      "<animate href='#owner' attributeName='textLength' to='30' dur='2s'/>");
  auto owner = document.querySelector("#owner");
  ASSERT_TRUE(owner.has_value());
  owner->cast<SVGTextElement>().setTextContent("HHH");
  document.setTime(3);
  const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
  document.setTime(1);
  std::ostringstream midpoint;
  midpoint.precision(std::numeric_limits<double>::max_digits10);
  midpoint << (natural + 30) / 2;
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='" +
      midpoint.str() + "'>HHH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "natural-text-length-source-mutation", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, InvalidUpperTextLengthKeepsValidLowerLayer) {
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='8'>HH"
      "<animate attributeName='textLength' from='10' to='20' dur='2s'/>"
      "<set attributeName='textLength' to='-1' dur='2s'/></text>");
  document.setTime(1);
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' "
      "textLength='15'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "invalid-upper-text-length-keeps-lower", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextLengthValuesIgnoreInvalidFromOperand) {
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='8'>HH"
      "<animate attributeName='textLength' from='-1' values='10;20' dur='2s'/></text>");
  document.setTime(1);
  auto expected = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' "
      "textLength='15'>HH</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-length-values-precedence", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextPositionBaseUsesAnimatedListArity) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='2'>AB</text>"
      "<animate href='#owner' attributeName='dx' to='10' dur='2s'/>");
  document.setTime(0.5);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='4'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-position-list-arity", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, AnimatedTextPositionListReplacesAuthoredTail) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='1 2'>AB</text>"
      "<animate href='#owner' attributeName='dx' from='10' to='10' dur='2s'/>");
  document.setTime(1);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='10'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-position-list-replaces-tail", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, NonemptyDeltaListIsNotTruncatedForInterpolation) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='1 2'>AB</text>"
      "<animate href='#owner' attributeName='dx' to='10' dur='2s'/>");
  document.setTime(0.5);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='1 2'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "nonempty-delta-list-is-not-truncated", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, InvalidUpperTextPositionKeepsValidLowerLayer) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='2'>AB</text>"
      "<animate href='#owner' attributeName='dx' from='2' to='6' dur='2s'/>"
      "<set href='#owner' attributeName='dx' to='invalid' dur='2s'/>");
  document.setTime(1);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='4'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "invalid-upper-text-position-keeps-lower", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextPositionValuesIgnoreInvalidFromOperand) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='2'>AB</text>"
      "<animate href='#owner' attributeName='dx' from='invalid' values='10;14' dur='2s'/>");
  document.setTime(1);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='12'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-position-values-precedence", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, TextPositionToIgnoresInactiveInvalidByOperand) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='2'>AB</text>"
      "<animate href='#owner' attributeName='dx' from='2' to='6' by='invalid' dur='2s'/>");
  document.setTime(1);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='4'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-position-to-precedence", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, EmptyDeltaBaseIsPaddedPerAnimationLayer) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10'>AB</text>"
      "<animate href='#owner' attributeName='dx' to='8' dur='2s'/>"
      "<animate href='#owner' attributeName='dx' to='10 20' dur='4s'/>");
  document.setTime(0.5);
  auto expected = ParsePresentation("<text y='12' font-size='10' dx='2'>AB</text>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "empty-delta-base-layer-padding", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, FrozenPartialTextLengthKeepsBoundaryLowerLayer) {
  auto document = ParsePresentation(
      "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='8'>HH"
      "<animate attributeName='textLength' from='10' to='20' dur='10s'/>"
      "<animate attributeName='textLength' to='30' dur='2s' repeatCount='0.5' "
      "fill='freeze'/></text>");
  struct Checkpoint {
    double time;
    double target;
  };
  for (const Checkpoint item :
       {Checkpoint{5, 20.5}, Checkpoint{0.5, 15.375}, Checkpoint{5, 20.5}}) {
    SCOPED_TRACE(item.time);
    document.setTime(item.time);
    auto expected = ParsePresentation(
        "<text x='1' y='12' font-size='10' lengthAdjust='spacingAndGlyphs' textLength='" +
        std::to_string(item.target) + "'>HH</text>");
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         "frozen-partial-text-length-" + std::to_string(item.time),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, FrozenPartialTextPositionKeepsBoundaryLowerLayer) {
  auto document = ParsePresentation(
      "<text id='owner' y='12' font-size='10' dx='2'>HH</text>"
      "<animate href='#owner' attributeName='dx' from='2' to='12' dur='10s'/>"
      "<animate href='#owner' attributeName='dx' to='20' dur='2s' repeatCount='0.5' "
      "fill='freeze'/>");
  struct Checkpoint {
    double time;
    double target;
  };
  for (const Checkpoint item : {Checkpoint{5, 11.5}, Checkpoint{0.5, 6.875}, Checkpoint{5, 11.5}}) {
    SCOPED_TRACE(item.time);
    document.setTime(item.time);
    auto expected = ParsePresentation("<text y='12' font-size='10' dx='" +
                                      std::to_string(item.target) + "'>HH</text>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "frozen-partial-text-position-" + std::to_string(item.time),
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, RtlTextLengthMatchesEquivalentStaticTarget) {
  const auto fontFile =
      donner::tests::ReadRequiredRunfile("third_party/resvg-test-suite/fonts/Amiri-Regular.ttf");
  ASSERT_THAT(fontFile.error, testing::IsEmpty());
  css::FontFaceSource fontSource;
  fontSource.kind = css::FontFaceSource::Kind::Data;
  const auto fontData = std::make_shared<const std::vector<uint8_t>>(fontFile.contents.begin(),
                                                                     fontFile.contents.end());
  fontSource.payload = fontData;
  fontSource.trusted = true;
  css::FontFace face;
  face.familyName = "Amiri";
  face.sources.push_back(std::move(fontSource));
  const auto parseArabic = [&](std::string_view source) {
    auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(200, 80));
    auto& context = document.registry().ctx();
    if (!context.contains<FontManager>()) {
      context.emplace<FontManager>(document.registry());
    }
    auto& fonts = context.get<FontManager>();
    fonts.addFontFace(face);
    EXPECT_THAT(fonts.fontData(fonts.findFont("Amiri")), testing::ElementsAreArray(*fontData));
    return document;
  };
  for (const std::string_view adjust : {"spacing", "spacingAndGlyphs"}) {
    SCOPED_TRACE(adjust);
    const std::string attributes =
        " x='20' y='45' font-size='32' font-family='Amiri' lengthAdjust='" + std::string(adjust) +
        "'";
    auto document = parseArabic("<text id='owner'" + attributes +
                                ">&#x627;&#x644;&#x648;&#x64A;&#x628;"
                                "<animate attributeName='textLength' to='120' dur='2s'/></text>");
    auto owner = document.querySelector("#owner");
    ASSERT_THAT(owner, testing::Ne(std::nullopt));
    document.setTime(3);
    const double natural = owner->cast<SVGTextElement>().getComputedTextLength();
    ASSERT_GT(natural, 0);
    document.setTime(1);
    std::ostringstream midpoint;
    midpoint.precision(std::numeric_limits<double>::max_digits10);
    midpoint << (natural + 120) / 2;
    auto expected = parseArabic("<text" + attributes + " textLength='" + midpoint.str() +
                                "'>&#x627;&#x644;&#x648;&#x64A;&#x628;</text>");
    const auto actualBitmap = RenderDocumentWithActiveBackend(document);
    const auto expectedBitmap = RenderDocumentWithActiveBackend(expected);
    ASSERT_GT(test::CountNonTransparentPixels(actualBitmap), 100u);
    ASSERT_GT(test::CountNonTransparentPixels(expectedBitmap), 100u);
    editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap,
                                         "rtl-text-length-" + std::string(adjust),
                                         editor::tests::PixelmatchIdentityParams());
  }
}
#endif

TEST(RendererAnimationPresentation, ToOnlyPathCalibrationUsesTypedBase) {
  const std::string prefix =
      "<path id='p' d='M0 8H16' stroke='black' stroke-width='4' "
      "stroke-dasharray='2 2' pathLength='";
  auto document = ParsePresentation(
      prefix + "16'><animate attributeName='pathLength' to='12' dur='2s'/></path>");
  auto path = document.querySelector("#p");
  ASSERT_THAT(path, testing::Ne(std::nullopt));
  path->cast<SVGPathElement>().setPathLength(8);
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    auto expected = ParsePresentation(prefix + (time == 1.0 ? "10" : "8") + "'/>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "typed-path-calibration-base", editor::tests::PixelmatchIdentityParams());
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, TypedTextFieldsRestoreAndAcceptEdits) {
  struct Case {
    const char* attribute;
    const char* opening;
    double authored;
    double typed;
    double to;
    double midpoint;
    std::function<void(SVGTextElement&, double)> write;
  };
  for (const auto& item :
       {Case{"y", "<text id='t' x='2' font-size='10'", 12, 10, 14, 12,
             [](SVGTextElement& text, double value) { text.setY(Lengthd(value)); }},
        Case{"rotate", "<text id='t' x='2' y='12' font-size='10'", 0, 10, 30, 20,
             [](SVGTextElement& text, double value) { text.setRotate(value); }},
        Case{"textLength",
             "<text id='t' x='2' y='12' font-size='10' lengthAdjust='spacingAndGlyphs'", 4, 8, 12,
             10, [](SVGTextElement& text, double value) { text.setTextLength(Lengthd(value)); }}}) {
    SCOPED_TRACE(item.attribute);
    const std::string prefix = std::string(item.opening) + " " + item.attribute + "='";
    auto document = ParsePresentation(prefix + std::to_string(item.authored) +
                                      "'>H<animate attributeName='" + item.attribute + "' to='" +
                                      std::to_string(item.to) + "' dur='2s'/></text>");
    auto text = document.querySelector("#t");
    ASSERT_THAT(text, testing::Ne(std::nullopt));
    auto typed = text->cast<SVGTextElement>();
    item.write(typed, item.typed);
    for (const double time : {1.0, 3.0, 1.0}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      auto expected = ParsePresentation(
          prefix + std::to_string(time == 1 ? item.midpoint : item.typed) + "'>H</text>");
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "typed-text-fields", editor::tests::PixelmatchIdentityParams());
    }
    item.write(typed, item.to);
    for (const double time : {1.0, 3.0}) {
      document.setTime(time);
      auto expected = ParsePresentation(prefix + std::to_string(item.to) + "'>H</text>");
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "edited-text-fields", editor::tests::PixelmatchIdentityParams());
    }
  }
}
#endif

TEST(RendererAnimationPresentation, ToOnlyViewBoxUsesTypedBase) {
  auto document = ParsePresentation(
      "<svg id='v' width='16' height='16' viewBox='0 0 16 16'><rect x='1' y='1' width='2' "
      "height='2'/>"
      "<animate attributeName='viewBox' to='0 0 12 12' dur='2s'/></svg>");
  auto viewport = document.querySelector("#v");
  ASSERT_THAT(viewport, testing::Ne(std::nullopt));
  viewport->cast<SVGSVGElement>().setViewBox(0, 0, 8, 8);
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const std::string box = time == 1 ? "0 0 10 10" : "0 0 8 8";
    auto expected = ParsePresentation("<svg width='16' height='16' viewBox='" + box +
                                      "'><rect x='1' y='1' width='2' height='2'/></svg>");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "typed-viewbox-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPresentation, RemovingAnimatedViewBoxResetsTypedValue) {
  auto document = ParsePresentation(
      "<svg id='v' width='16' height='16' viewBox='0 0 8 8'>"
      "<rect x='1' y='1' width='2' height='2'/>"
      "<set attributeName='viewBox' to='0 0 4 4' dur='1s'/></svg>");
  auto viewport = document.querySelector("#v");
  ASSERT_THAT(viewport, testing::Optional(testing::_));
  document.setTime(0.5);
  auto active = ParsePresentation(
      "<svg width='16' height='16' viewBox='0 0 4 4'>"
      "<rect x='1' y='1' width='2' height='2'/></svg>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(active),
      "removed-viewbox-active", editor::tests::PixelmatchIdentityParams());
  ASSERT_THAT(viewport->cast<SVGSVGElement>().viewBox(),
              testing::Optional(Box2d::FromXYWH(0, 0, 4, 4)));

  viewport->removeAttribute("viewBox");
  EXPECT_THAT(viewport->getAttribute("viewBox"), testing::Eq(std::nullopt));
  EXPECT_THAT(viewport->cast<SVGSVGElement>().viewBox(), testing::Eq(std::nullopt));
  document.setTime(2);
  auto expected = ParsePresentation(
      "<svg width='16' height='16'>"
      "<rect x='1' y='1' width='2' height='2'/></svg>");
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "removed-viewbox-restored", editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(viewport->cast<SVGSVGElement>().viewBox(), testing::Eq(std::nullopt));
}

TEST(RendererAnimationPresentation, MarkerAngleCompositionPacingAndTypedBase) {
  const std::string prefix =
      "<defs><marker id='m' markerUnits='userSpaceOnUse' markerWidth='16' markerHeight='16' "
      "refX='0' refY='0' orient='";
  const std::string suffix =
      "'><rect width='6' height='2'/></marker></defs><path d='M8 8H12' marker-start='url(#m)'/>";
  struct Case {
    const char* attributes;
    double typed;
    double time;
    const char* expected;
  };
  for (const auto& item :
       {Case{"to='90deg'", 45, 1, "67.5"}, Case{"by='90deg'", 45, .5, "67.5"},
        Case{"values='0deg;90deg;0.5turn' calcMode='paced'", 0, .5, "45"},
        Case{"from='0rad' to='90deg' accumulate='sum' repeatCount='2'", 0, 2.5, "112.5"}}) {
    SCOPED_TRACE(item.attributes);
    auto document =
        ParsePresentation(prefix + "0" + suffix + "<animate href='#m' attributeName='orient' " +
                          item.attributes + " dur='2s'/>");
    auto marker = document.querySelector("#m");
    ASSERT_THAT(marker, testing::Ne(std::nullopt));
    marker->cast<SVGMarkerElement>().setOrient(MarkerOrient::AngleDegrees(item.typed));
    for (const double time : {item.time, 5.0, item.time}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      auto expected = ParsePresentation(
          prefix + (time == 5 ? std::to_string(item.typed) : item.expected) + suffix);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "marker-composition", editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(RendererAnimationPresentation, PatternTileWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/>
      <rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@..@@..@@..@@.."));
}

TEST(RendererAnimationPresentation, FilterRegionWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="4" height="16">
      <set attributeName="width" to="12" dur="1s"/><feOffset/>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, FilterPrimitiveWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
      <feFlood x="0" y="0" width="4" height="16" flood-color="black">
        <set attributeName="width" to="12" dur="1s"/>
      </feFlood>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, TextPositionMatchesAnIndependentStaticSample) {
  auto document = ParsePresentation(R"svg(
    <text x="0" y="12" font-size="12">H<set attributeName="x" to="8" dur="1s"/></text>)svg");
  auto expected = ParsePresentation(R"(<text x="8" y="12" font-size="12">H</text>)");
  auto base = ParsePresentation(R"(<text x="0" y="12" font-size="12">H</text>)");
  document.setTime(0.5);
  const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
  EXPECT_THAT(expectedAscii, testing::HasSubstr("@"));
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(base).generated);
}
#endif

TEST(RendererAnimationPresentation, EditedPatternWidthRestoresAuthorCascadePriority) {
  auto document = ParsePresentation(R"svg(
    <style>pattern { width: 6px; }</style>
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/><rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.querySelector("#p")->setAttribute("width", "12");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@....@@....@@.."));
}

TEST(RendererAnimationPresentation, MotionUsesCssDefinedPathGeometry) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="route" d="M0 0L8 0" style="d: 'M0 0L0 8'"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath href="#route"/></animateMotion></rect>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, TextLengthAndRotationMatchStaticSamples) {
  struct Case {
    std::string_view attribute;
    std::string_view value;
  };
  for (const Case item : {Case{"textLength", "12"}, Case{"rotate", "15"}}) {
    SCOPED_TRACE(item.attribute);
    const std::string opening = "<text x='2' y='12' font-size='12' lengthAdjust='spacingAndGlyphs'";
    auto document =
        ParsePresentation(opening + ">H<set attributeName='" + std::string(item.attribute) +
                          "' to='" + std::string(item.value) + "' dur='1s'/></text>");
    auto expected = ParsePresentation(opening + " " + std::string(item.attribute) + "='" +
                                      std::string(item.value) + "'>H</text>");
    auto base = ParsePresentation(opening + ">H</text>");
    document.setTime(0.5);
    const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
    EXPECT_THAT(
        expectedAscii,
        testing::Contains(testing::Not(testing::AnyOf(testing::Eq('.'), testing::Eq('\n')))));
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
    document.setTime(2);
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
              RendererTestUtils::renderToAsciiImage(base).generated);
  }
}
#endif

TEST(RendererAnimationPresentation, UseHrefSwitchesAndRestoresWithoutChangingSource) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="4" height="16"/><rect id="b" x="8" width="4" height="16"/></defs>
    <use href="#a"><set attributeName="href" to="#b" begin="0.5s" dur="1s"/></use>)svg");
  const std::string source(document.source());
  for (int cycle = 0; cycle < 3; ++cycle) {
    document.setTime(1);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("........@@@@...."));
    document.setTime(2);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("@@@@............"));
  }
  EXPECT_EQ(document.source(), source);
}

TEST(RendererAnimationPresentation, GradientHrefSwitchesInheritedStops) {
  auto document = ParsePresentation(R"svg(
    <defs><linearGradient id="a"><stop stop-color="black"/></linearGradient>
      <linearGradient id="b"><stop stop-color="red"/></linearGradient>
      <linearGradient id="g" href="#a"/>
      <set href="#g" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(255, 0, 0, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 255));
}

TEST(RendererAnimationPresentation, PatternHrefSwitchesInheritedContent) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="a" patternUnits="userSpaceOnUse" width="16" height="16"><rect width="4" height="16"/></pattern>
      <pattern id="b" patternUnits="userSpaceOnUse" width="16" height="16"><rect x="8" width="4" height="16"/></pattern>
      <pattern id="p" href="#a"/>
      <set href="#p" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("........@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, AnimatedMpathHrefChangesTheSameMotionSample) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 0L8 0"/><path id="b" d="M0 0L0 8"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath id="m" href="#a"/></animateMotion></rect>
    <set href="#m" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, TextPathHrefSwitchesLayout) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 12H16"/><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath id="t" href="#a">H</textPath></text>
    <set href="#t" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath href="#b">H</textPath></text>)svg");
  document.setTime(1);
  const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
  EXPECT_THAT(expectedAscii,
              testing::Contains(testing::Not(testing::AnyOf(testing::Eq('.'), testing::Eq('\n')))));
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
}
#endif

TEST(RendererAnimationPresentation, ConstantImageReferenceKeepsItsDecodedResource) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"><set attributeName="href" dur="10s"
      to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="/>
    </image>)svg");
  auto& registry = document.registry();
  registry.ctx().erase<components::ResourceManagerContext>();
  registry.ctx()
      .emplace<components::ResourceManagerContext>(registry, 256)
      .setProcessingMode(ProcessingMode::DynamicInteractive);
  const Entity target = document.querySelector("#i")->entityHandle().entity();
  uint64_t firstRevision = 0;
  for (int frame = 0; frame < 12; ++frame) {
    SCOPED_TRACE(frame);
    document.setTime(1 + frame / 60.0);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
    const auto* loaded = registry.try_get<components::LoadedImageComponent>(target);
    ASSERT_THAT(loaded, testing::Ne(nullptr));
    if (frame == 0) {
      firstRevision = loaded->revision();
    }
    EXPECT_EQ(loaded->revision(), firstRevision);
  }
}

TEST(RendererAnimationPresentation, NestedReferenceMutationDoesNotLeakShadowEntities) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="4"/></pattern>
      <rect id="shape" width="4" height="16" fill="url(#p)"/>
      <g id="a"><use href="#shape"/></g><g id="b"><use href="#shape" x="8"/></g>
    </defs><use id="u" href="#a"><set attributeName="href" to="#b" dur="10s"/></use>)svg");
  std::size_t firstCount = 0;
  for (int frame = 0; frame < 5; ++frame) {
    document.setTime(1 + frame / 60.0);
    (void)RendererTestUtils::renderToAsciiImage(document);
    const auto count = document.registry().view<components::ShadowEntityComponent>().size();
    ASSERT_GT(count, 0);
    if (frame == 0) {
      firstCount = count;
    }
    EXPECT_EQ(count, firstCount);
    document.querySelector("#u")->setId("u");
  }
}

class AnimationRecordingLoader final : public ResourceLoaderInterface {
public:
  std::vector<std::string> requests;
  ResourceLoaderError failure = ResourceLoaderError::NotFound;
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    requests.emplace_back(url);
    return failure;
  }
};

TEST(RendererAnimationPresentation, AnimatedResourceReferencesRespectSecureLoading) {
  for (const auto mode : {ProcessingMode::DynamicInteractive, ProcessingMode::SecureAnimated}) {
    auto document = ParsePresentation(R"svg(
      <image id="i" width="16" height="16"/>
      <set href="#i" attributeName="href" to="https://example.invalid/image.png" dur="10s"/>)svg");
    auto loader = std::make_unique<AnimationRecordingLoader>();
    const auto& requests = loader->requests;
    auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
    resources.setResourceLoader(std::move(loader));
    resources.setProcessingMode(mode);
    document.setTime(1);
    (void)RenderDocumentWithActiveBackend(document);
    if (mode == ProcessingMode::DynamicInteractive) {
      EXPECT_THAT(requests, testing::ElementsAre("https://example.invalid/image.png"));
    } else {
      EXPECT_THAT(requests, testing::IsEmpty());
    }
  }
}

TEST(RendererAnimationPresentation, AnimatedUsePreservesSecureModesAndLoaderDenial) {
  for (const auto mode : {ProcessingMode::DynamicInteractive, ProcessingMode::SecureAnimated,
                          ProcessingMode::SecureStatic}) {
    SCOPED_TRACE(static_cast<int>(mode));
    SVGDocument::Settings settings;
    settings.processingMode = mode;
    auto loader = std::make_unique<AnimationRecordingLoader>();
    loader->failure = ResourceLoaderError::SandboxViolation;
    const auto& requests = loader->requests;
    settings.resourceLoader = std::move(loader);
    int parseCalls = 0;
    settings.svgParseCallback = [&parseCalls](
                                    const std::vector<uint8_t>&,
                                    ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
      ++parseCalls;
      return std::nullopt;
    };
    ParseWarningSink warnings;
    auto parsed =
        parser::SVGParser::ParseSVG(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
        <defs><rect id="r" width="16" height="16" fill="red"/></defs>
        <use href="#r"><set attributeName="href" to="denied.svg#r" begin="1s" dur="1s"/></use>
      </svg>)svg",
                                    warnings, parser::SVGParser::Options(), std::move(settings));
    ASSERT_THAT(parsed, NoParseError());
    auto& document = parsed.result();
    const std::string source(document.source());
    bool requestExpected = false;
    for (const double time : {0.5, 1.5, 3.0, 1.5}) {
      SCOPED_TRACE(time);
      auto expected = ParsePresentation(mode != ProcessingMode::SecureStatic && time == 1.5
                                            ? "<rect width='0' height='0'/>"
                                            : "<rect width='16' height='16' fill='red'/>");
      document.setTime(time);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "animated-use-loader-boundary", editor::tests::PixelmatchIdentityParams());
      EXPECT_EQ(document.source(), source);
      requestExpected |= mode == ProcessingMode::DynamicInteractive && time == 1.5;
      if (requestExpected) {
        EXPECT_THAT(requests, testing::ElementsAre("denied.svg"));
      } else {
        EXPECT_THAT(requests, testing::IsEmpty());
      }
      EXPECT_EQ(parseCalls, 0);
    }
  }
}

TEST(RendererAnimationPresentation, ForeignResourceTargetDoesNotLoadAnimatedHref) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"/>
    <set href="#i" attributeName="href" to="https://example.invalid/image.png" dur="10s"/>)svg");
  auto loader = std::make_unique<AnimationRecordingLoader>();
  const auto& requests = loader->requests;
  document.registry().ctx().get<components::ResourceManagerContext>().setResourceLoader(
      std::move(loader));
  document.querySelector("#i")->setAttribute("xmlns", "urn:foreign");
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  EXPECT_THAT(requests, testing::IsEmpty());
}

TEST(RendererAnimationPresentation, RepeatingImageReferencesReuseDecodedFrames) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16">
      <set attributeName="href" begin="0;2;4;6;8;10;12;14" dur="1s"
        to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg=="/>
      <set attributeName="href" begin="1;3;5;7;9;11;13;15" dur="1s"
        to="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgPRmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"/>
    </image>)svg");
  auto& registry = document.registry();
  registry.ctx().erase<components::ResourceManagerContext>();
  registry.ctx()
      .emplace<components::ResourceManagerContext>(registry, 512)
      .setProcessingMode(ProcessingMode::DynamicInteractive);
  const Entity target = document.querySelector("#i")->entityHandle().entity();
  std::array<uint64_t, 2> revisions = {};
  for (int frame = 0; frame < 16; ++frame) {
    SCOPED_TRACE(frame);
    const int index = frame % 2;
    document.setTime(frame + 0.25);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(index ? 0 : 255, 0, index ? 255 : 0, 255));
    const auto* loaded = registry.try_get<components::LoadedImageComponent>(target);
    ASSERT_THAT(loaded, testing::Ne(nullptr));
    if (frame < 2) {
      revisions[index] = loaded->revision();
    }
    EXPECT_EQ(loaded->revision(), revisions[index]);
  }
}

class AnimationImageLoader final : public ResourceLoaderInterface {
public:
  explicit AnimationImageLoader(bool blue) : blue_(blue) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view) override {
    return DecodeBase64Data(blue_ ? "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYP"
                                    "gPRmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"
                                  : "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8"
                                    "AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg==")
        .result();
  }

private:
  bool blue_;
};

class CyclingAnimationImageLoader final : public ResourceLoaderInterface {
public:
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    requests.emplace_back(url);
    const std::string_view encoded =
        url == "b.png"   ? "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgP"
                           "RmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"
        : url == "c.png" ? "iVBORw0KGgoAAAANSUhEUgAAAAMAAAAECAIAAADETxJQAAAAD0lEQVR4nGNg+M8A"
                           "RXhZAO84C/UG7FlZAAAAAElFTkSuQmCC"
                         : "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AA"
                           "RAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg==";
    return DecodeBase64Data(encoded).result();
  }

  std::vector<std::string> requests;
};

TEST(RendererAnimationPresentation, AnimatedImageCacheEvictsBeforeFamilyBudgetRefusal) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16" preserveAspectRatio="none"><animate attributeName="href"
      values="a.png;b.png;c.png" calcMode="discrete" dur="3s"
      repeatCount="indefinite"/></image>)svg");
  auto& registry = document.registry();
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.decodedImageBytes = 64;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  registry.ctx().get<components::DocumentResourceFamilyContext>().budget = family;
  registry.ctx().erase<components::ResourceManagerContext>();
  auto& resources = registry.ctx().emplace<components::ResourceManagerContext>(registry, 1024);
  auto loader = std::make_unique<CyclingAnimationImageLoader>();
  const auto& requests = loader->requests;
  resources.setResourceLoader(std::move(loader));
  resources.setProcessingMode(ProcessingMode::DynamicInteractive);
  const Entity target = document.querySelector("#i")->entityHandle().entity();

  for (const double time : {0.25, 1.25, 2.25, 3.25, 4.25, 5.25}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const int phase = static_cast<int>(time) % 3;
    auto expected =
        ParsePresentation(phase == 0   ? R"(<rect width="16" height="16" fill="red"/>)"
                          : phase == 1 ? R"(<rect width="16" height="16" fill="blue"/>)"
                                       : R"(<rect width="16" height="16" fill="lime"/>)");
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "animated-image-cache-family-budget", editor::tests::PixelmatchIdentityParams());
    const auto* loaded = registry.try_get<components::LoadedImageComponent>(target);
    ASSERT_THAT(loaded, testing::Ne(nullptr));
    ASSERT_TRUE(loaded->image.has_value());
    EXPECT_EQ(loaded->image->data.size(), phase == 2 ? 48u : 16u);
    EXPECT_LE(family->retainedBytes(components::DocumentResourceFamilyBudget::Kind::DecodedImage),
              64u);
  }
  EXPECT_THAT(requests, testing::Contains("c.png"));
}

TEST(RendererAnimationPresentation, DecodedAnimationCacheRespectsSecureModeChanges) {
  auto document = ParsePresentation(R"svg(
    <image id="i" width="16" height="16"><animate attributeName="href"
      values="a.png;b.png" dur="2s" repeatCount="indefinite"/></image>)svg");
  auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(false));
  for (const double time : {0.25, 1.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
  }
  resources.setProcessingMode(ProcessingMode::SecureAnimated);
  document.setTime(2.25);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 0));
}

TEST(RendererAnimationPresentation, DecodedAnimationCacheRespectsLoaderReplacement) {
  auto document = ParsePresentation(R"svg(
    <image width="16" height="16"><animate attributeName="href"
      values="a.png;b.png" dur="2s" repeatCount="indefinite"/></image>)svg");
  auto& resources = document.registry().ctx().get<components::ResourceManagerContext>();
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(false));
  for (const double time : {0.25, 1.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(255, 0, 0, 255));
  }
  resources.setResourceLoader(std::make_unique<AnimationImageLoader>(true));
  for (const double time : {2.25, 3.25, 4.25}) {
    document.setTime(time);
    EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
                test::Rgba(0, 0, 255, 255));
  }
}

TEST(RendererAnimationPresentation, TypedReferenceEditEqualToPresentationBecomesBase) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="4" height="16"/><rect id="b" x="8" width="4" height="16"/></defs>
    <use id="u" href="#a"><set attributeName="href" to="#b" dur="1s"/></use>)svg");
  document.setTime(0.5);
  (void)RendererTestUtils::renderToAsciiImage(document);
  document.querySelector("#u")->cast<SVGUseElement>().setHref("#b");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("........@@@@...."));
}

TEST(RendererAnimationPresentation, MissingAndCyclicReferencesDisappearThenRestore) {
  for (const auto* target : {"#missing", "#u"}) {
    auto document = ParsePresentation(
        std::string("<defs><rect id='a' width='4' height='16'/></defs><use id='u' href='#a'>") +
        "<set attributeName='href' to='" + target + "' dur='1s'/></use>");
    document.setTime(0.5);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("................"));
    document.setTime(2);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("@@@@............"));
  }
}

TEST(RendererAnimationPresentation, XlinkReferenceAnimationRespectsHrefPrecedence) {
  for (bool preferred : {false, true}) {
    auto document =
        ParsePresentation(std::string("<defs><rect id='a' width='4' height='16'/><rect id='b' "
                                      "x='8' width='4' height='16'/></defs>") +
                          "<use xmlns:xlink='http://www.w3.org/1999/xlink' xlink:href='#a' " +
                          (preferred ? "href='#a'" : "") +
                          "><set attributeName='xlink:href' to='#b' dur='1s'/></use>");
    document.setTime(0.5);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith(preferred ? "@@@@............" : "........@@@@...."));
  }
}

TEST(RendererAnimationPresentation, MaskBoundsAnimateAndRestore) {
  auto document = ParsePresentation(R"svg(
    <defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="4" height="16">
      <rect width="16" height="16" fill="white"/>
    </mask></defs><rect width="16" height="16" mask="url(#m)"/>
    <set href="#m" attributeName="width" to="12" dur="1s"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, ClipPathUnitsAnimateAndRestore) {
  auto document = ParsePresentation(R"svg(
    <defs><clipPath id="c" clipPathUnits="userSpaceOnUse"><rect width=".5" height="1"/></clipPath></defs>
    <rect width="8" height="16" clip-path="url(#c)"/>
    <set href="#c" attributeName="clipPathUnits" to="objectBoundingBox" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><clipPath id="c" clipPathUnits="objectBoundingBox"><rect width=".5" height="1"/></clipPath></defs>
    <rect width="8" height="16" clip-path="url(#c)"/>)svg");
  document.setTime(2);
  const auto base = RendererTestUtils::renderToAsciiImage(document).generated;
  document.setTime(0.5);
  const auto result = RendererTestUtils::renderToAsciiImage(document).generated;
  EXPECT_EQ(result, RendererTestUtils::renderToAsciiImage(expected).generated);
  EXPECT_NE(result, base);
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, base);
}

TEST(RendererAnimationPresentation, MarkerReferencePointAnimatesAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="16" markerHeight="16" refX="0" refY="0" orient="0">
      <rect width="4" height="4"/>
    </marker></defs><path d="M8 8H12" stroke="black" marker-start="url(#m)"/>
    <set href="#m" attributeName="refX" to="4" dur="1s"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 6, 10),
              test::Rgba(0, 0, 0, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 6, 10),
              test::Rgba(0, 0, 0, 0));
}

TEST(RendererAnimationPresentation, PreserveAspectRatioAnimatesAndRestores) {
  auto document = ParsePresentation(R"svg(
    <svg width="16" height="16" viewBox="0 0 8 4" preserveAspectRatio="none">
      <rect width="8" height="4"/>
      <set attributeName="preserveAspectRatio" to="xMidYMid meet" dur="1s"/>
    </svg>)svg");
  document.setTime(0.5);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 1),
              test::Rgba(0, 0, 0, 0));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 1),
              test::Rgba(0, 0, 0, 255));
}

TEST(RendererAnimationPresentation, PathLengthAnimatesDashCalibration) {
  auto document = ParsePresentation(R"svg(
    <path d="M0 8H16" pathLength="16" stroke="black" stroke-width="4" stroke-dasharray="4 4">
      <set attributeName="pathLength" to="8" dur="1s"/>
    </path>)svg");
  document.setTime(0.5);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 6, 8),
              test::Rgba(0, 0, 0, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 6, 8),
              test::Rgba(0, 0, 0, 0));
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, TextPathOffsetAnimatesAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="p" d="M0 12H16"/></defs>
    <text font-size="12"><textPath id="t" href="#p" startOffset="0">H</textPath></text>
    <set href="#t" attributeName="startOffset" to="8" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><path id="p" d="M0 12H16"/></defs>
    <text font-size="12"><textPath href="#p" startOffset="8">H</textPath></text>)svg");
  document.setTime(2);
  const auto base = RendererTestUtils::renderToAsciiImage(document).generated;
  document.setTime(0.5);
  const auto result = RendererTestUtils::renderToAsciiImage(document).generated;
  EXPECT_EQ(result, RendererTestUtils::renderToAsciiImage(expected).generated);
  EXPECT_NE(result, base);
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, base);
}
#endif

TEST(RendererAnimationPresentation, ToOnlyGeometryStartsFromTypedWidth) {
  auto document = ParsePresentation(R"svg(
    <rect id="r" width="4" height="16"><animate attributeName="width" to="20" dur="2s"/></rect>)svg");
  document.querySelector("#r")->cast<SVGRectElement>().setWidth(Lengthd(12));
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@@@@@"));
}

TEST(RendererAnimationPresentation, GradientToOnlyUsesTheDefaultEndpoint) {
  auto document = ParsePresentation(R"svg(
    <defs><linearGradient id="g"><stop stop-color="red"/><stop offset="1" stop-color="blue"/>
      <animate attributeName="x2" to="0%" dur="2s"/>
    </linearGradient></defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><linearGradient id="g" x2="50%"><stop stop-color="red"/><stop offset="1" stop-color="blue"/>
    </linearGradient></defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "gradient-underlying-endpoint", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPresentation, FilterImageFragmentHrefAnimatesAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="16" height="16" fill="red"/>
      <rect id="b" width="16" height="16" fill="blue"/>
      <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
        <feImage id="i" href="#a" x="0" y="0" width="16" height="16"/>
      </filter>
    </defs><rect width="16" height="16" filter="url(#f)"/>
    <set href="#i" attributeName="href" to="#b" dur="1s"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 255, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(255, 0, 0, 255));
}

TEST(RendererAnimationPresentation, DiscreteToOnlyUsesTheCascadedBaseValue) {
  auto document = ParsePresentation(R"svg(
    <rect width="16" height="16" visibility="visible" style="visibility:hidden">
      <animate attributeName="visibility" to="visible" dur="2s"/>
    </rect>)svg");
  document.setTime(0.25);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 0));
  document.setTime(1.5);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 255));
}

TEST(RendererAnimationPresentation, MixedGradientUnitsUseTheirCoordinateSystem) {
  struct Case {
    const char* base;
    const char* extra;
    const char* to;
    const char* expected;
  };
  for (const auto item : {Case{"", "", "0.5", "0.75"},
                          Case{"<linearGradient id='base' gradientUnits='userSpaceOnUse' x2='16'/>",
                               " href='#base'", "50%", "12"}}) {
    SCOPED_TRACE(item.to);
    const std::string beginning =
        std::string("<defs>") + item.base + "<linearGradient id='g'" + item.extra;
    const std::string ending =
        "><stop stop-color='red'/><stop offset='1' "
        "stop-color='blue'/></linearGradient></defs><rect width='16' height='16' fill='url(#g)'/>";
    auto document =
        ParsePresentation(beginning + ending + "<animate href='#g' attributeName='x2' to='" +
                          item.to + "' dur='2s'/>");
    auto expected = ParsePresentation(beginning + " x2='" + item.expected + "'" + ending);
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         std::string("gradient-coordinate-system-") + item.expected,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationPresentation, RemovingAnimatedTextPathHrefPreservesInlineGeometry) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 8H16"/><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath id="t" href="#a" path="M0 12H16" startOffset="4">H</textPath></text>
    <set href="#t" attributeName="href" to="#b" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <text font-size="12"><textPath path="M0 12H16" startOffset="4">H</textPath></text>)svg");
  document.setTime(0.5);
  (void)RenderDocumentWithActiveBackend(document);
  document.querySelector("#t")->removeAttribute("href");
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, TextPathSourceRestorationHasAnIndependentWorkBudget) {
  auto document = ParsePresentation(R"svg(
    <text font-size="12"><textPath id="t" path="M0 12H16">H</textPath></text>
    <set href="#t" attributeName="path" to="M0 8H16" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <text font-size="12"><textPath path="M8 12H16">H</textPath></text>)svg");
  document.setTime(0.5);
  (void)RenderDocumentWithActiveBackend(document);
  auto& budget = document.registry().ctx().get<components::AnimationResourceBudget>();
  EXPECT_EQ(budget.reserveGeometryWork(budget.remainingGeometryWork()), true);
  document.querySelector("#t")->setAttribute("path", "M8 12H16");
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}
#endif

TEST(RendererAnimationPresentation, RemovingAnimatedFilterImageHrefPreservesAspectRatio) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="16" height="8"/>
      <filter id="f"><feImage id="i" href="#a" preserveAspectRatio="none"/></filter>
    </defs><rect width="16" height="16" filter="url(#f)"/>
    <set href="#i" attributeName="href" to="#a" dur="1s"/>)svg");
  document.setTime(0.5);
  (void)RenderDocumentWithActiveBackend(document);
  document.querySelector("#i")->removeAttribute("href");
  document.setTime(2);
  (void)RenderDocumentWithActiveBackend(document);
  const auto* image =
      document.querySelector("#i")->entityHandle().try_get<components::FEImageComponent>();
  ASSERT_THAT(image, testing::Ne(nullptr));
  EXPECT_EQ(image->preserveAspectRatio, PreserveAspectRatio::None());
}

}  // namespace
}  // namespace donner::svg
